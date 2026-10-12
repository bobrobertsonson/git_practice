"""Sawblade hero shot: a road-worn pedalboard carrying the PLAY ALONG deck, TIGHTEN, STOCKHOLM SYNDROME and
BOG BURIAL, patched together with short right-angle-plug cables, on a dark stage floor.

    python board_shot.py --out /path/outside/repo                # board_hero.png, 2400x1200
    python board_shot.py --out /path/outside/repo --scale 40     # 40% preview

Every piece comes from its script's importable `build(mats, origin, rot_z, mode)`; this file only places them,
builds the board, tape, zip ties, jacks, plugs and cables, and owns the camera / lights / world / render settings.
Signal flow (see the cables): deck -> TIGHTEN -> STOCKHOLM SYNDROME -> BOG BURIAL -> out (off the right edge).
Units: millimetres in the helpers, metres in Blender.
"""
import argparse, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy, bmesh
import numpy as np
from mathutils import Vector, Matrix
import common as C
from common import (MM, M, hexcol, empty, lathe, prism, rbox, rrect, finish_bm, new_obj, plane, tube_path, circle_pts,
                    mat_ink, make_standard_mats, parent_new, build_enclosure, setup_render, make_camera, softbox,
                    mat_chrome, mat_black_anodised, fetch_font)
import pedal_b2, pedal_tighten, pedal_fuzz, playalong_deck

# ---------------------------------------------------------------- layout (mm, world; x right, y back, z up)
ZB = 26.0                                   # top of the board
BOARD = dict(cx=-60.0, cy=255.0, w=610.0, l=580.0, t=20.0, feet=6.0)
JACK_Y = 120.0                              # world y of the side jacks of the three pedals
ROT = dict(tighten=1.2, stock=-0.8, fuzz=1.6, deck=-0.6)
X_TIGHT, X_STOCK, X_FUZZ = -248.0, -65.0, 125.0
DECK_C = (-115.0, 382.0)
DECK_JACK = (-183.0, 10.0, 20.0)            # on the deck's left cheek: local (x, y, z)
CABLE_R = 3.0


def rz(v, deg):
    a = math.radians(deg)
    return (v[0] * math.cos(a) - v[1] * math.sin(a), v[0] * math.sin(a) + v[1] * math.cos(a))


# ---------------------------------------------------------------- materials
def mat_floor():
    """matte dark painted plywood / rubber matting: uniform, only fine grain and very faint scuffs."""
    s = M('stage_floor')
    tc = s.n('ShaderNodeTexCoord')
    n2 = s.noise(900, 3, 0.6, (tc, 'Object'))
    n3 = s.noise(60, 2, 0.5, (tc, 'Object'))
    ln = s.n('ShaderNodeVectorMath', operation='LENGTH'); s.l(tc, 'Object', ln, 0)
    vig = s.math('SUBTRACT', 1.0, s.math('DIVIDE', (ln, 'Value'), 1.6), clamp=True)
    vig = s.math('ADD', 0.35, s.math('MULTIPLY', (vig, 'Value'), 0.65))
    scuff = s.math('MULTIPLY', s.math('SUBTRACT', (n2, 'Fac'), 0.60, clamp=True), 0.08)
    base = s.math('MULTIPLY', s.math('ADD', s.math('ADD', 0.016, s.math('MULTIPLY', (n3, 'Fac'), 0.003)), scuff), (vig, 'Value'))
    col = s.n('ShaderNodeCombineXYZ')
    s.l(base, 'Value', col, 'X'); s.l(base, 'Value', col, 'Y'); s.l(s.math('MULTIPLY', (base, 'Value'), 1.05), 'Value', col, 'Z')
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.2; bump.inputs['Distance'].default_value = 0.0004
    s.l(n2, 'Fac', bump, 'Height')
    rr = s.math('ADD', 0.88, s.math('MULTIPLY', s.math('SUBTRACT', (n2, 'Fac'), 0.5), 0.1))
    b = s.bsdf(Specular_IOR_Level=0.12)
    s.l(col, 'Vector', b, 'Base Color'); s.l(rr, 'Value', b, 'Roughness'); s.l(bump, 'Normal', b, 'Normal')
    return s.m


