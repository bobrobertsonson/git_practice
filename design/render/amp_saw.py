"""SAW amp head (path A) - 600x260x250 mm, black tolex body, crust-stencil front plate:
jawbone teeth + gear cogs in rust orange, bone lettering.
    python amp_saw.py --mode all --out /path/outside/repo"""
import math
import numpy as np
from common import *

W, L, S = 600.0, 260.0, 5

def gear_px(ctx, d, x, y, r, teeth, spokes=5):
    cx, cy = ctx.P(x, y)
    gear(ctx, d, cx, cy, r * ctx.S, teeth, hole=0.42, spokes=spokes)
    d.ellipse((cx - 0.5 * r * ctx.S, cy - 0.5 * r * ctx.S, cx + 0.5 * r * ctx.S, cy + 0.5 * r * ctx.S), fill=0)
    d.ellipse((cx - 0.18 * r * ctx.S, cy - 0.18 * r * ctx.S, cx + 0.18 * r * ctx.S, cy + 0.18 * r * ctx.S), fill=255)
    d.ellipse((cx - 0.66 * r * ctx.S, cy - 0.66 * r * ctx.S, cx + 0.66 * r * ctx.S, cy + 0.66 * r * ctx.S), outline=255, width=int(2.2 * ctx.S))

def molar(ctx, d, x, y_gum, w, h, up):
    """tooth growing from the gum line at y_gum (down if up=False)."""
    s = -1 if up else 1
    pts = [(-w / 2, 0), (-w / 2, 0.55 * h), (-w / 4, h), (0, 0.78 * h), (w / 4, h), (w / 2, 0.55 * h), (w / 2, 0)]
    P = [ctx.P(x + px, y_gum - s * py) for px, py in pts]
    d.polygon(P, fill=255)
    d.line((*ctx.P(x, y_gum - s * 0.15 * h), *ctx.P(x, y_gum - s * 0.62 * h)), fill=0, width=int(1.3 * ctx.S))

def motif(ctx):
    rng = ctx.rng
    A, d = ctx.new()
    for sx in (-1, 1):                                     # big meshing cogs left/right
        gear_px(ctx, d, sx * 238, -62, 56, 18)
        gear_px(ctx, d, sx * 182, 2, 30, 12, spokes=4)
        gear_px(ctx, d, sx * 150, -128, 30, 12, spokes=4)
    # jawbone: gum bars + teeth, a few broken / missing
    for (y_gum, up, n) in ((32, False, 17), (-123, True, 17)):
        gx0, gy0 = ctx.P(-282, y_gum - 3); gx1, gy1 = ctx.P(282, y_gum + 3)
        d.rectangle((gx0, min(gy0, gy1), gx1, max(gy0, gy1)), fill=255)
        xs = np.linspace(-270, 270, n)
        for i, x in enumerate(xs):
            if abs(x) > 120 and abs(x) < 180: pass
            if rng.random() < 0.12: continue
            w = rng.uniform(22, 29); h = rng.uniform(26, 42) * (0.7 if i in (0, n - 1) else 1)
            molar(ctx, d, x + rng.uniform(-2, 2), y_gum, w, h, up)
    # bite lines between the jaws
    for k in range(7):
        x = -270 + k * 90 + rng.uniform(-6, 6)
        d.line((*ctx.P(x, 10), *ctx.P(x + rng.uniform(-6, 6), -105)), fill=0, width=int(0.9 * ctx.S))
    # bone extra: scratchy tally block under the display + thin cog outlines
    E, de = ctx.new()
    for g in range(3):
        x0, y0 = ctx.P(-285 + g * 9, 12)
    for xx, yy in ((-268, 4), (-250, 4)):
        x0, y0 = ctx.P(xx, yy)
        for kk in range(4):
            de.line((x0 + kk * 4.0 * ctx.S, y0 - 9 * ctx.S, x0 + kk * 4.0 * ctx.S + rng.uniform(-3, 3), y0 + 9 * ctx.S), fill=255, width=int(0.9 * ctx.S))
        de.line((x0 - 2 * ctx.S, y0 + 6 * ctx.S, x0 + 14 * ctx.S, y0 - 6 * ctx.S), fill=255, width=int(0.9 * ctx.S))
    return arr_of(A), arr_of(E)

def art(font):
    return make_artwork(font, amp_layout(), W, L, S, motif,
                        words=[("SAW", 0, -55, 205, 0.0), ("100W CRUST HEAD", 0, 120, 220, 0)],
                        tapes=[(-120, 108, 90, 14, -3), (230, -112, 70, 13, 6), (40, -4, 55, 11, -8)], seed=21)

SPEC = amp_spec('saw', art, '#ff6a1a', '#e8e4d8', ["JCM800 2203 · CRUNCH", "std · A2 · 48k"])

if __name__ == '__main__':
    run(SPEC)
