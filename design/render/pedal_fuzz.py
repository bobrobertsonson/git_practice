"""BOG BURIAL - 120x150x50 mm doom/sludge/stoner fuzz pedal (Big-Muff-style layout: three knobs across the top,
footswitch + LED below).  Motif: a melting skull sinking into a bog, drips, two crude swamp sigils.
Inks: acid green (ink B, skull/wordmark/extras) + rust (ink A, bog/sigils/mis-registered second pass) on dark olive powder coat.

CLI (renders go OUTSIDE the repo):
    python pedal_fuzz.py --mode hero|ortho|strip|all --out /path/outside/repo [--scale 40]

Importable builder (no camera / lights / render settings are touched):
    import pedal_fuzz
    root = pedal_fuzz.build(mats=None, origin=(x_mm, y_mm, z_mm), rot_z=0.0)
"""
import math, os
import numpy as np
import common as C
from common import *

W, L, H = 120.0, 150.0, 50.0
S = 14
INK_RUST, INK_GREEN = '#a4471c', '#8fe040'
COAT = (0.0135, 0.0185, 0.0075)             # dark olive powder coat (linear RGB)
KS = 1.0
LED_RGB, LED_OFF = (0.30, 1.0, 0.04, 1), '#143008'     # knob value-ring colour (acid green)
KNOB_Y, LABEL_DY = 50.0, -19.5
KNOBS = [("VOLUME", -36, 0.55), ("TONE", 0, 0.45), ("SUSTAIN", 36, 0.80)]
LED_Y, FOOT_Y = -41.0, -58.0
SCREW = (53.2, 68.2)


def layout():
    circles = [(x, KNOB_Y, 18.5) for _, x, _ in KNOBS]
    circles += [(0, LED_Y, 6.0), (0, FOOT_Y, 15.0)]
    circles += [(sx * SCREW[0], sy * SCREW[1], 4.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, KNOB_Y + LABEL_DY, 24, 5.8) for _, x, _ in KNOBS]
    return dict(circles=circles, rects=rects)


