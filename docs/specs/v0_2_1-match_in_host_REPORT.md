# v0.2.1 — MATCH inside the DAW + import a prerecorded DI: REPORT

Branch `claude/sawblade-v0_2_1-match-in-host`, based on 75f4f64; v0.2 (`claude/sawblade-plugin-setup-7k0b8q` at
cdb4b9a+) merged in at 621d5e9 (merge commit, no textual conflicts, CI run 129 green). Lead decisions are in the spec
("Lead decisions", 4e8b27d).

**Final CI: see "CI of record" at the end.**

## Tasks and reviewer verdicts

| Task | Commits | Reviewer |
|---|---|---|
| G — LOAD SONG / drop cannot pick a .wav on macOS | e3af8da, d98f41f, 14f3758 (test fix) | REVISE (layout, docs) → **ACCEPT** |
| F — refuse a folder that is not a stem set | 855cb30, f9942e3, 3f6b70f, 14f3758 (test fix) | **ACCEPT**; follow-up (refusal notice, wrapped command field) **ACCEPT** |
| E — mac_update installs the separation model | 314ee46, 1ea78d8, 955a39a (merge 0a2657a); panel 14851ca, 3f6b70f | **ACCEPT** (script, should-fix applied); panel **ACCEPT** |
| D — MATCH screen holds its own song + DI inputs | 5e2b992, 5456a4d, d6bf0ba (CI fix) | REVISE (duplicate CANCEL title, stale assertion, hidden empty-picker note) → **ACCEPT** |
| A — MATCH in plugin mode | 7d05323, 32e0246, 2a06ca8, 10300ed (test fix); undo 0d51755, 563be77 | REVISE (test compile error) → **ACCEPT**; undo **ACCEPT** (should-fixes applied) |
| B — matcher: whole-song DI placement | 71b81e2, 726afb4 (merge 5695ba9), 704b55b, 656cd09, af194fb, 224f3da | ACCEPT → REVISE (uncaught FloatingPointError) → **ACCEPT** ×3; 224f3da **ACCEPT** (follow-up: pin `r1 − r2 < STRONG_MARGIN` in the looped-riff test) |
| B — plugin: IMPORT DI | 4aae42b, 1d1042e, 9f86f7e, 9a833d0 (macOS include), 667f253 (no orphan WAV on throw) | REVISE (worker-thread throw → host crash; clipped rule rejected normalised low notes) → **ACCEPT** |
| C — doc: the record-in-Logic path | 1beb235, 2e2699e, 26bf0e9 | **ACCEPT** |

## Task G root cause (required by the spec)

