# Phase 9a: cab mic placement page + `irMix` cab mode

Reference: `design/mockups/MicPage.dc.html` (layout reference only), the "open" cab renders
from `design/render/cab_4x12.py` and `cab_2x12.py` (`cab_open_ortho.png`,
`cab2x12_open_ortho.png`).

An **IR pack** is one TONE3000 IR tone with many models (one IR per model), or, offline, a
local folder of WAVs. Each model name describes a mic shot. The page shows where the pack has
shots and swaps the cab IR to the one the user picks.

Rules for this phase: new code goes in new files (`plugin/src/mic/`, plus
`plugin/src/presets/T3kTool.*`, shared with 9b). Edits to `PluginEditor.*`,
`PluginProcessor.*` and `skin/RigView.*` stay minimal (a few lines each): other sessions are
editing those files in parallel.

## 1. Core: cab mode `irMix` (dsp-engineer)
1. **Schema.** `"cab": { "mode": "irMix", "irA": Capture, "irB": Capture, "mix": 0.5,
   "enabled": true, "normalize": true }`. `mix` is in [0, 1], default 0.5; out of range is a
   preset error. `irA` and `irB` are both required. The strict-key rules stay: `ir` is
   rejected in `irMix` mode, `mix` is rejected in the other modes.
2. **Combined IR.** Load each IR exactly as `shared` loads one (left channel, resample,
   truncate to 2.0 s, L2-normalise when `normalize` is true). Then
   `h = (1 − mix)·hA + mix·hB`, with the shorter IR zero-padded. Do **not** re-normalise the
   sum. Put this in a new `core/include/sawblade/ir_mix.h` / `core/src/ir_mix.cpp`
   (`mixIrs(a, b, mix)`), so the plugin and tests share it.
3. **Processing.** One convolver on `h`, at the same place in the chain and with the same
   latency as `shared`. Nothing new runs in `process()`.
4. **Derived properties.** `liveCompatible = !cab.enabled || mode is shared or irMix`. The render report
   names the mode and both captures. The "studio" UI chip shows LIVE for `irMix`.
5. **Writer.** The preset writer round-trips `irMix` (and every existing mode unchanged).
6. **Docs.** Update `docs/PRESET_SCHEMA.md`: the Cab section, derived properties, and a
   sentence that `irMix` is still one combined IR, so the no-cab export is exact.

## 2. Python (match-engineer)
1. `export/plan.py`: `irMix` counts as live-compatible (no-cab export allowed);
   `captures()` lists `cab:irA` and `cab:irB`. The cab fold already renders through the core,
   so it needs no change; prove it with a test.
2. New `sawblade-t3k pack <toneId> [--cache-dir D] -o manifest.json`: list every model of an
   IR tone, download each to the cache, and write a manifest:
   `{ "toneId", "title", "creator", "license", "url", "models": [ { "modelId", "name",
   "file" (absolute), "sha256" } ] }`. With `--progress-json`, print one JSON line per model to
   stdout (`{"done": i, "total": n, "name": ...}`), flushed.
3. **Exit codes for all `sawblade-t3k` commands:** 4 = not logged in / re-auth required
   (`ReauthRequired`, or no stored token), 1 = other errors. The plugin relies on 4.
4. pytest: plan for `irMix`, the pack manifest built with the client mocked (as existing tests
   do), and exit code 4. No network in tests.

## 3. Plugin: model-name parser (dsp-engineer), `plugin/src/mic/IrNameParser.{h,cpp}`
JUCE-free. `MicShot parseIrName(std::string_view)` never throws and returns:
- `speaker`: normalised type string (`"V30"`, `"G12T75"`, `"G12M"`, `"G12H"`, `"Greenback"`,
  `"Creamback"`, `"EVM12L"`, …) or `"unknown"`;
- `speakerSlot`: 1..4, or 0 if unknown. Accept `UL/UR/LL/LR` (1/2/3/4), `TL/TR/BL/BR`, a
  number right after the speaker type ("V30 1", "G12 3"), "spk2", and "speaker 2";
- `mic`: normalised model (`"SM57"`, `"MD421"`, `"e906"`, `"R121"`, `"U87"`, `"M160"`,
  `"SM7B"`, `"i5"`, `"C414"`, …) or `"unknown"`; `micType` = dynamic / ribbon / condenser /
  unknown, from the model;
