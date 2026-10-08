"""VERMIN - 110x150x48 mm rat-style distortion pedal: three knobs across the top (DIST, FILTER, VOLUME), LED and
footswitch below.  Motif: a gnawing rodent skull (big chisel incisors) over a field of gnaw marks and chewed edges,
two drain-grate bars at the bottom.  Original artwork; no lettering, logo or layout copied from any real pedal.
Inks: sulphur yellow (ink A, gnaw marks / grate / mis-registered second pass) + bone (ink B, skull and wordmark) on a
dark gunmetal powder coat worn through to raw aluminium.

CLI (renders go OUTSIDE the repo):
    python pedal_vermin.py --mode hero|ortho|strip|all --out /path/outside/repo [--scale 40]

Importable builder (no camera / lights / render settings are touched):
    import pedal_vermin
    root = pedal_vermin.build(mats=None, origin=(x_mm, y_mm, z_mm), rot_z=0.0)
"""
import math, os
import numpy as np
import common as C
from common import *

W, L, H = 110.0, 150.0, 48.0
S = 14
INK_YELLOW, INK_BONE = '#d8b81c', '#e4e0cc'
COAT = (0.0085, 0.0095, 0.0115)             # dark gunmetal powder coat (linear RGB)
LED_RGB, LED_OFF = (1.0, 0.07, 0.02, 1), '#4a1008'     # knob value-ring colour (red)
KNOB_Y, LABEL_DY = 54.0, -19.0
KNOBS = [("DIST", -33, 0.70), ("FILTER", 0, 0.45), ("VOLUME", 33, 0.58)]
LED_Y, FOOT_Y = -38.0, -56.0
SCREW = (48.2, 68.2)


def layout():
    circles = [(x, KNOB_Y, 17.5) for _, x, _ in KNOBS]
    circles += [(0, LED_Y, 6.0), (0, FOOT_Y, 15.0)]
    circles += [(sx * SCREW[0], sy * SCREW[1], 4.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, KNOB_Y + LABEL_DY, 24, 5.8) for _, x, _ in KNOBS]
    return dict(circles=circles, rects=rects)


