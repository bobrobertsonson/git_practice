"""SAWBLADE 2x12 open-back cab (generic, unbranded) for the "studio split" layout: same construction as cab_4x12.py
(black tolex, metal corner caps, piping, stitched seams, recessed side handles, woven grille cloth, generic 12-inch
drivers, one draggable generic dynamic mic) but a landscape 2-up baffle and its own motif:
crossed bones + crust spikes, painted on the grille cloth in bruise purple + bone.  PLACEHOLDER ART pending the
user's call on the studio-split cab motif.
    python cab_2x12.py --mode all --out /path/outside/repo      # hero, ortho (grille on), open (grille off + mic)"""
import math
import numpy as np
import common as C
from common import *
from cab_4x12 import box_obj, bool_cut

BW, BD, BH = 740.0, 290.0, 520.0
GW, GL = 684.0, 468.0                    # grille cloth / art plate (mm)
S = 3
from cab_layout import CABS
SPK = CABS['2x12']['speakers']
BAFFLE_Z = -22.0
GRILLE_TOP = -6.0
LOGO = (252.0, -192.0)
LIFT = CABS['2x12']['lift_mm']                              # mm, ortho views only (see _build)
INK_A, INK_B = '#8f55b8', '#e8e4d8'      # bruise purple, bone

def layout():
    return dict(circles=[], rects=[(LOGO[0], LOGO[1], 100, 36)])

def bone(ctx, d, cx, cy, length, ang, w, gap=0.0):
    """stencil femur-style bone (mm, y up): waisted shaft + a pair of knuckle lobes on each end; gap > 0 first cuts a
    clear halo (stencil bridge) so it reads as passing over whatever is already drawn."""
    ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    def pt(u, v):                                   # along-bone u, across v -> panel px
        return ctx.P(cx + u * ca - v * sa, cy + u * sa + v * ca)
    h = length / 2
    for fill, e in ((0, gap), (255, 0.0)):
        if fill == 0 and gap <= 0:
            continue
        shaft = []
        for t in np.linspace(-1, 1, 21):
            shaft.append(pt(t * (h - w * 0.6), (0.36 + 0.14 * t * t) * w + e))
        for t in np.linspace(1, -1, 21):
            shaft.append(pt(t * (h - w * 0.6), -((0.36 + 0.14 * t * t) * w + e)))
        d.polygon(shaft, fill=fill)
        for sgn in (-1, 1):
            for off in (-0.52, 0.52):
                x, y = pt(sgn * (h - w * 0.55), off * w)
                r = (0.62 * w + e) * ctx.S
                d.ellipse((x - r, y - r, x + r, y + r), fill=fill)

def spikes_row(ctx, d, y_edge, direction, rng, x0=-342, x1=342, h_range=(9, 26), pitch=(14, 24)):
    """jagged crust crown along a horizontal edge (direction +1 grows upward, -1 downward)."""
    x = x0
    while x < x1:
        wdt = rng.uniform(*pitch); hh = rng.uniform(*h_range)
        d.polygon([ctx.P(x, y_edge), ctx.P(x + wdt, y_edge), ctx.P(x + wdt * rng.uniform(0.25, 0.75), y_edge + direction * hh)], fill=255)
        x += wdt * rng.uniform(0.8, 1.0)

def motif(ctx):
    rng = ctx.rng
    A, d = ctx.new()
    # purple: burst of long thin spikes behind the bones + jagged crowns along top and bottom edge
    for k in range(26):
        a = 2 * math.pi * (k + rng.uniform(-0.3, 0.3)) / 26
        r0, r1 = 40.0, rng.uniform(190, 330)
        wa = rng.uniform(0.05, 0.11)
        d.polygon([ctx.P(r0 * math.cos(a - wa), r0 * 0.62 * math.sin(a - wa)), ctx.P(r1 * math.cos(a) * 1.05, r1 * 0.62 * math.sin(a)),
                   ctx.P(r0 * math.cos(a + wa), r0 * 0.62 * math.sin(a + wa))], fill=255)
    spikes_row(ctx, d, GL / 2 - 1, -1, rng)
    spikes_row(ctx, d, -GL / 2 + 1, +1, rng)
    d.ellipse((*ctx.P(-60, 36), *ctx.P(60, -36)), fill=255)
    E, de = ctx.new()
    bone(ctx, de, 0, 0, 560, 27, 36, 0.0)
    bone(ctx, de, 0, 0, 560, -27, 36, gap=4.5)
    return arr_of(A), arr_of(E)