def mat_board():
    """scuffed black-painted plywood: paint chips down to bare wood along edges / randomly, pale rub marks on top."""
    s = M('board_ply')
    tc = s.n('ShaderNodeTexCoord')
    sep = s.n('ShaderNodeSeparateXYZ'); s.l(tc, 'Normal', sep, 'Vector')
    nz = (sep, 'Z')
    edge = s.math('SUBTRACT', 1.0, s.math('MULTIPLY', s.math('SUBTRACT', nz, 0.80, clamp=True), 5.0, clamp=True))   # 1 on sides/bevels
    big = s.noise(14, 3, 0.55, (tc, 'Object'))
    mid = s.noise(160, 4, 0.6, (tc, 'Object'))
    fine = s.noise(1100, 3, 0.6, (tc, 'Object'))
    chip = s.math('ADD', s.math('MULTIPLY', (mid, 'Fac'), 0.55), s.math('MULTIPLY', (fine, 'Fac'), 0.45))
    thr = s.math('SUBTRACT', 0.57, s.math('MULTIPLY', (edge, 'Value'), 0.30))            # edges chip much more easily
    thr = s.math('ADD', (thr, 'Value'), s.math('MULTIPLY', s.math('SUBTRACT', (big, 'Fac'), 0.5), 0.14))
    wood_m = s.math('MULTIPLY', s.math('SUBTRACT', (chip, 'Value'), (thr, 'Value'), clamp=True), 14.0, clamp=True)
    # rub marks: paler, rougher paint
    rub = s.math('MULTIPLY', s.math('SUBTRACT', (fine, 'Fac'), 0.56, clamp=True), 6.0, clamp=True)
    rub = s.math('MULTIPLY', (rub, 'Value'), s.math('SUBTRACT', (big, 'Fac'), 0.42, clamp=True), clamp=True)
    # plywood: layered stripes across the thickness (object z), warm pale wood
    wv = s.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='Z')
    wv.inputs['Scale'].default_value = 330; wv.inputs['Distortion'].default_value = 3.0; wv.inputs['Detail'].default_value = 2.0
    s.l(tc, 'Object', wv, 'Vector')
    wood = s.n('ShaderNodeMix', data_type='RGBA')
    wood.inputs['A'].default_value = hexcol('#5a3b1f'); wood.inputs['B'].default_value = hexcol('#a87b4a')
    s.l(wv, 'Fac', wood, 'Factor')
    paint = s.n('ShaderNodeMix', data_type='RGBA')
    paint.inputs['A'].default_value = (0.010, 0.010, 0.011, 1); paint.inputs['B'].default_value = (0.17, 0.165, 0.155, 1)
    s.l(rub, 'Value', paint, 'Factor')
    out = s.n('ShaderNodeMix', data_type='RGBA')
    s.l(paint, 'Result', out, 'A'); s.l(wood, 'Result', out, 'B'); s.l(wood_m, 'Value', out, 'Factor')
    rough = s.math('ADD', 0.50, s.math('MULTIPLY', s.math('SUBTRACT', (fine, 'Fac'), 0.5), 0.3))
    rough = s.math('ADD', s.math('MULTIPLY', (rough, 'Value'), s.math('SUBTRACT', 1.0, (wood_m, 'Value'))), s.math('MULTIPLY', (wood_m, 'Value'), 0.72))
    rough = s.math('ADD', (rough, 'Value'), s.math('MULTIPLY', (rub, 'Value'), 0.15), clamp=True)
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.35; bump.inputs['Distance'].default_value = 0.0004
    s.l(chip, 'Value', bump, 'Height')
    b = s.bsdf(Specular_IOR_Level=0.3)
    s.l(out, 'Result', b, 'Base Color'); s.l(rough, 'Value', b, 'Roughness'); s.l(bump, 'Normal', b, 'Normal')
    return s.m


def mat_tape(col, name='gaffer'):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    ck = s.n('ShaderNodeTexChecker'); ck.inputs['Scale'].default_value = 1400
    s.l(tc, 'Object', ck, 'Vector')
    n = s.noise(70, 4, 0.55, (tc, 'Object'))
    fine = s.noise(1500, 2, 0.6, (tc, 'Object'))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.35; bump.inputs['Distance'].default_value = 0.0002
    s.l(s.math('ADD', (ck, 'Fac'), (fine, 'Fac')), 'Value', bump, 'Height')
    dirt = s.math('MULTIPLY', s.math('ADD', 0.45, s.math('MULTIPLY', (n, 'Fac'), 0.9)), 1.0)
    mc = s.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY'); mc.inputs['Factor'].default_value = 1.0
    mc.inputs['A'].default_value = col
    cm = s.n('ShaderNodeCombineXYZ')
    for k in 'XYZ': s.l(dirt, 'Value', cm, k)
    s.l(cm, 'Vector', mc, 'B')
    b = s.bsdf(Specular_IOR_Level=0.25, Roughness=0.55, Sheen_Weight=0.5, Sheen_Roughness=0.4)
    s.l(mc, 'Result', b, 'Base Color'); s.l(bump, 'Normal', b, 'Normal')
    return s.m


