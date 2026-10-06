# v0.2.1 — MATCH inside the DAW + import a prerecorded DI: REPORT (interim)

Status: **interim, phase on budget hold** (spec-lead instruction 2026-10-06 11:37 UTC: get the head green, then stop;
resume after the v0.2 merge). Branch `claude/sawblade-v0_2_1-match-in-host`, based on 75f4f64.

**Green head: `10300ed`, CI run 112 (id 37460878996): linux-gcc (ctest + pluginval VST3 level 10), linux-clang-werror,
macos-arm64 (ctest + auval + pluginval AU and VST3 level 10), python: all success.**

## Tasks

| Task | State | Commits | Reviewer |
|---|---|---|---|
| G — LOAD SONG / drop cannot pick a .wav on macOS | done, CI green; **macOS hand check pending (user)** | e3af8da, d98f41f, 14f3758 (test fix) | REVISE → ACCEPT |
| F — refuse a folder that is not a stem set | done, CI green | 855cb30, f9942e3, 3f6b70f, 14f3758 (test fix) | ACCEPT; follow-up ACCEPT |
| E — mac_update installs the separation model | done, CI green | 314ee46, 1ea78d8, 955a39a (merge 0a2657a); panel 14851ca, 3f6b70f | ACCEPT (script); ACCEPT (panel) |
| D — MATCH screen holds its own song + DI inputs | done, CI green (IMPORT DI slot reserved for B) | 5e2b992, 5456a4d, d6bf0ba (CI fix) | REVISE → ACCEPT |
| A — MATCH in plugin mode | done, CI green | 7d05323, 32e0246, 2a06ca8, 10300ed (test fix) | REVISE → ACCEPT |
| B — import DI + whole-song offset search | **matcher part done, CI green; plugin part (IMPORT DI dialog, copy, `--matched`) NOT STARTED** (stopped by the budget hold) | 71b81e2, 726afb4 (merge 5695ba9), 704b55b, 656cd09, af194fb, 224f3da | matcher ACCEPT ×4 rounds (read-only audits); 224f3da **not reviewed** (budget hold) |
| C — doc: the record-in-Logic path | **not started** | — | — |

Lead decisions are recorded in the spec ("Lead decisions", commit 4e8b27d).

## Task G root cause (required by the spec)

