# Phase 2.5 spec — skinned prototype editor (dsp-engineer)

Replace the phase-2 generic editor with a **skinned prototype** of the main rig screen
(`design/mockups/RigReal.dc.html`), built from our own procedural renders (`design/render/`).
This is a prototype for the user's review: the user designs the final UI (CLAUDE.md, phase 2).
**Editor only. No DSP, processor, preset or parameter changes.**

## 1. Assets

### 1.1 `design/render/export_ui_assets.py`
A driver (plain Python 3.11, run with the bpy 4.2.0 venv interpreter) that renders the UI set by
invoking the existing piece scripts (`amp_saw.py`, `amp_body.py`, `cab_4x12.py`, `pedal_b2.py`,
`pedal_tighten.py`, `ui_sprites.py`) as subprocesses with `--mode ortho` / `--part …`, into a
temporary directory outside the repo, then post-processes with Pillow into `plugin/assets/`:

| asset (`plugin/assets/`)   | source                                   | stored size (2x of layout)     |
|----------------------------|------------------------------------------|--------------------------------|
| `amp_saw.png`              | `saw_ortho.png` (front panel)             | 660 px wide, aspect kept       |
| `amp_body.png`             | `body_ortho.png`                          | 660 px wide                    |
| `cab_4x12.png`             | `cab_front_ortho.png` (grille on)         | 660 px wide                    |
| `pedal_saw.png`            | `stockholm_ortho.png` (top view)          | 360 px wide                    |
| `pedal_body.png`           | `tighten_ortho.png`                       | 280 px wide                    |
| `knob_amp.png` + `.json`   | `ui_sprites.py --part knob_amp`           | as rendered (128 x 128 frames) |
| `knob_pedal.png` + `.json` | `ui_sprites.py --part knob_pedal`         | as rendered                    |
| `footswitch.png` + `.json` | `--part footswitch`                       | as rendered (2 frames)         |
| `led_orange.png` + `.json` | `--part led_orange`                       | as rendered (2 frames)         |

- Options: `--out` (default `plugin/assets`), `--python` (interpreter for the piece scripts,
  default `sys.executable`), `--scale` and `--samples` passed through to the renders,
  `--work-dir` (temp, outside the repo). Downscaling uses Lanczos with premultiplied alpha.
  Sidecar JSONs are copied unchanged (`frames`, `frame_width/height`, `angle_min/max_deg`, `pivot_px`).
