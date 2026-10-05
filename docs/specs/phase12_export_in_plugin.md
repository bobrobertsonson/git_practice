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
