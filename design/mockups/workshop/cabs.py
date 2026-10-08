"""v4 cab directions for screen 01 (mockup level, procedural Pillow / numpy, our own drawing; no maker logos or trade dress).

    (a) worn black tolex, steel corners, salt-and-pepper grille, riveted steel CAB nameplate, IR title on an LCD strip
    (b) stripped open-back bare plywood, stencilled lettering, visible generic speakers
    (c) road-case armour: aluminium edges, latches, stencilled lettering

Every cab shows SHARED · LIVE vs PER PATH · STUDIO as two lamps with words (shared is lit).  All text sits on flat wells."""
import math
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'render')))
import workshop_style as ws  # noqa: E402

np, Image, ImageDraw, ImageFilter = ws.np, ws.Image, ws.ImageDraw, ws.ImageFilter
S = ws.S


def _dev(r):
    return tuple(int(round(v * S)) for v in r)


def tolex_tex(w, h, key):
    """Worn black tolex: pebble grain, soft scuffs near the edges and corners."""
    rng = ws._rng('tolex:' + key)
    W, H = int(w * S), int(h * S)
    g = ws._noise(rng, H, W, 2.2) * 0.5 + ws._noise(rng, H, W, 7.0) * 0.5
    base = 24.0 + 7.5 * g
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    dist = np.minimum(np.minimum(xx, W - 1 - xx), np.minimum(yy, H - 1 - yy)) / S
    strong = ws.get_wear() == 'strong'
    scuff = np.clip(1.0 - dist / (14.0 if strong else 8.0), 0, 1) * np.clip(ws._noise(rng, H, W, 18) * 0.6 + 0.25, 0, 1)
    img = base + scuff * (46.0 if strong else 24.0)
    img = np.clip(img, 0, 255)
    out = np.stack([img, img * 0.97, img * 0.93], axis=-1)
    return Image.fromarray(out.astype(np.uint8), 'RGB')


def ply_tex(w, h, key, tone=(184, 148, 100)):
    """Bare birch plywood: warm tan, grain bands, a few dark knots and edge ply lines."""
    rng = ws._rng('ply:' + key)
    W, H = int(w * S), int(h * S)
    yy = np.arange(H, dtype=np.float32)[:, None]
    warp = ws._noise(rng, H, W, 90, 14) * 6.0
    band = 0.5 + 0.5 * np.sin((yy + warp) / (3.0 * S) + rng.uniform(0, 6))
    fine = ws._noise(rng, H, W, 40, 2.0)
    k = 0.82 + 0.13 * band + 0.05 * fine
    if ws.get_wear() == 'strong':
        k = k * (0.9 + 0.1 * np.clip(ws._noise(rng, H, W, 60, 30), -1, 1))
    img = np.stack([tone[0] * k, tone[1] * k, tone[2] * k], axis=-1)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')