- No CMake step renders anything. CMake only embeds what is in `plugin/assets/`.
- Run it **once** in this session under `nice -n 19` (bpy 4.2.0 wheel in a venv outside the repo,
  `--scale` low enough that each ortho render is at least the stored size above; knob strips at
  the scripts' native 128 px frames, 128 frames). Commit the outputs.
- `plugin/assets/README.md`: what each file is, that these are our own procedural renders (not
  third-party; the Black Ops One OFL font is rasterised into the panel art at render time and the
  `.ttf` is not committed), and the **exact command line** used, with bpy version and date.
- Total size of `plugin/assets/` **< 25 MB** (acceptance test checks it).
- `design/render/README.md`: add a short section pointing to the exporter and stating that
  `plugin/assets/` is the one place UI renders are committed.

### 1.2 Embedding
`juce_add_binary_data(SawbladeAssets …)` over every PNG and JSON in `plugin/assets/` (explicit
list, not a glob). Linked into the plugin shared code and into the new editor test target.

## 2. Editor layout (`SawbladeEditor`)
A fixed **1280 x 800 logical** design, laid out in one content component; the editor scales it with
an `AffineTransform` (`setResizable(true, true)`, fixed aspect 1.6, limits 640x400 … 2560x1600).
Colours from RigReal (`#0b0a09` background, `#121110` panels, `#2a2622` rules, `#ff6a1a` saw orange,
`#4f8fd0` body blue, `#e8e1d2` text, `#a39a8a` dim). JUCE default fonts (no font embedding).

- **Top bar** (58 px): "SAWBLADE" wordmark; preset name button (opens the existing preset file
  chooser — keeps phase-2 functionality) with ‹ › buttons (disabled placeholders); `A / B`
  (placeholder, disabled); chip `LAT <n> smp · CPU —` (latency from `status()`; CPU is a
  placeholder: measuring it would be a processor change); `LIVE` / `STUDIO` chip from
  `status().liveCompatible`; `MATCH` and `EXPORT NAM` buttons (placeholders, disabled, tooltip
  says "not available in this prototype"). Load errors / loading state shown in the top bar or
  status line as in phase 2.
- **Rig area** (left, 940 x 742): RigReal positions — SAW head at (34,46) w330, BODY head at
  (34,210) w330, 4x12 cab at (400,40) w330 with its caption, cable curves (orange / blue) drawn in
  code, pedalboard (drawn in code) with the SAW pedal (w180) and BODY pedal (w140) images, each
  with a footswitch sprite and an LED, two dashed `+ PEDAL` placeholders (disabled). Images get a
  code-drawn drop shadow. Clicking an amp, the cab or a pedal **selects** it (orange outline on
  the selected one; default selection = SAW pedal).
- **Inspector** (right, 340 px): `SELECTED · <path> <type>` and the block's name for the current
  selection (static names in this prototype: STOCKHOLM SYNDROME, TIGHTEN, SAW HEAD, BODY HEAD,
  4x12 CAB); a disabled `BROWSE CAPTURES`; the **BLEND** knob (88 px, `knob_amp` strip, bound to
  `blend`) with read-out `SAW xx / BODY yy`; **MASTER** row of 44 px `knob_pedal` knobs:
  INPUT (`inputGain`), GATE (`gateThreshold`), SAW (`levelA`), BODY (`levelB`), OUTPUT
  (`outputGain`); **POST EQ** row of six 36 px knobs (`postEq1..6`); `LEARN GATE` button
  (placeholder, disabled) with the current gate threshold read-out (`thr −xx.x dB`); bottom box
  `MATCH vs ORIGINAL` with "—" (placeholder). Every one of the 12 APVTS parameters is bound to
  exactly one knob.
- All look-and-feel stays in the replaceable layer (`SawbladeLookAndFeel` + the new skin classes
  under `plugin/src/skin/`). The old generic slider rows are removed.

## 3. Controls (`plugin/src/skin/`)
- **`SkinAssets`**: loads each PNG (`juce::ImageCache`/`ImageFileFormat` from BinaryData) and
  parses its JSON sidecar once (message thread). A `Filmstrip` value type: image, frame count,
  frame size, vertical layout. Missing/garbled sidecar → `jassert` + a code-drawn fallback, never
  a crash.
- **`FilmstripKnob : juce::Slider`** (RotaryVerticalDrag, no text box), bound with
  `SliderAttachment`. `paint` draws frame `round(v * (frames-1))` where `v` is the normalised
  (0..1) parameter value, scaled into the bounds with high-quality resampling, plus a code-drawn
  drop shadow and value arc (-135..+135°). Drag-to-turn is **vertical** (full range over 250 px),
  **shift = fine** (x0.1 sensitivity), **double-click resets** to the parameter default
  (`setDoubleClickReturnValue` with the APVTS default), mouse wheel works. Exposes
  `static int frameForProportion(double v, int frames)` for testing.
- **`FootswitchButton : juce::Button`** (toggle): draws the `footswitch` sprite frame 0 (up);
  while pressed (mouse down) draws frame 0 **offset 2 px down and darkened** (~25% black overlay
  inside the sprite alpha). Toggling flips the paired LED. Prototype behaviour: visual bypass
  toggle only (no parameter exists; no DSP changes) — tooltip says so.
- **`LedIndicator : juce::Component`**: the `led_orange` sprite (off frame) with the **glow drawn in
  code** when on: radial gradient (`#ff6a1a`, alpha ~0.6 → 0) over ~3x the LED diameter plus the
  `on` frame on top.
- **Accessibility:** every interactive control and every image-button gets `setTitle`, a
  `setTooltip` (a `juce::TooltipWindow` is owned by the editor), and sliders keep their value
  text for screen readers (`Slider::getTextFromValue` → parameter text with unit). Placeholders are
  disabled but still titled.

## 4. No DSP changes
`git diff` must not touch `core/`, `cli/`, `bindings/`, `match/`, `PluginProcessor.*`, `Engine*`,
`PresetMapping*`. The editor only reads `status()` and the APVTS. `getStateInformation` output is
unchanged (a test in the existing suite already covers the round-trip).

## 5. Screenshots / editor tests
New target **`sawblade_editor_tests`** (Catch2, registered with ctest under prefix `editor: `;
run under `xvfb-run -a` if present, like pluginval). It creates `ScopedJuceInitialiser_GUI`, a
`SawbladeProcessor`, `prepareToPlay(48000, 512)`, and the editor (`createEditorIfNeeded`) with the
`presets/` barbaric preset loaded if available (Init otherwise; never fail on missing captures).

Acceptance tests:
1. **Snapshot 1x / 2x**: `createComponentSnapshot(editor bounds, true, 1.0f / 2.0f)` → PNGs written to
   `${CMAKE_BINARY_DIR}/screenshots/sawblade_skin_1x.png` (1280x800) and `_2x.png` (2560x1600).
   Assert sizes, and that the image is not blank (pixel std-dev over luminance > a small threshold,
   and the amp image region is not the background colour).
2. **Resizing**: `setSize(640,400)` and `(2560,1600)` keep the 1.6 aspect and the content
   transform scale is 0.5 / 2.0; a 1000x800 request is constrained to the aspect.
3. **Bindings**: for each of the 12 parameters there is exactly one `FilmstripKnob` whose
   attachment drives it: setting the knob value changes the APVTS parameter, and setting the
   parameter (`setValueNotifyingHost`) moves the knob.
4. **Filmstrip mapping**: `frameForProportion(0,128)=0`, `(1,128)=127`, `(0.5,128)=64`, clamped
   outside 0..1; sidecar JSON of every embedded strip parses with the expected frame count
   (knobs 128, footswitch 2, LED 2) and the PNG height == frames x frame_height.
5. **Interaction** (synthesised `MouseEvent`s on a knob): vertical drag up 100 px raises the
   proportion by 0.4 ± 0.02; the same drag with shift raises it by 0.04 ± 0.005; horizontal drag
   leaves it unchanged; double-click returns to the parameter default.
6. **Footswitch**: pressed-state paint differs from up-state paint (snapshot pixel diff in the
   switch bounds), clicking toggles the paired LED; LED on vs off snapshot differs outside the LED
   sprite bounds (glow).
7. **Accessibility**: walk the editor's component tree; every `Slider`, `Button` and selectable rig
   image has a non-empty title and tooltip.
8. **Assets budget**: a ctest (CMake script or test case) asserts total size of `plugin/assets/` <
   25 MB and that every file referenced by the exporter's table exists.

The existing `sawblade_plugin_tests` must not need a display (keep editor tests in the new target).

### Artifact (lead)
The lead publishes the 1x and 2x screenshots as an Artifact page titled **"Sawblade Skin
Prototype"** and records the URL in `docs/specs/phase2_5_skin_REPORT.md` (the implementer writes
the rest of the report: files, decisions, test summary, pluginval output, asset sizes, render time).

## 6. Validation
- Full build with `-DSAWBLADE_BUILD_PLUGIN=ON`; the whole ctest suite passes (core + plugin + editor).
- **pluginval v1.0.4, `--strictness-level 10 --validate-in-process`, on the VST3**, under
  `xvfb-run -a`: SUCCESS (build pluginval outside the repo; paste the summary into the report).
- **Standalone**: pluginval does not load standalone apps, so the Standalone is smoke-tested:
  launch `Standalone/Sawblade` under `xvfb-run -a` for 10 s, it must stay alive and exit cleanly on
  SIGTERM (no crash / assertion abort). Paste the result.
- `-Werror` on our sources, including the skin and the test target.

## Out of scope
Real CPU metering, A/B, MATCH/EXPORT/BROWSE flows, mic page, real bypass/pedal add, font
embedding, block-specific knobs on the panel images. Propose them in the report.