**Not reproduced; the cause is inferred.** Proven by reading JUCE 8.0.15 (91ad83ae):
- The old LOAD SONG chooser (`plugin/src/PlayAlongPanel.cpp:635` at 75f4f64) combined `canSelectFiles |
  canSelectDirectories` with a `*.mp3;*.wav;…` filter. JUCE's mac chooser turns that into `setAllowedFileTypes([mp3, wav,
  …])` (`juce_FileChooser_mac.mm:39-61, 107`) plus a `panel:shouldEnableURL:` delegate (`:279-287`, wildcard match on the
  file name, ignoring case). Read in isolation, both enable a `.wav`.
- An AU in Logic runs sandboxed (AUHostingService), so JUCE uses a plain `NSOpenPanel` (`:80`), i.e. the remote
  (powerbox) panel. **Inferred:** the greying comes from that panel's handling of files+directories + allowedFileTypes +
  delegate.
- Drops: JUCE walks from the component under the mouse up to the first interested `FileDragAndDropTarget`
  (`juce_ComponentPeer.cpp:468-474`), so the editor already received drops on the play-along panel in code. **Inferred:**
  Logic / AUHostingService may not forward Finder drags into an out-of-process AU view; the plugin cannot fix that.
- **Logic vs Standalone:** expected to differ (Standalone is not sandboxed, uses JUCE's SafeOpenPanel, its window is a
  real peer). Which app the user saw it in is unknown.

Fix (does not depend on the inferred cause): two one-purpose choosers (SONG FILE… files-only, STEMS FOLDER…
directories-only); on macOS the song chooser uses the filter `*` (JUCE then sets no allowedFileTypes and the delegate
enables every file) and the pick is validated afterwards (`handlePicked`, non-song files refused with a message, never
reaching `loadSong`); the play-along panel is itself a drop target (structural; does not change what a host forwards).

**How it was checked on macOS:** CI macos-arm64 builds and runs the chooser-configuration tests (mac and non-mac
variants), auval and pluginval. A native dialog cannot be driven in CI. **The user is testing SONG FILE… on the Mac by
hand; their result (and which app: Logic / Standalone) is the confirmation the spec requires — pending.**

## Notable findings

- **Task A, job isolation hole (real bug, fixed):** `JobRunner::attachExisting()` adopted the newest job on disk
  regardless of which instance started it, so instance B's MATCH screen could show and apply instance A's result.
  Jobs now carry a per-instance owner id; adoption and pruning are scoped to it.
- **Task B, offset was a no-op in the plugin:** the plugin never passed `--matched`, and the matcher reads
  `--offset-ms` only for a matched pair. Lead decision: "same performance as the song" checkbox (spec, Lead decisions).
- **Task B, NaN bug (fixed):** a running-sum smoothing filter returned tiny negative energies → `sqrt` NaN → NaN NCC →
  `argmax` 0 → every placement 20 ms early (CI run 100). Clamp + finite checks + tests.
- **Task B, placement acceptance:** `(r1 − r2)/σ ≥ 4` alone rejects perfect placements of short DIs (CI run 105:
  r1 0.949, r2 0.492, σ 0.118 → 3.88). Two-route rule; constants uncalibrated (synthetic only).
- **Task D, real UX bug caught by CI (fixed):** a take chosen before any song was loaded showed "No DI chosen".
- **Task F:** AppleDouble `._name.wav` files on exFAT/USB would have refused a valid stem set and been decoded as audio;
  dot-files are now ignored by both the rule and the loader.

## CI history (validation of record)

Run 100 (f9942e3): red — 3 C++ test bugs (F/G), 5 python (NaN). Run 105 (5456a4d): red — 2 Task D tests, 1 python
(confidence). Run 108 (4e8b27d): red — Task A test compile error, 1 stale python assertion. Run 111 (224f3da): python
green, 1 C++ test (ownerless seed). **Run 112 (10300ed): all green.** Runs 93–99, 101–102, 107, 109 were cancelled by
newer pushes.

## Screenshots

None taken in this interim pass (the plugin does not build in the cloud container; CI does not upload screenshots of the
take band). Owed with Task B's take band (IMPORTED tag, IMPORT DI dialog).

## What the user must check on the Mac

1. **Task G:** SONG FILE… in Logic and in the Standalone app — can you pick the `.wav`? Which app did the original
   bug happen in? Dragging a `.wav` onto the window in Logic (may not work: host limitation — use the button).
2. **Task E:** `scripts/mac_update.sh` installs the separation model (torch download is large). Unverified offline:
   onnx 1.23.1 / onnxruntime 1.30.0 macOS arm64 wheels — if pip fails, a macOS constraints file is needed.
3. **Task A:** MATCH in Logic; two Sawblade tracks each running a match keep their own results.
4. **Task F:** pick a non-stem folder → refused, song keeps playing.

## Open items / remaining work

- Task B plugin part (IMPORT DI button + drop, import dialog with "don't know" and "same performance", copy to takes,
  `--matched mono` / `--offset-ms`, IMPORTED tag); Task C (docs + host-follow offset test or citation).
- Review 224f3da (acceptedBy, negative tests).
- Merge v0.2 when it lands on the base branch; then verify `RigController::undo` records `PresetAudition::apply` as one
  entry (Task A undo is deferred to it; today apply = one preset load + `updateHostDisplay`).
- Known limits recorded in docs/PLUGIN.md: match settings (selected take) shared per user across instances; jobs from
  older builds have no owner and are not adopted.
- Calibrate placement thresholds (MIN_CONFIDENCE, STRONG_*) on real stems.
