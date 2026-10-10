"""STOCKHOLM SYNDROME (design B2) - 120x190x55 mm pedal, saw-blade crust stencil, burnt orange + bone.
    python pedal_b2.py --mode all --out /path/outside/repo"""
import math, os
import numpy as np
import common as C
from common import *

KNOBS = [("LOW", -36, 26, 0.95), ("HIGH", 0, 26, 0.92), ("DIST", 36, 26, 0.80),
         ("IN GAIN", -36, -15, 0.50), ("OUT", 0, -15, 0.55), ("MIX", 36, -15, 1.00)]
LABEL_DY = -18.0
TOGGLE_Y, TOGGLE_LABEL_Y = -47.5, -58.5
FOOT_Y, FOOT_LED_Y = -80.0, -64.5
OLED_Y = 56.0
W, L, H = 120.0, 190.0, 55.0
S = 14

def layout():
    circles = [(x, y, 19.5) for _, x, y, _ in KNOBS]
    circles += [(x, TOGGLE_Y, 11.0) for x in (-36, 0, 36)]
    circles += [(x, FOOT_Y, 14.5) for x in (-30, 30)]
    circles += [(x, FOOT_LED_Y, 6.0) for x in (-30, 30)]
    circles += [(sx * 53.2, sy * 88.2, 4.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, y + LABEL_DY, 20, 5.5) for _, x, y, _ in KNOBS]
    rects += [(x, TOGGLE_LABEL_Y, 14, 5.5) for x in (-36, 0, 36)]
    rects += [(0, OLED_Y, 68, 25)]
    return dict(circles=circles, rects=rects)

def motif(ctx):
    """oversized stencilled 16-tooth saw blade with concentric rings and stencil bridges."""
    rng = ctx.rng; S_ = ctx.S
    O, d = ctx.new()
    cx, cy = ctx.P(16, -6)
    pol = lambda a, r: (cx + r * S_ * math.cos(math.radians(a)), cy - r * S_ * math.sin(math.radians(a)))
    pts = []
    for t in range(16):
        a0 = t * 360 / 16
        pts += [pol(a0, 78), pol(a0 + 2, 91)]
        pts += [pol(a0 + 2 + 18 * u, 91 - 13 * u ** 0.6) for u in np.linspace(0.1, 1.0, 7)]
    d.polygon(pts, fill=255)
    d.ellipse((cx - 70 * S_, cy - 70 * S_, cx + 70 * S_, cy + 70 * S_), fill=0)
    for ro, ri in ((60, 53), (41, 37), (25, 21), (9, 6)):
        d.ellipse((cx - ro * S_, cy - ro * S_, cx + ro * S_, cy + ro * S_), fill=255)
        d.ellipse((cx - ri * S_, cy - ri * S_, cx + ri * S_, cy + ri * S_), fill=0)
    d.ellipse((cx - 2.5 * S_, cy - 2.5 * S_, cx + 2.5 * S_, cy + 2.5 * S_), fill=255)
    for ra, rb in ((66, 92), (50, 64), (34, 45), (18, 29), (3, 12)):
        for k in range(5):
            a = rng.uniform(0, 360)
            d.line((*pol(a, ra), *pol(a, rb)), fill=0, width=int(rng.uniform(0.9, 1.5) * S_))
    for k in range(6):
        a = k * 60 + 17
        d.line((*pol(a, 71), *pol(a, 93)), fill=0, width=int(1.1 * S_))
    return arr_of(O), None

def art(font):
    return make_artwork(font, layout(), W, L, S, motif,
                        words=[("STOCKHOLM", 0, 85, 100, 2.2), ("SYNDROME", 0, 73.2, 88, 2.2)],
                        tapes=[(-47, 91, 44, 8, -33), (48, -91, 40, 8, -30), (53, 4, 36, 7.5, 84)],
                        tally=[(-9, -80), (1, -80)])

def populate(mats, mode):
    ink_w = mat_ink('print_white', hexcol('#e8e4d8'), 0.5)
    knobs = []
    if mode == 'strip':
        knobs.append(Knob('DIST', 0, 0, 0.0, mats))
        return knobs
    for name, x, y, v in KNOBS:
        knobs.append(Knob(name, x, y, v, mats))
        add_text(name, 3.4, x, y + LABEL_DY, ink_w, embolden=0.03)
    for (nm, x, tilt) in (("SLOT", -36, 20), ("SIZE", 0, 0), ("NORM", 36, -20)):
        build_toggle(x, TOGGLE_Y, tilt, mats)
        add_text(nm, 3.4, x, TOGGLE_LABEL_Y, ink_w, embolden=0.03)
    build_footswitch(-30, FOOT_Y, mats); build_footswitch(30, FOOT_Y, mats)
    build_led(-30, FOOT_LED_Y, True, mats); build_led(30, FOOT_LED_Y, False, mats)
    for sx in (-1, 1):
        for sy in (-1, 1):
            build_screw(sx * 53.2, sy * 88.2, mats)
    build_oled(mats, 0, OLED_Y)
    return knobs

INK_A, INK_B = '#ff6a1a', '#e8e4d8'
OLED_LINES = ["HM-2w CHAINSAW", "std · A2 · 48k"]

def build(mats=None, origin=(0.0, 0.0, 0.0), rot_z=0.0, mode='hero', art_image=None, font_dir=None,
          return_knobs=False):
    """Build the complete STOCKHOLM SYNDROME pedal and return a root empty at `origin` (mm; centre of the footprint
    at the base of the enclosure), rotated `rot_z` degrees about Z.  Everything is parented to the root; camera,
    lights, world and render settings are not touched.
    mats: dict from common.make_standard_mats / run() (None -> created here).  art_image: packed Blender image of the
    face art (None -> generated, cached in mats['stock_art']).  mode='strip': bare enclosure + one knob at the origin.
    return_knobs=True returns (root, knobs)."""
    set_piece_dims(W, L, H)
    if mats is None:
        mats = make_standard_mats(None)
    if 'oled' not in mats:                 # make_standard_mats has no OLED material; run() adds it
        oimg = oled_image(OLED_LINES)
        mats['oled'] = mat_oled_display(oimg, oimg.size[0], 28)
    if mode == 'strip':
        art_img = mats.get('art')
    else:
        art_img = art_image or mats.get('stock_art')
        if art_img is None:
            art_img = art_to_image(art(fetch_font(font_dir or os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))))
            mats['stock_art'] = art_img
    before = set(bpy.data.objects)
    root = empty('stockholm_root', 0, 0, 0)
    powder = mat_powder(art_img, INK_A, INK_B, gain_a=0.45)
    build_enclosure(powder, W, L, H, r=7.0, bev=1.0, nb=5, name='stockholm_enclosure')
    knobs = populate(mats, mode)
    parent_new(before, root)
    root.location = (origin[0] * MM, origin[1] * MM, origin[2] * MM)
    root.rotation_euler = (0, 0, math.radians(rot_z))
    return (root, knobs) if return_knobs else root

def _run_build(mats, mode):
    return build(mats, mode=mode, art_image=mats['art'], return_knobs=True)[1]

SPEC = dict(
    name='stockholm', kind='piece', plate=(W, L, H), art=art, ink_a=INK_A, ink_b=INK_B, build=_run_build,
    oled_lines=OLED_LINES, strip=True,
    hero=dict(kind='persp', loc=(0.20, -0.44, 0.59), target=(0, -0.008, H * MM), lens=85),
    ortho=dict(kind='ortho', loc=(0, 0, 0.5), ortho_scale=0.205), ortho_res=(1200, 1800))

if __name__ == '__main__':
    run(SPEC)
