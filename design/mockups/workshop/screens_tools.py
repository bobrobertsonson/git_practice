"""Workshop mockup screens: the tool overlays (05 MATCH, 06 NAM FORGER, 07 WOODSHED, 08 rig editor).

Each overlay is drawn on top of the real rig screen (screens_rig) so the screens feel like one app.  Every string goes
through ``workshop_style.text`` (flat wells only, contrast logged).  The backdrop rig is dimmed and its own text log is
dropped: that text is scenery behind an overlay, not UI of these screens."""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'render')))
import workshop_style as ws  # noqa: E402
import screens_rig  # noqa: E402
from workshop_style import text, well, label_well, text_width, inset  # noqa: E402


# --------------------------------------------------------------------------------------------------------------------
# shared helpers
# --------------------------------------------------------------------------------------------------------------------
def backdrop(name, dim=0.35, area=(0, 58, 1280, 800)):
    """The rig screen, dimmed.  The canvas is renamed and its (scenery) text log cleared."""
    cv = screens_rig.screen_01_main_rig()
    cv.name = name
    cv.log = []
    if dim:
        ws.darken(cv, area, dim)
    return cv


def frame(cv, rect, title, key, right=None, close=True):
    """Riveted steel overlay frame with a riveted aluminium title plate (Black Ops One title).  Returns the content rect."""
    x0, y0, x1, y1 = rect
    ws.panel(cv, rect, key, 1.3, 8, rivets=True)
    ws.plate(cv, (x0 + 14, y0 + 10, x1 - 14, y0 + 52), None, key=key + ':plate')
    strip = (x0 + 36, y0 + 15, x1 - (96 if close else 36), y0 + 47)
    well(cv, strip, 'alu_well', 2)
    text(cv, (strip[0] + 10, (strip[1] + strip[3]) / 2.0), title, 'brand', bg=strip, fg='ink')
    if right:
        text(cv, (strip[2] - 10, (strip[1] + strip[3]) / 2.0), right, 'label_ink', bg=strip, align='r')
    if close:
        cb = (x1 - 80, y0 + 17, x1 - 40, y0 + 45)
        well(cv, cb, 'ink', 2)
        ws.glyph_icon(cv, 'close', (cb[0] + cb[2]) / 2.0, (cb[1] + cb[3]) / 2.0, 13, 'bone')
    return (x0 + 18, y0 + 62, x1 - 18, y1 - 16)


def sec(cv, rect, title, key, right=None):
    """Steel sub-panel with a riveted section plate.  Returns the content rect."""
    x0, y0, x1, y1 = rect
    ws.panel(cv, rect, key, 0.8, 5, shadow=True)
    ws.plate(cv, (x0 + 6, y0 + 6, x1 - 6, y0 + 36), title, right=right, key=key + ':p')
    return (x0 + 12, y0 + 44, x1 - 12, y1 - 10)


def cy_of(r):
    return (r[1] + r[3]) / 2.0


def line_in(cv, rect, x, s, style='body', **kw):
    """Left-aligned text vertically centred in a (flat) rect."""
    return text(cv, (x, cy_of(rect)), s, style, bg=rect, **kw)


def strip(cv, rect, s, style='body', fg=None, pad=8, kind='well', align='l', size=None):
    """A flat well with one string in it."""
    well(cv, rect, kind, 2)
    if align == 'c':
        return text(cv, None, s, style, bg=rect, fg=fg, size=size)
    if align == 'r':
        return text(cv, (rect[2] - pad, cy_of(rect)), s, style, bg=rect, fg=fg, align='r', size=size)
    return text(cv, (rect[0] + pad, cy_of(rect)), s, style, bg=rect, fg=fg, size=size)


def rule(cv, x0, x1, y):
    cv.blend((x0, y, x1, y + 1), 'bench_dark', 0.9)
    cv.blend((x0, y + 1, x1, y + 2), 'bone', 0.1)


def btn(cv, x, y, w, h, label, kind='secondary', state='normal', icon=None, caption=None):
    return ws.button(cv, (x, y, x + w, y + h), label, kind, state, icon=icon, caption=caption)


def top(cv, **kw):
    return ws.top_bar(cv, ws.TopBarState(**kw))


# --------------------------------------------------------------------------------------------------------------------
# 05 MATCH
# --------------------------------------------------------------------------------------------------------------------
def _spectrum_curves(n=96):
    """Deterministic reference / match curves (dB) over log frequency 50 Hz .. 10 kHz."""
    xs = [i / (n - 1.0) for i in range(n)]
    ref, mat = [], []
    for u in xs:
        f = 50.0 * (200.0 ** u)
        lf = math.log10(f)
        r = (3.0 * math.exp(-((lf - 2.15) / 0.28) ** 2) - 5.5 * math.exp(-((lf - 2.95) / 0.22) ** 2)
             + 4.2 * math.exp(-((lf - 3.38) / 0.18) ** 2) - 9.0 * max(0.0, lf - 3.55) ** 1.4 * 2.2
             + 0.7 * math.sin(lf * 17.0) - 1.0)
        d = (1.7 * math.sin(lf * 9.0 + 0.7) + 3.2 * math.exp(-((lf - 2.62) / 0.14) ** 2)
             - 2.6 * math.exp(-((lf - 3.65) / 0.12) ** 2))
        ref.append(r)
        mat.append(r + d)
    return xs, ref, mat