**Not reproduced; inferred.** Proven by reading JUCE 8.0.15 (91ad83ae):
- The old LOAD SONG chooser (`plugin/src/PlayAlongPanel.cpp:635` at 75f4f64) combined `canSelectFiles |
  canSelectDirectories` with a `*.mp3;*.wav;…` filter. JUCE's mac chooser turns that into `setAllowedFileTypes([mp3, wav,
  …])` (`juce_FileChooser_mac.mm:39-61, 107`) plus a `panel:shouldEnableURL:` delegate (`:279-287`, case-insensitive
  wildcard match on the name). Read in isolation both enable a `.wav`.
- An AU in Logic runs sandboxed (AUHostingService) → JUCE uses a plain `NSOpenPanel` (`:80`), the remote (powerbox)
  panel. **Inferred:** the greying comes from that panel's handling of files+directories + allowedFileTypes + delegate.
- Drops: JUCE walks up from the component under the mouse to the first interested `FileDragAndDropTarget`
  (`juce_ComponentPeer.cpp:468-474`), so the editor already received drops on the panel in code. **Inferred:** Logic /
  AUHostingService may not forward Finder drags into an out-of-process AU view; the plugin cannot fix that.
- **Logic vs Standalone:** expected to differ (Standalone is not sandboxed: JUCE's SafeOpenPanel, a real peer window).
  Which app the user saw it in is not known.

Fix, independent of the inferred cause: one-purpose choosers (SONG FILE… files-only, STEMS FOLDER… directories-only);
on macOS the song chooser uses the filter `*` (no allowedFileTypes; the delegate enables every file) and the pick is
validated afterwards (`handlePicked`); the panel is itself a drop target (structural).

**How it was checked on macOS:** CI macos-arm64 builds and runs the chooser-configuration tests (mac and non-mac
variants), auval and pluginval AU/VST3. A native dialog cannot be driven in CI. **The user's hand test of SONG FILE… on
the Mac (and which app) is still pending** — it is the confirmation the spec asks for.

## Lead decisions taken during the phase

- **Task B, matched pair:** the matcher reads `--offset-ms` only with `--matched`; the plugin never passed it, so a take's
  offset was a no-op. A recorded (play-along) take is a different performance and stays unmatched; IMPORT DI has a
  "same performance as the song (my own recording)" checkbox (default off) → `--matched mono` + `--offset-ms`, or no
  offset ("don't know" → whole-song search). The start-time field is enabled only with that checkbox. A recorded take's
  plan note now says its position is kept but not used (2e2699e).
- **Task B, placement acceptance has two routes** (`offset.py` `placement_accepted`): relative `(r1 − r2)/σ ≥ 4.0`, or
  strong (`r1 ≥ 0.85`, `r1 − r2 ≥ 0.40`, `(r1 − r2)/σ ≥ 3.0`). Reason: for short DIs the best chance rival sits ≈ 4σ, so
  the relative test alone rejects perfect placements (CI run 105: r1 0.949, r2 0.492, σ 0.118 → 3.88). Uncalibrated.
- **Task E:** reused the Linux constraints file on macOS (Linux-only pins are inert there); no macOS pins file.
- **Task F:** distinct stem names count toward "≥ 2 stems"; `piano` recognised; dot-files (AppleDouble `._*.wav` on
  exFAT/USB) ignored by the rule and the loader.
- **Task A undo:** an applied match is one Cmd/Ctrl+Z step through v0.2's `RigController::undo` (entry {pre-audition
  preset, applied preset}, produced by `PresetAudition::apply`, adopted by RigController). Cmd/Ctrl+Z works after
  closing MATCH (the screen says so). The host's own undo history does not list it (not achievable generically).

## Notable findings / bugs fixed

- **Task A isolation hole:** `JobRunner::attachExisting()` adopted the newest job on disk regardless of which instance
  started it — instance B could show and apply instance A's result. Jobs now carry a per-instance owner id; adoption and
  pruning are scoped to it.
- **Task B NaN bug:** a running-sum smoothing filter produced tiny negative energies → `sqrt` NaN → NaN NCC → `argmax`
  0 → every placement 20 ms early (CI run 100). Clamped, finite checks, tests.
- **Task B plugin:** an exception on the import thread would have terminated the host; the clipped rule would have
  refused normalised low-tuned DIs (a 55 Hz sine at 0 dBFS stays ≥ 0.999 for 13 samples). Now: try/catch, 30 min cap,
  flat-top clip rule (run ≥ 4 of equal samples ≥ 0.999), non-finite refused, atomic name+create, no orphan files.
- **Task D:** a take chosen before any song was loaded showed "No DI chosen" (caught by CI).

## Known limits (documented in docs/PLUGIN.md)

- Match settings (selected take) are shared per user across instances; jobs from builds before v0.2.1 have no owner and
  are not adopted.
- `--matched mono` aligns against the reference stem's left channel while the LTAS uses `mid` (harmless for a mono DI).
- FLAC files without a stored length are refused as "no audio"; files over 30 minutes are refused.
- Placement thresholds (MIN_CONFIDENCE, STRONG_*) and import thresholds (−60 dBFS silent, flat-top clip) are reasoned,
  not calibrated on real stems / DIs.

## Screenshots

The editor tests write `import_di_dialog.png` (IMPORT DI dialog) and `import_di_take_band.png` (take band with an
IMPORTED take) to the screenshot folder in CI (test_di_import_ui.cpp). CI does not upload them as artifacts; regenerate
locally with the editor tests. Description: the take band shows REC with IMPORT DI… under it; imported takes carry an
IMPORTED tag instead of "@ x s"; the dialog has LEFT / RIGHT / SUM (stereo only), "same performance as the song",
"DI starts at 0:00.000" with the hint and "don't know" (greyed unless same performance), IMPORT / CANCEL.

## What the user must check on the Mac

1. **Task G:** SONG FILE… in Logic and in the Standalone app — can you pick the `.wav`? Which app did the original bug
   happen in? A Finder drag into the plugin in Logic may still do nothing (host limitation) — use the button.
2. **Task E:** `scripts/mac_update.sh` installs the separation model (large torch download). Unverified offline: onnx
   1.23.1 / onnxruntime 1.30.0 macOS arm64 wheels — if pip fails, a macOS constraints file is needed.
3. **Task A:** MATCH in Logic; two Sawblade tracks each running a match keep their own results; APPLY then close MATCH
   and Cmd/Ctrl+Z returns to the previous sound.
4. **Task B:** IMPORT DI… with a DI bounced from your own recording ("same performance" ticked, start 0:00 or "don't
   know") and with a DI of someone else's song (unticked).

## Merge notes for the integration lead / v0.3

- v0.3 rewrites undo/redo: keep the applied-match entry (pre = pre-audition preset, post = applied preset, keyed to
  `userLoadSerial`; `PresetAudition::takeUndoStep` is the single producer).
- Conflict footprint: `PluginProcessor.*` (instance id, `"instance"` state key), `PluginEditor.cpp` (MATCH button
  lambda), `RigController` (`adoptAppliedMatch`, `syncLoadSerial`, non-const `canUndo`).

## CI history

Run 100: red (3 C++ test bugs, 5 python — NaN). Run 105: red (2 Task D tests, 1 python confidence). Run 108: red (Task A
test compile error, stale assertion). Run 111: 1 C++ test (ownerless seed). Run 112 (10300ed): green. Run 129 (621d5e9,
v0.2 merge): green. Run 145 (1d1042e): clang green incl. all import tests; macOS compile error (`getpid`), fixed in
9a833d0. Many runs were cancelled by newer pushes.

## CI of record

(filled in below)
