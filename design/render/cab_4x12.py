"""SAWBLADE 4x12 cab (generic, unbranded): black tolex, metal corner caps, piping, stitched seams, recessed side handles,
woven grille cloth with a crust-stencil howling wolf + crescent moon (bone + toxic green) painted onto the cloth,
and a grille-off baffle view with four generic 12-inch drivers and one draggable dynamic mic.
    python cab_4x12.py --mode all --out /path/outside/repo      # hero, ortho (grille on), open (grille off + mic)"""
import math
import numpy as np
import common as C
from common import *

W, L, S = 700.0, 700.0, 3
BW, BD, BH = 760.0, 360.0, 760.0
SPK = [(-180, 180), (180, 180), (-180, -180), (180, -180)]
BAFFLE_Z = -22.0
GRILLE_TOP = -6.0
LOGO = (262.0, -322.0)

def layout():
    return dict(circles=[], rects=[(LOGO[0], LOGO[1], 100, 36)])

def wolf(ctx, d, ox, oy, sc):
    """howling wolf head, profile facing up-right (unit box ~100x105, y up) at ox,oy (mm), sc mm/unit."""
    T = lambda pts: [ctx.P(ox + x * sc, oy + y * sc) for x, y in pts]
    ang = math.radians(50)
    skull = [(48 + 24 * math.cos(t) * math.cos(ang) - 19 * math.sin(t) * math.sin(ang),
              58 + 24 * math.cos(t) * math.sin(ang) + 19 * math.sin(t) * math.cos(ang)) for t in np.linspace(0, 2 * math.pi, 40)]
    d.polygon(T(skull), fill=255)
    d.polygon(T([(53.5, 68), (89, 101), (97, 95), (70.5, 54)]), fill=255)                      # muzzle
    d.ellipse((*ctx.P(ox + 88 * sc, oy + 104 * sc), *ctx.P(ox + 100 * sc, oy + 92 * sc)), fill=255)   # nose
    d.polygon(T([(32, 66), (25, 99), (47, 77)]), fill=255)                                       # ear
    d.polygon(T([(28, 50), (14, 42), (22, 36), (8, 26), (16, 20), (2, 8), (10, 2), (8, 0), (76, 0), (66, 10), (74, 18),
                 (62, 28), (70, 38), (56, 46), (64, 56), (46, 66)]), fill=255)                    # mane / chest fur
    d.polygon(T([(33, 75), (31, 91), (40, 79)]), fill=0)                                         # ear hollow
    d.polygon(T([(53, 69), (62, 74), (60, 70), (54, 65)]), fill=0)                               # eye slit
    d.line(T([(97, 95), (84, 84), (70, 70), (62, 58)]), fill=0, width=int(1.8 * ctx.S))         # mouth line
    d.line(T([(40, 3), (44, 46)]), fill=0, width=int(2.4 * ctx.S))                               # stencil bridge

def motif(ctx):
    rng = ctx.rng
    A, d = ctx.new()
    wolf(ctx, d, -230, -270, 4.6)
    # crescent moon + stars, upper right
    cx, cy = ctx.P(180, 205)
    r = 125 * ctx.S
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    ox, oy = 52 * ctx.S, -22 * ctx.S
    r2 = 108 * ctx.S
    d.ellipse((cx - r2 - ox, cy - r2 - oy, cx + r2 - ox, cy + r2 - oy), fill=0)
    d.line((*ctx.P(120, 320), *ctx.P(150, 100)), fill=0, width=int(2.5 * ctx.S))
    E, de = ctx.new()
    for (sx, sy, sr) in ((-250, 280, 14), (-130, 310, 8), (60, 300, 10), (300, 60, 9), (-300, 120, 7), (260, -240, 9)):
        px, py = ctx.P(sx, sy); R = sr * ctx.S
        de.polygon([(px, py - R), (px + 0.22 * R, py - 0.22 * R), (px + R, py), (px + 0.22 * R, py + 0.22 * R),
                    (px, py + R), (px - 0.22 * R, py + 0.22 * R), (px - R, py), (px - 0.22 * R, py - 0.22 * R)], fill=255)
    return arr_of(A), arr_of(E)

def art(font):
    return make_artwork(font, layout(), W, L, S, motif, words=[("HOWL", -150, -300, 250, 0), ("NO REST", 195, -300, 140, 0)],
                        tapes=[(-250, 330, 120, 22, 4), (270, 300, 90, 20, -6)], seed=41)

def box_obj(name, cx, cy, cz, sx, sy, sz, mat):
    bm = bmesh.new()
    vs = [bm.verts.new(((cx + a * sx / 2) * MM, (cy + b * sy / 2) * MM, (cz + c * sz / 2) * MM)) for c in (-1, 1) for b in (-1, 1) for a in (-1, 1)]
    for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([vs[k] for k in f])
    return finish_bm(bm, name, [mat], angle=40)

def bool_cut(target, cutter):
    m = target.modifiers.new('cut_' + cutter.name, 'BOOLEAN')
    m.operation = 'DIFFERENCE'; m.object = cutter; m.solver = 'EXACT'
    cutter.hide_render = True; cutter.display_type = 'WIRE'