def mat_cable(col, name='cable', rough=0.45, weave=False):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(500 if weave else 1500, 3, 0.6, (tc, 'Object'))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.5 if weave else 0.12; bump.inputs['Distance'].default_value = 0.0002
    s.l(n, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=col, Roughness=rough, Specular_IOR_Level=0.35)
    s.l(bump, 'Normal', b, 'Normal')
    return s.m


def mat_plain(col, rough=0.5, metal=0.0, name='plain'):
    s = M(name)
    b = s.bsdf(Base_Color=col, Roughness=rough, Metallic=metal)
    return s.m


def mat_psu():
    s = M('psu_case')
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(900, 3, 0.6, (tc, 'Object'))
    big = s.noise(30, 3, 0.55, (tc, 'Object'))
    wear = s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.62, clamp=True), 3.0, clamp=True)
    wear = s.math('MULTIPLY', (wear, 'Value'), s.math('SUBTRACT', (big, 'Fac'), 0.35, clamp=True), clamp=True)
    mc = s.n('ShaderNodeMix', data_type='RGBA'); mc.inputs['A'].default_value = (0.018, 0.019, 0.02, 1)
    mc.inputs['B'].default_value = (0.35, 0.36, 0.37, 1); s.l(wear, 'Value', mc, 'Factor')
    b = s.bsdf(Metallic=0.2, Roughness=0.5)
    s.l(mc, 'Result', b, 'Base Color')
    return s.m


# ---------------------------------------------------------------- generic geometry helpers
def catmull(pts, per=10):
    P = [Vector(p) for p in pts]
    ext = [P[0] * 2 - P[1]] + P + [P[-1] * 2 - P[-2]]
    out = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for k in range(per):
            t = k / per
            q = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3)
            out.append(tuple(q))
    out.append(tuple(P[-1]))
    return out


def jack(parent, side, y, z, wall_x, mats, name='jack'):
    """generic 1/4in side jack on the wall at x = wall_x, protruding outward (side=+1 right, -1 left); mm, parent-local."""
    root = empty(name, wall_x, y, z)
    barrel = lathe('jack_barrel', [(0, -1.0), (4.6, -1.0), (4.6, 3.0), (4.1, 3.4), (0, 3.4)], [mats['chrome_sat']], 40, 40)
    nut = prism('jack_nut', circle_pts(6.8, 6, math.pi / 6), 0.0, 2.2, [mats['chrome']], 40)
    m = nut.modifiers.new('b', 'BEVEL'); m.width = 0.4 * MM; m.segments = 2; m.limit_method = 'ANGLE'
    for p in (barrel, nut):
        p.parent = root
    root.rotation_euler = (0, math.radians(90 * side), 0)
    root.parent = parent
    return root


class Plugs:
    def __init__(self):
        self.m_rub = mat_cable((0.012, 0.012, 0.014, 1), 'plug_boot', 0.42)
        self.m_nickel = mat_chrome('plug_nickel', 0.12)

    def make(self, pos, yaw, ey, name):
        """right-angle plug on a side jack. pos: world mm of the jack wall point, yaw: outward direction angle (deg,
        0 = +x), ey: world sign of the cable exit (+1 back / -1 front).  Returns the cable start point (mm, world)."""
        root = empty(name, *pos)
        root.rotation_euler = (0, 0, math.radians(yaw))
        sleeve = lathe(name + '_sleeve', [(0, 2.0), (4.5, 2.0), (4.7, 3.0), (4.7, 14.0), (5.2, 15.0), (0, 15.0)], [self.m_nickel], 40, 40)
        sleeve.rotation_euler = (0, math.radians(90), 0)
        sleeve.parent = root
        body = rbox(name + '_body', 14.0, 13.6, 13.6, 3.2, [self.m_rub], z0=-6.8, seg=4)
        body.location = (22 * MM, 0, 0); body.parent = root
        bv = body.modifiers.new('b', 'BEVEL'); bv.width = 1.0 * MM; bv.segments = 2; bv.limit_method = 'ANGLE'
        # outward == +x for yaw 0 (right-hand jack); on the left (yaw 180) local +y points world -y
        sgn = ey * (1 if math.cos(math.radians(yaw)) > 0 else -1)
        sr = lathe(name + '_relief', [(0, 0), (4.4, 0), (3.9, 6.0), (3.35, 13.0), (0, 13.0)], [self.m_rub], 32, 40)
        sr.rotation_euler = (math.radians(-90 * sgn), 0, 0)
        sr.location = (22 * MM, sgn * 6.0 * MM, 0); sr.parent = root
        bpy.context.view_layer.update()
        loc = root.matrix_world @ Vector((22 * MM, sgn * 19.0 * MM, 0))
        return (loc.x / MM, loc.y / MM, loc.z / MM)