- `distanceIn`: in inches. Accept `0.50in`, `1"`, `2 in`, `25mm`, `2.5cm`, `0in`. NaN if
  unknown;
- `position`: Cap / CapEdge / Cone / Edge / OffAxis / Unknown, from words like "cap",
  "cap edge", "CE", "cone", "edge", "off axis", "45°";
- `positionIndex`: a bare numeric position, e.g. `"SM57 3"`, or the trailing "- V30 3" in
  `"Mesa OS - V30 1 SM57 - V30 3"`. 0 if none;
- `cabSize`: "4x12" / "2x12" / "1x12" / "" (unknown). The string "4FB" is not a cab size;
- `raw`: the original name.

Parsing is tolerant: case-insensitive, any separator (space, `_`, `-`, `·`, `,`), unknown
tokens are ignored, and a mic model is never taken as a speaker or the reverse.

**Test table:** at least 20 real-world formats. These must be in it:
`"V30 UL 4FB 4x12 SM57 0.50in"`, `"Mesa OS - V30 1 SM57 - V30 3"`,
`"Marshall G12 1 SM57 3"`, `"SM57_CapEdge_1in"`, `"G12T75 LR MD421 cone 2\""`,
`"R121 2in off axis"`, `"Greenback 2x12 e906 25mm cap"`, `"impulse 7"` (all unknown), `""`.
List each case's expected fields in the test.

## 4. Plugin: IR pack and snapping (dsp-engineer), `plugin/src/mic/IrPack.{h,cpp}`
1. **Sources:**
   - (a) a manifest from `sawblade-t3k pack`;
   - (b) a local folder of `.wav` files, with names = file stems and no attribution.
   Loading never throws; errors come back as a message.
2. **Dot layout.** Every model maps to a point in normalised cab coordinates using the cab
   layout sidecar (§5):
   - The speaker slot picks the driver. An unknown slot means driver 1.
   - The radial offset from the dust-cap centre, as a fraction of the cone radius, is
     Cap 0.0, CapEdge 0.3, Cone 0.6, Edge 0.9, OffAxis 0.6 (drawn with an angle mark).
   - `positionIndex` i of the pack's max index N maps to `0.9·(i−1)/max(N−1, 1)`.
   - Unknown means 0.3.
   - All offsets point towards the cab centre along the horizontal.
   - Models at the same point (they differ only in mic or distance) are **one dot**.
3. **Snap.** `snapToNearest(point, current)` returns the dot nearest in Euclidean distance;
   ties go to the lower model index. Inside that dot, it keeps the current mic and the nearest
   distance to the current one when the dot has them, else the first model of the dot.
4. **Choosing a model** builds a new preset from `processor.currentPreset()`, with `cab.ir`
   (shared) or the dragged mic of `irMix` replaced by that model's Capture:
   - For a manifest pack: `file` is the absolute cache path, plus `sha256`, and `source` is
     `{provider tone3000, id = pack tone id, modelId, title = model name, creator, license,
     url}`.
   - For a folder pack: `file` only.

   The preset goes through `processor.loadPreset()`, i.e. the normal off-thread loader and the
   existing 30 ms equal-power swap. No new audio-thread code.
5. **Current pack:**
   - When the cab capture has a TONE3000 `source.id`, the page offers "LOAD PACK", which runs
     `sawblade-t3k pack` as a child process (§6) with a progress bar. Manifests are cached at
     `<appdata>/sawblade/packs/<toneId>.json` and reused.
   - "LOAD IR FOLDER…" picks a local folder.
   - With no pack loaded, the page shows the single current IR as one dot.
   - Not part of the preset: on reopen, the page finds the cached manifest by the cab's tone id.

## 5. Plugin: the page (dsp-engineer), `plugin/src/mic/MicPage.{h,cpp}` + assets
1. **Assets.** Render the open views with the README's `bpy` setup (bpy 4.2.0, Python 3.11)
   and export them like `export_ui_assets.py` does: `assets/cab_4x12_open.png` and
   `assets/cab_2x12_open.png`, at UI size. Each gets a sidecar `.json` with every driver's
   centre and cone radius in px, **computed from the script's geometry** (not eyeballed).
   Add them to `juce_add_binary_data` and `check_assets.cmake`.
   If `bpy` truly cannot be installed or rendered here, stop and report it to the lead; do
   not draw a substitute.
