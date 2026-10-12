"""PLAY ALONG deck - a battered 4-track cassette portastudio as the hardware metaphor for Sawblade's play-along
transport (on-device stem separation; you play over the backing track).  340x262 mm sloped top, cassette well with a
cassette (window + reels), chunky piano-key transport (REW FF STOP PLAY/PAUSE), LOOP A / LOOP B, COUNT-IN slide
switch, BACKING slide fader, GUITAR STEM toggle (GHOST / MUTE), tape-counter OLED, crust-punk stickers.

CLI (renders go OUTSIDE the repo):
    python playalong_deck.py --mode hero|ortho|all --out /path/outside/repo [--scale 40]

Importable builder (no camera / lights / render settings are touched):
    import playalong_deck
    root = playalong_deck.build(mats=None, origin=(x_mm, y_mm, z_mm), rot_z=0.0)
"""
import math, os, zlib
import numpy as np
import common as C
from common import *

PW, PL, PT = 340.0, 262.0, 14.0           # top panel: width, length (along the slope), thickness (mm)
SLOPE = 10.5                              # deg, panel rises toward the back
FRONT_H = 11.0                            # body height under the panel's front edge
FOOT = 3.0
INK_PINK, INK_BONE = '#e23a7c', '#e8e4d8'
S = 8                                     # art px/mm (panel is large; 8 keeps memory sane and is still >1 px per render px)
TAU = math.tau

# ---- layout (panel-local mm, origin at panel centre, +y toward the back)
WELL = dict(x=36.0, y=86.0, w=114.0, h=76.0)
OLED_P = (-106.0, 103.0)
FADER = dict(x=-139.0, y0=-6.0, y1=66.0)
TOGGLE = (-93.0, 36.0)
COUNTIN = (-50.0, 33.0)
LOOP_A, LOOP_B, LOOP_Y = (-112.0, -28.0), (-74.0, -28.0), -28.0
KEYS = [("REW", -128.0, 40.0), ("FF", -82.0, 40.0), ("STOP", -36.0, 40.0), ("PLAY/PAUSE", 24.0, 70.0)]
KEY_Y, KEY_L = -90.0, 46.0
SCREWS = [(sx * 160.0, sy * 121.0) for sx in (-1, 1) for sy in (-1, 1)] + [(-160.0, 8.0), (160.0, 8.0)]


def layout():
    circles = [(x, y, 5.0) for x, y in SCREWS]
    circles += [(TOGGLE[0], TOGGLE[1], 13.0), (LOOP_A[0], LOOP_Y + 16, 5.0), (LOOP_B[0] + 0.0, LOOP_Y + 16, 5.0)]
    rects = [(WELL['x'], WELL['y'], WELL['w'] + 6, WELL['h'] + 6), (OLED_P[0], OLED_P[1], 74, 29),
             (FADER['x'], (FADER['y0'] + FADER['y1']) / 2 + 2, 40, FADER['y1'] - FADER['y0'] + 34),
             (COUNTIN[0], COUNTIN[1] + 4, 36, 28), (LOOP_A[0], LOOP_Y, 32, 24), (LOOP_B[0], LOOP_Y, 32, 24),
             (TOGGLE[0], TOGGLE[1], 34, 46)]
    rects += [(x, KEY_Y - 3, w + 4, KEY_L + 18) for _, x, w in KEYS]
    return dict(circles=circles, rects=rects)