def _dashed(cv, pts, color, width, dash=7.0, gap=5.0):
    """Dashed polyline (line style carries the legend, not colour alone)."""
    run = 0.0
    on = True
    cur = [pts[0]]
    for a, b in zip(pts, pts[1:]):
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        pos = 0.0
        while pos < seg:
            lim = (dash if on else gap) - run
            step = min(lim, seg - pos)
            t0, t1 = pos / seg, (pos + step) / seg
            p1 = (a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1)
            if on:
                cv.line([(a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0), p1], color, width, 1.0, False)
            pos += step
            run += step
            if run >= (dash if on else gap) - 1e-9:
                on = not on
                run = 0.0


def _cand_row(cv, rect, rank, chain, score, selected=False, preview=False):
    x0, y0, x1, y1 = rect
    well(cv, rect, 'well_raised' if selected else 'well', 3)
    c = cy_of(rect)
    if selected:
        cv.fill((x0, y0 + 2, x0 + 4, y1 - 2), 'blade')
    if preview:
        ws.badge(cv, (x0 + 14, c), 'PREVIEW', h=20)
        cx = x0 + 14 + 100
    else:
        text(cv, (x0 + 14, c), ('▶ ' if selected else '   ') + rank, 'mono', bg=(x0 + 6, y0 + 3, x0 + 62, y1 - 3),
             fg='blade_hi' if selected else 'bone')
        cx = x0 + 70
    text(cv, (cx, c), chain, 'body_strong', bg=(cx - 4, y0 + 3, cx + 392, y1 - 3))
    ws.lcd(cv, (x1 - 266, c), score, 'dB', digits=4, h=16)
    btn(cv, x1 - 186, y0 + 6, 110, rect[3] - rect[1] - 12, 'AUDITIONING' if selected else 'AUDITION', 'secondary',
        'pressed' if selected else 'normal')
    btn(cv, x1 - 70, y0 + 6, 62, rect[3] - rect[1] - 12, 'APPLY', 'secondary')