2. **Opening.**
   - Double-clicking the cab in the rig opens the page as an overlay over the rig + inspector.
     This needs a `RigView` hook: one `std::function` plus a `mouseDoubleClick` on the cab
     piece.
   - "‹ RIG" closes it.
   - The image is the 2x12 when the majority of the pack's parsed `cabSize` is 2x12, else the
     4x12.
3. **Layout** per the mockup:
   - Left: the open cab, with dots numbered by dot index. The selected dot is green
     (`#7fd13b`); a mic sprite or drawn mic sits at the selected point.
   - Right: an IR PACK card (title, `@creator · licence · N IRs · VIA TONE3000`, or the folder
     name), the fields SPEAKER / MIC / DISTANCE / POSITION, the SELECTED IR name, and the
     magnitude response of the selected (or mixed) IR, 20 Hz–20 kHz, log-frequency.
     Computing it on the message thread is fine (≤ 2 s IR).
   - Buttons: A/B (toggle between the last two chosen models), NEXT POSITION (the next dot),
     BLEND 2 MICS.
   - A footer note: "Positions come from the IR pack's own mic shots…".
   - When several models share the selected dot, MIC and DISTANCE are combo boxes listing that
     dot's options.
4. **Drag.** The mic follows the mouse; on mouse-up it snaps (§4.3) and loads. While
   dragging, the nearest dot is highlighted, and nothing loads until mouse-up.
5. **BLEND 2 MICS.**
   - Switches the cab to `irMix`: `irA` = the current IR, `irB` = the same IR at first (so the
     sound does not change), `mix` 0.5.
   - A second mic (bone colour, "2") becomes draggable, and a MIX fader appears (A ↔ B, 0–100 %).
   - The fader submits a rebuild at most every 150 ms while dragging and once on release
     (latest wins in the loader). It is not a host parameter.
   - Turning BLEND off goes back to `shared` with `irA`.
   - `irMix` presets loaded from disk open with BLEND on.
   - In `perPath` mode the page is read-only, with a note ("studio blend: per-path cabs; mic
     placement edits the shared cab only").

## 6. Plugin: `sawblade-t3k` runner (dsp-engineer), `plugin/src/presets/T3kTool.{h,cpp}`
Shared with 9b.
1. **Settings.**
   - `<appdata>/sawblade/settings.json` (Linux `~/.local/share/sawblade/`, macOS
     `~/Library/Application Support/Sawblade/`), key `t3kExecutable`.
   - The default is `<repo>/match/.venv/bin/sawblade-t3k`, through a compile definition with
     the source dir.
   - Read-modify-write keeps unknown keys, because other features share the file.
2. **Running.**
   - `juce::ChildProcess` on a background thread; the message thread never blocks.
   - Progress comes from `--progress-json` stdout lines.
   - Cancel kills the child.
3. **Results.**
   - Exit 4 → "Not logged in to TONE3000. Run `sawblade-t3k login` in a terminal, then try
     again."
   - Missing executable → a clear message plus a "LOCATE…" button that saves the setting.
   - Exit 1 → the last stderr line.
4. **Tests.** Use a fake executable (a shell script) for progress, exit 4, the missing-exe
   message and cancel.

## Acceptance
- **Core tests:**
  - `irMix` preset round-trip (parse → write → parse equal), the strict-key errors and a `mix`
    range error;
  - **render equivalence:** a chain with `irMix` (two different synthetic IRs of different
    lengths, mix 0.3) renders, within 1e-6 absolute, the same as
    `0.7·render(shared irA) + 0.3·render(shared irB)` for the same preset and input;
  - `mix = 0` is bit-identical to `shared` with `irA`;
  - block-size invariance;
  - zero allocations in `process()`.
- **Plugin tests:**
  - the parser table;
  - the dot layout (shared points merge into one dot);
  - snap to nearest (including the tie rule and keeping the mic/distance);
  - choosing a model from a synthetic folder pack loads a preset whose cab file is that WAV
    (through the processor and `waitForLoader`);
  - BLEND on/off produces `irMix`/`shared` presets;
  - the T3kTool fake-exe tests;
  - an editor test: double-click on the cab opens the page, the dot count matches, and the
    screenshot test includes the page (single mic and BLEND on).
- **Python:** pytest passes.
- **Gates:** full `ctest` passes, a clang `-Werror` build passes, and pluginval level 10
  passes.
- **Never commit** capture files or IR WAVs from TONE3000. Tests synthesise IRs into temp
  dirs.