def cable(name, pts, mat, r=CABLE_R, per=10):
    dense = catmull(pts, per)
    ob = tube_path(name, dense, r, mat, closed=False, ns=10, angle=70)
    return ob, dense


def zip_tie(pos, tangent, mat, r=CABLE_R, name='ziptie'):
    """a zip-tie loop around the cable at `pos` (mm, cable centre) closing through the board; tangent = cable direction."""
    t = Vector(tangent); t.z = 0; t.normalize()
    n = Vector((-t.y, t.x, 0))                            # horizontal, perpendicular to the cable
    rad = r + 0.9
    bm = bmesh.new()
    n_seg = 28; ns = 5; rt = 0.75
    rings = []
    for i in range(n_seg):
        a = 2 * math.pi * i / n_seg
        c = Vector(pos) + n * (rad * math.cos(a)) + Vector((0, 0, rad * math.sin(a)))
        radial = (c - Vector(pos)).normalized()
        ring = []
        for k in range(ns):
            b = 2 * math.pi * k / ns
            q = c + radial * (rt * math.cos(b)) + t * (rt * 1.4 * math.sin(b))
            ring.append(bm.verts.new((q.x * MM, q.y * MM, q.z * MM)))
        rings.append(ring)
    for i in range(n_seg):
        A, B = rings[i], rings[(i + 1) % n_seg]
        for k in range(ns):
            j = (k + 1) % ns
            bm.faces.new((A[k], A[j], B[j], B[k]))
    ring_ob = finish_bm(bm, name + '_loop', [mat], 60)
    # head block + tail
    hd = Vector(pos) + n * rad + Vector((0, 0, 0.5))
    head = rbox(name + '_head', 5.0, 3.6, 4.4, 0.8, [mat], z0=-2.2, seg=2)
    head.location = (hd.x * MM, hd.y * MM, hd.z * MM)
    head.rotation_euler = (0, 0, math.atan2(t.y, t.x))
    tail = rbox(name + '_tail', 16.0, 2.5, 0.9, 0.3, [mat], z0=-0.45, seg=1)
    tail.rotation_euler = (0, math.radians(-22), math.atan2(n.y, n.x))
    tl = hd + n * 9.0 + Vector((0, 0, 3.0))
    tail.location = (tl.x * MM, tl.y * MM, tl.z * MM)
    return [ring_ob, head, tail]


def tape_strip(name, cx, cy, length, width, rot_deg, mat, z=ZB, over=None, r=CABLE_R, seed=0):
    """flat gaffer strip with ragged torn ends; `over` = (x, y) of a cable crossing it, which the tape tents over."""
    rng = np.random.default_rng(seed)
    nx, ny = 60, 6
    verts, faces = [], []
    ca, sa = math.cos(math.radians(rot_deg)), math.sin(math.radians(rot_deg))
    tear = [rng.uniform(0, 2.2) for _ in range(ny + 1)]
    tear2 = [rng.uniform(0, 2.2) for _ in range(ny + 1)]
    for j in range(ny + 1):
        for i in range(nx + 1):
            u = (i / nx - 0.5) * length
            v = (j / ny - 0.5) * width
            u = u * (1 - 2 * tear[j] / length) if i == 0 else (u * (1 - 2 * tear2[j] / length) if i == nx else u)
            if i == 0: u = -length / 2 + tear[j]
            if i == nx: u = length / 2 - tear2[j]
            x, y = cx + u * ca - v * sa, cy + u * sa + v * ca
            h = 0.30
            if over is not None:
                dd = math.hypot(x - over[0], 0)
                if dd < r * 1.25:
                    h = 0.30 + r + math.sqrt(max(0.0, (r * 1.25) ** 2 - dd * dd)) * 0.92
                elif dd < r * 3.2:
                    h = 0.30 + (0.30 + r * 0.6) * (1 - (dd - r * 1.25) / (r * 1.95)) ** 2
            verts.append((x * MM, y * MM, (z + h) * MM))
    for j in range(ny):
        for i in range(nx):
            a = j * (nx + 1) + i
            faces.append((a, a + 1, a + nx + 2, a + nx + 1))
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    for p in me.polygons:
        p.use_smooth = True
    return new_obj(name, me, [mat])


