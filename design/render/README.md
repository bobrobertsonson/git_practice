# Sawblade design B2 - procedural "STOCKHOLM SYNDROME" pedal renders

All renders are built entirely procedurally in Blender (Cycles): geometry, shader nodes, tolex grain, the OLED
pixel font and the crust-stencil face artwork (numpy/Pillow: stencilled motifs, overspray, drips, xerox grain,
torn tape, mis-registered second ink, scratched-through paint). No downloaded models, textures or HDRIs.

| script              | piece                                            | art motif / inks                          | outputs (`--out`)                                  |
|---------------------|--------------------------------------------------|-------------------------------------------|----------------------------------------------------|
| `pedal_b2.py`       | STOCKHOLM SYNDROME pedal, 120x190x55 mm          | saw blade; orange + bone                  | `stockholm_hero_3q.png` 1600x1200, `stockholm_ortho.png` 1200x1800, `stockholm_knob_strip.png` 128x8192 |
| `pedal_tighten.py`  | TIGHTEN boost pedal, 100x150x42 mm               | vise jaws + barbed wire; steel blue + bone| `tighten_hero_3q.png`, `tighten_ortho.png` 1000x1500 |
| `amp_saw.py`        | SAW amp head, 600x260x250 mm                     | jawbone teeth + cogs; rust orange + bone  | `saw_hero_3q.png`, `saw_ortho.png` 1600x700 (front panel) |
| `amp_body.py`       | BODY amp head, same construction                 | anvil + hammer + chains; blood red + bone | `body_hero_3q.png`, `body_ortho.png` 1600x700 |
| `cab_4x12.py`       | SAWBLADE 4x12 cab (760x760x360 mm, generic)      | howling wolf + crescent moon painted on woven grille cloth; bone + toxic green | `cab_hero_3q.png` 1600x1200, `cab_front_ortho.png` 1400x1400 (grille on), `cab_open_ortho.png` 1400x1400 (grille off, 4 drivers + mic) |
| `cab_2x12.py`       | SAWBLADE 2x12 open-back cab (740x520x290 mm, generic) for the "studio split" layout | **placeholder art, pending the user's call:** crossed bones + crust spikes + "CRUST" stencil painted on the grille cloth; bone + bruise purple | `cab2x12_hero_3q.png` 1600x1200, `cab2x12_front_ortho.png` 1400x1000 (grille on), `cab2x12_open_ortho.png` 1400x1000 (grille off, 2 drivers + mic) |
| `ui_sprites.py`     | UI control filmstrips (real sprites, transparent) | none (parts only)                         | see "UI sprites" below                              |

Heads and the cab share real-cab detail parts in `common.py`: tolex, metal corner caps, piping, stitched seams, strap handles; the cab adds grille cloth, generic drivers and a generic unbranded mic.

`common.py` holds the shared toolkit (materials, knob/toggle/footswitch/LED/OLED/jewel parts, artwork generator,
lighting/camera rig and the `run(spec)` driver); each piece script only defines its layout, motif and spec.

## UI sprites (`ui_sprites.py`)

Real UI assets for the JUCE build: top-down orthographic, transparent background (straight RGBA), 2x supersampled and
downsampled with premultiplied alpha, each with a JSON sidecar (`frame_width/height`, `frames`, `layout: vertical`,
`angle_min_deg/angle_max_deg`, `mm_per_px`, `pivot_px`). Parts come from `common.py` (the knurled `Knob`, `build_footswitch`,
`build_led`, `build_toggle`) plus a generic chicken-head pointer knob defined in the script. Pedal parts use the pedal ortho
camera and light rig; the amp knob uses the amp-head front-panel orientation, camera and light rig, so highlights match the
existing `*_ortho.png` renders. Centre of every frame is the part's pivot.

| `--part`                              | output (`--out`)                         | frames                                                         |
|---------------------------------------|------------------------------------------|----------------------------------------------------------------|
| `knob_pedal` (knurled, 40 mm frame)   | `knob_pedal.png` 128x16384 + `.json`     | 128 x 128x128, -135..+135 deg clockwise, pointer up = 0 deg    |
| `knob_amp` (chicken-head, 64 mm frame)| `knob_amp.png` 128x16384 + `.json`       | 128 x 128x128, same angle range                                |
| `footswitch`                          | `footswitch.png` 128x256 + `.json`       | up, down (cap 2.4 mm below the collar rim)                     |
| `led_orange` / `led_bone` / `led_green`| `led_<colour>.png` 64x128 + `.json`     | off, on                                                        |
| `toggle`                              | `toggle.png` 64x192 + `.json`            | lever toward top, centre, toward bottom                        |

`--part all` renders everything; `--scale 40` is a preview, `--samples N` (default 16 at 2x supersample), `--frames N` for
rotary parts, `--ring` bakes the `common.py` LED value ring into the rotary frames (off by default: the UI draws its own
value arc; the ring radius is in the JSON). Frame for a value v in 0..1 is `round(v * (frames - 1))`; frame 0 is -135 deg and the last
frame +135 deg. No cast shadow is baked in (the UI draws its own drop shadow).

## Run

Tested with the `bpy` wheel **4.2.0** on Python 3.11 (CPU, 3 threads, OpenImageDenoise).

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    /path/outside/repo/venv/bin/python design/render/amp_saw.py --mode all --out /path/outside/repo/out

`--mode hero|ortho|strip|open|all` (strip only for `pedal_b2.py`, open only for the cab scripts), `--scale 40` renders a 40% preview,
`--samples N` overrides sampling (default 40), `--dump-art` writes only the artwork channel PNG,
`--font-dir` sets the font cache (default `~/.cache/pedal_b2_fonts`).

**Renders go outside the repo** (`--out`); do not commit PNGs here.

## Third-party font (fetched at run time, not committed)

The stencil lettering uses **Black Ops One** by Wojciech Kalinowski, licensed under the
SIL Open Font License 1.1:
https://raw.githubusercontent.com/google/fonts/main/ofl/blackopsone/BlackOpsOne-Regular.ttf
(licence: https://github.com/google/fonts/tree/main/ofl/blackopsone).
The script downloads it into `--font-dir` on first run; the `.ttf` must not be committed.
If the download fails it falls back to Pillow's default bitmap font.

## Exporting the plugin UI assets

`export_ui_assets.py` is the driver that renders the set the JUCE editor needs (amp heads, cab, both pedals,
knob / footswitch / LED filmstrips) with the piece scripts above and writes the post-processed results into
`plugin/assets/`. **`plugin/assets/` is the one place UI renders are committed**; no CMake step renders anything.
Usage and the exact command line used are in `plugin/assets/README.md`.
