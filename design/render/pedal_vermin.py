"""VERMIN - 110x150x48 mm rat-style distortion pedal: three knobs across the top (DIST, FILTER, VOLUME), LED and
footswitch below.  Motif: a spray-painted stencil RAT silhouette (side profile, scurrying, long tail curling across the
face; stencil bridges, overspray, chips, drips) under the stencilled name, with gnaw marks and drain slots in the margins.  Original artwork; no lettering, logo or layout copied from any real pedal.
Inks: sulphur yellow (ink A, gnaw marks / grate / mis-registered second pass) + bone (ink B, rat and wordmark) on a
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
def _spline(pts, closed=False, n=14):
    """Catmull-Rom through `pts` (list of (x, y) mm); returns a dense polyline."""
    p = list(pts)
    if closed:
        p = [p[-1]] + p + [p[0], p[1]]
    else:
        p = [p[0]] + p + [p[-1]]
    out = []
    for i in range(1, len(p) - 2):
        p0, p1, p2, p3 = (np.array(q, float) for q in p[i - 1:i + 3])
        for t in np.linspace(0, 1, n, endpoint=False):
            out.append(tuple(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3)))
    if not closed:
        out.append(tuple(p[-2]))
    return out


def motif(ctx):
    """bone layer: a spray-painted STENCIL RAT (side profile, scurrying right, long tail curling across the face) with
    stencil bridges, overspray, chips and drips.  yellow layer: the rat's mis-registered second pass, gnaw fans,
    chewed border, drain slots."""
    from PIL import ImageFilter
    rng = ctx.rng; S_ = ctx.S
    P = ctx.P
    A, da = ctx.new()           # yellow
    E, de = ctx.new()           # bone extra (rat)
    lw = lambda mm: max(1, int(mm * S_))
    pp = lambda pts: [P(x, y) for x, y in pts]
    BRIDGE = 1.0                # mm of stencil material left between cut areas

    def bridge(p0, p1, w=BRIDGE):
        """a stencil bridge: a strip of unpainted material across painted ink."""
        de.line((*P(*p0), *P(*p1)), fill=0, width=lw(w))

    # ---- body: one hand-cut outline (nose right), smooth spline through control points
    body = [(41.5, -5.6), (38.5, -1.4), (33.5, 2.4), (28.5, 4.0), (24.0, 5.4), (19.0, 6.8), (12.0, 8.6), (4.0, 9.2), (-4.0, 8.4),
            (-11.0, 6.2), (-16.2, 2.6), (-18.8, -3.0), (-17.2, -8.6), (-13.0, -11.8), (-5.0, -12.4), (4.0, -13.0), (13.0, -13.2),
            (21.0, -12.8), (28.0, -11.4), (34.0, -9.8), (38.6, -8.4)]
    de.polygon(pp(_spline(body, closed=True)), fill=255)
    # round ear (a disc that overlaps the head), set back from the eye
    ex, ey, er = 22.6, 8.6, 4.3
    de.ellipse((*P(ex - er, ey + er), *P(ex + er, ey - er)), fill=255)
    # legs and paws: front leg, second front leg (behind), hind foot (long), each with toes
    de.polygon(pp(_spline([(19.5, -11.5), (21.2, -15.5), (21.8, -17.6), (25.2, -17.9), (28.2, -17.4), (26.6, -16.2), (25.4, -14.6), (25.6, -11.6)], closed=True, n=6)), fill=255)
    de.polygon(pp(_spline([(27.0, -10.8), (29.2, -14.8), (30.6, -17.2), (33.4, -17.5), (35.2, -16.8), (33.4, -15.6), (32.2, -13.6), (31.6, -10.0)], closed=True, n=6)), fill=255)
    de.polygon(pp(_spline([(-14.0, -10.2), (-11.5, -14.8), (-11.0, -17.6), (-6.0, -17.9), (-1.0, -17.5), (-3.2, -16.2), (-6.4, -15.4), (-6.4, -11.8)], closed=True, n=6)), fill=255)
    for (x0, y0) in ((28.0, -17.5), (35.0, -17.0), (-1.4, -17.5)):               # toe cuts
        for k in (0, 1):
            bridge((x0 - 1.7 - 1.3 * k, y0 + 0.6), (x0 - 1.5 - 1.3 * k, y0 - 1.0), 0.5)
    # tail: tapering stroke along a long spline that leaves the rump and curls across the left of the face
    tail_c = _spline([(-17.6, -1.4), (-24.0, 2.6), (-31.0, 5.4), (-37.0, 2.2), (-39.2, -4.0), (-35.4, -9.4), (-29.6, -9.6), (-26.6, -13.6),
                      (-29.4, -18.6), (-35.6, -21.6), (-41.4, -19.2), (-43.4, -14.4), (-40.6, -12.0)], n=10)
    tc = np.array(tail_c)
    seg = np.hypot(*np.diff(tc, axis=0).T)
    s_arc = np.concatenate([[0], np.cumsum(seg)])
    tang = np.gradient(tc, axis=0); tang /= np.hypot(tang[:, 0], tang[:, 1])[:, None]
    nrm = np.stack([-tang[:, 1], tang[:, 0]], axis=1)
    wid = 1.35 * (1.0 - 0.72 * s_arc / s_arc[-1]) + 0.22
    left = tc + nrm * wid[:, None]; right = tc - nrm * wid[:, None]
    de.polygon(pp([tuple(q) for q in left] + [tuple(q) for q in right[::-1]]), fill=255)
    for sa in np.arange(7.0, s_arc[-1] - 3.0, 10.5):                              # tail bridges: a cut every ~10 mm
        i = int(np.searchsorted(s_arc, sa))
        a, b = tc[i] + nrm[i] * 2.6, tc[i] - nrm[i] * 2.6
        bridge(tuple(a), tuple(b), 0.8)
    # whiskers (thin, fanned off the snout)
    for (dx_, dy_) in ((8.0, 3.6), (9.0, -0.6), (8.0, -4.6)):
        de.line([P(37.8, -4.2), P(37.8 + dx_ * 0.55, -4.2 + dy_ * 0.7), P(37.8 + dx_, -4.2 + dy_)], fill=255, width=lw(0.42), joint='curve')

    # ---- stencil bridges inside the body (unpainted strips and islands)
    # ear: the inner ear is an uncut island of the stencil, held by two bridges to the outside
    de.ellipse((*P(ex - 0.2 - 2.2, ey - 0.3 + 2.5), *P(ex - 0.2 + 2.2, ey - 0.3 - 2.5)), fill=0)
    bridge((ex - 0.2, ey + 1.6), (ex - 0.6, ey + 5.2))
    bridge((ex - 0.6, ey - 2.2), (ex - 2.0, ey - 5.0))
    # eye: unpainted almond with a painted pupil; the almond is held by one bridge running up to the forehead edge
    gx, gy = 32.4, 0.4
    de.ellipse((*P(gx - 2.3, gy + 1.7), *P(gx + 2.3, gy - 1.7)), fill=0)
    de.ellipse((*P(gx - 0.5 - 0.9, gy + 0.1 + 1.0), *P(gx - 0.5 + 0.9, gy + 0.1 - 1.0)), fill=255)
    bridge((gx - 1.2, gy + 1.2), (gx - 2.2, gy + 3.6))
    # nostril and mouth line, a cut tooth under the snout
    de.ellipse((*P(39.0, -3.2), *P(40.4, -4.4)), fill=0)
    de.line([P(40.8, -6.8), P(36.6, -7.2), P(33.8, -6.4)], fill=0, width=lw(0.5), joint='curve')
    # shoulder cut-line and haunch cut-line (open arcs that stop short of the outline, so the shape stays one piece)
    de.line(pp(_spline([(18.4, 6.0), (16.2, 0.8), (17.0, -5.0), (19.6, -9.6)], n=8)), fill=0, width=lw(BRIDGE), joint='curve')
    de.line(pp(_spline([(-14.6, 3.4), (-9.0, 0.0), (-7.0, -5.6), (-9.4, -10.4)], n=8)), fill=0, width=lw(BRIDGE), joint='curve')
    # tail root: a cut separates the tail from the rump, with two bridges holding it on
    de.line(pp(_spline([(-16.4, 2.0), (-19.6, -1.6), (-18.8, -4.6)], n=8)), fill=0, width=lw(0.7), joint='curve')
    # spine ridge dashes (bridged line along the back)
    for x0 in np.arange(-10.0, 12.0, 6.0):
        de.line(pp(_spline([(x0, 6.8), (x0 + 2.6, 7.4), (x0 + 4.2, 7.2)], n=5)), fill=0, width=lw(0.5), joint='curve')

    # ---- wear: chips and rub-through on the cut ink (same RNG stream, deterministic)
    mask0 = np.asarray(E, dtype=np.float32) / 255.0
    for _ in range(46):
        ys_, xs_ = np.nonzero(mask0[::8, ::8] > 0.5)
        j = int(rng.integers(0, len(ys_)))
        cx_, cy_ = xs_[j] * 8 + 4, ys_[j] * 8 + 4
        r = rng.uniform(0.25, 1.1) * S_
        de.polygon([(cx_ + r * rng.uniform(0.4, 1.3) * math.cos(a), cy_ + r * rng.uniform(0.4, 1.3) * math.sin(a))
                    for a in np.linspace(0, 2 * math.pi, 9, endpoint=False)], fill=0)
    for _ in range(14):                                                           # rub lines
        ys_, xs_ = np.nonzero(mask0[::8, ::8] > 0.5)
        j = int(rng.integers(0, len(ys_)))
        x, y = xs_[j] * 8 + 4, ys_[j] * 8 + 4
        a = rng.uniform(0, math.pi)
        ln = rng.uniform(2.0, 7.0) * S_
        de.line((x, y, x + ln * math.cos(a), y - ln * math.sin(a)), fill=0, width=max(1, int(rng.uniform(0.25, 0.5) * S_)))
    # two drips off the belly and one off the tail curl
    for (x, y, ln, w) in ((4.0, -12.9, 5.2, 0.8), (-6.0, -12.4, 3.4, 0.6), (-26.6, -14.0, 3.0, 0.55)):
        xs0, ys0 = P(x, y)
        de.line((xs0, ys0, xs0, ys0 + ln * S_), fill=255, width=lw(w))
        de.ellipse((xs0 - w * 0.9 * S_, ys0 + ln * S_ - w * 0.5 * S_, xs0 + w * 0.9 * S_, ys0 + ln * S_ + w * 1.4 * S_), fill=255)

    # ---- overspray: soft mist of speckles around the cut edge, thinning with distance
    solid = np.asarray(E, dtype=np.float32) / 255.0
    halo = np.asarray(E.filter(ImageFilter.GaussianBlur(2.2 * S_)), dtype=np.float32) / 255.0
    dots = (rng.random(solid.shape, dtype=np.float32) < (halo ** 1.5) * 0.050).astype(np.uint8) * 255
    from PIL import Image as _Im
    dots = np.asarray(_Im.fromarray(dots).filter(ImageFilter.MaxFilter(5)), dtype=np.float32) / 255.0
    mist = dots * (1.0 - solid)
    # the yellow second pass: the whole rat again, shifted (sloppy registration)
    shifted = np.roll(np.roll(solid, int(0.9 * S_), axis=1), int(0.7 * S_), axis=0)
    k = 2 * int(0.8 * S_) + 1                                                    # close the cut lines so they stay dark
    closed = np.asarray(E.filter(ImageFilter.MaxFilter(k)).filter(ImageFilter.MinFilter(k)), dtype=np.float32) / 255.0
    A_arr = np.maximum(arr_of(A), shifted * (1.0 - closed))

    # ---- secondary marks (yellow), pushed to the margins so the rat has the room
    def gnaw_cluster(cx, cy, ang, n, span):
        for i in range(n):
            a = math.radians(ang + (i - (n - 1) / 2) * span / max(1, n - 1))
            ln = rng.uniform(4.0, 8.0)
            for off in (-0.55, 0.55):                       # two parallel grooves per bite
                px, py = cx + off * math.cos(a + math.pi / 2), cy + off * math.sin(a + math.pi / 2)
                seg_ = [P(px + t * ln * math.cos(a) + 0.5 * math.sin(t * 5), py + t * ln * math.sin(a)) for t in np.linspace(0, 1, 10)]
                da.line(seg_, fill=255, width=lw(0.42 + 0.2 * rng.random()), joint='curve')
    gnaw_cluster(-42, 14, -70, 5, 55)
    gnaw_cluster(43, 15, -110, 5, 55)
    # chewed border: bite scallops eaten out of the four edges (stencil drop-outs)
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
    for _ in range(40):                                      # wood-shaving curls, kept off the rat
        x, y = rng.uniform(-50, 50), rng.uniform(-34, 34)
        if -47 < x < 50 and -26 < y < 27:
            continue
        a0 = rng.uniform(0, 360)
        r = rng.uniform(0.7, 1.7)
        cx_, cy_ = P(x, y)
        da.arc((cx_ - r * S_, cy_ - r * S_, cx_ + r * S_, cy_ + r * S_), a0, a0 + 250, fill=255, width=lw(0.33))
    # drain-grate bars either side of the LED, below the rat: two rounded slots each side
    for sx in (-1, 1):
        for k in range(2):
            x = sx * (19.0 + k * 6.4)
            da.rounded_rectangle((*P(x - 1.4, -28.0), *P(x + 1.4, -46.0)), radius=int(1.4 * S_), outline=255, width=lw(0.7))
    return np.maximum(A_arr, arr_of(A)), np.maximum(arr_of(E), mist * 0.85)


def art(font):
    return make_artwork(font, layout(), W, L, S, motif,
                        words=[("VERMIN", 0, 24.0, 60, 0.0), ("DISTORTION", 36, -63.0, 24, -4.0)],
                        tapes=[(-42, -52, 30, 6.5, 24), (49, -30, 22, 6.5, 86)],
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