def build(mats, mode):
    tol = mats['leather']
    body = build_enclosure(tol, BW, BD, BH, r=8.0, bev=3.5, nb=5, name='cab_body')
    root = empty('cab_front', 0, -BD / 2, BH / 2)
    root.rotation_euler = (math.pi / 2, 0, 0)
    before = set(bpy.data.objects)
    # front pocket for the baffle / grille frame
    pocket = prism('pocket', rrect(354, 354, 6, 4), -105, 3, [tol])
    bool_cut(body, pocket)
    # recessed side handles
    for sx in (-1, 1):
        rec = box_obj('handle_recess', sx * (BW / 2 - 6), 0, BH - 95, 40, 190, 48, tol)
        bool_cut(body, rec)
        box_obj('handle_strap', sx * (BW / 2 - 16), 0, BH - 95, 6, 150, 16, mats['black'])
        for sy in (-1, 1):
            box_obj('handle_mount', sx * (BW / 2 - 14), sy * 78, BH - 95, 8, 12, 34, mats['chrome_sat'])
    # corner caps, piping, stitched front border
    corner_caps(BW, BD, BH, mats, front=True, size=34.0)
    thread = mat_ink('thread', (0.12, 0.115, 0.10, 1), 0.8)
    for (p0, p1) in (((-330, -BD / 2, BH / 2 + 363), (330, -BD / 2, BH / 2 + 363)), ((-330, -BD / 2, BH / 2 - 363), (330, -BD / 2, BH / 2 - 363)),
                     ((-362, -BD / 2, BH / 2 - 330), (-362, -BD / 2, BH / 2 + 330)), ((362, -BD / 2, BH / 2 - 330), (362, -BD / 2, BH / 2 + 330))):
        stitch_line('stitch', p0, p1, (0, -1, 0), thread)
    piping_loop(374, 374, 10, 1.2, 3.2, mats['black'], 'piping')
    # baffle with four 12" cut-outs + drivers
    baffle = prism('baffle', rrect(352, 352, 4, 4), -40, BAFFLE_Z, [mats['baffle']], angle=40)
    for i, (x, y) in enumerate(SPK):
        cut = prism('hole', [(x + px, y + py) for px, py in circle_pts(141.0, 64)], -60, -10, [mats['black']])
        bool_cut(baffle, cut)
        build_speaker(x, y, BAFFLE_Z, mats)
    if mode != 'open':
        g = build_enclosure(mats['grille'], 700, 700, 4, r=8.0, bev=0.4, nb=3, name='grille_cloth')
        g.location.z = (GRILLE_TOP - 4) * MM
        plate = rbox('logo_plate', 100, 30, 2.2, 3.0, [mats['alu']], z0=GRILLE_TOP - 0.3)
        plate.location = (LOGO[0] * MM, LOGO[1] * MM, 0)
        cu = bpy.data.curves.new('logo_txt', 'FONT'); cu.body = "SAWBLADE 4x12"; cu.size = 6.4 * MM
        cu.align_x = 'CENTER'; cu.align_y = 'CENTER'; cu.extrude = 0.25 * MM
        fp = C_FONT.get('path')
        if fp:
            cu.font = bpy.data.fonts.load(fp)
        cu.materials.append(mats['label_ink'])
        t = new_obj('logo_txt', cu)
        bpy.context.view_layer.update()
        t.scale = (min(1.0, 86 * MM / max(t.dimensions.x, 1e-9)),) * 2 + (1.0,)
        t.location = (LOGO[0] * MM, LOGO[1] * MM, (GRILLE_TOP + 1.95) * MM)
    else:
        # draggable mic: ~1 in off the grille plane, slightly off the dust cap of the upper-left driver, 30 deg off axis
        tip = Vector((-180 + 38, 180 + 6, GRILLE_TOP + 25.4))
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
    # objects authored in world coordinates (caps, stitches, handles) must not be parented: restore them
    for ob in list(bpy.data.objects):
        if ob.parent is root and (ob.name.startswith(('corner_cap', 'stitch', 'handle_'))):
            ob.parent = None
    return None

C_FONT = {}
SPEC = dict(
    name='cab', kind='cab', plate=(W, L, 4.0), art=art, ink_a='#7fd13b', ink_b='#e8e4d8', populate=None, build=None,
    k=4.0, center=(0, 0, 0.38), modes=['hero', 'ortho', 'open'], ortho_res=(1400, 1400),
    fnames={'hero': 'cab_hero_3q.png', 'ortho': 'cab_front_ortho.png', 'open': 'cab_open_ortho.png'},
    hero=dict(kind='persp', loc=(1.15, -2.15, 1.25), target=(0, 0, 0.36), lens=70, k=4.0),
    ortho=dict(kind='ortho', loc=(0, -4.0, 0.38), rot=(90, 0, 0), ortho_scale=0.86, k=4.0))

def _build(mats, mode):
    mats['grille'] = mat_grille(mats['art'], W, L, SPEC['ink_a'], SPEC['ink_b'])
    return build(mats, mode)
SPEC['build'] = _build

if __name__ == '__main__':
    import sys
    # the stencil font is fetched by common.run(); expose its path for the logo plate
    fd = os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts')
    for i, a in enumerate(sys.argv):
        if a == '--font-dir' and i + 1 < len(sys.argv):
            fd = sys.argv[i + 1]
    C_FONT['path'] = fetch_font(fd)
    run(SPEC)
