# plugin/assets: UI renders for the skinned editor

Our own procedural Blender renders (`design/render/`), post-processed by
`design/render/export_ui_assets.py`. Not third-party art. The stencil lettering in the panel art uses
Black Ops One (SIL OFL 1.1), rasterised into the PNGs at render time; the `.ttf` is not committed (see
`design/render/README.md`). This is the one place UI renders are committed. CMake embeds exactly these
files (`juce_add_binary_data(SawbladeAssets ...)` in `plugin/CMakeLists.txt`, explicit list); nothing
renders at build time. Total size is well under the 25 MB budget (checked by the ctest
`editor: assets budget`).

| file | what | size |
|---|---|---|
| `amp_saw.png`, `amp_body.png` | SAW / BODY amp head front panels | 660 px wide |
| `cab_4x12.png` | 4x12 cab, grille on | 660 x 660 |
| `pedal_saw.png` | STOCKHOLM SYNDROME pedal, top view | 360 px wide |
| `pedal_body.png` | TIGHTEN pedal, top view | 280 px wide |
| `knob_amp.png` + `.json` | chicken-head knob, 128 frames of 128 x 128, -135..+135 deg | strip |
| `knob_pedal.png` + `.json` | knurled knob, same layout | strip |
| `footswitch.png` + `.json` | footswitch, 2 frames (up, down) | strip |
| `led_orange.png` + `.json` | LED, 2 frames (off, on), 64 x 64 | strip |
| `preview_riff.wav` | the capture browser's PREVIEW riff: 6.0 s, 48 kHz, mono, 24-bit, peak -12 dBFS; synthesised (Karplus-Strong), no third-party audio; `design/render/make_preview_riff.py` regenerates it byte for byte | 864 KB |

Stored sizes are 2x the layout size (the editor lays out at 1280 x 800 and draws panels at half the stored
width). Sidecar JSONs are copied unchanged from `ui_sprites.py`.

## Regenerating

Run once per art change, then commit the outputs. bpy 4.2.0 on Python 3.11 (the wheel is large; keep the
venv and the work directory outside the repo):

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    nice -n 19 /path/outside/repo/venv/bin/python design/render/export_ui_assets.py \
        --scale 48 --samples 24 --work-dir /path/outside/repo/work

Exact command used for the committed files (2026-10-04, bpy 4.2.0, Cycles CPU, about 490 s on 4 CPUs under
`nice -n 19`): the one above with `--sprite-samples` at its default of 16 and the sprites at their native
128 px frames and 128 frames. `--scale 48` renders the ortho views at 768 / 672 / 576 / 480 px wide, just
above the stored sizes, which are then downsampled with Lanczos on premultiplied alpha.
`export_ui_assets.py --list` prints the file names the editor embeds.
