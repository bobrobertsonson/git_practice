"""BODY amp head (path B) - same construction as SAW; crust stencil of anvil + hammer + chain links,
blood red + bone.
    python amp_body.py --mode all --out /path/outside/repo"""
import math
import numpy as np
from common import *

W, L, S = 600.0, 260.0, 5

def link(ctx, d, x, y, ang, length, width, edge_on, thick):
    """one chain link (stadium ring) centred at x,y (mm); edge_on links are drawn as a slim bar."""
    S_ = ctx.S
    if edge_on:
        w = width * 0.32
    else:
        w = width
    pts = []
    n = 14
    r = w / 2
    for i in range(n + 1):                     # right cap
        a = -math.pi / 2 + math.pi * i / n
        pts.append((length / 2 - r + r * math.cos(a), r * math.sin(a)))
    for i in range(n + 1):                     # left cap
        a = math.pi / 2 + math.pi * i / n
        pts.append((-length / 2 + r + r * math.cos(a), r * math.sin(a)))
    ca, sa = math.cos(ang), math.sin(ang)
    P = [ctx.P(x + px * ca - py * sa, y + px * sa + py * ca) for px, py in pts]
    d.polygon(P, fill=255)
    if not edge_on:
        inner = []
        for px, py in pts:
            px2 = px * (1 - 2 * thick / length); py2 = py * (1 - 2 * thick / max(w, 0.1))
            inner.append(ctx.P(x + px2 * ca - py2 * sa, y + px2 * sa + py2 * ca))
        d.polygon(inner, fill=0)

def chain(ctx, d, path, pitch=15.0, length=21.0, width=12.0, thick=2.6):
    """links along a polyline of (x,y) mm points, alternating flat / edge-on."""
    acc = 0.0; i = 0
    pts = np.array(path)
    seg = np.hypot(*(pts[1:] - pts[:-1]).T)
    total = seg.sum(); s = 0.0; k = 0
    while s < total:
        j = np.searchsorted(np.cumsum(seg), s)
        j = min(j, len(seg) - 1)
        t = (s - (np.cumsum(seg)[j] - seg[j])) / seg[j]
        x, y = pts[j] + (pts[j + 1] - pts[j]) * t
        ang = math.atan2(*(pts[j + 1] - pts[j])[::-1])
        link(ctx, d, x, y, ang, length, width, k % 2 == 1, thick)
        s += pitch; k += 1

def poly_rot(ctx, d, pts, cx, cy, ang, fill=255):
    ca, sa = math.cos(ang), math.sin(ang)
    d.polygon([ctx.P(cx + px * ca - py * sa, cy + px * sa + py * ca) for px, py in pts], fill=fill)

def motif(ctx):
    rng = ctx.rng
    A, d = ctx.new()
    # anvil (horn left, hardy hole, waisted body, wide foot), centre (95,-62)
    anvil = [(-118, 4), (-92, 8), (-60, 12), (-40, 16), (62, 16), (62, 0), (50, -4), (40, -26), (50, -44), (80, -46), (80, -62),
             (-72, -62), (-72, -46), (-36, -44), (-22, -26), (-36, -6), (-62, -4), (-92, -2)]
    poly_rot(ctx, d, [(x * 1.25, y * 1.25) for x, y in anvil], 70, -42, 0.0)
    poly_rot(ctx, d, [(37, 20), (55, 20), (55, 11), (37, 11)], 70, -42, 0.0, fill=0)           # hardy hole
    for xx in (-0.5, 30):                                                                    # stencil bridges
        pass
    d.line((*ctx.P(70 + 20, -42 + 28), *ctx.P(70 + 20, -42 - 40)), fill=0, width=int(1.4 * ctx.S))
    d.line((*ctx.P(70 - 100, -42 + 12), *ctx.P(70 - 100, -42 - 40)), fill=0, width=int(1.4 * ctx.S))
    # sledgehammer leaning right of the anvil
    ang = math.radians(28)
    poly_rot(ctx, d, [(-4.5, -92), (4.5, -92), (3.4, 78), (-3.4, 78)], 262, -60, ang)
    poly_rot(ctx, d, [(-22, 78), (22, 78), (22, 108), (-22, 108)], 262, -60, ang)
    poly_rot(ctx, d, [(-26, 82), (-22, 82), (-22, 104), (-26, 104)], 262, -60, ang)
    d.line((*ctx.P(262 + 30 * math.sin(ang), -60 + 94 * math.cos(ang)), *ctx.P(262 + 30 * math.sin(ang) - 20 * math.cos(ang), -60 + 94 * math.cos(ang) - 20 * math.sin(ang))), fill=0, width=int(1.2 * ctx.S))
    # bone extra: chain links along the top gum line and sagging along the bottom
    E, de = ctx.new()
    chain(ctx, de, [(-286, 31), (286, 31)])
    path = [(x, -118 + 0.00028 * x * x - 14 * math.cos(x / 55.0) * 0.0) for x in np.linspace(-286, 286, 40)]
    chain(ctx, de, path, pitch=16.5, length=22, width=13, thick=2.8)
    return arr_of(A), arr_of(E)

def art(font):
    return make_artwork(font, amp_layout(), W, L, S, motif,
                        words=[("BODY", -185, -62, 175, 1.5), ("HEAVY DUTY BODY WORK", 0, 119, 215, 0)],
                        tapes=[(-110, 112, 80, 13, 3), (250, -105, 60, 12, -7), (120, 14, 70, 12, 5)], tally=[(-285, 6)], seed=33)

SPEC = amp_spec('body', art, '#b3121a', '#e8e4d8', ["HM-2w BODY", "std · A2 · 48k"],
                ink_gain=1.0, led_rgb=(1.0, 0.04, 0.03, 1), led_off='#3a0507')

if __name__ == '__main__':
    run(SPEC)