# ---------------------------------------------------------------- artwork
def motif(ctx):
    """yellow layer: gnaw marks, chewed border scallops, drain bars.  bone layer: the gnawing rodent skull."""
    rng = ctx.rng; S_ = ctx.S
    P = ctx.P
    A, da = ctx.new()           # yellow
    E, de = ctx.new()           # bone extra (skull)
    lw = lambda mm: max(1, int(mm * S_))
    jit = lambda v: v + rng.uniform(-0.35, 0.35)
    K = 0.8                                          # skull scale; SK maps skull-local mm to the face
    SK = lambda x, y: P(K * x, K * y - 3.2)

    # ---- skull, front view, right half mirrored (cranium, cheek arches, narrow snout)
    half = [(0, 21.5), (6.5, 21.0), (11.5, 18.6), (14.6, 13.8), (15.0, 8.2), (13.6, 3.4), (15.6, 0.6), (14.4, -2.6),
            (10.2, -3.6), (6.8, -5.6), (5.2, -9.5), (4.6, -13.0), (0, -13.6)]
    pts = [SK(jit(x), jit(y)) for x, y in half] + [SK(-jit(x), jit(y)) for x, y in reversed(half)]
    de.polygon(pts, fill=255)
    # eye sockets (small, set high) and zygomatic arch cut-outs
    for sx in (-1, 1):
        cx, cy = SK(sx * 7.6, 11.0)
        de.ellipse((cx - 2.9 * (S_ * K), cy - 3.3 * (S_ * K), cx + 2.9 * (S_ * K), cy + 3.3 * (S_ * K)), fill=0)
        de.polygon([SK(sx * 14.2, 3.2), SK(sx * 10.6, 2.4), SK(sx * 9.8, -0.4), SK(sx * 13.6, 0.0)], fill=0)
    # sagittal crest, nasal cleft
    de.line([SK(0, 21), SK(0.4, 16), SK(-0.3, 11), SK(0, 5.6)], fill=0, width=lw(0.55))
    de.polygon([SK(-1.3, -3.0), SK(1.3, -3.0), SK(0.5, -7.2), SK(-0.5, -7.2)], fill=0)
    # chisel incisors: two long curved teeth, a gap line between, bevelled tips
    for sx in (-1, 1):
        x0, x1 = (0.25, 3.7) if sx > 0 else (-3.7, -0.25)
        tooth = [SK(x0, -12.8), SK(x1, -12.8), SK(x1 + sx * 0.25, -20.0), SK(x1 + sx * 0.1, -27.5),
                 SK((x0 + x1) / 2 - sx * 0.2, -28.8), SK(x0, -27.0), SK(x0 - sx * 0.05, -20.0)]
        de.polygon(tooth, fill=255)
        de.line([SK(x0 + sx * 0.7, -15.0), SK(x0 + sx * 0.7, -26.0)], fill=0, width=lw(0.3))     # enamel highlight cut
    # molar row hint beside the snout
    for sx in (-1, 1):
        for k in range(3):
            de.rectangle((*SK(sx * (6.0 + 1.9 * k) - 0.8, -3.4), *SK(sx * (6.0 + 1.9 * k) + 0.8, -5.4 - 0.3 * k)), fill=0)
    # cracks in the cranium
    for pts_ in (((-6, 20.4), (-5.2, 16), (-7.4, 13.6)), ((9, 18), (10.6, 14.4), (9.4, 12.6))):
        de.line([SK(x, y) for x, y in pts_], fill=0, width=lw(0.5), joint='curve')

    # ---- gnaw marks (yellow): paired incisor grooves in arcs, fanned across the flanks
    def gnaw_cluster(cx, cy, ang, n, span):
        for i in range(n):
            a = math.radians(ang + (i - (n - 1) / 2) * span / max(1, n - 1))
            ln = rng.uniform(5.0, 11.0)
            for off in (-0.55, 0.55):                       # two parallel grooves per bite
                px, py = cx + off * math.cos(a + math.pi / 2), cy + off * math.sin(a + math.pi / 2)
                seg = [P(px + t * ln * math.cos(a) + 0.5 * math.sin(t * 5), py + t * ln * math.sin(a)) for t in np.linspace(0, 1, 10)]
                da.line(seg, fill=255, width=lw(0.42 + 0.2 * rng.random()), joint='curve')
    gnaw_cluster(-38, 20, -70, 7, 70)
    gnaw_cluster(38, 18, -110, 7, 70)
    gnaw_cluster(-35, -12, 20, 5, 55)
    gnaw_cluster(36, -14, 160, 5, 55)
    # chewed border: bite scallops eaten out of the four edges (stencil drop-outs), shavings flying off
    for x in np.arange(-W / 2 + 4, W / 2 - 3, 5.3):
        for y_edge, d in ((L / 2 - 2.0, 1), (-L / 2 + 2.0, -1)):
            r = rng.uniform(1.3, 2.4)
            cx_, cy_ = P(x + rng.uniform(-0.8, 0.8), y_edge)
            da.arc((cx_ - r * S_, cy_ - r * S_, cx_ + r * S_, cy_ + r * S_), 0 if d > 0 else 180, 180 if d > 0 else 360, fill=255, width=lw(0.55))
    for y in np.arange(-L / 2 + 6, L / 2 - 5, 5.9):
        for x_edge, d in ((-W / 2 + 2.0, 1), (W / 2 - 2.0, -1)):
            r = rng.uniform(1.3, 2.4)
            cx_, cy_ = P(x_edge, y + rng.uniform(-0.8, 0.8))
            da.arc((cx_ - r * S_, cy_ - r * S_, cx_ + r * S_, cy_ + r * S_), 270 if d > 0 else 90, 450 if d > 0 else 270, fill=255, width=lw(0.55))
    for _ in range(40):                                      # wood-shaving curls
        x, y = rng.uniform(-50, 50), rng.uniform(-34, 34)
        if abs(x) < 17 and -36 < y < 26:
            continue
        a0 = rng.uniform(0, 360)
        r = rng.uniform(0.7, 1.7)
        cx_, cy_ = P(x, y)
        da.arc((cx_ - r * S_, cy_ - r * S_, cx_ + r * S_, cy_ + r * S_), a0, a0 + 250, fill=255, width=lw(0.33))

    # ---- drain-grate bars under the skull (yellow): three rounded slots each side of the LED
    for sx in (-1, 1):
        for k in range(3):
            x = sx * (17.0 + k * 6.2)
            da.rounded_rectangle((*P(x - 1.4, -22.5), *P(x + 1.4, -44.5)), radius=int(1.4 * S_), outline=255, width=lw(0.7))
    return arr_of(A), arr_of(E)


