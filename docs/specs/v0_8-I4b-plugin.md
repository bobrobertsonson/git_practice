# v0.8 I4b — plugin: calibration everywhere it plays, stereo DI, legacy presets

Parent: `docs/specs/v0_8-I4-default_on.md` (lead decisions 1, 2, 4 and 7). Builds on I4a (schema v5, `autoTrimCal`, offline
tools, export rule).
- Owner: dsp-engineer.
- Reviewer: reviewer.
- The plugin cannot be built in the lead's container. CI is the build of record, so keep plugin edits small and covered by
  plugin/tests.
- I4b flips no default. With "Calibrated input levels" off, playback stays bit-identical to today, except for the stereo-DI fix
  (part 1), which is a bug fix independent of calibration.

## 1. Stereo DI (lead decision 4; the main lead's constraints)

Today `PluginProcessor.cpp:1209` sums `0.5 * (L + R)`. A guitar on one channel of a stereo track therefore arrives −6 dB, which
moves every planned NAM drive.

- **Settings, next to the device record (not in the preset):** "Input channel": Auto (the default) / L / R / Mix. Stored with
  the device record and written off the audio thread.
- **Auto, on a stereo input layout:**
  - Start as Mix.
  - Latch a single channel once there are ≈2 s of *played* frames (the same played test as the drift tap: DI floor + 12 dB)
    in which one channel is ≥ 30 dB below the other. Measure the levels over those played frames, not over the whole block.
  - Re-evaluate only:
    - on `prepare()`;
    - on a bus-layout change;
    - after a sustained reversal: the latched channel is ≥ 30 dB below the other for ≥ 10 s of played frames.
  - Never switch mid-note: switch only at the start of a block whose played state is false (or after the 10 s reversal), and
    cross-fade old → new over ≥ 20 ms.
  - Allocation-free and lock-free. The decision (Mix / L / R, auto or forced) is published through an atomic for the UI.
- **Forced L / R / Mix:** take that channel (Mix = today's 0.5·(L+R)) with no detection. A change from Settings fades over
  ≥ 20 ms.
- **Mono layout:** unchanged. Bit-identical.
- **UI:** a small read-only line where input notices already appear, e.g. "Input: L only (auto)". Reuse existing components;
  the user designs the UI.
- **Order of the signal path:** the decision acts where the 0.5·(L+R) sum is today. The input meter, the DI recorder, the drift
  tap and the gate key all see the chosen signal.
- **Tests** (plugin/tests):
  - Silent R: latches L within 3 s of played audio, and the level equals L's (not −6 dB).
  - Silent L: latches R.
  - Both channels active (within 30 dB): stays Mix.
  - Mid-stream reversal: no switch before 10 s of played frames; switches after; the cross-fade has no step greater than the
    20 ms ramp allows.
  - Mono layout: output bit-identical to before.
  - The forced modes work, and the UI string shows the decision.
  - Zero allocations in `processBlock` across a latch and a switch.

## 2. Calibrated trim at play time (lead decision 2)

`output.autoTrimCalDb` / `autoTrimCalHash` are written by I4a but the plugin never reads them.
- For a `calibrated` preset with calibration on:
  - LEVEL MATCH uses `autoTrimCal` when its hash matches. Otherwise it re-measures.
  - Correct it for the user's device by re-measuring on the calibrated chain at the user's device level, cached per device
    under the existing `|cal:` key.
  - Never write a device-specific value back into the preset. Only a measurement at the assumed +12 dBu reference may be
    stored as `autoTrimCal`.
- For `legacy` presets, or with calibration off: today's `autoTrim`. Unchanged.
- **Export and match glue:** `MatchGlue.cpp:~160` and `ExportGlue.cpp:~139` clear `autoTrim` before hashing and writing. Clear
  `autoTrimCal` (db and hash) there too. The trained chain never carries a trim, and a stale `autoTrimCal` must not change the
  "same rig" key.
- **Tests:**
  - A matching hash is used without a re-measure.
  - A stale hash re-measures.
  - A different device level gives a different cached value and leaves the preset unchanged.
  - Export and match keys are identical with and without an `autoTrimCal` value.

## 3. PreviewWorker calibration (decision 7 prerequisite)

`browser/PreviewWorker.cpp` renders previews with no calibration. It must render with the same `ChainCalibration` as playback
(device record, toggle, preset mode), including the hop rules.
- Test: with calibration on, a preview of an amp swap shows the same planned gain difference as playback, within the existing
  preview tolerance. With calibration off, the preview is bit-identical to before.

## 4. Legacy presets (decision 1, cheap paths)

- **Hint.** A loaded `legacy` preset with calibration on shows a small non-blocking hint: "Legacy levels — this preset was
  made before calibrated input." Put it where preset notices already appear.
- **"Use calibrated levels"** (one click, on the hint):
  - Sets the current preset's `calibration.mode` to `calibrated`. It is a normal edit: undoable with the existing EditHistory,
    marks the preset dirty, and is written on save.
  - Factory presets can be switched for the session. Saving one goes through the existing save-as path; factory files are
    never written.
- **Settings, "Calibrate all user presets…":**
  - A confirmation names the count ("Switch N user presets to calibrated levels? Factory presets are not changed.").
  - On confirm, every user preset file that is `legacy` is rewritten as v5 `calibrated`, off the message thread, with an
    atomic write per file. A failure on one file is reported and does not stop the rest.
  - Never touches `presets/` (factory).
- **Tests:**
  - The hint shows for legacy and not for calibrated.
  - One click flips the mode, undo restores it, and a save writes `calibrated`.
  - Bulk: count correct, factory untouched, legacy user files rewritten, already-calibrated files byte-identical, one
    unwritable file reported while the others still convert.

## 5. Drift notice wording (main lead, 2026-10-08)

Replace the text in `core/src/drift.cpp` `driftNoticeText` with exactly:
`Your playing level is running ~N dB hotter|quieter than when this interface was set up — did the interface gain change?`
Update every test that asserts the old string, and the wording in `docs/specs/v0_8-I3-drift_check.md` and docs/PLUGIN.md.

## 6. Export notes parity

`plugin/src/ExportNotes.cpp` `formatNotesTxt` must show the same calibration lines the match-side `export/notes.py` writes
since I4a:
- the device level, and whether it was assumed;
- `input_level_dbu` / `output_level_dbu` written, or why not, naming the missing term;
- the "set your interface to X dBu" line for loaders that ignore the fields.

Test: the same notes JSON gives the same calibration lines on both sides (a fixture shared by plugin/tests and pytest, or
golden strings).

## 7. Carried polish

- `match/tests/test_export.py`: the G_post sweep covers audible B with a disabled partner at an interior blend value (today
  only the endpoints).

## Acceptance

- Full ctest and pytest green.
- The python-export selection, run locally with the trainer installed, is green:
  `pip install -e 'match[dev,export]' -c match/constraints-export.txt` (CI's index or PyPI), then
  `SAWBLADE_TEST_TRAIN=1 pytest match/tests/test_export*.py match/tests/test_a2_*.py match/tests/test_calibrate_device_null.py`.
- CI green, including the plugin tests.
- With calibration off and a mono input: bit-identical to before I4b.
- Zero allocations in `processBlock` across every new path.
- docs/PLUGIN.md: input channel setting, legacy hint, calibrate-all.

## Out of scope

- The default flip (I4c).
- Restamping factory presets (I4c, after the user's `[i4a-table]` run).
- Guided Measure.