def screen_05_match():
    cv = backdrop('05_match')
    top(cv)
    c = frame(cv, (12, 66, 1268, 792), 'MATCH', 'ov05', right='REFINED READY')
    L0, L1 = c[0], 410
    R0, R1 = 426, c[2]

    # ---- 1 · REFERENCE SONG ----------------------------------------------------------------------------------------
    k = sec(cv, (L0, c[1], L1, c[1] + 168), '1 · REFERENCE SONG', 'm05a')
    y = k[1]
    btn(cv, k[0], y, 128, 30, 'SONG FILE…')
    btn(cv, k[0] + 138, y, 150, 30, 'STEMS FOLDER…')
    strip(cv, (k[0], y + 40, k[2], y + 66), 'Cinder Pit — reference mix.flac', 'body_strong')
    strip(cv, (k[0], y + 72, k[2], y + 96), '✓ separation done · 4 stems · 04:12', 'body', fg='ok')

    # ---- 2 · YOUR DI -----------------------------------------------------------------------------------------------
    sy0 = c[1] + 176
    k = sec(cv, (L0, sy0, L1, sy0 + 262), '2 · YOUR DI', 'm05b')
    y = k[1]
    ws.footswitch(cv, k[0] + 24, y + 22, 40, False, 'REC')
    ws.footswitch(cv, k[0] + 92, y + 22, 40, False, 'STOP')
    ws.led(cv, k[0] + 152, y + 20, False, 20, 'alert')
    ws.lcd(cv, (k[2] - 4, y + 22), '00:00.0', '', digits=7, h=18, align='r')
    y += 66
    label_well(cv, (k[0], y + 8), 'TAKES · NEWEST FIRST', 'label', h=18)
    y += 24
    takes = [('take_2026-10-08_2114', '01:48', 'USED FOR MATCH', True), ('take_2026-10-08_2102', '02:31', '', False),
             ('take_2026-10-07_2250', '00:52', '1 overrun', False)]
    for i, (nm, ln, tag, sel) in enumerate(takes):
        r = (k[0], y + i * 31, k[2], y + i * 31 + 27)
        well(cv, r, 'well_raised' if sel else 'well', 2)
        if sel:
            cv.fill((r[0], r[1] + 2, r[0] + 4, r[3] - 2), 'blade')
        text(cv, (r[0] + 10, cy_of(r)), ('▶ ' if sel else '   ') + nm, 'mono', bg=(r[0] + 6, r[1] + 3, r[0] + 196, r[3] - 3),
             fg='blade_hi' if sel else 'bone')
        text(cv, (r[0] + 204, cy_of(r)), ln, 'mono_dim', bg=(r[0] + 200, r[1] + 3, r[0] + 246, r[3] - 3))
        if tag:
            text(cv, (r[2] - 8, cy_of(r)), tag, 'label_b', bg=(r[0] + 250, r[1] + 3, r[2] - 4, r[3] - 3),
                 fg='ok' if sel else 'warn', align='r')
    y += 3 * 31 + 4
    strip(cv, (k[0], y, k[2], y + 22), 'The selected take is the one MATCH listens to.', 'body_dim')

    # ---- TOOLS -----------------------------------------------------------------------------------------------------
    ty = sy0 + 270
    k = sec(cv, (L0, ty, L1, ty + 100), 'TOOLS', 'm05c')
    cy = k[1] + 22
    ws.toggle(cv, k[0] + 56, cy, 1, ['OFF', 'ON'], 32, key='tg05')
    label_well(cv, (k[0] + 134, cy - 11), 'AUTO-REFINE', 'label_b', h=18)
    strip(cv, (k[0] + 134, cy + 3, k[2], cy + 25), 'quick pass, then refine the best 3', 'body_dim', pad=6)

    # ---- START ------------------------------------------------------------------------------------------------------
    by = ty + 108
    btn(cv, L0, by, L1 - L0, 40, 'START MATCH', 'primary')
    ws.progress(cv, (L0, by + 50, L1 - 150, by + 66), 1.0, 'ok')
    label_well(cv, (L1 - 142, by + 58), '✓ DONE · 1 m 12 s', 'label_b', h=20, fg='ok')

    # ---- RESULTS ----------------------------------------------------------------------------------------------------
    k = sec(cv, (R0, c[1], R1, c[1] + 440), 'RESULTS', 'm05d')
    y = k[1]
    ws.badge(cv, (k[0] + 2, y + 11), 'REFINED', h=22)
    strip(cv, (k[0] + 100, y, k[2], y + 22), 'ready · 3 refined candidates scored on the full song', 'body', pad=8)
    y += 32
    label_well(cv, (k[0], y + 10), 'REFINED · full-song score, lower is closer', 'label_b', h=20, fg='ok')
    y += 26
    rows = [('#1', 'BUZZSAW → SAW HEAD ║ TS-STYLE → BODY HEAD · 4x12', '1.53'),
            ('#2', 'TAR PIT → SAW HEAD ║ BODY HEAD · 4x12', '1.71'),
            ('#3', 'SERRATED → SAW HEAD · single path · 2x12', '2.08')]
    for i, (rk, ch, sc) in enumerate(rows):
        _cand_row(cv, (k[0], y + i * 46, k[2], y + i * 46 + 42), rk, ch, sc, selected=(i == 0))
    y += 3 * 46 + 4
    label_well(cv, (k[0], y + 10), 'PREVIEW · quick pass, not yet refined', 'label_b', h=20, fg='body')
    y += 26
    prev = [('BUZZSAW → SAW HEAD ║ BODY HEAD · 4x12', '2.41'), ('HATCHET → SAW HEAD · single path · 4x12', '2.87')]
    for i, (ch, sc) in enumerate(prev):
        _cand_row(cv, (k[0], y + i * 46, k[2], y + i * 46 + 42), '', ch, sc, preview=True)
    y += 2 * 46 + 6
    strip(cv, (k[0], y, k[2] - 262, y + 34), 'APPLY is one undo step: Ctrl+Z restores the preset you had.', 'body_dim')
    btn(cv, k[2] - 252, y, 252, 34, 'APPLY REFINED BEST', 'primary')

    # ---- spectrum ---------------------------------------------------------------------------------------------------
    sy = c[1] + 448
    k = sec(cv, (R0, sy, R1, c[3]), 'SPECTRUM · REFERENCE vs MATCH', 'm05e')
    g = (k[0], k[1], k[0] + 560, k[1] + 92)
    cv.fill((g[0] - 1, g[1] - 1, g[2] + 1, g[3] + 1), 'steel_bare', 3)
    cv.fill(g, 'glass', 2)
    for fr in (100, 200, 500, 1000, 2000, 5000):
        u = math.log10(fr / 50.0) / math.log10(200.0)
        cv.fill((g[0] + u * (g[2] - g[0]), g[1] + 1, g[0] + u * (g[2] - g[0]) + 1, g[3] - 1), 'steel_bare')
        cv.blend((g[0] + u * (g[2] - g[0]), g[1] + 1, g[0] + u * (g[2] - g[0]) + 1, g[3] - 1), 'glass', 0.78)
    for yy in (0.25, 0.5, 0.75):
        cv.blend((g[0] + 1, g[1] + yy * (g[3] - g[1]), g[2] - 1, g[1] + yy * (g[3] - g[1]) + 1), 'steel_bare', 0.18)
    xs, ref, mat = _spectrum_curves()
    pw, ph = g[2] - g[0] - 8, g[3] - g[1] - 12

    def P(i, v):
        return (g[0] + 4 + xs[i] * pw, g[1] + 6 + ph * (0.58 - v / 22.0))
    rp = [P(i, v) for i, v in enumerate(ref)]
    mp = [P(i, v) for i, v in enumerate(mat)]
    cv.poly(rp + mp[::-1], 'blade', 0.16)
    cv.line(rp, 'bone', 2.2)
    _dashed(cv, mp, 'blade_hi', 2.2)
    ax = (g[0], g[3] + 4, g[2], g[3] + 22)
    well(cv, ax, 'well', 2)
    for fr, lab in ((100, '100'), (200, '200'), (500, '500'), (1000, '1k'), (2000, '2k'), (5000, '5k')):
        u = math.log10(fr / 50.0) / math.log10(200.0)
        text(cv, (g[0] + u * (g[2] - g[0]), cy_of(ax)), lab, 'mono_dim', bg=ax, size=13, align='c')
    text(cv, (ax[2] - 6, cy_of(ax)), 'Hz', 'mono_dim', bg=ax, size=13, align='r')
    # legend + readouts
    lx = g[2] + 18
    ly = k[1] + 4
    lg = (lx, ly - 2, k[2], ly + 22)
    well(cv, lg, 'well', 2)
    cv.line([(lg[0] + 10, ly + 10), (lg[0] + 46, ly + 10)], 'bone', 2.2)
    text(cv, (lg[0] + 56, ly + 10), 'REFERENCE · solid', 'label_b', bg=(lg[0] + 52, lg[1] + 2, lg[2] - 2, lg[3] - 2))
    lg2 = (lx, ly + 28, k[2], ly + 52)
    well(cv, lg2, 'well', 2)
    _dashed(cv, [(lg2[0] + 10, ly + 40), (lg2[0] + 46, ly + 40)], 'blade_hi', 2.2, 7, 4)
    text(cv, (lg2[0] + 56, ly + 40), 'MATCH · dashed', 'label_b', bg=(lg2[0] + 52, lg2[1] + 2, lg2[2] - 2, lg2[3] - 2))
    ws.lcd(cv, (lx, ly + 76), '9', '/ 10 rules', digits=2, h=20)
    strip(cv, (lx, ly + 100, k[2], ly + 124), '6.45 → 1.53 dB', 'mono', pad=8)
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 06 NAM FORGER
# --------------------------------------------------------------------------------------------------------------------
def _lines_well(cv, rect, items, pad=8, lh=18, top=None):
    """One flat well with several left-aligned lines; items = [(string, style, fg)]."""
    well(cv, rect, 'well', 2)
    y = rect[1] + (top if top is not None else (rect[3] - rect[1] - lh * len(items)) / 2.0 + lh / 2.0)
    for (s, st, fg) in items:
        text(cv, (rect[0] + pad, y), s, st, bg=rect, fg=fg)
        y += lh


