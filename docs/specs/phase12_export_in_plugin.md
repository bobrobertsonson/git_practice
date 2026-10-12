# Phase 12: NAM export from inside the plugin

The last step of the loop for prototype v0.1: the player has a rig they like (matched or
hand-built) and wants it on a live loader pedal. `sawblade-export` (match/, phase 4 + 4.1)
already trains the model from a resolved preset; this phase drives it from the plugin,
shows progress, enforces the export rules, and hands back a `.nam` (+ IR) with the
acceptance report. No new training code.

Owner: `dsp-engineer` (plugin + core glue), `match-engineer` only if the CLI needs a flag.
Reviewer audits; lead accepts.

## Rules carried from CLAUDE.md (the UI must enforce, not just document)
- Gate, reverb/delay/modulation and long-release compression are never trained in. The
  export resolves the preset with the gate off; if the bus comp is on, the UI offers
  "drop the comp (exact)" or "keep it (inexact, reported)" — `--allow-inexact`.
- Live-compatible blend (shared cab) → `nocab` export is exact: `.nam` + the cab IR `.wav`.
- Studio blend (per-path IRs) → only `withcab` is exact; the UI says so and defaults to it.
- Non-commercial: if any capture in the rig is `cc-by-nc*`, the export is marked
  non-commercial in the report and the file name gets a `-nc` suffix.
- Personal use: the export dialog carries the one-line notice that exports trained from
  TONE3000 captures are for the user's own use.

## Plugin
1. **EXPORT NAM** (top-bar button, currently disabled) opens an export panel in the skin
   style: rig summary (chain, cab mode, licences), mode selector (NO CAB / WITH CAB, with the
   exactness note), size (FEATHER / LITE / STANDARD, with the rough train time per size on
   this machine from the last run), DI source (the last recorded take if one exists, else the
   built-in training signal), the comp choice when relevant, output folder, and EXPORT.
2. **Runner:** reuse 6a's child-process runner (`match/.venv` discovery, `--progress-json`)
   to run `sawblade-export <resolved preset> --mode … --size … --out … --progress-json …
   --require-accept` on the loader thread; progress bar with epoch / ESR / ETA; CANCEL sends
   SIGINT and keeps the checkpoint (4.1 resume) so a later EXPORT with the same rig offers
   RESUME.
3. **Result:** the acceptance report (ESR, LTAS error, MET / NOT MET per the phase 4
   thresholds) rendered in the panel; buttons REVEAL IN FINDER / OPEN FOLDER, and A/B LISTEN
   which plays the export's `listen/ab_original_then_export` file if present.