# ---------------------------------------------------------------- the board
def build_board(mat):
    b = BOARD
    plate = build_enclosure(mat, b['w'], b['l'], b['t'], r=14.0, bev=2.6, nb=4, name='board_plate')
    plate.location = (b['cx'] * MM, b['cy'] * MM, b['feet'] * MM)
    rub = mat_plain((0.006, 0.006, 0.006, 1), 0.8, 0, 'rubber_foot')
    for sx in (-1, 1):
        for sy in (-1, 1):
            f = lathe('foot', [(0, 0), (11, 0), (11, b['feet']), (0, b['feet'])], [rub], 24, 40)
            f.location = ((b['cx'] + sx * (b['w'] / 2 - 38)) * MM, (b['cy'] + sy * (b['l'] / 2 - 38)) * MM, 0)
    # brushed aluminium angle along the front edge (generic board trim)
    alu = C.mat_alu_brushed()
    lip = prism('front_trim', [(-b['w'] / 2 + 30, 0), (b['w'] / 2 - 30, 0), (b['w'] / 2 - 30, 3.0), (-b['w'] / 2 + 30, 3.0)], 0, b['t'] + 0.2, [mat_plain((0.55, 0.56, 0.58, 1), 0.34, 1.0, 'trim_alu')], 30)
    lip.location = (b['cx'] * MM, (b['cy'] - b['l'] / 2 - 2.9) * MM, b['feet'] * MM)
    return plate


