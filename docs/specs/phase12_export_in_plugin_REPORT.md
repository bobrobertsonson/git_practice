# Phase 12 report: NAM export from inside the plugin

Spec: `docs/specs/phase12_export_in_plugin.md`, with the lead's "Lead refinements (2026-10-05)" section
(the CLI-to-plugin contract) added in `a630e12`.
Status: **accepted by the lead after reviewer ACCEPT** (plugin half: round 1 ACCEPT with non-blocking
items, done in `fde695c`; CLI half: REVISE, REVISE, then ACCEPT in round 3). No audio, captures, exports,
weights or screenshots are committed. Tests synthesise their presets, takes and fake tools into temp dirs.

Artifact: **Sawblade Export**, https://claude.ai/artifact/UxxHQd4hyQ1U8S9pzc1YyF (private until shared). It shows the Standalone app at 1x, 1280x800, in the
three panel states: configure, training (epoch 7 of 10, best ESR, ETA, CANCEL), result (MET, with the
numbers, REVEAL / OPEN FOLDER / A/B LISTEN, the model and sidecar paths). The rig, capture names and
figures in the shots are test labels from a synthetic preset and a fake `sawblade-export`.

Branch: `claude/sawblade-p12-export-plugin` (head `516e201`). Commits, in order:
- `7ea4efc` spec (patch of the working branch's `4e0f271`), `a630e12` lead refinements.
- `9341904` runner contract, export glue, plugin-state `export` object.
- `336b69f` `ExportPanel` (configure / training / result), top bar + play-along wiring, `MatchScreen` is
  MATCH only.
- `70b3c8a` tests (runner, glue, allocation guard, editor, screenshots) and docs.
- `fde695c` review follow-up (core `export` key test, refused WITH CAB comp disables TRAIN, cached export
  plan, safe `MatchSettings::props`).
- `f10228f`, `499f1c7`, `75ec463` CLI contract (`--progress-json`, exit codes, SIGINT cancel,
  `--exports-root`, `--di builtin`, `-nc` stem; README), merged as `407b8f1`.
- `6a03e61` merge of the working branch (`d6b6c4c`: phases 5.1b, 6b, 7c), three conflicts resolved.
- `cefedb7`, `fafa54a` CLI review fixes (cancel through `_Cancel`, late stop checks, last-batch handling),
  merged as `516e201`.

## Test counts
| suite | before | after |
|---|---|---|
| `ctest --test-dir build-plugin` (clang, `-Werror`, `-DSAWBLADE_BUILD_PLUGIN=ON`) | 462 | 559 (483 before the working-branch merge brought in 76 upstream tests); 1 env-gated separator test skipped |
| pluginval, VST3, strictness 10 | not registered here | SUCCESS, 14 s |
| `match/` pytest (core bindings built, no torch / nam) | 313 passed, 2 failed, 19 skipped | 353 passed, 2 failed, 13 skipped |

The two pytest failures are pre-existing and unrelated (`tests/test_t3k_browser_cli.py`, exit code 4 vs 1).
The skips are the `SAWBLADE_TEST_TRAIN=1` real-trainer smoke tests and one irMix case.

## What was built

### CLI (`match/sawblade_match/export/`)
- `--progress-json <path>`: `progress.py` writes
  `{stage, fraction, etaSeconds, epoch, epochs, bestEsr, message, outDir, resumable, elapsedSeconds}`
  atomically, at every stage change, every epoch end, throttled to 1 Hz per batch and per validation batch,
  and once more as the last act. Stages `plan → signal → render → train → validate → done`, or `cancelled` /
  `error`. `fraction` is clamped monotonic within a run (0.02 / 0.05 / 0.10 / 0.10–0.90 / 0.90–0.99 / 1.0).
- Exit codes: 0 finished (met, or not judged for FEATHER / LITE), 2 trained but NOT MET (with
  `--require-accept`; files written), 1 refused or error, 130 interrupted. `--require-accept` with
  `--no-validate` is refused up front.
- SIGINT (`stop.py`): the first SIGINT sets a flag and restores the default handler. The trainer callback
  raises a private `_Cancel` at the next batch end (no validation pass, no epoch-end hooks), so the last
  complete epoch's `checkpoint/` stays intact; `progress.json` gets `interrupted: true`, the progress file
  says `cancelled` / `resumable: true`, no report is written, exit 130. A SIGINT during an epoch's last
  batch lets that epoch validate and checkpoint first, then cancels (one validation pass). Stop checks
  also run after training, between the renders, between the validation steps and before the listening
  file, so a late SIGINT never ends as a silent exit 0.
- `--exports-root <dir>`: output dir `<dir>/<name>-<mode>-<size>-<ts>`; `--resume auto` searches it.
- `--di builtin`: the DI-excerpt validation and the A/B listening file use the built-in held-out signal;
  a missing default test DI falls back to it with a log line.
- `-nc` stem when any capture is `cc-by-nc*` (`.nam`, IR and directory). The `.nam`
  `metadata.sawblade` block already carried preset sha, mode, size, attribution with licences, the licence
  note and `nonCommercial` (phase 4); unchanged.

### Plugin (`plugin/src/`)
- **`ExportPanel`** replaces the Export mode of `MatchScreen`; opened from the top-bar EXPORT NAM and the
  play-along EXPORT NAM, in Standalone and plugin mode alike. Layout after `design/mockups/FullExport.dc.html`:
  the three option cards with the exactness line (NO-CAB + IR / WITH CAB / STUDIO BLEND), size with "last
  run: N min" per size from `MatchSettings`, validation DI (LAST TAKE / BUILT-IN SIGNAL), the comp row
  only when the comp is on (DROP COMP exact / KEEP COMP → `--allow-inexact`), output folder with CHOOSE…,
  the rig summary, credits with licences and the NON-COMMERCIAL badge, "what goes into the model", the
  personal-use notice, TRAIN EXPORT, and a RESUME button when a cancelled run with a checkpoint exists for
  the same rig (keyed by the sha256 of the exported preset file; the CLI's identity check is the final
  guard). Mode defaults follow the cab mode (shared / irMix → NO CAB, perPath → WITH CAB with NO CAB
  disabled); a saved mode is honoured only while exact.
- **Training view:** progress bar, epoch N / M, best ESR, elapsed and ETA, CANCEL (SIGINT to the process
  group, then SIGTERM, then SIGKILL, 15 s grace each).
- **Result view:** held-out ESR and DI LTAS error with limits, MET / NOT MET / NOT JUDGED, trained-in time,
  REVEAL, OPEN FOLDER, A/B LISTEN (`listen/ab_original_then_export.mp3`, else `.wav`, system player; hidden
  when absent), model and sidecar paths, the licence note.
- **`JobRunner`:** export jobs probe `--help` for `--progress-json` and pass it; `--require-accept`,
  `--di`, `--allow-inexact`, `--exports-root` or `--resume <dir>`; no `--out`. Exit 2 with a report is
  `Succeeded` with `accepted = "NOT MET"`, 130 is `Cancelled`. On success the monitor thread copies the
  exported preset to `<outDir>/<nam stem>.sawblade.json` (byte-identical) and records
  `exportWallSeconds.<size>`. `job.json` gains `sourceSha256`, `sourcePreset`, `exportsRoot`,
  `allowInexact`, `diBuiltin`, `outDir`, `accepted`, `resumable`, `sidecar`. The 4.1 checkpoint parser
  stays as the fallback.
- **`ExportGlue`:** rig summary (chain, cab mode, licences, NC), effective mode, `planExport`,
  `buildExportRequest`, `findResumableExport`, `buildResumeRequest`. The resolved preset is written to
  `<jobs>/inputs/<sha16>.preset.json`, content-addressed, so the same rig is always the same file.
  DROP COMP writes the copy with `busComp.enabled = false`; the sidecar is that exported file.
- **State:** optional `"export"` object `{mode, size, diSource, compChoice, outputFolder}` beside
  `playAlong` in `getStateInformation`, omitted when all defaults, dropped when not an object. The core
  parser accepts the key (`core/src/preset.cpp`, documented in `docs/PRESET_SCHEMA.md`); `toJson` never
  writes it, so preset hashing is unchanged.
- **Audio thread:** untouched. A test runs `processBlock` under the allocation and lock guards while a
  gated fake export runs and a second thread polls the glue like the panel's timer: 0 allocations, 0 locks.

## Deviations from the spec as written (lead decisions)
- DROP COMP is applied in the exported preset copy, because `plan.py` refuses a no-cab export with the comp
  on unless `--allow-inexact`; the live rig is untouched and the sidecar is the file that was trained.
- WITH CAB with a comp release over 150 ms shows the refusal note and disables TRAIN EXPORT.
- A/B LISTEN opens the listening file with the system player (no in-plugin file player).
- The spec's "matcher has no `--progress-json`" note in the lead refinements predates the merge of phase
  6b, which added one; the export shape is a superset of the matcher's.
- `--require-accept` exits 0 for FEATHER / LITE ("not judged"): the phase 4 thresholds apply to STANDARD.
- The real Lightning callback path (`train.py`) could not be executed here (no torch, no
  `neural-amp-modeler`); it was reviewed against Lightning 2.x hook order and covered by fake-trainer
  decision tests. **An end-to-end check on a machine with the export extra installed is still owed:** a
  short FEATHER run, SIGINT mid-epoch, then RESUME from the panel.

## Proposed extras (not implemented)
- NEW EXPORT on the result view; show `plan.bypassed` / inexact notes from the report.
- Offer `--allow-inexact` when WITH CAB is refused for a long release.
- An early check that an explicit `--di FILE` exists, before training.
- README note: a SIGINT inside the final configured epoch reports "cancelled"; `--resume` then finishes at
  once through the already-done path and exports, so nothing is lost.
- Clean old `inputs/*.preset.json` files; a 1 Hz elapsed-time tick independent of the editor timer.

## Process
- The spec commit was delayed on the working branch; the parent handed it over as a patch, applied with
  `git am` (same patch-id as `4e0f271`), and the lead appended the contract section so both implementers
  could start from one document.
- Both implementers (`dsp-engineer`, `match-engineer` in a worktree) ran in parallel against the contract;
  the plugin's fake `sawblade-export` emits exactly the contract shape, so the plugin never depended on the
  real CLI landing first.
- The reviewer audited each half separately: plugin ACCEPT (four non-blocking items done), CLI REVISE
  (Lightning kept running a validation pass after `should_stop`; late SIGINTs were swallowed), REVISE
  (a SIGINT in an epoch's last batch discarded that epoch), ACCEPT.
- The container's usage limit stopped both implementers once (01:39 to 04:50 UTC); both resumed from their
  on-disk state without loss.
- Environment work: JUCE's Linux packages, Xvfb and pluginval were installed and built here; the match
  venv was created and the core bindings built so the mocked export tests run without torch.