4. **Preset provenance:** the exported `.nam` metadata (NAM's `metadata` block) records the
   Sawblade preset sha, mode, size, licences and the non-commercial flag; the plugin writes a
   sidecar `<name>.sawblade.json` with the full resolved preset.
5. **State:** last export settings persist in plugin state (not in the preset).
6. **Standalone and plugin:** identical; in a host, long trainings keep running if the editor
   closes (the runner lives in the processor, like 6a's match job).

## CLI (match-engineer, only if missing)
- `sawblade-export --progress-json <path>` emitting `{stage, fraction, etaSeconds, epoch,
  bestEsr, message}` at least once per second (same shape family as the matcher's).
- Exit codes: 0 accepted, 2 trained but NOT MET (file still written), 1 error.

## Tests
- Editor test: panel opens from the top bar, mode defaults follow cab mode (shared → NO CAB,
  perPath → WITH CAB), the comp choice appears only when the comp is on, licences shown.
- Runner test with a fake child that emits progress JSON and writes a dummy `.nam` + report:
  progress reaches the UI, cancel sends SIGINT, exit code 2 shows NOT MET, result buttons
  enabled.
- Sidecar JSON equals the resolved preset used.
- Zero allocations on the audio thread while an export runs; pluginval still passes.

## Acceptance
- All tests green; clang -Werror clean.
- Report `docs/specs/phase12_export_in_plugin_REPORT.md` with screenshots of the panel in
  the three states (configure / training / result) on an Artifact "Sawblade Export".
- Reviewer ACCEPT.

## Lead refinements (2026-10-05)

Decisions that pin down what the spec leaves open, so the CLI and the plugin can be built in
parallel against one contract. Where these differ from today's code, the code changes.

### CLI contract (`sawblade-export`, match-engineer)
1. **`--progress-json <path>`** (new). The exporter writes `<path>` atomically (temp + rename)
   at every stage change, at every epoch end, and at least once per second during training
   (throttled per batch), and once more as its last act before exiting. Shape:
   ```json
   {"stage": "plan|signal|render|train|validate|done|cancelled|error",
    "fraction": 0.0, "etaSeconds": -1, "epoch": 0, "epochs": 0, "bestEsr": null,
    "message": "", "outDir": "/abs/path", "resumable": false, "elapsedSeconds": 0.0}
   ```
   `fraction` is monotonic over the whole run: plan ≤ 0.02, signal ≤ 0.05, render ≤ 0.10,
   train 0.10 → 0.90 (by epochs done + batch fraction, or elapsed / `--max-minutes`, whichever
   is larger), validate 0.90 → 0.99, done 1.0. `etaSeconds` is −1 until it can be estimated
   from the training rate. `bestEsr` is the best validation ESR so far (null before the first
   epoch). `resumable` is true once a checkpoint exists. `outDir` is the final output directory.
   The matcher has no `--progress-json` today, so this is the family's first member; the plugin
   already parses `{stage, fraction, etaSeconds, message}` and gains `epoch`, `epochs`,
   `bestEsr`, `outDir`.
2. **Exit codes:** 0 = finished (acceptance met, or not judged for FEATHER/LITE), 2 = trained
   but acceptance NOT MET (files still written; only with `--require-accept`), 1 = refused or
   error (replaces today's 2/3/4), 130 = interrupted by SIGINT. `--require-accept` now means
   "exit 2 when the status is NOT MET"; non-standard sizes are not judged and exit 0 with the
   numbers reported. Update the match tests and the README accordingly.
3. **SIGINT = cancel, keep the checkpoint.** The CLI installs a SIGINT handler that asks the
   trainer to stop at the end of the current batch (`trainer.should_stop`), skips the
   checkpoint write of the partial epoch (the last complete epoch's `checkpoint/` stays, with
   `progress.json` marked `"interrupted": true`), skips validation, writes the progress file
   with `stage: "cancelled", resumable: true`, and exits 130. A second SIGINT, SIGTERM or
   SIGKILL still leaves a consistent checkpoint (4.1's atomic writes).
4. **`--exports-root <dir>`** (new): the output directory becomes `<dir>/<name>-<mode>-<size>-
   <ts>`. `--out` still overrides fully. `--resume auto` searches `--exports-root` when given.
5. **`--di builtin`**: the DI-excerpt validation and the A/B listening file use an excerpt of
   the built-in signal's held-out segment instead of a DI file; the report says
   `diExcerpt.excerpt.file = "builtin"`. When `--di` is omitted and the default test DI does
   not exist, fall back to `builtin` with a log line instead of failing.
6. **`-nc` suffix:** when any capture is `cc-by-nc*`, the file stem (`--name` or the preset
   slug) gets `-nc` appended unless it already ends with it. This names the `.nam`, the IR and
   the output directory. The report's `nonCommercial` and the `.nam` `metadata.sawblade`
   block already carry the flag, the attribution list and the licence note (phase 4); no
   change there.
7. **Interrupt/cancel in the report:** nothing is written to `export_report.json` on cancel.
   On NOT MET the report is complete and `validation.acceptance.status == "NOT MET"`.

### Plugin contract (dsp-engineer)
1. **`ExportPanel`** (`plugin/src/ExportPanel.{h,cpp}`) replaces the Export mode of
   `MatchScreen` (delete that column; `MatchScreen` keeps MATCH only). Opened from the
   top-bar EXPORT NAM and the play-along panel's EXPORT NAM, in Standalone and plugin mode
   alike (the "open the Standalone app" notice goes for EXPORT; MATCH keeps it). Layout after
   `design/mockups/FullExport.dc.html` (option cards with the exactness line, "what goes into
   the model" checklist, credits with licences, the personal-use notice, TRAIN EXPORT), plus
   the spec's size / DI / comp / output-folder rows and a right-hand column for progress and
   the result.
2. **Mode default:** `cab.mode == shared` → NO CAB, `perPath` → WITH CAB, and the NO CAB card
   is disabled for `perPath` with the studio-blend note. A mode saved in plugin state is
   restored only if it is still exact for the loaded rig; otherwise the default applies.
3. **Comp row:** visible only when the bus comp is enabled. In NO CAB it offers DROP COMP
   (exact, default) / KEEP COMP (inexact → `--allow-inexact`); in WITH CAB it reads "trained
   into the model (release ≤ 150 ms)" or the refusal note for a longer release.
4. **DI source:** LAST TAKE (the newest take in the takes dir, shown by name) when one
   exists, else BUILT-IN SIGNAL; `--di <take wav>` or `--di builtin`.
5. **Output folder:** default `appDataDir()/exports`; CHOOSE… changes it; passed as
   `--exports-root`. The runner no longer passes `--out` for exports; the result folder comes
   from the progress file's `outDir` (fallback: `export_report.json` found by scanning the
   root for the newest dir created after the job started).
6. **Runner changes (`JobRunner`):** export jobs probe `--help` for `--progress-json` like
   match jobs and pass it; the 4.1 checkpoint parser stays as the fallback. `ExportRequest`
   gains `exportsRoot`, `allowInexact`, `diBuiltin`, `resumeDir`. Cancel of an export job =
   SIGINT to the process group, SIGTERM after the grace period, SIGKILL after another; match
   cancel is unchanged. `job.json` records `sourceSha256` (sha256 of the resolved preset
   file's bytes), `exportsRoot`, `allowInexact`, and on finish `outDir`, `accepted`
   (`"met" | "NOT MET" | "not judged"`), `resumable`. Exit 2 with a report = state
   `Succeeded` with `accepted = "NOT MET"`; exit 130 = `Cancelled`; else as today.
7. **Sidecar:** on `Succeeded` (met or not) the monitor thread copies the resolved preset file
   that was exported to `<outDir>/<nam stem>.sawblade.json` (byte-identical) and the
   snapshot names it.
8. **RESUME:** when the newest export job is `Cancelled`, its `outDir/checkpoint/progress.json`
   exists without `complete`, and its `sourceSha256` equals the current export source's, the
   panel shows RESUME (epoch N of M, mode, size) next to TRAIN EXPORT. RESUME starts the job
   with `--resume <outDir>` and that run's mode/size; the CLI's own identity check is the
   final guard and its refusal message is shown.
9. **Result view:** held-out ESR, DI LTAS error, limits, and MET / NOT MET / NOT JUDGED in the
   skin's green / red / amber; REVEAL (File::revealToUser), OPEN FOLDER (startAsProcess on the
   folder), A/B LISTEN (opens `listen/ab_original_then_export.mp3`, else `.wav`, with the
   system player; hidden when neither exists). Train time per size "last run: N min" comes
   from `MatchSettings` (`exportWallSeconds.<size>`, written on every finished run).
10. **State:** `getStateInformation` adds an optional `"export"` object
    `{mode, size, diSource, compChoice, outputFolder}` beside `playAlong`, omitted when all
    defaults; non-object values are dropped like `playAlong`. Nothing goes into the preset.
11. **Audio thread:** the panel, runner and sidecar never touch it; the test runs the
    allocation guard around `processBlock` while a fake export job is active.
