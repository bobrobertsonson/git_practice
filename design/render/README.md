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

Heads and the cab share real-cab detail parts in `common.py`: tolex, metal corner caps, piping, stitched seams, strap handles; the cab adds grille cloth, generic drivers and a generic unbranded mic.

`common.py` holds the shared toolkit (materials, knob/toggle/footswitch/LED/OLED/jewel parts, artwork generator,
lighting/camera rig and the `run(spec)` driver); each piece script only defines its layout, motif and spec.

## Run

Tested with the `bpy` wheel **4.2.0** on Python 3.11 (CPU, 3 threads, OpenImageDenoise).

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    /path/outside/repo/venv/bin/python design/render/amp_saw.py --mode all --out /path/outside/repo/out

`--mode hero|ortho|strip|open|all` (strip only for `pedal_b2.py`, open only for `cab_4x12.py`), `--scale 40` renders a 40% preview,
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
