"""Workshop mockup screens: top bar (02), settings / INPUT CALIBRATION (03), preset browser (04), notices (09).

Overlay screens reuse the rig of screens_rig (darkened) so the app feels like one app; the backdrop's own text log is
dropped (it is dimmed, non-interactive scenery), except the undimmed inspector which stays measured."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'render')))
import workshop_style as ws  # noqa: E402
import screens_rig  # noqa: E402

Image = ws.Image


# --------------------------------------------------------------------------------------------------------------------
# helpers local to this module
# --------------------------------------------------------------------------------------------------------------------
def flat_box(cv, rect, kind='well', border='steel_bare', radius=3):
    """A flat dialog-body / field well with a 1 px border; text may sit anywhere on it."""
    if border:
        cv.fill(rect, border, radius)
        cv.fill(ws.inset(rect, 1), kind, max(radius - 1, 0))
    else:
        cv.fill(rect, kind, radius)
    return rect


def hsep(cv, x0, x1, y):
    cv.blend((x0, y, x1, y + 1), 'bench_dark', 0.9)
    cv.blend((x0, y + 1, x1, y + 2), 'bone', 0.1)


def rig_backdrop(name, bar_state, dim_rect, keep_inspector=False):
    """The finished rig (screen 01) re-tagged ``name``, dimmed inside ``dim_rect`` (multiply ~35 %), with the top bar
    redrawn in ``bar_state``.  Returns (canvas, bar rects)."""
    cv = screens_rig.screen_01_main_rig()
    cv.name = name
    keep = []
    if keep_inspector:
        idx = next(i for i, e in enumerate(cv.log) if e['text'] == 'INSPECTOR')
        keep = cv.log[idx:]
    cv.log = keep
    ws.darken(cv, dim_rect, 0.35)
    bar = ws.top_bar(cv, bar_state)
    return cv, bar


def zoom2x(cv, src, dest_xy, shadow=True):
    """2x zoom of the logical rect ``src`` of the canvas, pasted with its top-left at ``dest_xy`` (logical)."""
    crop = cv.crop2x(src)
    big = crop.resize((crop.width * 2, crop.height * 2), Image.LANCZOS)
    w, h = big.width / ws.S, big.height / ws.S
    x, y = dest_xy
    if shadow:
        cv.shadow((x, y, x + w, y + h), 2, (0, 4), 8, 0.7)
    cv.im.paste(big, (int(round(x * ws.S)), int(round(y * ws.S))))
    cv.blend((x, y, x + w, y + 1), 'bench_dark', 0.8)
    cv.blend((x, y + h - 1, x + w, y + h), 'bench_dark', 0.8)
    return (x, y, x + w, y + h)


def lines(cv, x, y, rows, bg, style='body', step=19, fg=None):
    """Left-aligned text rows, each centred on its y."""
    for i, s in enumerate(rows):
        ws.text(cv, (x, y + i * step), s, style, bg=bg, fg=fg)


def list_row(cv, rect, selected=False):
    """One list row well: selected = raised well + blade left bar (the marker glyph is drawn by the caller)."""
    if selected:
        cv.fill(rect, 'steel_bare', 2)
        cv.fill(ws.inset(rect, 1), 'well_raised', 2)
        cv.fill((rect[0], rect[1] + 1, rect[0] + 4, rect[3] - 1), 'blade')
    else:
        ws.well(cv, rect, 'well', 2)
    return rect


# --------------------------------------------------------------------------------------------------------------------
# 02 top bar
# --------------------------------------------------------------------------------------------------------------------
def screen_02_top_bar():
    cv = ws.new_screen('02_top_bar', sawdust_n=500)
    ab = {'a': 'GRAVE DIRT · MATCHED v2', 'b': 'THRASH TIGHT · DOWNTUNED TAKE 4' if ws.is_v3() else 'THRASH TIGHT', 'active': 'a'}
    states = [
        ('1 · DEFAULT', 'plain bar: preset selector, A / B, RIG, WOODSHED, settings, LAT / CPU, LIVE, MATCH, NAM FORGER',
         ws.TopBarState()),
        ('2 · A / B SLOTS', 'slot names in the selector; the active slot has a pressed footswitch and the word ACTIVE',
         ws.TopBarState(ab=ab)),
        ('3 · UNCAL', ('interface not calibrated: glowing lamp with the words' if ws.is_v3() else 'interface not calibrated: chip with warning glyph and the words'),
         ws.TopBarState(uncal=True)),
        ('4 · OUT OF TRUE', ('input drifted since calibration: glowing lamp with the words and the dB offset' if ws.is_v3() else 'input drifted since calibration: chip with the skewed-level glyph and the dB offset'),
         ws.TopBarState(out_of_true=True)),
        ('5 · WOODSHED OPEN + MATCH RUNNING', 'WOODSHED pressed (RIG released), MATCH shows 42 %, STUDIO chip',
         ws.TopBarState(woodshed_open=True, match_pct=42, mode='STUDIO')),
    ]
    pitch = 92
    bars = []
    for i, (title, note, st) in enumerate(states):
        y = i * pitch
        bars.append((y, ws.top_bar(cv, st, y=y)))
        cy = y + 58 + 6 + 10
        r = ws.label_well(cv, (20, cy), title, 'label_b', fg='blade_hi', h=20, pad=8)
        ws.label_well(cv, (r[2] + 6, cy), note, 'body_dim', h=20, pad=8)
    yz = 5 * pitch + 6
    yl = bars[1][0]
    yr = bars[4][0]
    if ws.is_v3():
        rg = bars[1][1]['rig']
        xr = int(rg[2] + 8) if rg[2] + 8 <= 640 else int(rg[0] - 6)          # whole RIG button, or stop before it
        gx0 = int(bars[4][1]['gear'][0] - 3)
        note_l = 'brand, preset selector with A / B slot names (the B name clips between ◀ ▶), A / B footswitches (x 0-%d)' % xr
        note_r = 'gear, LAT / CPU display, STUDIO lamp, MATCH 42 %%, NAM FORGER (x %d-1280)' % gx0
    else:
        xr, gx0 = 581, 665
        note_l = 'brand, preset selector with A / B slot names, A / B footswitches, RIG (x 0-581)'
        note_r = 'gear, LAT / CPU glass, STUDIO chip, MATCH 42 %, NAM FORGER (x 665-1280)'
    r1 = zoom2x(cv, (0, yl, xr, yl + 58), ((1280 - 2 * xr) / 2.0, yz))
    c1 = ws.label_well(cv, (20, r1[3] + 14), 'ZOOM 2x · LEFT · STATE 2', 'label_b', fg='blade_hi', h=20, pad=8)
    ws.label_well(cv, (c1[2] + 6, r1[3] + 14), note_l, 'body_dim', h=20, pad=8)
    yz2 = r1[3] + 30
    r2 = zoom2x(cv, (gx0, yr, 1280, yr + 58), ((1280 - 2 * (1280 - gx0)) / 2.0, yz2))
    c2 = ws.label_well(cv, (20, r2[3] + 14), 'ZOOM 2x · RIGHT · STATE 5', 'label_b', fg='blade_hi', h=20, pad=8)
    ws.label_well(cv, (c2[2] + 6, r2[3] + 14), note_r, 'body_dim', h=20, pad=8)
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 03 settings: INPUT CALIBRATION
# --------------------------------------------------------------------------------------------------------------------
def screen_03_settings_calibration():
    cv, _bar = rig_backdrop('03_settings_calibration', ws.TopBarState(uncal=True), ws.RIG_RECT, keep_inspector=True)
    ov = ws.overlay_frame(cv, (10, 68, 930, 794), 'SETTINGS', close=True, done='DONE', name='settings')
    cx0, cy0, cx1, cy1 = ov['content']
    # ---- section list (left) ---------------------------------------------------------------------------------------
    sections = ['SETUP CHECKLIST', 'TOOLS', 'TONE3000', 'INPUT CALIBRATION', 'CAPTURES', 'SEPARATION', 'RECORDING',
                'APPEARANCE', 'ABOUT SAWBLADE…']
    ly = cy0
    for s in sections:
        r = (cx0, ly, cx0 + 196, ly + 30)
        sel = s == 'INPUT CALIBRATION'
        list_row(cv, r, sel)
        if sel:
            ws.text(cv, (r[0] + 12, (r[1] + r[3]) / 2.0), '▶ ' + s, 'button', bg=ws.inset(r, 3), fg='blade_hi')
        else:
            ws.text(cv, (r[0] + 12, (r[1] + r[3]) / 2.0), s, 'button', bg=ws.inset(r, 3), fg='bone_dim')
        ly += 36
    # ---- body ---------------------------------------------------------------------------------------------------------
    body = (cx0 + 210, cy0, cx1, cy1)
    flat_box(cv, body, 'well', 'steel_bare', 4)
    bx0, bx1 = body[0] + 18, body[2] - 18
    y = body[1] + 24
    ws.text(cv, (bx0, y), 'INPUT CALIBRATION', 'title', bg=body, size=22)
    y += 34
    # status strips: not calibrated now, calibrated after APPLY
    s1 = (bx0, y, bx1, y + (42 if ws.is_v3() else 34))
    flat_box(cv, s1, 'well_raised', 'warn', 3)
    if ws.is_v3():
        ws.glow_led(cv, s1[0] + 16, (s1[1] + s1[3]) / 2.0, True, 'amber', 4.0)
        ws.dm_display(cv, (s1[0] + 34, s1[1] + 2, s1[2] - 4, s1[3] - 2),
                      ['UNCAL · interface not calibrated', 'captures play at the +9.0 dBu default · NOW'], h=11, tone='amber', row_gap=1.5, pad=5)
    else:
        b = ws.badge(cv, (s1[0] + 10, y + 17), 'UNCAL')
        ws.text(cv, (b[2] + 10, y + 17), '— interface not calibrated: captures play at the +9.0 dBu default', 'body',
                bg=ws.inset(s1, 3))
        ws.text(cv, (s1[2] - 10, y + 17), 'NOW', 'label_b', bg=ws.inset(s1, 3), align='r', fg='warn')
    y += 48 if ws.is_v3() else 40
    s2 = (bx0, y, bx1, y + (42 if ws.is_v3() else 30))
    flat_box(cv, s2, 'well_raised', 'ok', 3)
    if ws.is_v3():
        ws.glow_led(cv, s2[0] + 16, (s2[1] + s2[3]) / 2.0, True, 'green', 4.0)
        ws.dm_display(cv, (s2[0] + 34, s2[1] + 2, s2[2] - 4, s2[3] - 2),
                      ['CALIBRATED · Scarlett 4i4 3rd Gen · INST · +12.5 dBu', '2026-10-08 · AFTER APPLY'], h=11, tone='green', row_gap=1.5, pad=5)
    else:
        ws.text(cv, (s2[0] + 10, y + 15), '✓ CALIBRATED · Scarlett 4i4 3rd Gen · INST · +12.5 dBu · 2026-10-08', 'body_strong',
                bg=ws.inset(s2, 3), fg='ok')
        ws.text(cv, (s2[2] - 10, y + 15), 'AFTER APPLY', 'label_b', bg=ws.inset(s2, 3), align='r', fg='ok')
    y += 56 if ws.is_v3() else 46
    # INTERFACE dropdown, shown open
    ws.text(cv, (bx0, y), 'INTERFACE', 'label_b', bg=body)
    y += 16
    ws.dropdown(cv, (bx0, y, bx0 + 392, y + 28), 'Scarlett 4i4 3rd Gen — INST +12.5 dBu')
    y += 32
    opts = ['Scarlett 4i4 3rd Gen — INST +12.5 dBu', 'Scarlett 4i4 3rd Gen — INST + PAD +14 dBu',
            'Scarlett 4i4 4th Gen — INST +12 dBu', 'Custom — enter dBu', 'Not sure — measure it (guided)']
    list_x1 = bx0 + 392
    top_list = y
    cv.fill((bx0 - 1, top_list - 1, list_x1 + 1, top_list + len(opts) * 26 + 3), 'steel_bare', 2)
    for i, o in enumerate(opts):
        r = (bx0, top_list + i * 26, list_x1, top_list + i * 26 + 24)
        sel = i == 0
        cv.fill(r, 'well_raised' if sel else 'well', 0)
        if sel:
            cv.fill((r[0], r[1], r[0] + 4, r[3]), 'blade')
            ws.text(cv, (r[0] + 12, (r[1] + r[3]) / 2.0), '▶ ' + o, 'body_strong', bg=ws.inset(r, 3))
        else:
            ws.text(cv, (r[0] + 24, (r[1] + r[3]) / 2.0), o, 'body_dim', bg=ws.inset(r, 3))
    note = (list_x1 + 18, top_list - 32, bx1, top_list + len(opts) * 26 + 2)
    ws.text(cv, (note[0], note[1] + 12), 'NOTE', 'label_b', bg=body, fg='warn')
    lines(cv, note[0], note[1] + 36, ['Levels are the instrument', 'input\'s max level from the', 'maker\'s spec sheet.'],
          body, 'body_dim', 19)
    y = top_list + len(opts) * 26 + 14
    # MAX INPUT LEVEL (left) and INPUT CHANNEL (right)
    colr = bx0 + 400
    ws.text(cv, (bx0, y), 'MAX INPUT LEVEL', 'label_b', bg=body)
    ws.text(cv, (bx0 + 124, y), 'Enter dBu', 'body_dim', bg=body)
    ws.text(cv, (colr, y), 'INPUT CHANNEL', 'label_b', bg=body)
    cs = (colr + 100, y - 11, bx1 - 4, y + 11)
    ws.well(cv, cs, 'well', 2)
    ws.text(cv, (cs[0] + 8, y), 'Input: L only (auto)', 'body_strong', bg=cs, fg='ok')
    y += 16
    ws.text_field(cv, (bx0, y, bx0 + 100, y + 30), '+12.5', caret=True)
    ws.button(cv, (bx0 + 106, y, bx0 + 134, y + 30), '−', 'secondary')
    ws.button(cv, (bx0 + 138, y, bx0 + 166, y + 30), '+', 'secondary')
    ws.lcd(cv, (bx0 + 190, y + 15), '+12.5', 'dBu', digits=4, h=22)
    if ws.is_v3():
        ws.led_meter(cv, (bx0, y + 38, bx0 + 340, y + 82), 'IN', -14.0, -8.0, ('-14', 'dBFS', 3), ticks=(-48, -24, -12, 0), n=22)
    else:
        ws.text(cv, (bx0, y + 50), 'Loudest level the input takes before it clips.', 'body_dim', bg=body)
    ws.rotary_selector(cv, colr + 118, y + 40, 52, ['AUTO', 'L', 'R', 'MIX'], 0, 'blade')
    y += 96 if ws.is_v3() else 80
    ws.text(cv, (bx0, y), 'AUTO: uses the channel with signal; MIX sums L+R (−6 dB)', 'body_dim', bg=body)
    y += 12 if ws.is_v3() else 18
    hsep(cv, bx0, bx1, y)
    y += 12 if ws.is_v3() else 14
    # CALIBRATED LEVELS toggle + BETA, GATE FLOOR
    ws.text(cv, (bx0, y), 'CALIBRATED LEVELS', 'label_b', bg=body)
    ws.badge(cv, (bx0 + ws.text_width('CALIBRATED LEVELS', 'label_b') + 14, y), 'BETA', h=18)
    ws.text(cv, (colr, y), 'GATE FLOOR', 'label_b', bg=body)
    y += 28 if ws.is_v3() else 34
    ws.toggle(cv, bx0 + 56, y, 1, ['OFF', 'ON'], size=30, key='tg_cal')
    ws.lcd(cv, (colr, y), '-42.0', 'dBFS', digits=4, h=22)
    ws.text(cv, (colr + 150, y), 'LEARNED', 'label_b', bg=body, fg='ok')
    if ws.is_v3():
        ws.text(cv, (bx0 + 134, y - 6), 'Every capture gets the level its', 'body_dim', bg=body)
        ws.text(cv, (bx0 + 134, y + 11), 'creator used, computed on load.', 'body_dim', bg=body)
        y += 32
        ws.text(cv, (colr, y - 2), 'read-only · from LEARN GATE', 'body_dim', bg=body)
    else:
        y += 32
        ws.text(cv, (bx0, y), 'Every capture gets the level its creator used, computed on load.', 'body_dim', bg=body)
        ws.text(cv, (colr, y), 'read-only · from LEARN GATE', 'body_dim', bg=body)
    # buttons
    by = body[3] - (14 if ws.is_v3() else 20) - 30
    ws.button(cv, (bx0, by, bx0 + 100, by + 30), 'APPLY', 'primary')
    ws.button(cv, (bx0 + 110, by, bx0 + 240, by + 30), 'MEASURE…', 'secondary')
    ws.text(cv, (bx0 + 254, by + 15), 'guided: play the reference tone, read the level', 'body_dim', bg=body)
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 04 preset browser
# --------------------------------------------------------------------------------------------------------------------
# (name, category, parse_ok)
USER_PRESETS = [
    ('BARBARIC · MATCHED v2', 'Swedish death', True),
    ('GRAVE DIRT · MATCHED v2', 'Death', True),
    ('THRASH TIGHT', 'Thrash', True),
    ('COLD STEEL TREMOLO', 'Black metal', True),
    ('TAR PIT CRAWL', 'Doom / sludge', True),
    ('WIRE WHEEL · MATCHED v1', 'Metalcore / djent', True),
    ('SPLINTER CRUST', 'Crust / hardcore', True),
    ('PIT FLOOR', 'Crust / hardcore', True),
    ('DRONE LUMBER', 'Doom / sludge', True),
    ('old_take_final2.json', '', False),
    ('BLEACHED BONES', 'Black metal', True),
    ('CHUG LATHE', 'Metalcore / djent', True),
    ('REAPER BENCH', 'Thrash', True),
    ('GUTTER MILL', 'Crust / hardcore', True),
    ('HEAVY HEWN', 'Death', True),
    ('SMOKED OAK FUZZ', 'Doom / sludge', True),
]
CATEGORIES = ['Thrash', 'Death', 'Swedish death', 'Black metal', 'Doom / sludge', 'Metalcore / djent', 'Crust / hardcore']


def screen_04_preset_browser():
    cv, _bar = rig_backdrop('04_preset_browser', ws.TopBarState(preset='GRAVE DIRT · MATCHED v2'),
                            (0, 58, 1280, 800))
    ov = ws.overlay_frame(cv, (10, 68, 1270, 794), 'PRESETS', close=True, name='presets')
    x0, y0, x1, y1 = ov['content']
    foot_y = y1 - 36
    # ---- left column ---------------------------------------------------------------------------------------------------
    lx1 = x0 + 240
    ws.text_field(cv, (x0, y0, lx1, y0 + 28), 'search presets', 'body', placeholder=True)
    y = y0 + 50
    ws.label_well(cv, (x0, y), 'BANKS', 'label_b', h=16, pad=6)
    y += 20
    ws.label_well(cv, (x0, y + 8), 'FACTORY', 'label', h=16, pad=6)
    y += 20
    tw = (lx1 - x0 - 8) / 3.0
    for i, nm in enumerate(['CLASSIC', 'STYLES', 'MATCHED']):
        ws.tab(cv, (x0 + i * (tw + 4), y, x0 + i * (tw + 4) + tw, y + 28), nm, active=False)
    y += 36
    ws.tab(cv, (x0, y, lx1, y + 30), 'USER · 16 FILES', active=True)
    y += 46
    ws.label_well(cv, (x0, y), 'CATEGORIES', 'label_b', h=16, pad=6)
    y += 14
    counts = {c: sum(1 for (_n, cat, ok) in USER_PRESETS if cat == c) for c in CATEGORIES}
    rows = [('ALL', sum(counts.values()))] + [(c.upper(), counts[c]) for c in CATEGORIES]
    for i, (nm, n) in enumerate(rows):
        r = (x0, y + i * 28, lx1, y + i * 28 + 26)
        sel = i == 0
        list_row(cv, r, sel)
        ws.text(cv, (r[0] + 12, (r[1] + r[3]) / 2.0), ('▶ ' if sel else '') + nm, 'button' if sel else 'label',
                bg=ws.inset(r, 3), fg='blade_hi' if sel else 'bone')
        ws.text(cv, (r[2] - 12, (r[1] + r[3]) / 2.0), '%d' % n, 'mono', bg=ws.inset(r, 3), align='r')
    # ---- middle: preset list ------------------------------------------------------------------------------------------
    mx0, mx1 = lx1 + 14, lx1 + 14 + 432
    hr = (mx0, y0, mx1, y0 + 26)
    ws.well(cv, hr, 'well_raised', 2)
    ws.text(cv, (hr[0] + 26, y0 + 13), 'NAME', 'label_b', bg=ws.inset(hr, 2))
    ws.text(cv, (hr[0] + 252, y0 + 13), 'CATEGORY', 'label_b', bg=ws.inset(hr, 2))
    ws.text(cv, (hr[2] - 12, y0 + 13), 'BANK', 'label_b', bg=ws.inset(hr, 2), align='r')
    ry = y0 + 32
    pitch = 34
    for i, (nm, cat, ok) in enumerate(USER_PRESETS):
        r = (mx0, ry + i * pitch, mx1, ry + i * pitch + 31)
        sel = nm == 'GRAVE DIRT · MATCHED v2'
        list_row(cv, r, sel)
        yc = (r[1] + r[3]) / 2.0
        inner = ws.inset(r, 3)
        if ok:
            ws.text(cv, (r[0] + (12 if not sel else 12), yc), ('▶ ' if sel else '') + nm,
                    'body_strong' if sel else 'body', bg=inner)
            ws.text(cv, (hr[0] + 252, yc), cat, 'body_dim', bg=inner)
        else:
            ws.text(cv, (r[0] + 12, yc), nm, 'body', bg=inner, fg='bone_mute')
            ws.chip(cv, (hr[0] + 252, yc), 'PARSE ERROR', 'alert', glyph='warn', h=22)
        ws.text(cv, (r[2] - 12, yc), 'USER', 'label', bg=inner, align='r', fg='bone_mute' if not ok else None)
    ws.label_well(cv, (mx0, ry + len(USER_PRESETS) * pitch + 12), '16 files · 15 loadable · 1 unparseable', 'body_dim', h=20, pad=8)
    # ---- right: info panel ---------------------------------------------------------------------------------------------
    ix0, ix1 = mx1 + 14, x1
    hint = (ix0, y0, ix1, y0 + 118)
    ws.plate(cv, hint, None, 'alu')
    hb = (ix0 + 8, y0 + 8, ix1 - 8, y0 + 110)
    flat_box(cv, hb, 'well', 'steel_bare', 3)
    if ws.is_v3():
        bd = ws.state_led(cv, hb[0] + 12, y0 + 26, True, 'amber', 'LEGACY LEVELS', r=4.0)
    else:
        bd = ws.badge(cv, (hb[0] + 12, y0 + 26), 'LEGACY LEVELS')
    ws.text(cv, (bd[2] + 10, y0 + 26), '· saved before input calibration.', 'body', bg=hb)
    ws.text(cv, (hb[0] + 12, y0 + 49), 'It plays with the input gains it was saved with.', 'body', bg=hb)
    ws.button(cv, (hb[0] + 12, y0 + 66, hb[0] + 214, y0 + 96), 'USE CALIBRATED LEVELS', 'primary')
    ws.button(cv, (hb[0] + 224, y0 + 66, hb[0] + 360, y0 + 96), 'KEEP AS SAVED', 'secondary')
    y = y0 + 142
    if ws.is_v3():
        nr = ws.dm_display(cv, (ix0, y - 18, ix0 + 318, y + 18), 'GRAVE DIRT · MATCHED v2', h=14, tone='amber', pad=6)
    else:
        nr = ws.label_well(cv, (ix0, y), 'GRAVE DIRT · MATCHED v2', 'title', fg='bone', size=18, h=32, pad=10)
    y += 30
    r = ws.label_well(cv, (ix0, y), 'CATEGORY · DEATH', 'label_b', h=20, pad=8)
    r = ws.label_well(cv, (r[2] + 6, y), 'BANK · USER', 'label_b', h=20, pad=8)
    ws.label_well(cv, (r[2] + 6, y), 'grave_dirt_matched_v2.json', 'mono_dim', h=22, pad=8)
    y += 20
    notes = (ix0, y, ix1, y + 50)
    flat_box(cv, notes, 'well', 'steel_bare', 3)
    ws.text(cv, (notes[0] + 10, y + 16), 'Matched from a Swedish-style death reference; tight low end.', 'body_dim', bg=notes)
    ws.text(cv, (notes[0] + 10, y + 35), 'The saw leans on the SERRATED circuit. Notes are free text.', 'body_dim', bg=notes)
    y += 70
    ws.label_well(cv, (ix0, y), 'CAPTURES · 5 · FETCHED FROM TONE3000 BY ID', 'label_b', h=16, pad=6)
    y += 16
    caps = [
        ('BLADE', 'blade', 'PEDAL', 'Swedish chainsaw pedal', '@coldiron_caps · cc-by · tone3000.com/tones/20417', None),
        ('BLADE', 'blade', 'AMP', 'British-style head, bright cap', '@marrow_amps · cc-by-nc · tone3000.com/tones/31207', 'NON-COMMERCIAL'),
        ('BODY', 'body', 'BOOST', 'Mid-focus overdrive, tight', '@coldiron_caps · cc-by · tone3000.com/tones/18840', None),
        ('BODY', 'body', 'AMP', 'High-gain US head', '@swamp_rig · cc-by-sa · tone3000.com/tones/9915', None),
        ('CAB', 'bone', 'IR', 'closed_4x12_dyn_cap.wav', None, None),
    ]
    ch = 52
    for i, (path, col, kind, title, credit, badge) in enumerate(caps):
        r = (ix0, y + i * (ch + 3), ix1, y + i * (ch + 3) + ch)
        ws.well(cv, r, 'well', 3)
        cv.fill((r[0], r[1] + 2, r[0] + 3, r[3] - 2), col)
        inner = ws.inset(r, 3)
        ws.text(cv, (r[0] + 12, r[1] + 14), path + ' · ' + kind, 'label_b', bg=inner, fg=col if col != 'bone' else 'bone_dim')
        ws.text(cv, (r[0] + 112, r[1] + 15), title, 'body_strong', bg=inner)
        if credit:
            ws.text(cv, (r[0] + 12, r[1] + 36), credit, 'body_dim', bg=inner)
        else:
            ws.text(cv, (r[0] + 12, r[1] + 36), '▲ local file: no attribution recorded', 'body', bg=inner, fg='warn')
        if badge:
            ws.badge(cv, (r[2] - 10, r[1] + 15), badge, align='r')
    # ---- footer ----------------------------------------------------------------------------------------------------------
    hsep(cv, x0, x1, foot_y - 8)
    bx = x0
    for nm, w, kind in (('LOAD FILE…', 112, 'secondary'), ('SAVE', 72, 'secondary'), ('SAVE AS', 86, 'secondary'),
                        ('RENAME', 88, 'secondary'), ('DELETE', 88, 'danger')):
        ws.button(cv, (bx, foot_y, bx + w, foot_y + 30), nm, kind)
        bx += w + 8
    sr = (bx + 14, foot_y - 1, x1 - 118, foot_y + 31)
    if ws.is_v3():
        ws.dm_display(cv, (sr[0], sr[1], sr[2] - 190, sr[3]), 'Resolving 2/5: Swedish chainsaw pedal', h=11, tone='green', pad=5)
        ws.led_bar(cv, (sr[2] - 182, foot_y + 2, sr[2], foot_y + 28), 0.4, n=16)
    else:
        cv.fill(sr, 'steel_bare', 3)
        cv.fill(ws.inset(sr, 1), 'bench_dark', 3)
        cv.fill(ws.inset(sr, 2), 'glass', 2)
        ws.text(cv, (sr[0] + 12, foot_y + 15), 'Resolving 2/5: Swedish chainsaw pedal', 'lcd_unit', bg=ws.inset(sr, 3))
        ws.progress(cv, (sr[2] - 170, foot_y + 8, sr[2] - 10, foot_y + 24), 0.4)
    ws.button(cv, (x1 - 104, foot_y, x1, foot_y + 30), 'CANCEL', 'secondary')
    return cv


# --------------------------------------------------------------------------------------------------------------------
# 09 notices
# --------------------------------------------------------------------------------------------------------------------
def rail_piece(cv, rect, key):
    cv.shadow(rect, 2, (0, 3), 6, 0.6)
    cv.texture(rect, ws.alu_tex(int(rect[2] - rect[0]), int(rect[3] - rect[1]), key), 2)


def screen_09_notices():
    cv = ws.new_screen('09_notices', sawdust_n=500)
    ws.top_bar(cv, ws.TopBarState(out_of_true=True, uncal=True))
    cap = lambda x, y, s: ws.label_well(cv, (x, y), s, 'label_b', fg='blade_hi', h=18, pad=8)  # noqa: E731
    # ---- OUT OF TRUE banner ---------------------------------------------------------------------------------------------
    cap(20, 78, 'OUT OF TRUE · BANNER AT THE TOP OF THE RIG')
    b = (20, 94, 960, 182)
    ws.panel(cv, b, 'oot', 1.1, 6, rivets=True)
    cv.outline(b, 'alert', 2, 6)
    gw = (36, 108, 82, 168)
    flat_box(cv, gw, 'well', 'alert', 3)
    ws.glyph_icon(cv, 'skew', 59, 138, 30, 'alert')
    tw = (92, 108, 690, 168)
    flat_box(cv, tw, 'well', 'steel_bare', 3)
    lx = 12
    if ws.is_v3():
        ws.glow_led(cv, tw[0] + 14, 126, True, 'red', 4.0)
        lx = 32
    ws.text(cv, (tw[0] + lx, 126), 'OUT OF TRUE', 'body_strong', bg=tw, fg='alert')
    ws.text(cv, (tw[0] + lx + ws.text_width('OUT OF TRUE', 'body_strong') + 6, 126),
            '· Your playing level is running ~4.5 dB hotter than', 'body', bg=tw)
    ws.text(cv, (tw[0] + 12, 150), 'when this interface was set up — did the interface gain change?', 'body', bg=tw)
    ws.button(cv, (708, 108, 934, 136), 'RE-CALIBRATE', 'primary')
    ws.button(cv, (708, 142, 934, 170), 'IGNORE THIS SESSION', 'secondary')
    # chip form
    cap(984, 78, 'COMPACT CHIP · TOP BAR')
    rp = (984, 94, 1260, 140)
    rail_piece(cv, rp, 'noticeA')
    if ws.is_v3():
        ws.state_led(cv, rp[0] + 14, 117, True, 'red', 'OUT OF TRUE · +4.5 dB', r=4.0, style='label_mx_b')
        ws.label_well(cv, (984, 160), 'glowing lamp + word + dB offset', 'body_dim', h=20, pad=8)
    else:
        ws.chip(cv, (rp[0] + 14, 117), 'OUT OF TRUE · +4.5 dB', 'alert', glyph='skew', h=24)
        ws.label_well(cv, (984, 160), 'word + skew glyph + dB offset', 'body_dim', h=20, pad=8)
    # ---- UNCAL banner ---------------------------------------------------------------------------------------------------------
    cap(20, 204, 'UNCALIBRATED · BANNER')
    b2 = (20, 220, 960, 286)
    ws.panel(cv, b2, 'uncal', 1.1, 6, rivets=True)
    cv.outline(b2, 'warn', 2, 6)
    tw2 = (36, 232, 700, 274)
    flat_box(cv, tw2, 'well', 'steel_bare', 3)
    if ws.is_v3():
        bd = ws.state_led(cv, tw2[0] + 12, 253, True, 'amber', 'UNCAL', r=4.0)
    else:
        bd = ws.badge(cv, (tw2[0] + 12, 253), 'UNCAL')
    ws.text(cv, (bd[2] + 10, 244), '· interface not calibrated — captures play at the +9.0 dBu default.', 'body', bg=tw2)
    ws.text(cv, (bd[2] + 10, 263), 'Levels are a guess until you calibrate.', 'body_dim', bg=tw2)
    ws.button(cv, (718, 233, 940, 273), 'CALIBRATE IN SETTINGS', 'secondary')
    cap(984, 204, 'COMPACT CHIP · TOP BAR')
    rp2 = (984, 220, 1260, 266)
    rail_piece(cv, rp2, 'noticeB')
    if ws.is_v3():
        ws.state_led(cv, rp2[0] + 12, 243, True, 'amber', 'UNCAL · interface not calibrated', r=4.0, style='label_mx_b')
    else:
        ws.chip(cv, (rp2[0] + 12, 243), 'UNCAL · interface not calibrated', 'warn', glyph='warn', h=24)
    # ---- UNCAL badge placements -------------------------------------------------------------------------------------------
    cap(20, 312, 'UNCAL BADGE · ON A BLOCK CARD')
    card = ws.card(cv, (20, 326, 320, 440), 'SAW HEAD', accent='blade', status='CAPTURE')
    bx, by = card['body'][0] + 6, card['body'][1] + 18
    ws.badge(cv, (bx, by), 'UNCAL')
    ws.label_well(cv, (bx, by + 28), 'no level metadata — default +9 dBu', 'body_dim', h=22, pad=8)
    ws.label_well(cv, (bx, by + 54), '@marrow_amps · cc-by-nc · VIA TONE3000', 'body_dim', h=22, pad=8)
    cap(344, 312, 'UNCAL BADGE · ON AN AMP HEAD LABEL')
    am = screens_rig.amp_head(cv, 'amp_saw.png', 344, 326, w=250)
    lr = ws.label_well(cv, (344, am[3] + 14), 'BLADE · SAW HEAD', 'label', fg='blade_hi', h=24, pad=8)
    ws.badge(cv, (lr[2] + 10, am[3] + 14), 'UNCAL')
    cap(640, 312, 'UNCAL BADGE · IN A RENDER-REPORT LINE')
    rr = (640, 326, 1260, 440)
    flat_box(cv, rr, 'well', 'steel_bare', 3)
    mono = [('render report · GRAVE DIRT · MATCHED v2', None),
            ('latency 92 smp · 48000 Hz · block 128', None),
            ('BLADE  SAW HEAD   input level +9.0 dBu (default)', 'UNCAL'),
            ('BODY   BODY HEAD  input level +12.5 dBu (header)', None)]
    for i, (ln, bg_) in enumerate(mono):
        yy = rr[1] + 22 + i * 26
        ws.text(cv, (rr[0] + 14, yy), ln, 'mono' if i else 'mono_dim', bg=rr)
        if bg_:
            ws.badge(cv, (rr[0] + 14 + ws.text_width(ln, 'mono') + 14, yy), bg_, h=20)
    # ---- dialogs and toast ----------------------------------------------------------------------------------------------------
    cap(20, 484, 'CONFIRM DIALOG')
    d1 = ws.overlay_frame(cv, (20, 500, 440, 716), 'DELETE TAKE?', close=False, name='dlg1')
    c = d1['content']
    flat_box(cv, (c[0], c[1], c[2], c[1] + 98), 'well', 'steel_bare', 3)
    bg1 = (c[0], c[1], c[2], c[1] + 98)
    lines(cv, c[0] + 12, c[1] + 20, ['This removes take_2026-10-08_2114 from the', 'take list and from disk. A match that used it',
                                      'keeps its results.'], bg1, 'body', 20)
    ws.text(cv, (c[0] + 12, c[1] + 80), '▲ This cannot be undone.', 'body_strong', bg=bg1, fg='warn')
    by = c[3] - 36
    ws.button(cv, (c[2] - 218, by + 6, c[2] - 112, by + 36), 'DELETE', 'danger')
    ws.button(cv, (c[2] - 100, by + 6, c[2], by + 36), 'CANCEL', 'secondary')
    cap(464, 484, 'ERROR DIALOG')
    d2 = ws.overlay_frame(cv, (464, 500, 884, 716), 'MATCH FAILED', close=False, name='dlg2')
    c = d2['content']
    bg2 = (c[0], c[1], c[2], c[1] + 98)
    flat_box(cv, bg2, 'well', 'alert', 3)
    ws.glyph_icon(cv, 'warn', c[0] + 24, c[1] + 24, 20, 'alert')
    lines(cv, c[0] + 48, c[1] + 20, ['The reference song is too short to match:', '3.2 s found, at least 10 s needed.'], bg2,
          'body', 20)
    ws.text(cv, (c[0] + 12, c[1] + 70), 'Nothing in your rig was changed.', 'body_dim', bg=bg2)
    by = c[3] - 36
    ws.button(cv, (c[2] - 218, by + 6, c[2] - 112, by + 36), 'LOCATE…', 'secondary')
    ws.button(cv, (c[2] - 100, by + 6, c[2], by + 36), 'CLOSE', 'primary')
    cap(908, 484, 'TOAST')
    t = (908, 504, 1260, 552 + (22 if ws.is_v3() else 0))
    ws.panel(cv, t, 'toast', 0.9, 6, rivets=False)
    tb = (t[0] + 8, t[1] + 8, t[2] - 8, t[3] - 8)
    flat_box(cv, tb, 'well', 'ok', 3)
    if ws.is_v3():
        ws.dm_display(cv, (tb[0] + 2, tb[1] + 2, tb[2] - 2, tb[3] - 2), ['Preset saved:', 'GRAVE DIRT · MATCHED v3'], h=14, tone='green', row_gap=1.6, pad=6)
    else:
        ws.glyph_icon(cv, 'check', tb[0] + 18, (tb[1] + tb[3]) / 2.0, 14, 'ok')
        ws.text(cv, (tb[0] + 34, (tb[1] + tb[3]) / 2.0), 'Preset saved: GRAVE DIRT · MATCHED v3', 'body', bg=tb)
    ws.label_well(cv, (908, 574 + (22 if ws.is_v3() else 0)), 'slides in bottom-right, fades after 4 s', 'body_dim', h=20, pad=8)
    return cv


SCREENS = {'02_top_bar': screen_02_top_bar,
           '03_settings_calibration': screen_03_settings_calibration,
           '04_preset_browser': screen_04_preset_browser,
           '09_notices': screen_09_notices}