def screen_06_nam_forger():
    cv = backdrop('06_nam_forger')
    top(cv)
    c = frame(cv, (12, 66, 1268, 792), 'NAM FORGER', 'ov06', right='NO-CAB + IR · LITE')
    x0, x1 = c[0], 850
    y = c[1]
    # ---- mode cards -------------------------------------------------------------------------------------------------
    cw = (x1 - x0 - 16) / 3.0
    cards = [('▶ NO-CAB + IR', 'SELECTED', 'blade',
              [('✓ LIVE-COMPATIBLE: exact', 'body_strong', 'ok'), ('cab IR loads after the model', 'body_dim', None)]),
             ('WITH CAB', None, None, [('cab baked into the model', 'body', None), ('exact for every rig', 'body_dim', None)]),
             ('STUDIO BLEND', 'INFO', 'body', [('per-path IRs: only the with-cab', 'body', None),
                                               ('export is exact. Your rig: shared cab.', 'body_dim', None)])]
    for i, (title, status, accent, items) in enumerate(cards):
        r = (x0 + i * (cw + 8), y, x0 + i * (cw + 8) + cw, y + 100)
        cd = ws.card(cv, r, title, accent, status=status, key='c06%d' % i)
        _lines_well(cv, (cd['body'][0], cd['body'][1], cd['body'][2], cd['body'][3]), items)
        if i == 0:
            cv.outline((r[0] - 3, r[1] - 3, r[2] + 3, r[3] + 3), 'blade', 2, 7)
    y += 108
    # ---- size / validation DI / VISE -------------------------------------------------------------------------------
    k = sec(cv, (x0, y, x0 + 280, y + 148), 'SIZE', 'm06a')
    rcx, rcy = k[0] + 104, k[1] + 58
    ws.rotary_selector(cv, rcx, rcy, 50, ['FEATHER', 'LITE', 'STANDARD'], 1, 'blade')
    strip(cv, (rcx + 34, k[1], k[2], k[1] + 20), 'last run: 14 min', 'body_dim', pad=6)
    k = sec(cv, (x0 + 288, y, x0 + 568, y + 148), 'VALIDATION DI', 'm06b')
    ws.dropdown(cv, (k[0], k[1] + 4, k[2], k[1] + 34), 'LAST TAKE · take_2026-10-08_2114', 'body')
    strip(cv, (k[0], k[1] + 44, k[2], k[1] + 68), 'held-out check · 01:48 · 48 kHz', 'body_dim')
    strip(cv, (k[0], k[1] + 74, k[2], k[1] + 98), 'built-in signal if no take exists', 'body_dim')
    k = sec(cv, (x0 + 576, y, x1, y + 148), 'VISE', 'm06c')
    ws.toggle(cv, k[0] + 100, k[1] + 24, 1, ['DROP COMP', 'KEEP COMP'], 34, key='tg06')
    strip(cv, (k[0], k[1] + 56, k[2], k[1] + 80), 'kept in the model: release 80 ms', 'body_dim')
    strip(cv, (k[0], k[1] + 86, k[2], k[1] + 110), 'DROP leaves it to the loader chain', 'body_dim')
    y += 156
    # ---- output folder ----------------------------------------------------------------------------------------------
    r = (x0, y, x1, y + 48)
    ws.panel(cv, r, 'm06d', 0.8, 5)
    label_well(cv, (r[0] + 14, y + 24), 'OUTPUT FOLDER', 'label_b', h=20)
    ws.text_field(cv, (r[0] + 140, y + 9, r[2] - 130, y + 39), '~/Library/Application Support/Sawblade/exports', 'mono')
    btn(cv, r[2] - 118, y + 9, 106, 30, 'CHOOSE…')
    y += 56
    # ---- rig summary / what goes into the model / credits ----------------------------------------------------------
    h4 = 220
    k = sec(cv, (x0, y, x0 + 220, y + h4), 'RIG', 'm06e')
    rows = [('BLADE', 'THE SAW MILL → SAW HEAD', 'blade_hi'), ('BODY', 'TS-STYLE → BODY HEAD', 'body'),
            ('BLEND · ALIGN', '79 / 21 · −17 smp · Ø NORMAL', None), ('CAB', 'SHARED 4x12 · one mic', None)]
    yy = k[1]
    for lab, val, col in rows:
        label_well(cv, (k[0], yy + 8), lab, 'label_b', h=16, fg=col, pad=5)
        strip(cv, (k[0], yy + 18, k[2], yy + 38), val, 'body', pad=6)
        yy += 42
    k = sec(cv, (x0 + 228, y, x0 + 548, y + h4), 'WHAT GOES INTO THE MODEL', 'm06f')
    items = [('✗', 'GATE — left out', 'warn'), ('✓', 'BLADE · THE SAW MILL + SAW HEAD', 'ok'),
             ('✓', 'BODY · TS-STYLE + BODY HEAD', 'ok'), ('✓', 'BLEND + ALIGN', 'ok'), ('✗', 'CAB IR — loads after the model', 'warn'),
             ('✗', 'POST EQ — loads after the cab', 'warn'), ('✓', 'VISE — release 80 ms, trainable', 'ok'),
             ('✗', 'DELAY · REVERB · MODULATION — none in rig', 'bone_dim')]
    well(cv, (k[0], k[1], k[2], k[1] + len(items) * 20 + 6), 'well', 2)
    for i, (g, s, col) in enumerate(items):
        wr = (k[0] + 2, k[1] + 3 + i * 20, k[2] - 2, k[1] + 3 + i * 20 + 20)
        text(cv, (wr[0] + 6, cy_of(wr)), g, 'body_strong', bg=wr, fg=col)
        text(cv, (wr[0] + 28, cy_of(wr)), s, 'body', bg=wr)
    k = sec(cv, (x0 + 556, y, x1, y + h4), 'CREDITS', 'm06g')
    cr = [('SAW HEAD · @marrow_amps · cc-by', None), ('BODY HEAD · @swamp_rig · cc-by-nc', 'nc'), ('CAB IR · @bench_tones · cc-by', None)]
    yy = k[1]
    for s, nc in cr:
        strip(cv, (k[0], yy, k[2], yy + 22), s, 'body', pad=6)
        yy += 25
    ws.badge(cv, (k[0] + 2, yy + 11), 'NON-COMMERCIAL', h=20)
    well(cv, (k[0] + 148, yy, k[2], yy + 22), 'well', 2)
    text(cv, (k[0] + 154, yy + 11), 'file ends -nc', 'body_dim', bg=(k[0] + 148, yy, k[2], yy + 22))
    yy += 28
    _lines_well(cv, (k[0], yy, k[2], yy + 66), [('For your own use only. Sharing a model', 'body_dim', None),
                                                ('needs permission from the capture', 'body_dim', None),
                                                ('creators and TONE3000.', 'body_dim', None)], lh=19)
    y += h4 + 8
    # ---- train / resume ---------------------------------------------------------------------------------------------
    ws.footswitch(cv, x0 + 24, y + 22, 40, False)
    btn(cv, x0 + 56, y + 2, 200, 40, 'TRAIN EXPORT', 'primary')
    btn(cv, x0 + 268, y + 2, 270, 40, 'RESUME · epoch 41 / 100', 'secondary')
    strip(cv, (x0 + 548, y + 4, x1, y + 40), 'resumes the cancelled NO-CAB · LITE run', 'body_dim')

    # ---- export notes ----------------------------------------------------------------------------------------------
    k = sec(cv, (862, c[1], c[2], y + 46), 'EXPORT NOTES', 'm06h', right='plain text')
    nb = (k[0], k[1], k[2], k[3] - 46)
    well(cv, nb, 'well', 2)
    notes = [('h', 'LOADER ORDER'), ('m', 'GATE > NAM model > CAB IR > POST EQ'), ('g', ''),
             ('h', 'LEFT OUT OF THE MODEL, IN SIGNAL ORDER'),
             ('m', '1 GATE'), ('d', 'open -42 dB, close -46 dB, attack 1 ms,'), ('d', 'hold 40 ms, release 90 ms, range'),
             ('d', '-60 dB. Runs before the model.'), ('g', ''),
             ('m', '2 CAB IR'), ('d', 'shared 4x12, one mic. Loads after the'), ('d', 'model in the pedal.'), ('g', ''),
             ('m', '3 POST EQ'), ('d', 'low shelf +1.5 dB at 90 Hz, bell -2.0 dB'), ('d', 'at 650 Hz Q 1.2, high cut 9.5 kHz.'),
             ('d', 'Loads after the cab.'), ('g', ''),
             ('h', 'TRAINED IN'), ('m', 'VISE: release 80 ms, trainable'), ('m', 'Time effects: none in this rig'), ('g', ''),
             ('x', 'End of notes · personal use only.')]
    ny = nb[1] + 14
    for kind, s in notes:
        if kind == 'g':
            ny += 8
            continue
        st, fg = {'h': ('label_b', 'blade_hi'), 'm': ('mono', None), 'd': ('mono_dim', None), 'x': ('mono_dim', None)}[kind]
        text(cv, (nb[0] + 10, ny), s, st, bg=nb, fg=fg, size=13 if kind != 'h' else None)
        ny += 19 if kind != 'h' else 21
    btn(cv, k[2] - 120, k[3] - 36, 120, 34, 'COPY')
    strip(cv, (k[0], k[3] - 36, k[2] - 130, k[3] - 2), '(computed by the plugin)', 'body_dim')

    # ---- training strip ---------------------------------------------------------------------------------------------
    ty = 728
    r = (c[0], ty, c[2], c[3])
    ws.panel(cv, r, 'm06t', 0.7, 5)
    cyy = cy_of(r)
    label_well(cv, (r[0] + 12, cyy), 'TRAINING · LITE', 'label_b', h=22)
    ws.progress(cv, (r[0] + 160, cyy - 9, r[0] + 480, cyy + 9), 0.41)
    label_well(cv, (r[0] + 490, cyy), 'epoch 41 / 100', 'mono', h=22, pad=7)
    label_well(cv, (r[0] + 628, cyy), 'BEST ESR', 'label_b', h=22)
    ws.lcd(cv, (r[0] + 720, cyy), '0.0123', '', digits=5, h=16)
    label_well(cv, (r[0] + 830, cyy), 'ETA 18 min', 'mono', h=22, pad=7)
    btn(cv, r[2] - 118, ty + 7, 106, r[3] - ty - 14, 'CANCEL')
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 07 WOODSHED
# --------------------------------------------------------------------------------------------------------------------
def _title_plate(cv, rect, title, key):
    ws.plate(cv, rect, None, key=key)
    st = (rect[0] + 22, rect[1] + 5, rect[2] - 22, rect[3] - 5)
    well(cv, st, 'alu_well', 2)
    text(cv, (st[0] + 10, cy_of(st)), title, 'brand', bg=st, fg='ink')
    return st


