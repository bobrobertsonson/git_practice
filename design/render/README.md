# Sawblade design B2 - procedural pedal renders

`pedal_b2.py` builds the "CHAINSAW" pedal entirely procedurally in Blender (Cycles) -
no downloaded models, textures or HDRIs - and renders three proofs:

| mode    | output          | size            |
|---------|-----------------|-----------------|
| `hero`  | `hero_3q.png`   | 1600x1200       |
| `ortho` | `top_ortho.png` | 1200x1800       |
| `strip` | `knob_strip.png`| 128x8192 (64 frames of 128x128, JUCE filmstrip) |

## Run

Tested with the `bpy` wheel **4.2.0** on Python 3.11 (CPU, 3 threads, OpenImageDenoise).

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    /path/outside/repo/venv/bin/python design/render/pedal_b2.py --mode all --out /path/outside/repo/out

`--scale 40` renders a 40% preview, `--samples N` overrides sampling.

**Renders go outside the repo** (`--out`); do not commit PNGs here.