# ---------------------------------------------------------------- scene
def lights():
    c = Vector((-0.05, 0.24, 0.03))
    k = 2.0
    P = lambda x, y, z: (c.x + x * k, c.y + y * k, c.z + (z - 0.03) * k)
    softbox('key', 0.95 * k, 0.75 * k, P(-0.55, -0.45, 0.60), c, 12.0, (1.0, 0.78, 0.55))
    softbox('rim', 0.14 * k, 0.9 * k, P(0.55, 0.55, 0.28), c, 80.0, (0.45, 0.62, 1.0), gradient=False)
    softbox('rim2', 0.14 * k, 0.9 * k, P(-0.60, 0.45, 0.26), c, 40.0, (1.0, 0.45, 0.25), gradient=False)
    softbox('fill', 0.8 * k, 0.8 * k, P(0.6, -0.5, 0.25), c, 1.3, (0.6, 0.7, 1.0))
    softbox('top', 0.9 * k, 0.9 * k, P(0.0, 0.0, 0.95), c, 0.9, (1, 0.97, 0.92))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=int, default=100)
    ap.add_argument('--samples', type=int, default=64)
    ap.add_argument('--font-dir', default=os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))
    ap.add_argument('--az', type=float, default=-14.0, help='camera azimuth (deg, + = camera to the right of front)')
    ap.add_argument('--el', type=float, default=28.0, help='camera elevation (deg)')
    ap.add_argument('--dist', type=float, default=1.2, help='camera distance (m)')
    ap.add_argument('--lens', type=float, default=45.0)
    ap.add_argument('--fstop', type=float, default=7.0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    font = fetch_font(a.font_dir)

    # ---- pieces: each builds its own materials right after its set_piece_dims (done inside build)
    tight = pedal_tighten.build(make_standard_mats(None, led_rgb=pedal_tighten.LED_RGB, led_off=pedal_tighten.LED_OFF),
                                origin=(X_TIGHT, 75.0, ZB), rot_z=ROT['tighten'], font_dir=a.font_dir)
    stock = pedal_b2.build(make_standard_mats(None), origin=(X_STOCK, 95.0, ZB), rot_z=ROT['stock'], font_dir=a.font_dir)
    fuzz = pedal_fuzz.build(make_standard_mats(None, led_rgb=pedal_fuzz.LED_RGB, led_off=pedal_fuzz.LED_OFF),
                            origin=(X_FUZZ, 82.0, ZB), rot_z=ROT['fuzz'], font_dir=a.font_dir)
    deck = playalong_deck.build(make_standard_mats(None, ink_a=playalong_deck.INK_PINK, ink_b=playalong_deck.INK_BONE),
                                origin=(DECK_C[0], DECK_C[1], ZB), rot_z=ROT['deck'], font_dir=a.font_dir)

    jmats = make_standard_mats(None)
    # generic side jacks on TIGHTEN and STOCKHOLM (BOG BURIAL has its own)
    jack(tight, -1, JACK_Y - 75.0, 21.0, -pedal_tighten.W / 2, jmats, 'tj_in')
    jack(tight, +1, JACK_Y - 75.0, 21.0, +pedal_tighten.W / 2, jmats, 'tj_out')
    jack(stock, -1, JACK_Y - 95.0, 27.0, -pedal_b2.W / 2, jmats, 'sj_in')
    jack(stock, +1, JACK_Y - 95.0, 27.0, +pedal_b2.W / 2, jmats, 'sj_out')
    jack(deck, -1, DECK_JACK[1], DECK_JACK[2], DECK_JACK[0], jmats, 'dj_out')

    def jpos(origin, rot, wall_x, ly, lz):
        x, y = rz((wall_x, ly), rot)
        return (origin[0] + x, origin[1] + y, ZB + lz)

    # jack world points (wall plane + 3.4 mm barrel is inside the plug sleeve)
    J = dict(
        d_out=jpos(DECK_C, ROT['deck'], DECK_JACK[0], DECK_JACK[1], DECK_JACK[2]),
        t_in=jpos((X_TIGHT, 75.0), ROT['tighten'], -pedal_tighten.W / 2, JACK_Y - 75.0, 21.0),
        t_out=jpos((X_TIGHT, 75.0), ROT['tighten'], +pedal_tighten.W / 2, JACK_Y - 75.0, 21.0),
        s_in=jpos((X_STOCK, 95.0), ROT['stock'], -pedal_b2.W / 2, JACK_Y - 95.0, 27.0),
        s_out=jpos((X_STOCK, 95.0), ROT['stock'], +pedal_b2.W / 2, JACK_Y - 95.0, 27.0),
        f_in=jpos((X_FUZZ, 82.0), ROT['fuzz'], -pedal_fuzz.W / 2, 38.0, 27.0),
        f_out=jpos((X_FUZZ, 82.0), ROT['fuzz'], +pedal_fuzz.W / 2, 38.0, 27.0))

    # ---- board, floor
    plate = build_board(mat_board())
    flo = plane('floor', 40000, 40000, [mat_floor()])
    flo.location = (BOARD['cx'] * MM, BOARD['cy'] * MM, 0)

    # ---- plugs
    pl = Plugs()
    p_d = pl.make(J['d_out'], ROT['deck'] + 180, -1, 'plug_deck_out')
    p_ti = pl.make(J['t_in'], ROT['tighten'] + 180, +1, 'plug_t_in')
    p_to = pl.make(J['t_out'], ROT['tighten'], +1, 'plug_t_out')
    p_si = pl.make(J['s_in'], ROT['stock'] + 180, +1, 'plug_s_in')
    p_so = pl.make(J['s_out'], ROT['stock'], -1, 'plug_s_out')
    p_fi = pl.make(J['f_in'], ROT['fuzz'] + 180, -1, 'plug_f_in')
    p_fo = pl.make(J['f_out'], ROT['fuzz'], +1, 'plug_f_out')

    r = CABLE_R
    zr = ZB + r                                           # cable centre height when lying on the board
    m_blk = mat_cable((0.010, 0.010, 0.012, 1), 'cable_black', 0.42)
    m_cloth = mat_cable((0.30, 0.13, 0.04, 1), 'cable_cloth', 0.85, weave=True)
    m_grey = mat_cable((0.07, 0.07, 0.075, 1), 'cable_grey', 0.5)
    ties = mat_plain((0.62, 0.60, 0.54, 1), 0.55, 0, 'ziptie')
    ties_blk = mat_plain((0.012, 0.012, 0.012, 1), 0.5, 0, 'ziptie_blk')
    cables = {}

    # deck -> TIGHTEN: a slack C-loop along the board's left margin
    x0 = p_d[0]
    ya, yb = p_d[1], p_ti[1]
    yy = lambda t: ya + (yb - ya) * t
    pts = [p_d, (p_d[0] - 3, p_d[1] - 8, p_d[2] - 10), (p_d[0] - 6, yy(0.12), zr + 3), (x0 - 12, yy(0.28), zr + 0.2), (x0 - 15, yy(0.5), zr),
           (x0 - 12, yy(0.72), zr), (x0 - 7, yy(0.88), zr + 1.0), (p_ti[0] - 3, p_ti[1] + 8, p_ti[2] - 8), p_ti]
    cables['c1'] = cable('cable_deck_tighten', pts, m_blk)
    # TIGHTEN -> STOCKHOLM: small slack U-loop in the gap, resting on the board
    mx = 0.5 * (p_to[0] + p_si[0])
    pts = [p_to, (p_to[0] + 1, p_to[1] + 10, p_to[2] - 5), (p_to[0] + 4, p_to[1] + 22, zr + 2), (mx - 2, p_to[1] + 36, zr),
           (mx + 5, p_si[1] + 38, zr), (p_si[0] - 4, p_si[1] + 21, zr + 2), (p_si[0] - 1, p_si[1] + 10, p_si[2] - 5), p_si]
    cables['c2'] = cable('cable_tighten_stock', pts, m_cloth)
    # STOCKHOLM -> BOG BURIAL: S-curve toward the front of the gap
    mx = 0.5 * (p_so[0] + p_fi[0])
    pts = [p_so, (p_so[0] + 1, p_so[1] - 10, p_so[2] - 5), (p_so[0] + 6, p_so[1] - 23, zr + 2), (mx - 3, p_so[1] - 34, zr),
           (mx + 8, p_fi[1] - 36, zr), (p_fi[0] - 5, p_fi[1] - 22, zr + 2), (p_fi[0] - 1, p_fi[1] - 10, p_fi[2] - 5), p_fi]
    cables['c3'] = cable('cable_stock_fuzz', pts, m_grey)
    # BOG BURIAL -> out: runs back, then off the right edge and trails away across the floor
    ex = BOARD['cx'] + BOARD['w'] / 2
    pts = [p_fo, (p_fo[0] + 2, p_fo[1] + 11, p_fo[2] - 6), (p_fo[0] + 7, p_fo[1] + 28, zr + 1.5), (p_fo[0] + 15, p_fo[1] + 55, zr),
           (ex - 22, p_fo[1] + 78, zr), (ex - 8, p_fo[1] + 90, zr + 0.5), (ex + 2.5, p_fo[1] + 96, ZB - 3), (ex + 8, p_fo[1] + 106, 14),
           (ex + 34, p_fo[1] + 125, 3), (ex + 80, p_fo[1] + 132, 3), (ex + 120, p_fo[1] + 160, 3)]
    cables['c4'] = cable('cable_out', pts, m_blk)

    # ---- zip ties anchoring the cables to the board
    def tie_at(key, y_target=None, x_target=None, mat=ties, name='tie'):
        dense = cables[key][1]
        best = min(range(1, len(dense) - 1), key=lambda i: abs(dense[i][1] - y_target) if y_target is not None else abs(dense[i][0] - x_target))
        p = dense[best]; q = dense[best + 1]; o = dense[best - 1]
        tg = (q[0] - o[0], q[1] - o[1], 0)
        zip_tie((p[0], p[1], ZB + r), tg, mat, name=name)
        return p
    tie_at('c1', y_target=ya + (yb - ya) * 0.3, name='tie_c1a')
    tie_at('c1', y_target=ya + (yb - ya) * 0.75, mat=ties_blk, name='tie_c1b')
    tie_at('c4', y_target=p_fo[1] + 55, name='tie_c4')

    # ---- gaffer tape
    gaff = mat_tape((0.055, 0.057, 0.06, 1), 'gaffer_black')
    gaff2 = mat_tape((0.20, 0.20, 0.19, 1), 'gaffer_grey')
    bone = mat_tape((0.62, 0.58, 0.46, 1), 'artist_tape')
    ex_c1 = min(cables['c1'][1], key=lambda p: abs(p[1] - yy(0.5)))
    tape_strip('tape_c1', ex_c1[0] - 2, ex_c1[1], 50, 22, 0, gaff, over=(ex_c1[0], ex_c1[1]), seed=3)

    # ---- PSU brick with a cord running off the back edge, a pick
    m_psu = mat_psu()
    psu_c = (150.0, 400.0)
    psu = rbox('psu', 70.0, 150.0, 34.0, 5.0, [m_psu], z0=0, seg=4)
    psu.location = (psu_c[0] * MM, psu_c[1] * MM, ZB * MM)
    psu.rotation_euler = (0, 0, math.radians(4))
    pb = psu.modifiers.new('b', 'BEVEL'); pb.width = 1.4 * MM; pb.segments = 2; pb.limit_method = 'ANGLE'
    lab = rbox('psu_label', 48.0, 70.0, 0.4, 1.0, [mat_plain((0.78, 0.74, 0.62, 1), 0.55, 0, 'psu_label')], z0=34.0, seg=2)
    lab.parent = psu
    led = lathe('psu_led', [(0, 0), (2.6, 0), (2.6, 1.0), (1.6, 2.0), (0, 2.2)], [C.mat_emit('psu_led_on', (0.1, 1.0, 0.15, 1), 5.0)], 24, 40)
    led.location = (0, 62 * MM, 34 * MM); led.parent = psu
    ang = math.radians(4)
    bpts = (psu_c[0] - math.sin(ang) * 75, psu_c[1] + math.cos(ang) * 75)
    ey = BOARD['cy'] + BOARD['l'] / 2
    pts = [(bpts[0], bpts[1], ZB + 12), (bpts[0] - 3, bpts[1] + 12, zr + 3), (bpts[0] - 14, bpts[1] + 32, zr), (bpts[0] - 4, bpts[1] + 55, zr),
           (bpts[0] - 6, ey - 12, zr), (bpts[0] - 8, ey - 2, zr + 0.3), (bpts[0] - 9, ey + 5.5, ZB - 3), (bpts[0] - 10, ey + 10, 16),
           (bpts[0] - 14, ey + 32, 3), (bpts[0] - 40, ey + 80, 3)]
    cables['psu'] = cable('cord_psu', pts, m_blk, r=2.2)
    pick = prism('pick', [(-13, 12), (13, 12), (0, -16)], 0.0, 0.9, [mat_plain((0.55, 0.22, 0.04, 1), 0.35, 0, 'pick_plastic')], 40)
    pick.location = (222 * MM, 38 * MM, ZB * MM); pick.rotation_euler = (0, 0, math.radians(25))
    pk = pick.modifiers.new('b', 'BEVEL'); pk.width = 0.35 * MM; pk.segments = 2

    # ---- world, lights, camera, render
    w = bpy.data.worlds.new('w'); bpy.context.scene.world = w; w.use_nodes = True
    bg = w.node_tree.nodes['Background']
    bg.inputs['Color'].default_value = hexcol('#1a2030'); bg.inputs['Strength'].default_value = 0.06
    lights()
    sc = bpy.context.scene
    setup_render((2400, 1200), a.scale, a.samples, adaptive=True)
    # ---- clearances between pieces (plan view, world AABBs of every mesh vertex in each piece incl. jacks)
    def aabb(root):
        bpy.context.view_layer.update()
        lo = [1e9, 1e9]; hi = [-1e9, -1e9]
        def walk(o):
            if o.type == 'MESH':
                for cc in o.bound_box:
                    q = o.matrix_world @ Vector(cc)
                    lo[0] = min(lo[0], q.x / MM); hi[0] = max(hi[0], q.x / MM); lo[1] = min(lo[1], q.y / MM); hi[1] = max(hi[1], q.y / MM)
            for ch in o.children:
                walk(ch)
        walk(root)
        return lo, hi
    boxes = dict(deck=aabb(deck), tighten=aabb(tight), stockholm=aabb(stock), bog=aabb(fuzz), psu=aabb(psu))
    names = list(boxes)
    min_gap = 1e9
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            (al, ah), (bl, bh) = boxes[names[i]], boxes[names[j]]
            dx = max(0.0, al[0] - bh[0], bl[0] - ah[0]); dy = max(0.0, al[1] - bh[1], bl[1] - ah[1])
            g = math.hypot(dx, dy)
            min_gap = min(min_gap, g)
            print('[gap] %s <-> %s : %.1f mm' % (names[i], names[j], g))
    print('[gap] MIN %.1f mm' % min_gap)

    # ---- camera: fixed angle, auto-fit distance + lens shift so the rig spans ~83% of the width, centred
    from bpy_extras.object_utils import world_to_camera_view
    az, el = math.radians(a.az), math.radians(a.el)
    tgt = Vector((-0.07, 0.225, 0.04))
    d = Vector((math.sin(az) * math.cos(el), -math.cos(az) * math.cos(el), math.sin(el)))
    dist = a.dist
    cam_ob = make_camera(dict(kind='persp', loc=tuple(tgt + d * dist), target=tuple(tgt), lens=a.lens, fstop=a.fstop, k=1.0))
    cam_ob.data.clip_end = 30.0
    bx, by, bw, bl_ = BOARD['cx'], BOARD['cy'], BOARD['w'], BOARD['l']
    fit = [(bx + sx * bw / 2, by + sy * bl_ / 2, z) for sx in (-1, 1) for sy in (-1, 1) for z in (0, ZB)]
    fit += [(DECK_C[0] + sx * 185, DECK_C[1] + 132, ZB + 95) for sx in (-1, 1)]
    fit += [(q[0], q[1], q[2]) for q in cables['c4'][1][-8:]]
    fitv = [Vector((p[0] * MM, p[1] * MM, p[2] * MM)) for p in fit]
    for _ in range(8):
        bpy.context.view_layer.update()
        uv = [world_to_camera_view(sc, cam_ob, p) for p in fitv]
        x0_, x1_ = min(u.x for u in uv), max(u.x for u in uv)
        y0_, y1_ = min(u.y for u in uv), max(u.y for u in uv)
        cam_ob.data.shift_x += ((x0_ + x1_) / 2 - 0.5)
        cam_ob.data.shift_y += ((y0_ + y1_) / 2 - 0.5) * 0.5
        dist *= max((x1_ - x0_) / 0.83, (y1_ - y0_) / 0.88)
        cam_ob.location = tgt + d * dist
    print('[cam] dist %.3f m, rig span x %.2f..%.2f y %.2f..%.2f' % (dist, x0_, x1_, y0_, y1_))
    sc.render.filepath = os.path.join(a.out, 'board_hero.png')
    bpy.ops.render.render(write_still=True)
    print('[render] board_hero done in %.1fs (scale=%d samples=%d)' % (time.time() - t0, a.scale, a.samples))


if __name__ == '__main__':
    main()