# ---------------------------------------------------------------- artwork
def motif(ctx):
    """rust layer: bog water lines, ripples, bubbles, sigils, reeds.  green layer: the melting skull."""
    rng = ctx.rng; S_ = ctx.S
    P = ctx.P
    A, da = ctx.new()           # rust
    E, de = ctx.new()           # green extra (skull)
    lw = lambda mm: max(1, int(mm * S_))
    WL = -9.0                   # waterline y (mm)

    # ---- skull silhouette (right half, mirrored); sinks into the bog at WL
    half = [(0, 31.0), (8, 30.6), (15, 28.2), (20.5, 23.2), (23.2, 16.5), (22.8, 9.5), (20.2, 5.2), (19.8, 1.0),
            (17.6, -2.6), (14.0, -4.8), (13.2, -9.0), (8, -12), (0, -13)]
    jit = lambda v: v + rng.uniform(-0.45, 0.45)
    pts = [P(jit(x), jit(y)) for x, y in half] + [P(-jit(x), jit(y)) for x, y in reversed(half)]
    de.polygon(pts, fill=255)
    # cheek hollows / temple cuts
    for sx in (-1, 1):
        de.polygon([P(sx * 20.2, 5.0), P(sx * 15.5, 3.6), P(sx * 16.8, -1.2), P(sx * 19.2, 0.4)], fill=0)
    # eye sockets: heavy, irregular, one drooping; tapered melt runs out of them
    for sx in (-1, 1):
        cx, cy = P(sx * 9.4, 12.0 - (1.2 if sx > 0 else 0))
        ph = rng.uniform(0, 6.28, 3)
        socket = []
        for t in np.linspace(0, 2 * math.pi, 56, endpoint=False):
            r = 1.0 + 0.09 * math.sin(2 * t + ph[0]) + 0.07 * math.sin(5 * t + ph[1]) + 0.04 * math.sin(9 * t + ph[2])
            ex, ey = 7.0 * r * math.cos(t), 6.0 * r * math.sin(t)
            if ey < 0:
                ey *= 1.18                            # sag toward the bottom
            a = math.radians(-18 * sx)
            socket.append((cx + (ex * math.cos(a) - ey * math.sin(a)) * S_, cy + (ex * math.sin(a) + ey * math.cos(a)) * S_))
        de.polygon(socket, fill=0)
        for off, ln, w in ((-3.4, 8.5, 1.5), (0.4, 15.0, 2.2), (3.6, 6.0, 1.2)):
            xs, ys = P(sx * 9.4 + off, 7.2 + (1.0 if sx > 0 else 0))
            wob = rng.uniform(-0.8, 0.8) * S_
            de.polygon([(xs - w * S_ / 2, ys), (xs + w * S_ / 2, ys), (xs + wob + w * 0.28 * S_, ys + ln * S_), (xs + wob - w * 0.28 * S_, ys + ln * S_)], fill=0)
            de.ellipse((xs + wob - w * 0.62 * S_, ys + ln * S_ - w * 0.4 * S_, xs + wob + w * 0.62 * S_, ys + ln * S_ + w * 1.1 * S_), fill=0)
    # nasal cavity
    de.polygon([P(-3.2, 2.4), P(3.2, 2.4), P(0.9, 8.2), P(-0.9, 8.2)], fill=0)
    de.polygon([P(-0.3, 8.6), P(0.3, 8.6), P(0, 6.6)], fill=0)
    # brow ridge notch + cranial cracks (stencil cuts)
    for pts_ in (((0, 31), (-1.2, 26), (1.2, 22), (-0.4, 18.4)), ((-12, 29), (-9.5, 25.4), (-11, 22.4)), ((11, 29.4), (13.4, 25), (12, 21.5))):
        de.line([P(x, y) for x, y in pts_], fill=0, width=lw(0.75), joint='curve')
    # teeth: block row with cuts, a few missing
    de.rectangle((*P(-13.8, -2.8), *P(13.8, -9.6)), fill=255)
    for i, x in enumerate(np.arange(-12.0, 12.1, 2.4)):
        de.line((*P(x, -2.4), *P(x, -9.6)), fill=0, width=lw(0.55))
    de.line((*P(-13.8, -6.1), *P(13.8, -6.1)), fill=0, width=lw(0.5))
    for x in (-9.6, 4.8):
        de.rectangle((*P(x + 0.35, -2.2), *P(x + 2.05, -5.8)), fill=0)
    # drips: bone/skull melting down into the bog (green)
    for x in np.linspace(-13, 13, 11):
        x += rng.uniform(-0.8, 0.8)
        ln = rng.uniform(2.5, 8.0)
        w = rng.uniform(0.7, 1.5)
        xs, ys = P(x, -8.6)
        de.line((xs, ys, xs, ys + ln * S_), fill=255, width=lw(w))
        de.ellipse((xs - w * 0.9 * S_, ys + ln * S_ - w * 0.6 * S_, xs + w * 0.9 * S_, ys + ln * S_ + w * 1.5 * S_), fill=255)

    # ---- bog (rust): wavy water lines under the waterline, ripples around the skull base
    for i, y in enumerate(np.arange(WL - 1.0, -41.0, -2.7)):
        k = (WL - y) / 30.0
        pts = []
        xs_ = np.linspace(-58, 58, 120)
        ph = rng.uniform(0, 6.28)
        for x in xs_:
            pts.append(P(x, y + (0.5 + 0.9 * k) * math.sin(x * (0.16 + 0.05 * k) + ph + i)))
        # broken line: dash segments, longer + fatter deeper in the bog
        j = 0
        while j < len(pts) - 2:
            seg = int(rng.integers(6, 22))
            if rng.random() < 0.78:
                da.line(pts[j:j + seg + 1], fill=255, width=lw(0.7 + 0.9 * k), joint='curve')
            j += seg + int(rng.integers(1, 4))
    for k in range(5):                                   # ripples spreading from the sinking skull
        rx, ry = 17 + k * 7.5, 2.0 + k * 1.2
        box = (*P(-rx, WL + ry), *P(rx, WL - ry))
        da.arc(box, 12, 168, fill=255, width=lw(0.8 + 0.12 * k))
    for _ in range(26):                                  # bubbles
        bx = rng.uniform(-52, 52); by = rng.uniform(-38, -10)
        if abs(bx) < 14 and by > -13:
            continue
        r = rng.uniform(0.5, 2.0)
        x0, y0 = P(bx, by)
        da.ellipse((x0 - r * S_, y0 - r * S_, x0 + r * S_, y0 + r * S_), outline=255, width=lw(0.38))
    # reeds / cattails on the flanks, rising out of the bog
    for bx, hgt, lean in ((-50, 21, -4), (-46.5, 15, 3), (-54, 11, -2), (50, 19, 5), (46.5, 13, -3), (54, 10, 2)):
        x0, y0 = P(bx, WL + 1.0)
        x1, y1 = P(bx + lean, WL + 1.0 + hgt)
        da.line((x0, y0, x1, y1), fill=255, width=lw(0.75))
        hx, hy = P(bx + lean * 0.96, WL + 1.0 + hgt * 0.86)
        da.ellipse((hx - 1.1 * S_, hy - 3.6 * S_, hx + 1.1 * S_, hy + 3.6 * S_), fill=255)
        da.line((x1, y1, x1 + lean * 0.4 * S_, y1 - 2.5 * S_), fill=255, width=lw(0.35))

    # ---- swamp sigils (rust): crude geometric marks, no specific religious symbol
    def sigil_a(cx, cy, r):
        c = P(cx, cy)
        da.ellipse((c[0] - r * S_, c[1] - r * S_, c[0] + r * S_, c[1] + r * S_), outline=255, width=lw(0.9))
        tri = [P(cx + r * 0.92 * math.cos(math.radians(a)), cy + r * 0.92 * math.sin(math.radians(a))) for a in (270, 30, 150)]
        da.polygon(tri, outline=255, width=lw(0.9))
        da.line((*P(cx, cy + r * 1.35), *P(cx, cy - r * 1.35)), fill=255, width=lw(0.8))
        da.line((*P(cx - r * 1.25, cy - r * 0.25), *P(cx + r * 1.25, cy + r * 0.2)), fill=255, width=lw(0.7))
        da.ellipse((c[0] - 1.1 * S_, c[1] - 1.1 * S_, c[0] + 1.1 * S_, c[1] + 1.1 * S_), fill=255)
        for a in (90, 210, 330):
            q0 = P(cx + r * 1.05 * math.cos(math.radians(a)), cy + r * 1.05 * math.sin(math.radians(a)))
            q1 = P(cx + r * 1.45 * math.cos(math.radians(a)), cy + r * 1.45 * math.sin(math.radians(a)))
            da.line((*q0, *q1), fill=255, width=lw(0.9))
    def sigil_b(cx, cy, r):
        c = P(cx, cy)
        da.polygon([P(cx, cy + r * 1.25), P(cx + r * 1.25, cy), P(cx, cy - r * 1.25), P(cx - r * 1.25, cy)], outline=255, width=lw(0.9))
        da.ellipse((c[0] - r * 0.55 * S_, c[1] - r * 0.55 * S_, c[0] + r * 0.55 * S_, c[1] + r * 0.55 * S_), outline=255, width=lw(0.8))
        for dx in (-0.5, 0.0, 0.5):                       # three falling drops
            q = P(cx + dx * r * 1.1, cy - r * 1.3)
            da.line((q[0], q[1], q[0], q[1] + r * 0.9 * S_), fill=255, width=lw(0.55))
            da.ellipse((q[0] - 0.55 * S_, q[1] + r * 0.9 * S_ - 0.3 * S_, q[0] + 0.55 * S_, q[1] + r * 0.9 * S_ + 1.0 * S_), fill=255)
        da.line((*P(cx - r * 1.8, cy), *P(cx - r * 1.25, cy)), fill=255, width=lw(0.8))
        da.line((*P(cx + r * 1.25, cy), *P(cx + r * 1.8, cy)), fill=255, width=lw(0.8))
        for k in range(3):                                # cross-hatch cut through the core
            a = math.radians(20 + 60 * k)
            da.line((c[0] - 0.5 * r * S_ * math.cos(a), c[1] - 0.5 * r * S_ * math.sin(a),
                     c[0] + 0.5 * r * S_ * math.cos(a), c[1] + 0.5 * r * S_ * math.sin(a)), fill=255, width=lw(0.45))
    sigil_a(-40.5, 21.0, 7.5)
    sigil_b(40.5, 21.0, 6.5)
    return arr_of(A), arr_of(E)