def art(font):
    return make_artwork(font, layout(), GW, GL, S, motif, words=[("CRUST", -150, -192, 300, 0)],
                        tapes=[(-262, 214, 120, 20, 4), (250, 205, 90, 18, -6)], seed=57)

def build(mats, mode):
    tol = mats['leather']
    body = build_enclosure(tol, BW, BD, BH, r=8.0, bev=3.5, nb=5, name='cab_body')
    root = empty('cab_front', 0, -BD / 2, BH / 2)
    root.rotation_euler = (math.pi / 2, 0, 0)
    before = set(bpy.data.objects)
    pocket = prism('pocket', rrect(346, 238, 6, 4), -105, 3, [tol])
    bool_cut(body, pocket)
    for sx in (-1, 1):                                    # recessed side handles
        rec = box_obj('handle_recess', sx * (BW / 2 - 6), 0, BH - 80, 40, 190, 48, tol)
        bool_cut(body, rec)
        box_obj('handle_strap', sx * (BW / 2 - 16), 0, BH - 80, 6, 150, 16, mats['black'])
        for sy in (-1, 1):
            box_obj('handle_mount', sx * (BW / 2 - 14), sy * 78, BH - 80, 8, 12, 34, mats['chrome_sat'])
    corner_caps(BW, BD, BH, mats, front=True, size=34.0)
    thread = mat_ink('thread', (0.12, 0.115, 0.10, 1), 0.8)
    hx, hy = BW / 2 - 28, BH / 2 - 28
    zc = BH / 2
    for (p0, p1) in (((-hx, -BD / 2, zc + hy), (hx, -BD / 2, zc + hy)), ((-hx, -BD / 2, zc - hy), (hx, -BD / 2, zc - hy)),
                     ((-hx - 4, -BD / 2, zc - hy + 4), (-hx - 4, -BD / 2, zc + hy - 4)), ((hx + 4, -BD / 2, zc - hy + 4), (hx + 4, -BD / 2, zc + hy - 4))):
        stitch_line('stitch', p0, p1, (0, -1, 0), thread)
    piping_loop(BW / 2 - 6, BH / 2 - 6, 10, 1.2, 3.2, mats['black'], 'piping')
    baffle = prism('baffle', rrect(344, 236, 4, 4), -40, BAFFLE_Z, [mats['baffle']], angle=40)
    for (x, y) in SPK:
        cut = prism('hole', [(x + px, y + py) for px, py in circle_pts(141.0, 64)], -60, -10, [mats['black']])
        bool_cut(baffle, cut)
        build_speaker(x, y, BAFFLE_Z, mats)
    if mode != 'open':
        g = build_enclosure(mats['grille'], GW, GL, 4, r=8.0, bev=0.4, nb=3, name='grille_cloth')
        g.location.z = (GRILLE_TOP - 4) * MM
        plate = rbox('logo_plate', 100, 30, 2.2, 3.0, [mats['alu']], z0=GRILLE_TOP - 0.3)
        plate.location = (LOGO[0] * MM, LOGO[1] * MM, 0)
        cu = bpy.data.curves.new('logo_txt', 'FONT'); cu.body = "SAWBLADE 2x12"; cu.size = 6.4 * MM
        cu.align_x = 'CENTER'; cu.align_y = 'CENTER'; cu.extrude = 0.25 * MM
        fp = C_FONT.get('path')
        if fp:
            cu.font = bpy.data.fonts.load(fp)
        cu.materials.append(mats['label_ink'])
        t = new_obj('logo_txt', cu)
        bpy.context.view_layer.update()
        t.scale = (min(1.0, 86 * MM / max(t.dimensions.x, 1e-9)),) * 2 + (1.0,)
        t.location = (LOGO[0] * MM, LOGO[1] * MM, (GRILLE_TOP + 1.95) * MM)
    elif not NO_MIC:
        # mic ~1 in off the grille plane, just off the dust cap of the left driver, 30 deg off axis
        tip = Vector((-165 + 38, 6, GRILLE_TOP + 25.4))
        az = math.radians(135); tilt = math.radians(30)
        u = Vector((math.sin(tilt) * math.cos(az), math.sin(tilt) * math.sin(az), math.cos(tilt)))
        mic = build_mic(mats)
        mic.location = tip * MM
        mic.rotation_euler = u.to_track_quat('Z', 'Y').to_euler()
        clip_p = tip + u * 92
        boom_end = clip_p + Vector((-520, 330, 60))
        tube_path('boom', [tuple(clip_p + Vector((-4, 3, 0))), tuple(boom_end)], 6.0, mats['chrome_sat'], closed=False, ns=14, ref=(0, 0, 1))
        clip = lathe('mic_clip', [(0, -10), (13.5, -10), (13.5, 10), (0, 10)], [mats['chrome_dark']], 32, 40)
        clip.location = clip_p * MM; clip.rotation_euler = mic.rotation_euler
    for ob in list(bpy.data.objects):
        if ob not in before and ob is not root and ob.parent is None and ob.type not in ('CAMERA',) and ob.name != body.name:
            ob.parent = root
    for ob in list(bpy.data.objects):
        if ob.parent is root and (ob.name.startswith(('corner_cap', 'stitch', 'handle_'))):
            ob.parent = None
    return None

