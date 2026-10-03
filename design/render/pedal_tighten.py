"""TIGHTEN (design B boost) - 100x150x42 mm pedal: bench-vise jaws + barbed wire crust stencil, cold steel blue + bone."""
import math
import numpy as np
from common import *

W, L, H = 100.0, 150.0, 42.0
S = 14
KS = 0.72                                   # knob scale vs the 120x190 pedal hardware
KNOBS = [("DRIVE", -30, 38, 0.65), ("TONE", 0, 38, 0.50), ("LEVEL", 30, 38, 0.72)]
LABEL_DY = -14.6
TOG_Y, TOG_LABEL_Y = 4.0, -6.0
LED_Y, FOOT_Y = -29.0, -50.0

def layout():
    circles = [(x, y, 14.5) for _, x, y, _ in KNOBS]
    circles += [(0, TOG_Y, 10.0), (0, LED_Y, 5.5), (0, FOOT_Y, 14.5)]
    circles += [(sx * 43.2, sy * 68.2, 4.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, y + LABEL_DY, 17, 5.0) for _, x, y, _ in KNOBS] + [(0, TOG_LABEL_Y, 14, 5.0)]
    return dict(circles=circles, rects=rects)

def motif(ctx):
    """vise: two serrated jaws + screw; the barbed wire (bone) is returned as the extra layer."""
    rng = ctx.rng; S_ = ctx.S
    A, d = ctx.new()
    p = ctx.P
    def rect(xa, ya, xb, yb, fill):
        (x0, y0), (x1, y1) = p(xa, ya), p(xb, yb)
        d.rectangle((min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)), fill=fill)
    def jaw(sign, inner, outer, y0, y1):
        pts = [p(sign * outer, y0), p(sign * outer, y1)]
        y = y1
        flip = 0
        while y > y0:
            pts.append(p(sign * (inner + (3.2 if flip else 0)), y)); y -= 3.0; flip ^= 1
        pts.append(p(sign * inner, y0))
        d.polygon(pts, fill=255)
        # recessed panel + bolt holes (stencil bridges keep them readable)
        rect(sign * (outer - 6), y1 - 7, sign * (inner + 12), y0 + 7, 0)
        for yy in (y1 - 14, y0 + 14):
            cx, cy = p(sign * (outer - 14), yy)
            d.ellipse((cx - 3 * S_, cy - 3 * S_, cx + 3 * S_, cy + 3 * S_), fill=0)
    jaw(-1, 11, 74, -44, 40)
    jaw(+1, 11, 74, -44, 40)
    # central threaded screw with T-handle under the jaws
    for i in range(14):
        y = -36 + i * 5.2
        d.polygon([p(-7, y), p(7, y - 1.5), p(7, y - 3.5), p(-7, y - 2)], fill=255)
    rect(-34, -62, 34, -57, 255)
    for sx in (-1, 1):
        cx, cy = p(sx * 37, -59.5)
        d.ellipse((cx - 4.2 * S_, cy - 4.2 * S_, cx + 4.2 * S_, cy + 4.2 * S_), fill=255)
    for yy in (22, -12):                     # stencil bridges across the jaws
        for xx in (-42, 42):
            d.line((*p(xx - 10, yy), *p(xx + 10, yy)), fill=0, width=int(1.1 * S_))
    # barbed wire: two twisted strands + barbs, bone ink
    E, de = ctx.new()
    for (x0, y0, x1, y1, ph) in ((-60, 70, 62, -68, 0.0), (-62, 20, 62, -30, 1.7)):
        n = 160
        for strand in (0, 1):
            pts = []
            for i in range(n + 1):
                t = i / n
                bx, by = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
                nx, ny = -(y1 - y0), (x1 - x0); ln = math.hypot(nx, ny); nx, ny = nx / ln, ny / ln
                off = 1.4 * math.sin(t * 38 + ph + strand * math.pi)
                pts.append(p(bx + nx * off, by + ny * off))
            de.line(pts, fill=255, width=int(0.7 * S_))
        for i in range(6, n - 4, 9):
            t = i / n
            bx, by = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            for ang in (35, -35, 145, -145):
                a = math.radians(ang) + math.atan2(y1 - y0, x1 - x0)
                de.line((*p(bx, by), *p(bx + 3.6 * math.cos(a), by + 3.6 * math.sin(a))), fill=255, width=int(0.55 * S_))
    return arr_of(A), arr_of(E)

def art(font):
    return make_artwork(font, layout(), W, L, S, motif,
                        words=[("TIGHTEN", 0, 65, 84, -1.6)],
                        tapes=[(-36, -67, 32, 7, -28), (38, 14, 30, 7, 80)],
                        tally=[(-36, -48), (-27, -48)], seed=11)

def populate(mats, mode):
    ink_w = mat_ink('print_white', hexcol('#e8e4d8'), 0.5)
    knobs = []
    for name, x, y, v in KNOBS:
        k = Knob(name, x, y, v, mats); k.root.scale = (KS, KS, KS); knobs.append(k)
        add_text(name, 3.0, x, y + LABEL_DY, ink_w, embolden=0.03)
    t = build_toggle(0, TOG_Y, 20, mats); t.scale = (0.9, 0.9, 0.9)
    add_text("TIGHT", 3.0, 0, TOG_LABEL_Y, ink_w, embolden=0.03)
    build_footswitch(0, FOOT_Y, mats)
    build_led(0, LED_Y, True, mats)
    for sx in (-1, 1):
        for sy in (-1, 1):
            build_screw(sx * 43.2, sy * 68.2, mats)
    return knobs

SPEC = dict(
    name='tighten', kind='pedal', plate=(W, L, H), art=art, ink_a='#4f8fd0', ink_b='#e8e4d8', populate=populate,
    corner=6.0, bevel=0.9, ink_gain=0.6, led_rgb=(0.12, 0.45, 1.0, 1), led_off='#0b2038',
    hero=dict(kind='persp', loc=(0.17, -0.37, 0.50), target=(0, -0.006, H * MM), lens=85),
    ortho=dict(kind='ortho', loc=(0, 0, 0.5), ortho_scale=0.165), ortho_res=(1000, 1500))

if __name__ == '__main__':
    run(SPEC)
