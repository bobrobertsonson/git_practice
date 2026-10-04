# Phase 2.5 skin: implementation report

Artifact: <TBD by lead>

## Files
- `design/render/export_ui_assets.py` (exporter, `--list`), `design/render/README.md` (section added)
- `plugin/assets/` (9 PNG, 4 JSON, README with the exact command), 4.76 MB
- `plugin/src/skin/`: `SkinAssets`, `FilmstripKnob`, `FootswitchButton`, `LedIndicator`, `RigView` (incl. `RigPiece`)
- `plugin/src/PluginEditor.{h,cpp}` (rewritten), `plugin/src/SawbladeLookAndFeel.h` (RigReal palette, button drawing)
- `plugin/CMakeLists.txt` (`SawbladeAssets` binary data, `sawblade_editor_tests`, assets-budget test), `plugin/check_assets.cmake`
- `plugin/tests/test_editor.cpp`, `docs/PLUGIN.md`
- Not touched: core/, cli/, bindings/, match/, PluginProcessor.*, Engine*, PresetMapping*.

## Design notes
- Content is a 1280x800 component scaled by `AffineTransform::scale(width/1280)`; editor resizable 640x400..2560x1600 at fixed 1.6 aspect.
- Sprite frames have a ~35% transparent margin, so knobs draw the frame larger than their component (pedal 1.38x, amp 1.12x) so the body fills the spec'd 88/44/36 px and the value arc runs just outside it.
- Knob drag is implemented incrementally in `mouseDrag` (250 px full range, shift x0.1); `Slider::mouseDown` still starts the host gesture; double-click default comes from the attachment.
- Footswitch pressed look is code-made (frame 0, 2 px lower, RGB x0.75 inside the sprite alpha). LED component is 3x the sprite so the glow can spill out.
- Cables are a child layer between amps/cab and pedals so they sit above the amps and under the pedals. Pedal footswitch/LED sprites are placed from the pedal scripts' mm coordinates (left switch of STOCKHOLM, the single one on TIGHTEN); STOCKHOLM's second switch/LED stay as baked art.
- Render: `--scale 48 --samples 24` (ortho views), sprites at native size; total export 487 s under `nice -n 19`.

## Test summary
Full `ctest` (Release, `-DSAWBLADE_BUILD_PLUGIN=ON`, `-Werror`, zero warnings): **100% passed, 0 failed out of 197** (includes 8 `editor:` tests: 7 Catch2 cases + assets budget, and `plugin: pluginval VST3`).

## pluginval (v1.0.4, `--strictness-level 10 --validate-in-process`, xvfb-run, VST3)
Ends with `Completed tests in pluginval / Fuzz parameters` then `SUCCESS`.

## Standalone smoke test
`xvfb-run -a Standalone/Sawblade`: alive at 10 s, SIGTERM exit status 143 (terminated by the signal, no crash or assertion abort). Only output: ALSA "open /dev/snd/seq failed" (no sound hardware in the sandbox).

## Screenshots
- /home/user/git_practice/build/screenshots/sawblade_skin_1x.png (1280x800)
- /home/user/git_practice/build/screenshots/sawblade_skin_2x.png (2560x1600)
The test rig loads `presets/matched/barbaric_v4.json` only if every capture it references exists (paths.*.blocks[].model, cab.ir/irA/irB); the captures are not committed, so the screenshots are taken on the Init preset: no error line, `LAT 0`, blend 50/50.

## Review round 1 changes
- Rig only loads a preset whose captures all exist; stale comment fixed.
- `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` on FilmstripKnob, FootswitchButton, LedIndicator, RigPiece, RigView, Cables, Content, SawbladeEditor.
- `kMaster` indentation fixed.
- Footswitch test produces the pressed state with a synthesised mouseDown (then mouseUp), not `setState`.
- Full ctest after the changes: 197/197 passed.

## Decisions / questions for lead
1. `createEditorIfNeeded()` is deprecated in this JUCE (`-Werror`); tests use `createEditorAndMakeActive()`.
2. `sawblade_editor_tests` compiles the JUCE modules a third time (plugin, plugin tests, editor tests): longer clean builds. An object library shared by the two test exes would fix it if wanted.
3. Assets-budget check is a ctest that runs `export_ui_assets.py --list` through a CMake script (needs python3; skipped if absent).
4. Cab caption omits "DOUBLE-CLICK FOR MIC" (no mic page in this prototype). Minus sign in `thr` read-out is ASCII `-`.
5. Pedal renders keep their grey render backdrop inside a rounded clip (as the mockup); a transparent-background pedal render would look cleaner.
6. Standalone "exit cleanly" is judged as terminated by SIGTERM without crash; the app has no SIGTERM handler.
7. Proposals (out of scope): CPU meter, real bypass, post-EQ band labels, pedal renders with alpha.
