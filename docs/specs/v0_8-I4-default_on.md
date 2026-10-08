# v0.8 I4 — calibration on by default: presets, offline tools, export, stereo DI

Parent: `docs/specs/v0_8-input_calibration.md` (Task C).
- Builds on I1 (chain), I2 (device record, calibration-aware measurements) and I3 (drift).
- The main lead decided (2026-10-08) that v0.8 is done only when calibration is ON by default, in the plugin and in
  match/tonerender. I4 delivers that.
- Owners: dsp-engineer (core, cli, plugin) and match-engineer (match/, export). The reviewer audits every part.
- Split into parts, each its own commit series and reviewer pass, pushed one at a time:
  - I4a: core and offline.
  - I4b: plugin.
  - I4c: the default flip.

## Lead decisions

1. **Preset schema v5 adds `calibration: {"mode": "calibrated" | "legacy"}`.**
   - New presets and new matches write `"calibrated"`.
   - A v4-or-older preset loads as `"legacy"`: calibration is off for that preset, so it sounds exactly as before.
   - The plugin shows a small, non-blocking "legacy levels — re-match for calibrated input" hint on such presets.
   - Baking per-device absolute gains into old presets was rejected: the device level differs per machine, which would
     break "presets store no absolute gains".
   - Factory presets (`presets/`) are re-stamped to v5 `"calibrated"` only where a render check shows the result is
     still within the existing golden/level tolerances. Otherwise they stay `"legacy"` and are listed in the REPORT
     for the user to re-match.
   - `docs/PRESET_SCHEMA.md` documents v5. The reader still reads v1–v4.
2. **Calibrated trim in the preset.**
   - Store `output.autoTrimCalDb` and `output.autoTrimCalHash` alongside the existing trim. They are measured with
     calibration at the assumed +12 dBu reference device, so they are machine-independent.
   - At play time the plugin applies them and corrects by the difference to the user's device. The output level is
     linear in the device offset only for the input stage, so the correction is a re-measure, not an offset.
   - Cached per-device (the existing `|cal:` key) and never written back with a device-specific value.
3. **Offline tools.**
   - `tonerender --device-dbu X`, and the matcher's equivalent option (and config key), set the device level. Absent,
     they use the assumed +12 dBu and say so in the report.
   - Calibration follows the preset's `calibration.mode`.
   - Both tools record in their reports: the device level, whether it was assumed, the per-block plan, and the
     stereo-DI rule used.
   - The reference DI's own level stays as it is (a NailTheMix DI is not the user's guitar). Calibrating the
     reference DI is out of scope.
4. **Stereo DI (main lead's constraints).**
   - Plugin, auto mode:
     - Latch once per stream after about 2 s of played frames with one channel ≥ 30 dB below the other.
     - Re-evaluate only on `prepare()`, on a layout change, or after a sustained reversal of ≥ 10 s. Never switch
       mid-note; fade any switch over ≥ 20 ms.
     - Allocation-free and lock-free on the audio thread.
     - The UI shows the decision, e.g. "Input: L only (auto)".
   - Settings override next to the device record, not in the preset: Auto / L / R / Mix.
   - Offline (tonerender, matcher): no auto-detection. A stereo DI file uses the louder channel by whole-file RMS
     (the default), or an explicit `--di-channel L|R|mix`. The rule is recorded in the report, and output is
     bit-identical regardless of block size.
5. **Export.**
   - The exported `.nam` writes `input_level_dbu` and `output_level_dbu`, in the NAM trainer's metadata fields
     (REPORT A1), computed from the device level used for the export render and the chain's output reference.
   - When the device level was assumed, the export notes say so, and say which interface level to set on a loader
     that does not honour the fields.
6. **Anagram device preset.** Search once more for a citable maximum input level, from the Darkglass manual or spec
   sheet. If none is found, add no preset; the user enters a value or measures. Never guess.
7. **Default flip (I4c, last).**
   - `calibratedInputLevels` defaults ON for new installs. Existing installs keep an explicit OFF only if the user set
     it; an untouched default becomes ON.
   - tonerender and the matcher default to calibrated.
   - Prerequisites before the flip:
     - PreviewWorker renders with the same calibration as playback.
     - Every measurement path is calibration-aware: auto trim, path LUFS, make-up, level match, preview.
     - The stereo-DI fix is in.

## Tests (each part)

- **I4a:**
  - Schema v5 round trip, and v4 loads as legacy, bit-identical to before.
  - Calibrated-trim fields and staleness.
  - tonerender `--device-dbu` changes the planned gains exactly, and the report fields are present.
  - The offline stereo rule: louder channel, `--di-channel`, block-size independent.
  - Export metadata is written, and the trainer-side fields round-trip in pytest.
  - The matcher records its calibration.
- **I4b:**
  - Stereo auto: silent R, silent L, both active (mean), a mid-stream reversal (no switch before 10 s), and the mono
    layout unchanged.
  - The override works, and the UI shows the decision.
  - PreviewWorker is calibrated.
  - The legacy-preset hint appears.
  - No allocation in `process()`.
- **I4c:**
  - The default is on for a fresh settings file, and an explicit off is preserved.
  - Every committed preset renders within its golden/level tolerance in its stamped mode.
  - The full suite is green.

## Carried I3 polish (rides with I4a's first push)

- REPORT CI-294 root-cause wording: the played-time arithmetic is the cause, and feeding both channels was
  defensive only.
- The stale test comment in `test_device_calibration_engine.cpp` (~433–436).
- A core test: a sustained ±4 dB passage of ≥ 35 s raises no notice.
- A chain.h comment on the key high-pass mismatch in the seed.
- A REPORT note on the −40 dBFS floor clamp.

## Out of scope

- Guided Measure: designed in REPORT I2; it needs a reference of known dBu.
- Calibrating the matcher's reference DI.
- The irlib flake, which v0.4M owns.