def _sawtooth(cv, y_base, x0, x1, h=14, pitch=26):
    """A row of saw teeth standing on y_base (decoration): dark steel with a lit leading edge."""
    x = x0
    while x < x1:
        pts = [(x, y_base + 1), (x + pitch * 0.9, y_base - h), (x + pitch * 0.9, y_base + 1)]
        cv.poly(pts, 'steel')
        cv.line([(x + 0.5, y_base), (x + pitch * 0.9, y_base - h)], 'steel_bare', 1.1, 0.65, False)
        x += pitch


def screen_07_woodshed():
    cv = backdrop('07_woodshed', 0.22)
    top(cv, woodshed_open=True)
    DY = 470
    ws.sawdust(cv, (0, DY - 34, 940, DY), 90, '07:dust', clumps=[(330, DY - 5, 150, 160), (700, DY - 4, 110, 110)])
    cv.shadow((0, DY - 14, 1280, DY), 0, (0, -4), 10, 0.5)
    _sawtooth(cv, DY, 0, 1280)
    ws.panel(cv, (0, DY, 1280, 800), 'dock07', 1.4, 0, shadow=False)
    cv.blend((0, DY, 1280, DY + 1), 'bone', 0.25)
    # ---- row A: title, pickers, song --------------------------------------------------------------------------------
    ya = DY + 12
    _title_plate(cv, (14, ya, 240, ya + 38), 'WOODSHED', 'p07')
    btn(cv, 256, ya + 4, 124, 30, 'SONG FILE…')
    btn(cv, 388, ya + 4, 150, 30, 'STEMS FOLDER…')
    strip(cv, (548, ya + 5, 800, ya + 33), 'Cinder Pit — reference mix.flac', 'body_strong')
    strip(cv, (806, ya + 5, 1034, ya + 33), '✓ separated · 4 stems · 04:12', 'body', fg='ok')
    strip(cv, (1042, ya + 5, 1266, ya + 33), 'drop a song or stems folder here', 'body_dim')
    # ---- row B: transport + seek ------------------------------------------------------------------------------------
    yb = DY + 74
    ws.footswitch(cv, 44, yb, 44, True, None, 'PLAYING')
    ws.footswitch(cv, 134, yb, 44, False, 'PAUSE')
    ws.lcd(cv, (196, yb), '01:23.4', '/ 04:12.0', digits=7, h=22)
    sx0, sx1 = 470, 1130
    slider_rect = (sx0, yb - 10, sx1, yb + 10)
    frac_a, frac_b, frac_p = 42.0 / 252.0, 118.5 / 252.0, 83.4 / 252.0
    ws.slider(cv, slider_rect, frac_p, [(frac_a, 'A'), (frac_b, 'B')], key='seek07')
    cv.fill((sx0 + (sx1 - sx0) * frac_a, yb + 14, sx0 + (sx1 - sx0) * frac_b, yb + 16), 'blade')
    for fr, nm, tm in ((frac_a, 'A', '00:42.0'), (frac_b, 'B', '01:58.5')):
        mx = sx0 + (sx1 - sx0) * fr
        label_well(cv, (mx, yb - 22), nm, 'label_b', h=16, pad=6, align='c')
        label_well(cv, (mx, yb + 28), nm + ' ' + tm, 'mono_dim', h=18, pad=6, align='c', size=13)
    ws.chip(cv, (1264, yb), 'LOOP ON', 'ok', glyph='check', h=24, align='r')
    # ---- row C: controls ---------------------------------------------------------------------------------------------
    rule(cv, 14, 1266, DY + 108)
    yc = DY + 170
    label_well(cv, (22, yc - 36), 'COUNT-IN', 'label_b', h=18)
    ws.toggle(cv, 72, yc, 1, ['OFF', 'ON'], 30, key='tg07a')
    label_well(cv, (160, yc - 36), 'TEMPO', 'label_b', h=18)
    ws.lcd(cv, (160, yc), '142', 'BPM', digits=3, h=20)
    label_well(cv, (306, yc - 36), 'GUITAR STEM', 'label_b', h=18)
    ws.rotary_selector(cv, 440, yc + 4, 44, ['MUTE', 'GHOST', 'FULL'], 1, 'body')
    label_well(cv, (580, yc - 36), 'KEEP KEYS', 'label_b', h=18)
    ws.toggle(cv, 618, yc, 1, ['OFF', 'ON'], 30, key='tg07b')
    label_well(cv, (730, yc - 36), 'BACKING', 'label_b', h=18)
    ws.knob(cv, 752, yc + 2, 44, 'pedal', 0.62, 'body')
    ws.lcd(cv, (788, yc), '-3.5', 'dB', digits=4, h=20)
    label_well(cv, (936, yc - 36), 'OFFSET', 'label_b', h=18)
    ws.lcd(cv, (936, yc), '+120', 'ms', digits=4, h=20)
    label_well(cv, (1086, yc - 36), 'SYNC TO HOST', 'label_b', h=18)
    ws.toggle(cv, 1122, yc, 0, ['OFF', 'ON'], 30, key='tg07c')
    strip(cv, (1086, yc + 20, 1266, yc + 40), 'plugin only · OFF = free-run', 'body_dim', pad=6)
    # ---- row D: record band -------------------------------------------------------------------------------------------
    rule(cv, 14, 1266, DY + 214)
    yd = DY + 262
    ws.footswitch(cv, 40, yd - 6, 38, True, 'REC')
    ws.led(cv, 92, yd - 6, True, 22, 'alert')
    ws.lcd(cv, (150, yd - 6), '00:47.3', '', digits=7, h=20)
    label_well(cv, (322, yd - 34), 'TAKES · NEWEST FIRST', 'label_b', h=16)
    takes = [('take_2026-10-08_2114', '01:48', 'at 00:12.0', '0 overruns', 'USED FOR MATCH', True),
             ('take_2026-10-08_2102', '02:31', 'at 00:00.0', '0 overruns', '', False),
             ('take_2026-10-07_2250', '00:52', 'at 01:04.5', '1 overrun', '', False)]
    tx0, tx1 = 322, 940
    for i, (nm, ln, at, ov, tag, sel) in enumerate(takes):
        r = (tx0, yd - 26 + i * 24, tx1, yd - 26 + i * 24 + 22)
        well(cv, r, 'well_raised' if sel else 'well', 2)
        if sel:
            cv.fill((r[0], r[1] + 2, r[0] + 4, r[3] - 2), 'blade')
        cy = cy_of(r)
        text(cv, (r[0] + 10, cy), ('▶ ' if sel else '   ') + nm, 'mono', bg=(r[0] + 6, r[1] + 2, r[0] + 214, r[3] - 2),
             fg='blade_hi' if sel else 'bone')
        text(cv, (r[0] + 224, cy), ln, 'mono_dim', bg=(r[0] + 220, r[1] + 2, r[0] + 276, r[3] - 2))
        text(cv, (r[0] + 286, cy), at, 'mono_dim', bg=(r[0] + 282, r[1] + 2, r[0] + 390, r[3] - 2))
        text(cv, (r[0] + 398, cy), ov, 'mono_dim' if ov.startswith('0') else 'body_strong', bg=(r[0] + 394, r[1] + 2, r[0] + 486, r[3] - 2),
             fg=None if ov.startswith('0') else 'warn')
        if tag:
            text(cv, (r[2] - 8, cy), '✓ ' + tag, 'label_b', bg=(r[0] + 490, r[1] + 2, r[2] - 4, r[3] - 2), fg='ok', align='r')
    btn(cv, 954, yd - 20, 134, 34, 'USE FOR MATCH')
    btn(cv, 1096, yd - 20, 66, 34, 'MATCH')
    btn(cv, 1170, yd - 20, 96, 34, 'NAM FORGER', 'primary')
    strip(cv, (954, yd + 20, 1266, yd + 42), 'recording the clean DI as you play', 'body_dim', pad=6)
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 08 rig editor
# --------------------------------------------------------------------------------------------------------------------
def _block_card(cv, rect, title, key, lines, accent, status=None, bypassed=False, uncal=False, input_db='0.0', knob_v=0.5):
    """Block card: stamped title strip, one flat well of text lines, BYPASS toggle, INPUT knob + LCD, move / remove."""
    cd = ws.card(cv, rect, title, accent, bypassed=bypassed, status=None if (bypassed or uncal) else status, key=key)
    bx0, by0, bx1, by1 = cd['body']
    if uncal:
        ws.badge(cv, (cd['title'][2] - 4, cy_of(cd['title'])), 'UNCAL', h=16, align='r')
    _lines_well(cv, (bx0, by0, bx1, by0 + 58), lines, lh=17, pad=7)
    y = by0 + 64
    # BYPASS toggle row
    label_well(cv, (bx0, y + 14), 'BYPASS', 'label_b', h=18, pad=5)
    ws.toggle(cv, bx0 + 108, y + 14, 1 if bypassed else 0, ['OFF', 'ON'], 26, key='tgb' + key)
    y += 34
    # INPUT knob + LCD
    ws.knob(cv, bx0 + 18, y + 19, 34, 'pedal', knob_v, accent or 'blade')
    label_well(cv, (bx0 + 44, y + 7), 'INPUT', 'label_b', h=16, pad=5)
    ws.lcd(cv, (bx0 + 44, y + 30), input_db, 'dB', digits=4, h=12)
    y += 46
    # move / remove
    for i, ic in enumerate(('chev_l', 'chev_r')):
        r = (bx0 + i * 40, y, bx0 + i * 40 + 34, y + 22)
        ws.button(cv, r, '', 'secondary')
        ws.glyph_icon(cv, ic, (r[0] + r[2]) / 2.0, cy_of(r), 11, 'bone')
    r = (bx1 - 34, y, bx1, y + 22)
    ws.button(cv, r, '', 'secondary')
    ws.glyph_icon(cv, 'close', (r[0] + r[2]) / 2.0, cy_of(r), 12, 'alert')
    label_well(cv, (bx0 + 84, y + 11), 'MOVE', 'label', h=16, pad=5)
    label_well(cv, (bx1 - 40, y + 11), 'REMOVE', 'label', h=16, pad=5, align='r')
    return cd