C_FONT = {}
NO_MIC = '--no-mic' in __import__('sys').argv   # open view without the baked mic + boom (the plugin draws its own)
SPEC = dict(
    name='cab2', kind='cab', plate=(GW, GL, 4.0), art=art, ink_a=INK_A, ink_b=INK_B, populate=None, build=None,
    k=3.4, center=(0, 0, BH / 2 * MM), modes=['hero', 'ortho', 'open'], ortho_res=CABS['2x12']['res'],
    fnames={'hero': 'cab2x12_hero_3q.png', 'ortho': 'cab2x12_front_ortho.png', 'open': 'cab2x12_open_ortho.png'},
    hero=dict(kind='persp', loc=(1.0, -1.85, 1.05), target=(0, 0, 0.25), lens=70, k=3.4),
    ortho=dict(kind='ortho', loc=(0, -4.0, (BH / 2 + LIFT) * MM), rot=(90, 0, 0), ortho_scale=CABS['2x12']['ortho_scale_m'], k=3.4))

def _build(mats, mode):
    mats['grille'] = mat_grille(mats['art'], GW, GL, SPEC['ink_a'], SPEC['ink_b'])
    r = build(mats, mode)
    if mode != 'hero':
        # front ortho: the 740x520 cab at 0.84 m frame width needs a 0.6 m tall frame, i.e. 40 mm below the cab; the floor
        # backdrop ends at z=0 (edge-on) so that margin rendered as a black band.  Lift the cab LIFT mm so the whole frame
        # stays above the floor (the camera is lifted by the same amount in SPEC['ortho']).
        for ob in bpy.data.objects:
            if ob.parent is None and ob.type != 'CAMERA':
                ob.location.z += LIFT * MM
    return r
SPEC['build'] = _build

if __name__ == '__main__':
    import sys
    sys.argv = [a for a in sys.argv if a != '--no-mic']
    fd = os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts')
    for i, a in enumerate(sys.argv):
        if a == '--font-dir' and i + 1 < len(sys.argv):
            fd = sys.argv[i + 1]
    C_FONT['path'] = fetch_font(fd)
    run(SPEC)
