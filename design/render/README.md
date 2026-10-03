# Sawblade design B2 - procedural "STOCKHOLM SYNDROME" pedal renders

`pedal_b2.py` builds the pedal entirely procedurally in Blender (Cycles): geometry, shader
nodes, the OLED pixel font and the top-face artwork (numpy/Pillow, "Swedish crust" stencil:
oversized stencilled saw blade, overspray, drips, xerox grain, torn tape, mis-registered
second ink, scratched-through paint) are all generated at build time.
No downloaded models, textures or HDRIs.

| mode    | output           | size            |
|---------|------------------|-----------------|
| `hero`  | `hero_3q.png`    | 1600x1200       |
| `ortho` | `top_ortho.png`  | 1200x1800       |
| `strip` | `knob_strip.png` | 128x8192 (64 frames of 128x128, JUCE filmstrip) |

## Run

Tested with the `bpy` wheel **4.2.0** on Python 3.11 (CPU, 3 threads, OpenImageDenoise).

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    /path/outside/repo/venv/bin/python design/render/pedal_b2.py --mode all --out /path/outside/repo/out

`--scale 40` renders a 40% preview, `--samples N` overrides sampling, `--dump-art` writes
only the artwork channel PNG, `--font-dir` sets the font cache (default `~/.cache/pedal_b2_fonts`).

**Renders go outside the repo** (`--out`); do not commit PNGs here.

## Third-party font (fetched at run time, not committed)

The stencil lettering uses **Black Ops One** by Wojciech Kalinowski, licensed under the
SIL Open Font License 1.1:
https://raw.githubusercontent.com/google/fonts/main/ofl/blackopsone/BlackOpsOne-Regular.ttf
(licence: https://github.com/google/fonts/tree/main/ofl/blackopsone).
The script downloads it into `--font-dir` on first run; the `.ttf` must not be committed.
If the download fails it falls back to Pillow's default bitmap font.