def pepper_tex(w, h, key):
    """Salt-and-pepper grille cloth: a fine basket weave, near-black with small silver flecks on the thread grid, low contrast,
    a slight vertical drape shading."""
    rng = ws._rng('pepper:' + key)
    W, H = int(w * S), int(h * S)
    cell = 3                                                    # device px per weave thread
    yy, xx = np.mgrid[0:H, 0:W]
    ty, tx = yy // cell, xx // cell
    over = ((tx // 2 + ty // 2) % 2 == 0)                       # basket weave: 2 x 2 thread blocks alternate warp / weft
    inner = np.where(over, (xx % cell) / (cell - 1.0), (yy % cell) / (cell - 1.0))
    thread = 0.5 + 0.5 * np.sin(inner * math.pi)
    a = 22.0 + 14.0 * thread * np.where(over, 1.0, 0.7)
    seed = zlib.crc32(('fleck' + key + ws.get_wear()).encode('utf-8'))
    fl = np.random.default_rng(seed).random((H // cell + 1, W // cell + 1)) < 0.07
    fl = np.kron(fl, np.ones((cell, cell), bool))[:H, :W]
    a = a + fl * (34.0 + 22.0 * thread)                         # small silver flecks, one thread long
    a = a * (1.12 - 0.22 * (yy / float(H)))                     # vertical drape: lighter at the top, darker toward the bottom
    a = a * (0.97 + 0.06 * ws._noise(rng, H, W, 60, 90)[:H, :W])
    img = np.stack([a, a * 0.99, a * 0.96], axis=-1)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')


def modes_panel(cv, rect, horizontal=False):
    """SHARED · LIVE (lit) / PER PATH · STUDIO (unlit): two lamps with their words in one flat well."""
    ws.well(cv, rect, 'well', 3, border='steel_bare')
    cy = (rect[1] + rect[3]) / 2.0
    items = (('SHARED · LIVE', True, 'green'), ('PER PATH · STUDIO', False, 'blue'))
    for i, (word, on, hue) in enumerate(items):
        if horizontal:
            x = rect[0] + 4 + i * ((rect[2] - rect[0]) * 0.42)
            ry = cy
        else:
            x = rect[0]
            ry = cy + (i - 0.5) * 20
        ws.glow_led(cv, x + 12, ry, on, hue, 3.8)
        ws.text(cv, (x + 26, ry), word, 'label_b' if on else 'label',
                bg=(x + 24, ry - 9, (x + (rect[2] - rect[0]) * 0.42 - 2) if (horizontal and i == 0) else rect[2] - 3, ry + 9),
                fg='ok' if on else 'bone_dim')


def _corner_caps(cv, rect, size=22):
    x0, y0, x1, y1 = rect
    for (cx, cy, sx, sy) in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        pts = [(cx, cy), (cx + sx * size, cy), (cx + sx * size, cy + sy * 5), (cx + sx * 5, cy + sy * 5), (cx + sx * 5, cy + sy * size), (cx, cy + sy * size)]
        cv.poly(pts, (150, 147, 140))
        cv.poly([(cx + sx * 1.2, cy + sy * 1.2), (cx + sx * (size - 1.5), cy + sy * 1.2), (cx + sx * 1.2, cy + sy * (size - 1.5))], (190, 187, 180), 0.5)
        ws.rivet(cv, cx + sx * 8, cy + sy * 8, 2.4, 'steel_bare')


def cab_a(cv, rect, ir_title='closed_4x12_dyn_cap'):
    """(a) worn black tolex, steel corners, salt-and-pepper grille, riveted steel CAB nameplate, IR title on an LCD strip."""
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0
    cv.shadow((x0 + 8, y0 + 10, x1 - 8, y1), 8, (0, 12), 20, 0.85)
    cv.texture(rect, tolex_tex(int(w), int(h), 'a'), 7)
    gr = (x0 + 18, y0 + 18, x1 - 18, y1 - 100)
    cv.fill((gr[0] - 3, gr[1] - 3, gr[2] + 3, gr[3] + 3), (12, 11, 10), 4)
    cv.texture(gr, pepper_tex(int(gr[2] - gr[0]), int(gr[3] - gr[1]), 'a'), 2)
    for i in range(7):                                   # the cloth runs under the tolex frame: a soft inner shadow on every edge
        a_ = 0.5 * (1 - i / 7.0)
        cv.blend((gr[0], gr[1] + i, gr[2], gr[1] + i + 1), 'bench_dark', a_)
        cv.blend((gr[0], gr[3] - i - 1, gr[2], gr[3] - i), 'bench_dark', a_ * 0.8)
        cv.blend((gr[0] + i, gr[1], gr[0] + i + 1, gr[3]), 'bench_dark', a_)
        cv.blend((gr[2] - i - 1, gr[1], gr[2] - i, gr[3]), 'bench_dark', a_)
    _corner_caps(cv, rect)
    ws.dm_display(cv, (x0 + 24, y1 - 90, x1 - 24, y1 - 52), ir_title, h=14, tone='amber', pad=8)
    ws.nameplate(cv, (x0 + 24, y1 - 44, x0 + 108, y1 - 14), 'CAB')
    modes_panel(cv, (x0 + 120, y1 - 44, x1 - 24, y1 - 14), horizontal=True)


def cab_b(cv, rect):
    """(b) stripped open-back bare plywood, stencilled lettering, four visible generic speakers."""
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0
    cv.shadow((x0 + 8, y0 + 10, x1 - 8, y1), 6, (0, 12), 20, 0.85)
    cv.texture(rect, ply_tex(int(w), int(h), 'b'), 5)
    cv.blend((x0 + 4, y0, x1 - 4, y0 + 1), 'bone', 0.35)
    # baffle opening with four speakers
    op = (x0 + 16, y0 + 16, x1 - 16, y1 - 78)
    cv.fill(op, (22, 19, 16), 4)
    cv.blend((op[0], op[1], op[2], op[1] + 6), 'bench_dark', 0.6)
    cw, chh = (op[2] - op[0]) / 2.0, (op[3] - op[1]) / 2.0
    r = min(cw, chh) * 0.44
    for i in range(2):
        for j in range(2):
            cx, cy = op[0] + cw * (i + 0.5), op[1] + chh * (j + 0.5)
            cv.ellipse(cx + 1.5, cy + 2.5, r + 3, color='bench_dark', alpha=0.7)
            cv.ellipse(cx, cy, r + 2.5, color=(120, 118, 112))                    # steel basket rim
            cv.ellipse(cx, cy, r, color=(30, 27, 24))                            # surround
            cv.ellipse(cx, cy, r * 0.82, color=(58, 52, 44))                     # paper cone
            for k in range(5):
                cv.ellipse(cx, cy, r * (0.78 - 0.13 * k), color=(70 + k * 4, 63 + k * 4, 52 + k * 3), width=0.8, alpha=0.8)
            cv.ellipse(cx, cy, r * 0.22, color=(22, 20, 18))                    # dust cap
            cv.ellipse(cx - 1.2, cy - 1.4, r * 0.08, color=(96, 90, 80), alpha=0.8)
            for a in range(4):                                                   # bolts
                ang = math.radians(45 + 90 * a)
                ws.rivet(cv, cx + (r + 1.2) * math.cos(ang), cy + (r + 1.2) * math.sin(ang), 1.9, 'steel_bare')
    # stencilled lettering on a painted strip
    st = (x0 + 24, y1 - 62, x0 + 24 + 246, y1 - 14)
    ws.well(cv, st, 'well', 2)
    ws.text(cv, None, 'CAB · 4x12', 'title', bg=st, size=24, fg='bone')
    modes_panel(cv, (x1 - 24 - 150, y1 - 60, x1 - 24, y1 - 18))
    for (sx, sy) in ((x0 + 8, y0 + 8), (x1 - 8, y0 + 8), (x0 + 8, y1 - 8), (x1 - 8, y1 - 8)):
        ws.rivet(cv, sx, sy, 2.2, 'steel_bare')


def cab_c(cv, rect):
    """(c) road-case armour: aluminium edges, corner caps, butterfly latches, perforated grille, stencilled lettering."""
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0
    cv.shadow((x0 + 8, y0 + 10, x1 - 8, y1), 6, (0, 12), 20, 0.85)
    cv.texture(rect, tolex_tex(int(w), int(h), 'c'), 4)
    # perforated grille
    gr = (x0 + 22, y0 + 22, x1 - 22, y1 - 84)
    cv.fill(gr, (58, 57, 54), 3)
    rng = ws._rng('perf:c')
    for yy in range(int(gr[1]) + 6, int(gr[3]) - 3, 9):
        off = 4.5 if ((yy - int(gr[1])) // 9) % 2 else 0.0
        xx = gr[0] + 6 + off
        while xx < gr[2] - 3:
            cv.ellipse(xx, yy, 2.1, color=(10, 10, 9))
            xx += 9
    cv.blend((gr[0], gr[1], gr[2], gr[1] + 5), 'bench_dark', 0.5)
    # aluminium edge extrusions + corner caps
    e = 9
    for er in ((x0, y0, x1, y0 + e), (x0, y1 - e, x1, y1), (x0, y0, x0 + e, y1), (x1 - e, y0, x1, y1)):
        cv.texture(er, ws.alu_tex(int(er[2] - er[0]), int(er[3] - er[1]), 'caseedge%d' % int(er[0] + er[1])), 1)
    for (cx, cy) in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
        cv.fill((cx - 11 if cx > x0 + 10 else cx, cy - 11 if cy > y0 + 10 else cy, cx if cx > x0 + 10 else cx + 11, cy if cy > y0 + 10 else cy + 11), (92, 90, 86), 2)
        ws.rivet(cv, cx + (-5.5 if cx > x0 + 10 else 5.5), cy + (-5.5 if cy > y0 + 10 else 5.5), 2.4, 'steel_bare')
    # butterfly latches
    for lx in (x0 + 50, x0 + 282):
        la = (lx - 17, y1 - 74, lx + 17, y1 - 50)
        cv.shadow(la, 3, (0, 2), 4, 0.6)
        cv.fill(la, (150, 147, 140), 3)
        cv.fill(ws.inset(la, 2), (100, 98, 93), 2)
        cv.fill((lx - 9, y1 - 69, lx + 9, y1 - 55), (172, 169, 162), 2)
        ws.rivet(cv, lx - 12, y1 - 62, 1.8, 'steel_bare')
        ws.rivet(cv, lx + 12, y1 - 62, 1.8, 'steel_bare')
    # handle recess
    cv.fill((x0 + w / 2.0 - 34, y0 + 12, x0 + w / 2.0 + 34, y0 + 17), (8, 8, 7), 2)
    # stencilled lettering on a painted panel
    st = (x0 + 86, y1 - 70, x0 + 246, y1 - 20)
    ws.well(cv, st, 'alu_well', 2)
    ws.text(cv, None, 'CAB · 4x12', 'section', bg=st, size=19, fg='ink')
    modes_panel(cv, (x1 - 22 - 146, y1 - 68, x1 - 22, y1 - 22))


DRAW = {'a': cab_a, 'b': cab_b, 'c': cab_c}