def art(font):
    return make_artwork(font, layout(), W, L, S, motif,
                        words=[("BOG BURIAL", 0, -26.0, 100, -1.4), ("FUZZ", 34, -58.5, 22, 3.0)],
                        tapes=[(-41, -70, 34, 7, -24), (54.5, -22, 30, 7, 82)],
                        tally=[(-44, -58), (-35, -58)], seed=23)


# ---------------------------------------------------------------- 3D build
def _jack(x, y, z, side, mats):
    """generic 1/4in side jack: hex nut + barrel, protruding from the wall at x=side*W/2."""
    root = empty('jack', side * W / 2, y, z)
    barrel = lathe('jack_barrel', [(0, -1.0), (4.6, -1.0), (4.6, 3.0), (4.1, 3.4), (0, 3.4)], [mats['chrome_sat']], 40, 40)
    nut = prism('jack_nut', circle_pts(6.8, 6, math.pi / 6), 0.0, 2.2, [mats['chrome']], 40)
    m = nut.modifiers.new('b', 'BEVEL'); m.width = 0.4 * MM; m.segments = 2; m.limit_method = 'ANGLE'
    for p in (barrel, nut):
        p.parent = root
    root.rotation_euler = (0, math.radians(90 * side), 0)
    return root


def build(mats=None, origin=(0.0, 0.0, 0.0), rot_z=0.0, mode='hero', art_image=None, font_dir=None,
          knob_values=None, led_on=True, return_knobs=False):
    """Build the complete BOG BURIAL pedal and return a root empty positioned at `origin` (mm; centre of the
    footprint at the base of the enclosure), rotated `rot_z` degrees about Z.  Everything created here is parented
    to the root.  No camera, lights, world or render settings are touched.
    mats: dict as made by common.make_standard_mats / run() (None -> created here).  art_image: a packed
    Blender image of the face art (None -> generated; cached in mats['fuzz_art']).
    mode='strip': just the bare enclosure + one knob at the origin (for filmstrip rendering).
    return_knobs=True returns (root, knobs) for animation (knobs[i].set(0..1))."""
    set_piece_dims(W, L, H)
    if mats is None:
        mats = make_standard_mats(None, led_rgb=LED_RGB, led_off=LED_OFF)
    if mode == 'strip':
        art_img = mats.get('art')
    else:
        art_img = art_image or mats.get('fuzz_art')
        if art_img is None:
            art_img = art_to_image(art(fetch_font(font_dir or os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))))
            mats['fuzz_art'] = art_img
    before = set(bpy.data.objects)
    root = empty('fuzz_root', 0, 0, 0)
    powder = mat_powder(art_img, INK_RUST, INK_GREEN, coat_rgb=COAT, gain_a=0.95, gain_b=0.70)
    build_enclosure(powder, W, L, H, r=8.0, bev=1.1, nb=5, name='fuzz_enclosure')
    knobs = []
    if mode == 'strip':
        knobs.append(Knob('SUSTAIN', 0, 0, 0.0, mats))
    else:
        ink_w = mat_ink('print_bone', hexcol('#d9d8b8'), 0.5)
        vals = knob_values or [v for _, _, v in KNOBS]
        for (name, x, _), v in zip(KNOBS, vals):
            k = Knob(name, x, KNOB_Y, v, mats); k.root.scale = (KS,) * 3; knobs.append(k)
            add_text(name, 3.6, x, KNOB_Y + LABEL_DY, ink_w, embolden=0.03)
        build_footswitch(0, FOOT_Y, mats)
        build_led(0, LED_Y, led_on, mats)
        for sx in (-1, 1):
            for sy in (-1, 1):
                build_screw(sx * SCREW[0], sy * SCREW[1], mats)
        _jack(0, 38, 27, -1, mats)         # input
        _jack(0, 38, 27, +1, mats)         # output
    parent_new(before, root)
    root.location = (origin[0] * MM, origin[1] * MM, origin[2] * MM)
    root.rotation_euler = (0, 0, math.radians(rot_z))
    return (root, knobs) if return_knobs else root


def _run_build(mats, mode):
    return build(mats, mode=mode, art_image=mats['art'], return_knobs=True)[1]


SPEC = dict(
    name='fuzz', kind='piece', plate=(W, L, H), art=art, ink_a=INK_RUST, ink_b=INK_GREEN, build=_run_build, strip=True, led_rgb=LED_RGB, led_off=LED_OFF,
    hero=dict(kind='persp', loc=(0.18, -0.385, 0.53), target=(0, -0.004, H * MM), lens=85),
    ortho=dict(kind='ortho', loc=(0, 0, 0.5), ortho_scale=0.165), ortho_res=(1200, 1500))

if __name__ == '__main__':
    run(SPEC)