# ---------------------------------------------------------------- artwork (panel)
def motif(ctx):
    """pink: dashed section lines, sprocket strip, reel mark; bone: the tape-skull (reel eyes) and cut details. (The unspooled tape is a real 3D strand, see build_tape_strand.)"""
    rng = ctx.rng; S_ = ctx.S; P = ctx.P
    A, da = ctx.new(); E, de = ctx.new()
    lw = lambda mm: max(1, int(mm * S_))
    cx, cy = 42.0, 4.0                                           # skull centre
    # cranium + cheeks + jaw (bone)
    half = [(0, 30.0), (10, 29.2), (17.5, 25), (21.5, 17.5), (21.8, 9), (19.0, 3.5), (18.5, -3), (15.5, -8.5),
            (11.5, -13.5), (0, -15.0)]
    jit = lambda v: v + rng.uniform(-0.4, 0.4)
    pts = [P(cx + jit(x), cy + jit(y)) for x, y in half] + [P(cx - jit(x), cy + jit(y)) for x, y in reversed(half)]
    de.polygon(pts, fill=255)
    # reel eyes: ring + hub with 6 spokes cut out
    for sx in (-1, 1):
        ex, ey = cx + sx * 9.8, cy + 10.5
        c = P(ex, ey)
        de.ellipse((c[0] - 8.3 * S_, c[1] - 8.3 * S_, c[0] + 8.3 * S_, c[1] + 8.3 * S_), fill=0)
        de.ellipse((c[0] - 6.4 * S_, c[1] - 6.4 * S_, c[0] + 6.4 * S_, c[1] + 6.4 * S_), fill=255)
        de.ellipse((c[0] - 4.6 * S_, c[1] - 4.6 * S_, c[0] + 4.6 * S_, c[1] + 4.6 * S_), fill=0)
        for k in range(6):
            a = k * TAU / 6 + (0.3 if sx > 0 else 0.0)
            de.line((c[0], c[1], c[0] + 4.7 * S_ * math.cos(a), c[1] + 4.7 * S_ * math.sin(a)), fill=255, width=lw(1.0))
        de.ellipse((c[0] - 1.5 * S_, c[1] - 1.5 * S_, c[0] + 1.5 * S_, c[1] + 1.5 * S_), fill=255)
    # nose + teeth block with cuts
    de.polygon([P(cx - 2.8, cy - 0.5), P(cx + 2.8, cy - 0.5), P(cx + 0.7, cy + 4.6), P(cx - 0.7, cy + 4.6)], fill=0)
    de.rectangle((*P(cx - 12.5, cy - 6.5), *P(cx + 12.5, cy - 13.4)), fill=255)
    for x in np.arange(-10.5, 10.6, 2.6):
        de.line((*P(cx + x, cy - 6.0), *P(cx + x, cy - 13.8)), fill=0, width=lw(0.55))
    de.line((*P(cx - 12.5, cy - 9.9), *P(cx + 12.5, cy - 9.9)), fill=0, width=lw(0.5))
    for pts_ in (((cx + 1, cy + 30), (cx - 1, cy + 24), (cx + 1.5, cy + 20)), ((cx - 14, cy + 25), (cx - 11, cy + 21))):
        de.line([P(x, y) for x, y in pts_], fill=0, width=lw(0.8), joint='curve')
    # unspooling tape: wide ribbons with sprocket dashes streaming off the eyes (pink), long wavy runs to the edges
    def ribbon(p0, p1, p2, p3, w, n=90, flutter=1.6, ph=0.0, hole=True):
        pts_l, pts_r = [], []
        for i in range(n + 1):
            t = i / n
            bx = (1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t ** 2 * p2[0] + t ** 3 * p3[0]
            by = (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t ** 2 * p2[1] + t ** 3 * p3[1]
            dx = 3 * (1 - t) ** 2 * (p1[0] - p0[0]) + 6 * (1 - t) * t * (p2[0] - p1[0]) + 3 * t ** 2 * (p3[0] - p2[0])
            dy = 3 * (1 - t) ** 2 * (p1[1] - p0[1]) + 6 * (1 - t) * t * (p2[1] - p1[1]) + 3 * t ** 2 * (p3[1] - p2[1])
            ln = math.hypot(dx, dy) + 1e-9
            nx, ny = -dy / ln, dx / ln
            ww = w * (0.55 + 0.45 * abs(math.sin(t * 5 + ph))) * (1 - 0.5 * t ** 3)      # twisting tape = changing apparent width
            wob = flutter * math.sin(t * 17 + ph) * (t)
            pts_l.append(P(bx + nx * (ww / 2 + wob), by + ny * (ww / 2 + wob)))
            pts_r.append(P(bx - nx * (ww / 2 - wob), by - ny * (ww / 2 - wob)))
        da.polygon(pts_l + pts_r[::-1], fill=255)
        if hole:
            for i in range(4, n - 6, 5):
                a, b = pts_l[i], pts_r[i]
                mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
                da.ellipse((mx - 0.5 * S_, my - 0.5 * S_, mx + 0.5 * S_, my + 0.5 * S_), fill=0)
    # sprocket-hole strip border under the wordmark
    for xx in np.arange(-150, 150, 4.2):
        c = P(xx, -55.5)
        da.rectangle((c[0] - 1.1 * S_, c[1] - 0.75 * S_, c[0] + 1.1 * S_, c[1] + 0.75 * S_), fill=255)
    # hand-painted group brackets (like printed portastudio section boxes), broken where labels sit
    def bracket(x0, y0, x1, y1, gaps=()):
        pc = lambda x, y: P(x, y)
        segs = [((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))]
        for (a, b) in segs:
            for k in range(14):
                t0, t1 = k / 14, (k + 0.82) / 14
                p0 = (a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0)
                p1 = (a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1)
                if any(gx0 < p0[0] < gx1 and gy0 < p0[1] < gy1 for (gx0, gy0, gx1, gy1) in gaps):
                    continue
                da.line((*pc(*p0), *pc(*p1)), fill=255, width=lw(0.7))
    bracket(-163, -12, -30, 84, gaps=[(-175, 76, -20, 90)])
    bracket(-163, -121, 63, -66)
    # tape reels, cut-circle pattern, bottom-left corner flourish
    for (rx, ry, rr) in ((-150, -62, 7.5),):
        c = P(rx, ry)
        da.ellipse((c[0] - rr * S_, c[1] - rr * S_, c[0] + rr * S_, c[1] + rr * S_), fill=255)
        da.ellipse((c[0] - rr * 0.62 * S_, c[1] - rr * 0.62 * S_, c[0] + rr * 0.62 * S_, c[1] + rr * 0.62 * S_), fill=0)
        for k in range(6):
            a = k * TAU / 6
            da.line((c[0], c[1], c[0] + rr * 0.7 * S_ * math.cos(a), c[1] + rr * 0.7 * S_ * math.sin(a)), fill=255, width=lw(0.9))
    return arr_of(A), arr_of(E)


def art(font):
    return make_artwork(font, layout(), PW, PL, S, motif,
                        words=[("PLAY ALONG", 16, -39.0, 126, -1.5)],
                        tapes=[(153, -14, 30, 7, 84), (-148, -60, 36, 7, -20), (-30, 122, 36, 7, -7)],
                        tally=[(-60, -62), (-50, -62), (-40, -62)], seed=41)


# ---------------------------------------------------------------- materials
def mat_plastic_worn(name, base, wear_col, rough=0.5, wear=1.0):
    """moulded ABS: bevel-node edge wear (paint/colour rubbed off at every edge), scuff noise, dirt."""
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    bev = s.n('ShaderNodeBevel'); bev.inputs['Radius'].default_value = 0.0011; bev.samples = 6
    geo = s.n('ShaderNodeNewGeometry')
    dot = s.n('ShaderNodeVectorMath', operation='DOT_PRODUCT')
    s.l(bev, 'Normal', dot, 0); s.l(geo, 'Normal', dot, 1)
    edge = s.ramp((s.math('SUBTRACT', 1.0, (dot, 'Value'), clamp=True), 'Value'), ((0.002, 0.0), (0.05, 1.0)))
    n_big = s.noise(14, 4, 0.6, (tc, 'Object'))
    n_fine = s.noise(900, 3, 0.6, (tc, 'Object'))
    n_scr = s.noise(260, 2, 0.7, (tc, 'Object'))
    scuff = s.ramp((s.math('MULTIPLY', (n_scr, 'Fac'), s.math('ADD', 0.35, (n_big, 'Fac'))), 'Value'), ((0.52, 0.0), (0.70, 1.0)))
    w = s.math('MAXIMUM', s.math('MULTIPLY', (edge, 'Color'), 1.4 * wear, clamp=True), s.math('MULTIPLY', (scuff, 'Color'), 0.30 * wear))
    w = s.math('MULTIPLY', (w, 'Value'), s.math('ADD', 0.65, s.math('MULTIPLY', (n_fine, 'Fac'), 0.5)), clamp=True)
    mc = s.n('ShaderNodeMix', data_type='RGBA')
    mc.inputs['A'].default_value = base; mc.inputs['B'].default_value = wear_col
    s.l(w, 'Value', mc, 'Factor')
    dirt = s.math('MULTIPLY', s.math('SUBTRACT', (n_big, 'Fac'), 0.2, clamp=True), 0.35)
    dm = s.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY')
    dm.inputs['Factor'].default_value = 1.0
    s.l(mc, 'Result', dm, 'A')
    dv = s.n('ShaderNodeCombineXYZ')
    inv = s.math('SUBTRACT', 1.0, (dirt, 'Value'))
    for sk in 'XYZ': s.l(inv, 'Value', dv, sk)
    s.l(dv, 'Vector', dm, 'B')
    rr = s.math('ADD', rough, s.math('MULTIPLY', s.math('SUBTRACT', (n_fine, 'Fac'), 0.5), 0.3))
    rr = s.math('SUBTRACT', (rr, 'Value'), s.math('MULTIPLY', (w, 'Value'), 0.12))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.25; bump.inputs['Distance'].default_value = 0.0003
    s.l(n_fine, 'Fac', bump, 'Height')
    b = s.bsdf(Specular_IOR_Level=0.35)
    s.l(dm, 'Result', b, 'Base Color'); s.l(rr, 'Value', b, 'Roughness'); s.l(bump, 'Normal', b, 'Normal')
    return s.m


def make_deck_mats(mats):
    """adds the deck-specific materials to a standard-mats dict (in place)."""
    mats.setdefault('body', mat_plastic_worn('deck_body', hexcol('#17181a'), hexcol('#6c6c68'), 0.55))
    mats.setdefault('key_dark', mat_plastic_worn('key_dark', hexcol('#242527'), hexcol('#77756e'), 0.42, 0.5))
    mats.setdefault('key_bone', mat_plastic_worn('key_bone', hexcol('#c9c19b'), hexcol('#e6dfc2'), 0.42))
    mats.setdefault('key_pink', mat_plastic_worn('key_pink', hexcol('#c42a68'), hexcol('#e8a0b8'), 0.38))
    mats.setdefault('btn_bone', mat_plastic_worn('btn_bone', hexcol('#bdb694'), hexcol('#e6dfc2'), 0.4))
    mats.setdefault('shell', mat_plastic_worn('cass_shell', hexcol('#2b2621'), hexcol('#5a5148'), 0.28, 0.4))
    mats.setdefault('hub', mat_plastic_worn('cass_hub', hexcol('#d8d4c4'), hexcol('#ffffff'), 0.4, 0.2))
    mats.setdefault('print', mat_ink('print_bone', hexcol('#dedbc4'), 0.5))
    mats.setdefault('print_pink', mat_ink('print_pink', hexcol('#e04c86'), 0.5))
    tp = M('tape_pack')
    tc = tp.n('ShaderNodeTexCoord')
    wv = tp.n('ShaderNodeTexWave', wave_type='RINGS', rings_direction='Z')
    wv.inputs['Scale'].default_value = 420; wv.inputs['Distortion'].default_value = 1.2
    tp.l(tc, 'Object', wv, 'Vector')
    nn = tp.noise(600, 3, 0.6, (tc, 'Object'))
    bm_ = tp.n('ShaderNodeBump'); bm_.inputs['Strength'].default_value = 0.6; bm_.inputs['Distance'].default_value = 0.00012
    tp.l(wv, 'Fac', bm_, 'Height')
    cm = tp.n('ShaderNodeMix', data_type='RGBA')
    cm.inputs['A'].default_value = hexcol('#4a2810'); cm.inputs['B'].default_value = hexcol('#7a4824')
    tp.l(wv, 'Fac', cm, 'Factor')
    bs = tp.bsdf(Roughness=0.26, Metallic=0.2, Specular_IOR_Level=0.7)
    tp.l(cm, 'Result', bs, 'Base Color'); tp.l(bm_, 'Normal', bs, 'Normal')
    mats.setdefault('tape_pack', tp.m)
    ts = M('tape_strand')
    tc = ts.n('ShaderNodeTexCoord')
    nn = ts.noise(900, 3, 0.6, (tc, 'Object'))
    bs = ts.bsdf(Base_Color=hexcol('#3d2210'), Roughness=0.16, Metallic=0.25, Specular_IOR_Level=0.8, Coat_Weight=0.35, Coat_Roughness=0.12)
    bm_ = ts.n('ShaderNodeBump'); bm_.inputs['Strength'].default_value = 0.15; bm_.inputs['Distance'].default_value = 0.00005
    ts.l(nn, 'Fac', bm_, 'Height'); ts.l(bm_, 'Normal', bs, 'Normal')
    mats.setdefault('tape_strand', ts.m)
    mats.setdefault('cheek', mat_plastic_worn('deck_cheek', hexcol('#6e6650'), hexcol('#b4ac90'), 0.5, 1.2))
    mats.setdefault('well', mat_black_anodised('well_floor'))
    return mats


# ---------------------------------------------------------------- parts
def _box(name, w, d, h, r, mat, x, y, z0, bev=None, rz=0.0):
    ob = rbox(name, w, d, h, min(r, w / 2 - 0.05, d / 2 - 0.05), [mat], z0=0.0, seg=4)
    ob.location = (x * MM, y * MM, z0 * MM)
    ob.rotation_euler = (0, 0, math.radians(rz))
    if bev:
        m = ob.modifiers.new('b', 'BEVEL'); m.width = bev * MM; m.segments = 4; m.limit_method = 'ANGLE'
    return ob

def _flat(name, pts, x, y, z0, z1, mat, rz=0.0):
    ob = prism(name, pts, 0.0, z1 - z0, [mat], 60)
    ob.location = (x * MM, y * MM, z0 * MM); ob.rotation_euler = (0, 0, math.radians(rz))
    return ob

def _text(body, size, x, y, mat, z=None, **kw):
    t = add_text(body, size, x, y, mat, **kw)
    if z is not None:
        t.location.z = z * MM
    return t

def bake_booleans(ob):
    """apply the object's modifiers into real geometry and keep large planar faces flat-shaded: the powder-coat
    shader keys the ink on the (interpolated) object-space normal, which a triangulated boolean top face would bend."""
    bpy.context.view_layer.update()
    ev = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    me = bpy.data.meshes.new_from_object(ev)
    cutters = [m.object for m in ob.modifiers if m.type == 'BOOLEAN' and m.object is not None]
    ob.modifiers.clear()
    ob.data = me
    for p in me.polygons:
        if abs(p.normal.z) > 0.99995:
            p.use_smooth = False
    for c in cutters:
        bpy.data.objects.remove(c, do_unlink=True)
    return ob

def transport_key(label, x, w, mats, pressed=0.0):
    KH = 10.0
    _box('key_bezel_' + label, w + 4.0, KEY_L + 4.0, 1.0, 2.0, mats['black'], x, KEY_Y, PT)
    mat = mats['key_bone'] if label.startswith('PLAY') else mats['key_dark']
    z0 = PT + 1.0 - pressed
    _box('key_' + label, w, KEY_L, KH, 2.4, mat, x, KEY_Y, z0, bev=2.0)
    zt = z0 + KH + 0.02
    ink = mats['print']; ink_dark = mats['black']
    ic = mats['print'] if not label.startswith('PLAY') else mats['black']
    iy = KEY_Y + 7.0
    tri = lambda sx, ox: [(ox + sx * -4.5, -6.0), (ox + sx * -4.5, 6.0), (ox + sx * 5.5, 0.0)]
    def icon(pts, ox=0.0):
        _flat('icon_' + label, [(px, py) for px, py in pts], x, iy, zt, zt + 0.25, ic)
    if label == 'REW':
        icon([(px, py) for px, py in tri(-1, 5.0)]); icon([(px, py) for px, py in tri(-1, -5.0)])
    elif label == 'FF':
        icon(tri(1, 4.5)); icon(tri(1, -6.5))
    elif label == 'STOP':
        icon([(-5.5, -5.5), (5.5, -5.5), (5.5, 5.5), (-5.5, 5.5)])
    else:
        icon([(-17, -6), (-17, 6), (-6, 0)])
        icon([(4, -6), (4, 6), (7.5, 6), (7.5, -6)]); icon([(11, -6), (11, 6), (14.5, 6), (14.5, -6)])
    # finger ridges at the front of the key
    for k in range(4):
        _flat('ridge', [(-w / 2 + 6, 0), (w / 2 - 6, 0), (w / 2 - 6, 0.9), (-w / 2 + 6, 0.9)], x, KEY_Y - KEY_L / 2 + 4.5 + k * 2.2, zt - 0.05, zt + 0.12, mats['black'])
    _text(label if label != 'PLAY/PAUSE' else 'PLAY / PAUSE', 3.4, x, KEY_Y - KEY_L / 2 - 8.5, mats['print'], embolden=0.03)

def slide_fader(mats):
    x, y0, y1 = FADER['x'], FADER['y0'], FADER['y1']
    ln = y1 - y0; yc = (y0 + y1) / 2
    _box('fader_plate', 12.0, ln + 14, 0.6, 1.4, mats['chrome_dark'], x, yc, PT)
    _box('fader_slot', 3.2, ln + 6, 0.9, 1.4, mats['black'], x, yc, PT + 0.05)
    val = 0.72
    cy = y0 + val * ln
    _box('fader_cap', 11.0, 8.0, 8.5, 1.6, mats['key_dark'], x, cy, PT + 0.7, bev=1.1)
    _flat('fader_mark', [(-4.6, -0.5), (4.6, -0.5), (4.6, 0.5), (-4.6, 0.5)], x, cy, PT + 9.18, PT + 9.45, mats['print'])
    for k in (-2.4, 2.4):
        _flat('fader_grip', [(-4.6, -0.3), (4.6, -0.3), (4.6, 0.3), (-4.6, 0.3)], x, cy + k, PT + 9.18, PT + 9.35, mats['black'])
    for i in range(11):                                   # scale ticks 0..10
        yy = y0 + ln * i / 10
        wdt = 3.6 if i % 5 == 0 else 2.2
        _flat('tick', [(0, -0.22), (wdt, -0.22), (wdt, 0.22), (0, 0.22)], x + 8.0, yy, PT, PT + 0.18, mats['print'])
    for v, yy in (("0", y0), ("5", y0 + ln * 0.5), ("10", y1)):
        _text(v, 2.9, x + 15.0, yy, mats['print'], embolden=0.03)
    _text("BACKING", 4.2, x, y1 + 18.5, mats['print'], embolden=0.04)
    _text("LEVEL", 3.0, x, y1 + 12.5, mats['print'], embolden=0.03)

def slide_switch(x, y, mats, on=True):
    _box('cnt_plate', 34.0, 11.0, 0.6, 2.0, mats['chrome_dark'], x, y, PT)
    _box('cnt_slot', 22.0, 3.4, 0.9, 1.4, mats['black'], x, y, PT + 0.05)
    cx = x + (6.0 if on else -6.0)
    _box('cnt_cap', 9.0, 6.5, 5.5, 1.4, mats['key_dark'], cx, y, PT + 0.7, bev=0.9)
    _flat('cnt_mark', [(-0.4, -2.8), (0.4, -2.8), (0.4, 2.8), (-0.4, 2.8)], cx, y, PT + 6.18, PT + 6.4, mats['print'])
    _text("COUNT-IN", 3.8, x, y + 11.0, mats['print'], embolden=0.04)
    _text("OFF", 2.6, x - 11.0, y - 8.2, mats['print'], embolden=0.03)
    _text("ON", 2.6, x + 11.0, y - 8.2, mats['print'], embolden=0.03)

def loop_button(x, y, name, cap, lit, mats):
    _box('btn_bezel_' + name, 32.0, 24.0, 1.0, 3.0, mats['black'], x, y, PT)
    _box('btn_' + name, 26.0, 18.0, 7.0, 2.5, cap, x, y, PT + 1.0, bev=1.6)
    _text(name, 4.0, x, y - 17.5, mats['print'], embolden=0.04)
    build_led(x, y + 16.0, lit, mats)

def build_cassette(cx, cy, mats, label_img_mat):
    """compact cassette lying in the well: smoked shell, tape window with two hubs and tape packs, label, screws."""
    z0 = 0.8
    CW, CH, ZB, ZT = 100.4, 63.5, 6.2, 6.2
    shell = _box('cass_shell', CW, CH, ZB + ZT, 3.0, mats['shell'], cx, cy, z0)
    cut = _box('cass_window_cut', 60.0, 17.0, 20.0, 3.0, mats['black'], cx, cy + 9.0, z0 + 1.1)
    bm = shell.modifiers.new('win', 'BOOLEAN'); bm.operation = 'DIFFERENCE'; bm.object = cut; bm.solver = 'EXACT'
    bake_booleans(shell)
    ztop = z0 + ZB + ZT
    for sx, rpk in ((-1, 16.5), (1, 9.6)):                 # oxide tape packs: left reel nearly full, right nearly empty
        hx, hy = cx + sx * 19.0, cy + 9.0
        zt_ = z0 + 8.2
        pack = lathe('tape_pack', [(5.8, z0 + 1.2), (rpk, z0 + 1.2), (rpk, zt_ - 0.25), (rpk - 0.25, zt_), (5.8, zt_)], [mats['tape_pack']], 96, 30)
        pack.location = (hx * MM, hy * MM, 0)
        hub = lathe('hub', [(2.9, z0 + 1.0), (6.1, z0 + 1.0), (6.1, z0 + 9.0), (5.5, z0 + 9.0), (5.5, z0 + 1.6), (2.9, z0 + 1.6)], [mats['hub']], 36, 40)
        hub.location = (hx * MM, hy * MM, 0)
        for k in range(6):                                   # hub teeth
            a = k * TAU / 6
            tooth = _flat('hub_tooth', [(-0.55, 2.8), (0.55, 2.8), (0.55, 3.9), (-0.55, 3.9)], hx, hy, z0 + 1.4, z0 + 8.8, mats['hub'], rz=math.degrees(a))
    # printed paper label over the lower half
    lab = build_sticker('cass_label', 82.0, 28.0, label_img_mat, cx, cy - 14.0, ztop + 0.1, rot=0.0, n=4)
    for (sx, sy) in ((-46.0, 28.0), (46.0, 28.0), (-46.0, -28.0), (46.0, -28.0), (0, -28.0)):
        scr = lathe('cass_screw', [(0, 0), (1.9, 0), (1.9, 0.5), (0, 0.5)], [mats['chrome_sat']], 16, 40)
        scr.location = ((cx + sx * 0.97) * MM, (cy + sy) * MM, (ztop - 0.08) * MM)
    # head openings along the bottom edge
    for k, xx in enumerate((-30, -16, 0, 16, 30)):
        _box('cass_slot', 8.0 if k != 2 else 12.0, 3.0, 0.3, 0.9, mats['black'], cx + xx, cy - CH / 2 + 3.0, ztop + 0.0)

def build_tape_strand(mats, width=4.0):
    """loose unspooled cassette tape: a thin glossy oxide ribbon that climbs out of the cassette window, wanders over the
    panel, loops and tangles around the skull.  Real geometry (flat strip with a travelling twist); panel-local mm."""
    # (x, y, lift above the panel top)
    pts = [(17, 95, -3.0), (9, 99, 4.0), (-8, 101, 5.5), (-24, 95, 3.2), (-30, 82, 0.8), (-33, 66, 0.4), (-24, 52, 0.6),
           (-8, 45, 1.4), (4, 40, 1.4), (-6, 34, 2.6), (-12, 24, 1.4), (-6, 15, 0.6), (5, 16, 2.2), (10, 27, 3.6),
           (22, 36, 1.8), (38, 38, 1.0), (52, 32, 0.5), (62, 18, 0.6), (63, 4, 2.4), (54, -6, 2.8), (46, -2, 1.4),
           (58, 6, 0.8), (72, -8, 0.5), (84, -13, 0.4), (96, -9, 0.4)]
    P = np.array(pts, dtype=float)
    def cr(p0, p1, p2, p3, u):
        return 0.5 * ((2 * p1) + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u ** 2 + (-p0 + 3 * p1 - 3 * p2 + p3) * u ** 3)
    ext = np.vstack([2 * P[0] - P[1], P, 2 * P[-1] - P[-2]])
    C = []
    for i in range(1, len(ext) - 2):
        for u in np.linspace(0, 1, 28, endpoint=False):
            C.append(cr(ext[i - 1], ext[i], ext[i + 1], ext[i + 2], u))
    C.append(P[-1])
    C = np.array(C)
    n = len(C)
    d = np.linalg.norm(np.diff(C[:, :2], axis=0), axis=1)
    sarc = np.concatenate([[0], np.cumsum(d)])
    verts, faces = [], []
    for i in range(n):
        j0, j1 = max(0, i - 1), min(n - 1, i + 1)
        T = np.array([C[j1][0] - C[j0][0], C[j1][1] - C[j0][1], 0.0])
        T /= np.linalg.norm(T) + 1e-9
        S_ = np.array([-T[1], T[0], 0.0])                              # lateral, horizontal
        phi = 1.15 * math.sin(sarc[i] * 0.055 + 0.5) + 0.7 * math.sin(sarc[i] * 0.021 + 2.0)   # slow flat <-> edge-on twist
        wv = S_ * math.cos(phi) + np.array([0, 0, 1.0]) * math.sin(phi)
        zc_ = PT + C[i][2] + width / 2 * abs(math.sin(phi)) + 0.12
        c = np.array([C[i][0], C[i][1], zc_])
        for sgn in (-1, 1):
            q = c + wv * (width / 2) * sgn
            verts.append((q[0] * MM, q[1] * MM, q[2] * MM))
    for i in range(n - 1):
        a, b = 2 * i, 2 * i + 2
        faces.append((a, a + 1, b + 1, b))
    me = bpy.data.meshes.new('tape_strand')
    me.from_pydata(verts, [], faces)
    for p in me.polygons:
        p.use_smooth = True
    ob = new_obj('tape_strand', me, [mats['tape_strand']])
    return ob

def build_oled_deck(x, y, mats, lines):
    oimg = oled_image(lines)
    m = dict(mats); m['oled'] = mat_oled_display(oimg, oimg.size[0], 28)
    build_oled(m, x, y, 1.1)

def build_body(mats):
    """sloped worn-ABS wedge under the panel: side profile extruded across the width."""
    t = math.tan(math.radians(SLOPE))
    zc = FOOT + FRONT_H + 131.5 * t
    hx = PW / 2 - 1.5
    yf, yb = -129.5, 129.5
    prof = [(yf, FOOT), (yb, FOOT), (yb, zc + yb * t + 0.4), (yf, zc + yf * t + 0.4)]
    bm = bmesh.new()
    L_ = [bm.verts.new((-hx * MM, y * MM, z * MM)) for y, z in prof]
    R_ = [bm.verts.new((hx * MM, y * MM, z * MM)) for y, z in prof]
    n = len(prof)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((L_[i], L_[j], R_[j], R_[i]))
    bm.faces.new(L_[::-1]); bm.faces.new(R_)
    body = finish_bm(bm, 'deck_body', [mats['body']], angle=40)
    m = body.modifiers.new('b', 'BEVEL'); m.width = 2.2 * MM; m.segments = 3; m.limit_method = 'ANGLE'
    # stepped end cheeks in contrasting worn bone plastic: they stand proud of the sloped panel and carry the wedge profile
    th = math.radians(SLOPE); sn, cs = math.sin(th), math.cos(th)
    hp = lambda y: zc + (y + PT * sn) * t + PT * cs               # panel top surface height at world y
    cprof = [(yf - 2.0, FOOT), (yb + 2.0, FOOT), (yb + 2.0, hp(yb) + 5.5), (-8.0, hp(-8.0) + 5.5),
             (-8.0, hp(-8.0) + 2.5), (-60.0, hp(-60.0) + 2.5), (-60.0, hp(-60.0) - 1.0), (yf - 2.0, hp(yf) - 1.0)]
    for sx in (-1, 1):
        x0, x1 = sx * (PW / 2 + 0.3), sx * (PW / 2 + 13.0)
        cb = bmesh.new()
        A_ = [cb.verts.new((x0 * MM, y * MM, z * MM)) for y, z in cprof]
        B_ = [cb.verts.new((x1 * MM, y * MM, z * MM)) for y, z in cprof]
        for i in range(len(cprof)):
            j = (i + 1) % len(cprof)
            cb.faces.new((A_[i], A_[j], B_[j], B_[i]))
        cb.faces.new(A_[::-1]); cb.faces.new(B_)
        ch = finish_bm(cb, 'deck_cheek', [mats['cheek']], angle=40)
        cm_ = ch.modifiers.new('b', 'BEVEL'); cm_.width = 1.6 * MM; cm_.segments = 3; cm_.limit_method = 'ANGLE'
        for (yy, dz) in ((90.0, 4.0), (-100.0, -6.0)):                 # chrome screws on the cheek faces
            sc = lathe('cheek_screw', [(0, 0), (4.2, 0), (4.2, 0.8), (3.4, 1.4), (0, 1.5)], [mats['chrome_sat']], 24, 40)
            sc.rotation_euler = (0, math.radians(90 * sx), 0)
            sc.location = (x1 * MM, yy * MM, (hp(yy) - 12.0 + (6 if yy > 0 else 0)) * MM)
    # rubber feet
    for sx in (-1, 1):
        for sy in (-1, 1):
            ft = lathe('foot', [(0, 0), (7.0, 0), (7.0, FOOT + 0.3), (0, FOOT + 0.3)], [mats['rubber']], 24, 40)
            ft.location = (sx * (hx - 16) * MM, sy * (yb - 16) * MM, 0)
    # front-face jacks: headphone + line out
    for xx, rr in ((100.0, 6.2), (130.0, 6.2)):
        j = empty('jack', xx, yf, FOOT + FRONT_H * 0.55)
        j.rotation_euler = (math.radians(90), 0, 0)
        nut = prism('jack_nut', circle_pts(rr + 1.2, 6, math.pi / 6), 0.0, 2.0, [mats['chrome']], 40)
        bar = lathe('jack_barrel', [(0, -1.0), (4.6, -1.0), (4.6, 4.0), (4.1, 4.4), (0, 4.4)], [mats['chrome_sat']], 40, 40)
        nut.parent = j; bar.parent = j
        hole = lathe('jack_hole', [(0, 4.3), (2.6, 4.3), (2.6, 4.45), (0, 4.45)], [mats['black']], 24, 40); hole.parent = j
    return zc

def stickers(mats, font):
    """3-5 crust stickers at angles with peeling corners (4 on the panel, 1 on the front lip). Returns the list."""
    out = []
    def glyph_spiral(d, ctx):
        c = ctx.P(-15, 0)
        pts = []
        for i in range(260):
            t = i / 260
            a = t * 5.2 * TAU; r = 0.6 + t * 8.2
            pts.append((c[0] + r * ctx.S * math.cos(a), c[1] + r * ctx.S * math.sin(a)))
        d.line(pts, fill=255, width=int(1.0 * ctx.S), joint='curve')
    def glyph_reel(d, ctx):
        c = ctx.P(-31, 0)
        d.ellipse((c[0] - 5.2 * ctx.S, c[1] - 5.2 * ctx.S, c[0] + 5.2 * ctx.S, c[1] + 5.2 * ctx.S), outline=255, width=int(0.9 * ctx.S))
        for k in range(6):
            a = k * TAU / 6
            d.line((c[0], c[1], c[0] + 4.4 * ctx.S * math.cos(a), c[1] + 4.4 * ctx.S * math.sin(a)), fill=255, width=int(0.8 * ctx.S))
    def glyph_snail(d, ctx):
        c = ctx.P(-19, 2.5)
        pts = []
        for i in range(160):
            t = i / 160; a = t * 3.6 * TAU; r = 0.5 + t * 6.4
            pts.append((c[0] + r * ctx.S * math.cos(a), c[1] + r * ctx.S * math.sin(a)))
        d.line(pts, fill=255, width=int(1.1 * ctx.S), joint='curve')
        d.line([ctx.P(-30, -6.5), ctx.P(-14, -7.0), ctx.P(-9, -3.5)], fill=255, width=int(2.0 * ctx.S), joint='curve')
        d.line([ctx.P(-9, -3.5), ctx.P(-7, 3.5)], fill=255, width=int(0.8 * ctx.S))
        d.line([ctx.P(-10.2, -3.2), ctx.P(-10.6, 3.2)], fill=255, width=int(0.8 * ctx.S))
    def glyph_bolt(d, ctx):
        pts = [ctx.P(-3.2, 12), ctx.P(-9, 0), ctx.P(-4.2, 0), ctx.P(-7.5, -12), ctx.P(4.2, 2.5), ctx.P(-1.2, 2.5), ctx.P(3.6, 12)]
        d.polygon(pts, fill=255)
    specs = [
        # (name, w, h, bg, ink, ink2, lines, glyph, shape, x, y, rot, peel=(corner, size_mm, lift_deg, curl rad/mm))
        ('stk_loud', 64, 27, '#f0c418', '#111111', '#e23a7c', [("PLAY LOUD", 0, 5.2, 58, 0), ("DIE FREE", 0, -6.8, 50, 0)], None, 'rect', 134, 100, 7, (2, 12, 30, 0.05)),
        ('stk_dbeat', 48, 48, '#e9e6da', '#111111', '#e23a7c', [("D-BEAT", 8, 5.0, 24, 0), ("4 TRACK", 8, -7.0, 24, 0)], glyph_spiral, 'round', 138, 54, -12, (1, 15, 28, 0.05)),
        ('stk_swe', 84, 20, '#161616', '#e9e6da', '#e23a7c', [("SWEDISH MASTERING", 11, 0.5, 56, 0)], glyph_reel, 'rect', 122, -60, -4, (3, 11, 26, 0.045)),
        ('stk_slow', 56, 28, '#e8601c', '#111111', None, [("SLOW IS", 10, 6.5, 32, 0), ("HEAVY", 10, -6.5, 32, 0)], glyph_snail, 'rect', 132, -92, 8, (1, 11, 28, 0.05)),
    ]
    for (nm, w, h, bg, ink, ink2, lines, gl, shp, x, y, rot, peel) in specs:
        img = rgba_to_image(sticker_rgba(font, w, h, bg, ink, lines, ink2=ink2, glyph=gl, shape=shp, S=14, seed=zlib.crc32(nm.encode()) % 1000), nm)
        ob = build_sticker(nm, w, h, mat_sticker(img, name=nm), x, y, PT + 0.14, rot=rot, peel=peel)
        out.append(ob)
    return out

def front_sticker(mats, font, zc):
    img = rgba_to_image(sticker_rgba(font, 54, 9, '#161616', '#e9e6da', [("KEEP THE HISS", 0, 0, 48, 0)], ink2='#e23a7c', S=14, seed=77), 'stk_hiss')
    ob = build_sticker('stk_hiss', 54, 9, mat_sticker(img, name='stk_hiss'), -92.0, -129.9, FOOT + FRONT_H * 0.5, rot=2.0, peel=(1, 5, 35, 0.0), n=24)
    ob.rotation_mode = 'ZYX'
    ob.rotation_euler = (math.radians(90), 0, math.radians(2.0))
    return ob


def build(mats=None, origin=(0.0, 0.0, 0.0), rot_z=0.0, mode='hero', art_image=None, font_dir=None, return_parts=False):
    """Build the complete PLAY ALONG deck and return a root empty positioned at `origin` (mm; centre of the footprint at
    table level), rotated `rot_z` degrees about Z.  The panel assembly hangs off a child empty `deck_panel` tilted by
    SLOPE.  No camera, lights, world or render settings are touched.
    mats: dict from common.make_standard_mats / run() (None -> created here).  art_image: packed Blender image of the
    panel art (None -> generated and cached in mats['deck_art']).
    return_parts=True returns (root, panel_root)."""
    if mats is None:
        mats = make_standard_mats(None, ink_a=INK_PINK, ink_b=INK_BONE)
    make_deck_mats(mats)
    font = fetch_font(font_dir or os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))
    art_img = art_image or mats.get('deck_art')
    if art_img is None:
        art_img = art_to_image(art(font))
        mats['deck_art'] = art_img
    set_piece_dims(PW, PL, PT)
    before = set(bpy.data.objects)
    root = empty('deck_root', 0, 0, 0)
    zc = build_body(mats)
    parent_new(before, root)
    panel = empty('deck_panel', 0, 0, zc)
    panel.rotation_euler = (math.radians(SLOPE), 0, 0)
    panel.parent = root
    before = set(bpy.data.objects)
    powder = mat_powder(art_img, INK_PINK, INK_BONE, coat=0.0060, gain_a=0.95, gain_b=0.75)
    plate = build_enclosure(powder, PW, PL, PT, r=12.0, bev=1.4, nb=5, name='deck_panel_plate')
    # cassette well cut through the plate
    wcut = _box('well_cut', WELL['w'], WELL['h'], PT + 8.0, 4.0, mats['black'], WELL['x'], WELL['y'], -4.0)
    bm_ = plate.modifiers.new('well', 'BOOLEAN'); bm_.operation = 'DIFFERENCE'; bm_.object = wcut; bm_.solver = 'EXACT'
    bake_booleans(plate)
    _box('well_floor', WELL['w'] - 0.4, WELL['h'] - 0.4, 0.8, 3.0, mats['well'], WELL['x'], WELL['y'], 0.0)
    # cassette (label sticker generated through the same pipeline)
    lab_img = rgba_to_image(sticker_rgba(font, 82, 28, '#e4dcc0', '#1a1a1a',
                                         [("MIX 03", -14, 6.2, 36, 0), ("SLUDGE BACKING", 1, -6.5, 70, 0)], ink2=None,
                                         S=14, seed=5, tear=0.0, wear=1.6), 'cass_label')
    build_cassette(WELL['x'], WELL['y'], mats, mat_sticker(lab_img, rough=0.55, name='cass_label'))
    # labels / legends
    _text("PLAY-ALONG DECK", 3.6, OLED_P[0], OLED_P[1] + 21.0, mats['print'], embolden=0.04)
    _text("COUNTER", 2.8, OLED_P[0], OLED_P[1] - 17.0, mats['print'], embolden=0.03)
    _text("4 STEM", 3.0, 100.0, 124.0, mats['print'], embolden=0.03)
    build_oled_deck(OLED_P[0], OLED_P[1], mats, ["PLAY 03:12", "A 01:04 B 02:10"])
    slide_fader(mats)
    build_toggle(TOGGLE[0], TOGGLE[1], -22, mats).scale = (1.25, 1.25, 1.25)
    _text("GHOST", 3.6, TOGGLE[0], TOGGLE[1] + 19.5, mats['print'], embolden=0.04)
    _text("MUTE", 3.6, TOGGLE[0], TOGGLE[1] - 16.0, mats['print'], embolden=0.04)
    _text("GUITAR STEM", 3.4, TOGGLE[0], TOGGLE[1] + 28.0, mats['print'], embolden=0.04)
    slide_switch(COUNTIN[0], COUNTIN[1], mats, on=True)
    loop_button(LOOP_A[0], LOOP_Y, "LOOP A", mats['key_pink'], True, mats)
    loop_button(LOOP_B[0], LOOP_Y, "LOOP B", mats['btn_bone'], False, mats)
    for lbl, x, w in KEYS:
        transport_key(lbl, x, w, mats, pressed=(4.0 if lbl.startswith('PLAY') else 0.0))
    for sx, sy in SCREWS:
        build_screw(sx, sy, mats).scale = (1.6, 1.6, 1.6)
    stickers(mats, font)
    build_tape_strand(mats)
    parent_new(before, panel)
    fs = front_sticker(mats, font, zc)
    fs.parent = root
    root.location = (origin[0] * MM, origin[1] * MM, origin[2] * MM)
    root.rotation_euler = (0, 0, math.radians(rot_z))
    return (root, panel) if return_parts else root


def _run_build(mats, mode):
    build(mats, mode=mode, art_image=mats['art'])
    return []


SPEC = dict(
    name='deck', kind='piece', plate=(PW, PL, PT), art=art, ink_a=INK_PINK, ink_b=INK_BONE, build=_run_build, k=1.9,
    center=(0, 0, 0.05), modes=['hero', 'ortho'],
    hero=dict(kind='persp', loc=(0.62, -0.78, 0.52), target=(0.0, 0.0, 0.05), lens=85, fstop=18.0, k=1.9),
    ortho=dict(kind='ortho', loc=(0, 0.0, 0.9), ortho_scale=0.385, rot=(0, 0, 0), k=1.9), ortho_res=(1800, 1400),
    fnames=dict(hero='deck_hero_3q.png', ortho='deck_ortho.png'))

if __name__ == '__main__':
    run(SPEC)       # ortho is rendered straight down (the 10.5 deg panel slope is foreshortened by ~1.7%)