def _lane_header(cv, rect, name, right, color):
    ws.plate(cv, rect, name, right=right, key='lane' + name)
    cv.fill((rect[0], rect[1] + 2, rect[0] + 5, rect[3] - 2), color)


def screen_08_rig_editor():
    cv = backdrop('08_rig_editor', 0)
    top(cv)
    c = frame(cv, (0, 58, 940, 800), 'RIG EDITOR', 'ov08', right='BLEND · 5 BLOCKS')
    x0, x1 = c[0], c[2]
    # ---- top strip: topology + tabs + status -----------------------------------------------------------------------
    label_well(cv, (x0, c[1] + 10), 'TOPOLOGY', 'label_b', h=18)
    ws.rotary_selector(cv, x0 + 150, c[1] + 56, 46, ['SINGLE', 'SINGLE + 2 PEDALS', 'BLEND'], 2, 'blade')
    tabs = [('CHAIN', 78, True), ('EQ', 54, False), ('BLEND', 66, False), ('CAB', 54, False), ('GATE', 62, False), ('VISE', 62, False)]
    tx = x0 + 290
    for nm, w, act in tabs:
        ws.tab(cv, (tx, c[1] + 4, tx + w, c[1] + 34), nm, act)
        tx += w + 6
    strip(cv, (x0 + 290, c[1] + 44, x1, c[1] + 68), '✓ built · 5 blocks · latency 92 smp · ALIGN −17 smp', 'body', fg='ok')
    strip(cv, (x0 + 290, c[1] + 72, x1, c[1] + 94), 'chain order is signal order; a pedal sits in front of its path amp', 'body_dim')
    ly = c[1] + 100
    cw, ch = 268, 214
    xs = [x0 + i * (cw + 8) for i in range(4)]
    # ---- lane A -------------------------------------------------------------------------------------------------------
    _lane_header(cv, (x0, ly, x1, ly + 26), 'A · BLADE', 'PATH A · 3 OF 8 BLOCKS', 'blade')
    cy0 = ly + 32
    _block_card(cv, (xs[0], cy0, xs[0] + cw, cy0 + ch), 'THE SAW MILL', 'k08a',
                [('CIRCUIT · BUZZSAW', 'body_strong', 'blade_hi'), ('modelled pedal (DSP circuit)', 'body', None),
                 ('sits in front of SAW HEAD', 'body_dim', None)], 'blade', status='MODELLED', input_db='0.0', knob_v=0.5)
    _block_card(cv, (xs[1], cy0, xs[1] + cw, cy0 + ch), 'SAW HEAD', 'k08b',
                [('@marrow_amps · cc-by · VIA TONE3000', 'body', None), ('no level metadata —', 'body_dim', None),
                 ('default +9 dBu', 'body_dim', None)], 'blade', uncal=True, input_db='+1.5', knob_v=0.56)
    _block_card(cv, (xs[2], cy0, xs[2] + cw, cy0 + ch), 'EQ', 'k08c',
                [('PRE · 4 bands', 'body_strong', None), ('low cut 80 Hz, bell −3 dB', 'body', None),
                 ('at 400 Hz, high cut 8 kHz', 'body_dim', None)], 'blade', status='PRE', input_db='0.0', knob_v=0.5)
    ws.empty_slot(cv, (xs[3], cy0, x1, cy0 + ch), '+ ADD')
    # ---- lane B -------------------------------------------------------------------------------------------------------
    ly2 = cy0 + ch + 10
    _lane_header(cv, (x0, ly2, x1, ly2 + 26), 'B · BODY', 'PATH B · 2 OF 8 BLOCKS', 'body')
    cy1 = ly2 + 32
    _block_card(cv, (xs[0], cy1, xs[0] + cw, cy1 + ch), 'TS-STYLE', 'k08d',
                [('modelled pedal (DSP circuit)', 'body', None), ('DRIVE 0.20 · TONE 0.55', 'body_dim', None),
                 ('signal passes straight through', 'body_dim', None)], 'body', bypassed=True, input_db='0.0', knob_v=0.5)
    _block_card(cv, (xs[1], cy1, xs[1] + cw, cy1 + ch), 'BODY HEAD', 'k08e',
                [('@swamp_rig · cc-by · VIA TONE3000', 'body', None), ('✓ calibrated level', 'body', 'ok'),
                 ('+12.5 dBu from the capture', 'body_dim', None)], 'body', status='ON', input_db='-2.0', knob_v=0.42)
    ws.empty_slot(cv, (xs[2], cy1, xs[2] + 76, cy1 + ch), '+ ADD')
    # ---- CAB notice -----------------------------------------------------------------------------------------------------
    ny = cy1 + ch + 10
    label_well(cv, (x0, ny + 12), 'CAB', 'label_b', h=20, pad=8)
    strip(cv, (x0 + 52, ny, x1 - 232, ny + 24), '✓ LIVE-COMPATIBLE: the no-cab NAM export is exact', 'body_strong', fg='ok')
    strip(cv, (x1 - 224, ny, x1, ny + 24), 'SHARED 4x12 · CAB ON', 'label_b', align='c')
    strip(cv, (x0 + 52, ny + 28, x1, ny + 50), 'Per-path IRs would make it a studio blend: only the with-cab export is exact.', 'body_dim')
    return cv


# --------------------------------------------------------------------------------------------------------------------
# registry
# --------------------------------------------------------------------------------------------------------------------
SCREENS = {'05_match': screen_05_match, '06_nam_forger': screen_06_nam_forger,
           '07_woodshed': screen_07_woodshed,
           '08_rig_editor': screen_08_rig_editor}