def art(font):
    return make_artwork(font, layout(), W, L, S, motif,
                        words=[("VERMIN", 0, 24.0, 60, 0.0), ("DISTORTION", 36, -63.0, 24, -4.0)],
                        tapes=[(-42, -50, 30, 6.5, 24), (46, 4, 26, 6.5, 86)],
                        tally=[(-40, -64), (-33, -64)], seed=41)


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
    """Build the complete VERMIN pedal and return a root empty positioned at `origin` (mm; centre of the footprint
    at the base of the enclosure), rotated `rot_z` degrees about Z.  Everything created here is parented to the root.
    No camera, lights, world or render settings are touched.
    mats: dict as made by common.make_standard_mats / run() (None -> created here).  art_image: a packed Blender
    image of the face art (None -> generated; cached in mats['vermin_art']).
    mode='strip': just the bare enclosure + one knob at the origin (for filmstrip rendering).
    return_knobs=True returns (root, knobs) for animation (knobs[i].set(0..1))."""
    set_piece_dims(W, L, H)
    if mats is None:
        mats = make_standard_mats(None, led_rgb=LED_RGB, led_off=LED_OFF)
    if mode == 'strip':
        art_img = mats.get('art')
    else:
        art_img = art_image or mats.get('vermin_art')
        if art_img is None:
            art_img = art_to_image(art(fetch_font(font_dir or os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))))
            mats['vermin_art'] = art_img
    before = set(bpy.data.objects)
    root = empty('vermin_root', 0, 0, 0)
    powder = mat_powder(art_img, INK_YELLOW, INK_BONE, coat_rgb=COAT, gain_a=0.80, gain_b=0.70)
    build_enclosure(powder, W, L, H, r=7.0, bev=1.1, nb=5, name='vermin_enclosure')
    knobs = []
    if mode == 'strip':
        knobs.append(Knob('DIST', 0, 0, 0.0, mats))
    else:
        ink_w = mat_ink('print_bone', hexcol('#d9d8b8'), 0.5)
        vals = knob_values or [v for _, _, v in KNOBS]
        for (name, x, _), v in zip(KNOBS, vals):
            k = Knob(name, x, KNOB_Y, v, mats); knobs.append(k)
            add_text(name, 3.6, x, KNOB_Y + LABEL_DY, ink_w, embolden=0.03)
        build_footswitch(0, FOOT_Y, mats)
        build_led(0, LED_Y, led_on, mats)
        for sx in (-1, 1):
            for sy in (-1, 1):
                build_screw(sx * SCREW[0], sy * SCREW[1], mats)
        _jack(0, 36, 24, -1, mats)         # input
        _jack(0, 36, 24, +1, mats)         # output
    parent_new(before, root)
    root.location = (origin[0] * MM, origin[1] * MM, origin[2] * MM)
    root.rotation_euler = (0, 0, math.radians(rot_z))
    return (root, knobs) if return_knobs else root


def _run_build(mats, mode):
    return build(mats, mode=mode, art_image=mats['art'], return_knobs=True)[1]


SPEC = dict(
    name='vermin', kind='piece', plate=(W, L, H), art=art, ink_a=INK_YELLOW, ink_b=INK_BONE, build=_run_build, strip=True,
    led_rgb=LED_RGB, led_off=LED_OFF,
    hero=dict(kind='persp', loc=(0.17, -0.36, 0.50), target=(0, -0.004, H * MM), lens=85),
    ortho=dict(kind='ortho', loc=(0, 0, 0.5), ortho_scale=0.165), ortho_res=(1100, 1500))

if __name__ == '__main__':
    run(SPEC)
