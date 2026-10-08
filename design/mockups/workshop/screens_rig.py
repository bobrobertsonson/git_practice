"""Workshop mockup screens: the rig (screen 01).  Each screens_*.py module exposes ``SCREENS = {'NN_name': fn}``; ``fn()``
returns a ``workshop_style.Canvas`` (render_all.py finishes, saves and contrast-checks it)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'render')))
import workshop_style as ws  # noqa: E402

CAB_CROP = (37, 37, 623, 610)   # the render has an opaque studio backdrop around the cab: trim it
RIG_Y = 58          # the rig area starts below the 58 px top bar


# The Blender renders in plugin/assets are opaque and carry a studio backdrop around the object; crop it off (source px).
AMP_CROP = (4, 0, 656, 284)
PEDAL_SAW_CROP = (21, 19, 339, 521)
PEDAL_BODY_CROP = (12, 18, 268, 402)


def amp_head(cv, name, x, y, w=330):
    """An amp-head render (backdrop cropped) with a floor shadow.  The baked OLED art names a real amp model, so a flat OLED
    patch covers it (Task B re-renders the faces).  Returns the rect."""
    spr = ws.sprite_image(name, w, crop=AMP_CROP, round_px=6)
    h = spr.height / ws.S
    rect = (x, y, x + w, y + h)
    sc = w / float(AMP_CROP[2] - AMP_CROP[0])
    cv.shadow((x + 8, y + 10, x + w - 8, y + h), 8, (0, 12), 18, 0.8)
    cv.sprite(spr, x + w / 2.0, y + h / 2.0)
    ws.oled(cv, (x + (26 - AMP_CROP[0]) * sc, y + 49 * sc, x + (100 - AMP_CROP[0]) * sc, y + 77 * sc))
    return rect


def pedal_face(cv, name, crop, x, y, w, plate_text, plate_rect, oled_rect=None, oled_lines=None, selected=False, oled_size=14):
    """A pedal render (backdrop cropped) with a riveted nameplate over the baked title and, optionally, a flat OLED well
    over the baked OLED (the baked titles are STOCKHOLM SYNDROME / TIGHTEN; Task B re-renders the faces).  Rects are in
    logical px relative to the pedal's top-left.  Returns the pedal rect."""
    spr = ws.sprite_image(name, w, crop=crop, round_px=16)
    h = spr.height / ws.S
    rect = (x, y, x + w, y + h)
    cv.shadow((x + 6, y + 10, x + w - 6, y + h), 10, (0, 10), 14, 0.8)
    cv.sprite(spr, x + w / 2.0, y + h / 2.0)
    ws.nameplate(cv, (x + plate_rect[0], y + plate_rect[1], x + plate_rect[2], y + plate_rect[3]), plate_text)
    if oled_rect:
        ws.oled(cv, (x + oled_rect[0], y + oled_rect[1], x + oled_rect[2], y + oled_rect[3]), oled_lines, size=oled_size)
    if selected:
        r = (x - 5, y - 5, x + w + 5, y + h + 5)
        cv.outline(r, 'blade', 3, 14)
        cv.outline((x - 7.5, y - 7.5, x + w + 7.5, y + h + 7.5), 'blade', 1, 16)
    return rect


def meters_panel(cv, rect, key='meters'):
    """v3: IN / OUT / VISE gain-reduction LED ladders in a worn-steel panel (ladders are 24 segments, peak hold, dB ticks, LCD value)."""
    ws.panel(cv, rect, key, 0.9, 6, rivets=False)
    x0, y0, x1, y1 = rect
    ws.plate(cv, (x0 + 6, y0 + 6, x1 - 6, y0 + 34), 'METERS', key=key + ':p')
    ix0, ix1 = x0 + 12, x1 - 12
    y = y0 + 40
    ws.led_meter(cv, (ix0, y, ix1, y + 68), 'IN', -17.0, -9.0, ('-17', 'dB', 3), stack=True)
    y += 71
    ws.led_meter(cv, (ix0, y, ix1, y + 68), 'OUT', -11.0, -6.0, ('-11', 'dB', 3), stack=True)
    y += 71
    ws.led_meter(cv, (ix0, y, ix1, y + 68), 'GR', 3.0, 5.5, ('3.0', 'dB', 4), lo=0.0, hi=12.0, ticks=(0, 6, 12), stack=True)


def screen_01_main_rig():
    cv = ws.new_screen('01_main_rig', sawdust_n=0)
    # ---- the floor: planks + sawdust, piled against the bottoms of everything that stands on it --------------------
    amp_saw_r = (56, RIG_Y + 6, 386, RIG_Y + 6 + 143.7)
    amp_body_r = (56, RIG_Y + 190, 386, RIG_Y + 190 + 143.7)
    cab_r = (440, RIG_Y + 6, 770, RIG_Y + 6 + 330 * 573 / 586.0)
    board_r = (100, 436, 924, 792)
    dust_clumps = [(220, amp_saw_r[3] + 22, 130, 260), (230, amp_body_r[3] + 20, 140, 260), (600, cab_r[3] + 18, 150, 300),
                   (180, board_r[1] - 8, 170, 190), (860, board_r[1] - 6, 100, 110), (60, 420, 40, 60)]
    ws.sawdust(cv, (0, RIG_Y, 940, 800), 380, '01:floor', clumps=dust_clumps,
               seams=ws.plank_layout(ws.PAGE_H, 'floor')[1:-1], seam_n=40)

    # ---- cables (behind the gear) ---------------------------------------------------------------------------------
    ws.cable(cv, ws.bezier((190, 486), (110, 452), (16, 330), (62, RIG_Y + 82)), 'blade')
    ws.cable(cv, ws.bezier((452, 560), (330, 480), (38, 500), (62, RIG_Y + 266)), 'body')

    # ---- amp heads, cab ---------------------------------------------------------------------------------------------
    amp_head(cv, 'amp_saw.png', amp_saw_r[0], amp_saw_r[1])
    amp_head(cv, 'amp_body.png', amp_body_r[0], amp_body_r[1])
    cv.shadow((cab_r[0] + 14, cab_r[1] + 14, cab_r[2] - 14, cab_r[3]), 8, (0, 12), 20, 0.8)
    cv.sprite(ws.sprite_image('cab_4x12.png', 330, crop=CAB_CROP, round_px=8), (cab_r[0] + cab_r[2]) / 2.0, (cab_r[1] + cab_r[3]) / 2.0)
    ws.label_well(cv, (cab_r[0], cab_r[3] + 14), 'CAB · 4x12 · SHARED · DOUBLE-CLICK FOR MIC', 'label', pad=8)
    ws.label_well(cv, (amp_saw_r[0], amp_saw_r[3] + 14), 'BLADE · THE SAW MILL → SAW HEAD', 'label', fg='blade_hi', pad=8)
    ws.label_well(cv, (amp_body_r[0], amp_body_r[3] + 14), 'BODY · CHISEL → BODY HEAD', 'label', fg='body', pad=8)

    # ---- pedalboard -------------------------------------------------------------------------------------------------
    ws.panel(cv, board_r, 'pedalboard', 1.8, 10, rivets=True)
    saw_x, saw_w = 150, 180
    body_x, body_w = 410, 140
    saw = pedal_face(cv, 'pedal_saw.png', PEDAL_SAW_CROP, saw_x, 470, saw_w, 'THE SAW MILL', (12, 6, 168, 40),
                     oled_rect=(38, 41, 142, 79), oled_lines=['BUZZSAW', 'A2 \u00b7 48k'], selected=True)
    body_y = saw[3] - 140 * 384 / 256.0
    body = pedal_face(cv, 'pedal_body.png', PEDAL_BODY_CROP, body_x, body_y, body_w, 'CHISEL', (8, 5, 132, 29))
    # tape strips under the pedals carry the path labels; one more holds the cable
    ws.gaffer_tape(cv, (saw_x - 14, saw[3] + 12, saw_x + saw_w + 14, saw[3] + 31), 'BLADE · THE SAW MILL', 'blade_hi')
    ws.gaffer_tape(cv, (body_x - 20, body[3] + 12, body_x + body_w + 20, body[3] + 31), 'BODY · CHISEL', 'body')
    for tr, ang in (((330, 424, 410, 442), -3), ((860, 438, 914, 454), 4), ((106, 746, 156, 762), -5), ((590, 758, 668, 775), 2)):
        ws.gaffer_tape(cv, tr, None, angle=ang)       # decoration: holds the cables / patch leads down
    ws.label_well(cv, (saw_x - 5, 464), '▶ SELECTED', 'label_b', fg='blade_hi', pad=7, border='blade')
    # empty slots: flat wells, dashed frames
    sy = body[3] - 190
    ws.empty_slot(cv, (625, sy, 745, body[3]))
    ws.empty_slot(cv, (775, sy, 895, body[3]))

    # ---- v3: the meter panel on the floor right of the cab ----------------------------------------------------------------
    if ws.is_v3():
        meters_panel(cv, (776, RIG_Y + 8, 934, RIG_Y + 264))

    # ---- top bar -----------------------------------------------------------------------------------------------------
    ws.top_bar(cv, ws.TopBarState())

    # ---- inspector ---------------------------------------------------------------------------------------------------
    c = ws.inspector(cv)
    x0, x1 = c[0], c[2]
    cx = (x0 + x1) / 2.0
    y = c[1] + 8
    ws.label_well(cv, (x0, y), 'SELECTED · BLADE PEDAL', 'label', h=18)
    y += 28
    ws.label_well(cv, (x0, y), 'THE SAW MILL', 'section', fg='blade', size=18, pad=9, h=30)
    ws.label_well(cv, (x1, y), 'CIRCUIT', 'label', h=18, align='r')
    y += 60
    ws.rotary_selector(cv, cx, y + 4, 62, ['BUZZSAW', 'TAR PIT', 'SERRATED', 'HATCHET'], 0, 'blade')
    y += 68
    ws.label_well(cv, (x0, y), '@coldiron_caps · cc-by · VIA TONE3000', 'body', h=22, pad=8)
    y += 36
    ws.button(cv, (x0, y - 14, x0 + 160, y + 14), 'BROWSE CAPTURES', 'secondary')
    y += 26
    cv.blend((x0, y, x1, y + 1), 'bench_dark', 0.9)
    cv.blend((x0, y + 1, x1, y + 2), 'bone', 0.1)
    # BLEND
    y += 14
    kcy = y + 42
    ws.knob(cv, x0 + 48, kcy, 84, 'amp', 0.79, 'blade', 'BLEND', ring=True)
    nx0 = x0 + 108
    ws.nixie(cv, (nx0, y + 2, x1, y + 52), '79 / 21', 34)
    ws.label_well(cv, ((nx0 + x1) / 2.0, y + 74), 'BLADE 79 · BODY 21', 'label_b', pad=8, align='c')
    y = kcy + 42 + 36
    ws.label_well(cv, (x0, y), 'ALIGN', 'label', h=18)
    ws.lcd(cv, (x0 + 62, y), '-17', 'smp · Ø NORMAL', digits=3, h=20)
    y += 32
    cv.blend((x0, y, x1, y + 1), 'bench_dark', 0.9)
    cv.blend((x0, y + 1, x1, y + 2), 'bone', 0.1)
    # master knobs 2 x 2
    y += 10
    masters = [('GATE', 0.45, ('-33.1', 'dB')), ('POST EQ', 0.55, ('0.0', 'dB')),
               ('VISE', 0.25, ('2.0', ':1')), ('OUTPUT', 0.5, ('-6.0', 'dB'))]
    for i, (nm, v, (rv, ru)) in enumerate(masters):
        col, row = i % 2, i // 2
        mx = x0 + col * 156
        my = y + row * 62 + 28
        ws.knob(cv, mx + 24, my, 46, 'pedal', v, 'blade', ring=True)
        ws.label_well(cv, (mx + 54, my - 14), nm, 'label_b', h=16, pad=5)
        ws.lcd(cv, (mx + 54, my + 11), rv, ru, digits=4, h=14)
    y += 2 * 62 + 6
    ws.button(cv, (x0, y, x0 + 110, y + 28), 'LEARN GATE', 'secondary')
    ws.label_well(cv, (x0 + 120, y + 14), 'thr −33.1 dB', 'mono_dim', h=22, pad=7)
    y += 42
    pr = (x0 - 6, y, x1 + 6, c[3] + 6)
    ws.panel(cv, pr, 'matchbox', 0.8, 5, shadow=False)
    pl = ws.plate(cv, (pr[0] + 6, pr[1] + 6, pr[2] - 6, pr[1] + 38), 'MATCH vs ORIGINAL', title_style='section_mixed')
    vw = (pr[0] + 12, pr[1] + 46, pr[2] - 12, pr[3] - 8)
    if ws.is_v3():
        ws.dm_display(cv, vw, '6.45 → 1.53 DB · 9/10', h=14, tone='green', align='c')
    else:
        ws.well(cv, vw, 'well', 3)
        ws.text(cv, None, '6.45 → 1.53 dB · 9/10 rules', 'mono', bg=vw)
    return cv


SCREENS = {'01_main_rig': screen_01_main_rig}
