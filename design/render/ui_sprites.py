"""Sawblade UI control sprites: real filmstrips for the JUCE build, rendered top-down orthographic on a transparent
background with the same parts, materials and light rig as the hero/ortho renders (common.py).

    python ui_sprites.py --part all --out /path/outside/repo/sprites      # everything
    python ui_sprites.py --part knob_pedal --scale 40 --out ...           # 40% preview of one part

parts (each writes <name>.png + <name>.json):
    knob_pedal   128 frames, 128x128 px each, stacked vertically (128 x 16384); knurled pedal knob, -135..+135 deg
    knob_amp     128 frames, 128x128 px each (128 x 16384); amp chicken-head pointer knob, -135..+135 deg
    footswitch   2 frames 128x128 (up, down; down = cap 3.6 mm lower, lip shadow ring, darker cap)
    led_orange / led_bone / led_green   2 frames 64x64 each (off, on), NO halo in the main sheet
    led_<colour>_glow   2 frames 128x128 (off = empty, on = soft coloured halo), same mm/px as the LED, same centre
    toggle       3 frames 64x64 (lever toward top / centre / toward bottom)

Pedal parts are lit exactly like the pedal ortho renders (camera straight down, k=1 rig); the amp knob is rendered in
the amp-head front-panel orientation (camera on the panel normal, k=3.2 rig + front panel light), so highlights fall
the same way as on the existing *_ortho.png renders.  Every frame is rendered at --supersample x the target size and
downsampled with premultiplied alpha.  Knob angle 0 = pointer straight up, positive = clockwise; frame 0 = -135 deg,
last frame = +135 deg.  No cast shadow is baked in (the UI draws its own drop shadow).
"""
import argparse, json, math, os, shutil, time
import numpy as np
import common as C
from common import *

PX_PEDAL_KNOB = (128, 40.0)      # (frame px, frame size in mm)
PX_AMP_KNOB = (128, 64.0)
PX_FOOT = (128, 40.0)
FS_TRAVEL = 3.6                  # mm the pressed cap sits lower than released
SPRITE_THREADS = 2
ENV_FS, ENV_LED, ENV_TOGGLE = 1.6, 0.8, 2.0      # chrome-environment strip strength per part (see build_chrome_env)
PX_LED = (64, 20.0)
PX_TOGGLE = (64, 20.0)
ANG0, ANG1 = -135.0, 135.0
# name: (linear emissive rgba of the lens, hex of the glow halo).  Sprites are rendered with the 'Standard' view transform so a
# saturated emission stays saturated instead of rolling off to white (AgX); 'bone' is a warm ivory-amber because a truly
# desaturated lit lens cannot read as "on".
LED_COLOURS = {
    'orange': ((1.0, 0.045, 0.0, 1.0), '#ff5a00'),
    'bone': ((1.0, 0.50, 0.10, 1.0), '#ffc860'),
    'green': ((0.03, 1.0, 0.015, 1.0), '#4dff1a'),
}
LED_LENS_R_MM = 2.2
GLOW_PX = 128
FACE_AMP = (0.0, -117.5, 130.0)  # where the amp head's front panel sits in the amp ortho scene (mm)


# ----------------------------------------------------------------- parts
def make_mats(led_rgb=None, led_hex='#ff6a1a'):
    m = dict(chrome=mat_chrome(), chrome_dark=mat_chrome('chrome_dark', 0.25), chrome_sat=mat_chrome('chrome_satin', 0.22),
             alu=mat_alu_brushed(), black=mat_black_anodised(), white=mat_ink('inlay_white', hexcol('#e8e4d8'), 0.45),
             led_on=mat_emit('led_on', (1.0, 0.20, 0.0, 1), 6.0), led_off=mat_led_off('led_off', hexcol('#5a1c05')))
    rgb = led_rgb or (1.0, 0.02, 0.01, 1)
    dark = tuple(c * 0.22 for c in rgb[:3]) + (1,)
    m['led_red_on'] = mat_lens('led_sprite_on', rgb, 1.0)
    m['led_red_off'] = mat_lens_off('led_sprite_off', rgb)
    m['glow'] = mat_glow_decal(rgb, 0.7)
    return m


def mat_lens(name, rgb, strength):
    """coloured translucent-looking lens: saturated base + emission of the same colour (no white core), glossy top."""
    s = M(name)
    s.bsdf(Base_Color=tuple(c * 0.55 for c in rgb[:3]) + (1,), Roughness=0.12, Specular_IOR_Level=0.5,
           Emission_Color=rgb, Emission_Strength=strength)
    return s.m


def mat_lens_off(name, rgb):
    """unlit lens: dark tinted glass (~5% linear of the lit colour), glossy so only the specular highlight reads; no emission."""
    s = M(name)
    s.bsdf(Base_Color=tuple(c * 0.05 for c in rgb[:3]) + (1,), Roughness=0.10, Specular_IOR_Level=0.5)
    return s.m


def mat_shadow_ring(r_in, r_out, peak):
    """radial soft shadow decal for the pressed footswitch: transparent inside r_in, black (alpha=peak) at r_out."""
    s = M('fs_shadow')
    tc = s.n('ShaderNodeTexCoord')
    ln = s.n('ShaderNodeVectorMath', operation='LENGTH')
    s.l(tc, 'Object', ln, 0)
    f = s.math('DIVIDE', s.math('SUBTRACT', (ln, 'Value'), r_in * MM), (r_out - r_in) * MM, clamp=True)
    f = s.math('MULTIPLY', s.math('POWER', (f, 'Value'), 1.6), peak)
    bl = s.n('ShaderNodeBsdfDiffuse')
    bl.inputs['Color'].default_value = (0.004, 0.004, 0.005, 1)
    tr = s.n('ShaderNodeBsdfTransparent')
    mx = s.n('ShaderNodeMixShader')
    s.l(f, 'Value', mx, 'Fac'); s.l(tr, 'BSDF', mx, 1); s.l(bl, 'BSDF', mx, 2)
    s.l(mx, 'Shader', s.out, 'Surface')
    return s.m


def build_chrome_env(strength=9.0, top_x=0.05, top_w=0.02):
    """top-down chrome environment: the camera looks straight down, so chrome parts reflect whatever is around/above them.
    A ring of tall softbox strips with dark gaps (crisp light/dark bands along vertical chrome), two long bright strips
    overhead and a low dark-to-bright fill card.  All are invisible to the camera and to shadows, so they never show in the
    frame or the alpha."""
    tgt = (0, 0, 0.06)
    ring = 0.075
    for i in range(6):
        a = math.radians(60 * i + 15)
        softbox('env_ring%d' % i, 0.034, 0.16, (ring * math.cos(a), ring * math.sin(a), 0.05), tgt, strength, (1, 0.98, 0.95), gradient=False)
    # top_x/top_w: how far off the camera axis and how wide the overhead strips are; flat chrome only mirrors what is straight
    # above, so wide/close strips wash it out (toggle uses far, thin ones)
    for i, x in enumerate((-top_x, top_x)):
        softbox('env_top%d' % i, top_w, 0.22, (x, 0.0, 0.30), tgt, strength * 0.8, (0.95, 0.97, 1.0), gradient=False)
    softbox('env_low', 0.20, 0.20, (0.0, 0.0, -0.05), tgt, strength * 0.12, (1, 0.97, 0.92), gradient=True)


class PedalKnob(Knob):
    """the common.py knurled knob; the LED ring is optional (the UI normally draws its own value arc)."""
    def __init__(self, mats, ring=False):
        self.ring = ring
        super().__init__('sprite', 0, 0, 0.0, mats)

    def set(self, v):
        if self.ring:
            return super().set(v)
        self.value = v
        self.spin.rotation_euler = (0, 0, -math.radians(ANG0 + (ANG1 - ANG0) * v))


def loft(name, secs, mat, z0, ns=16, angle=50.0):
    """body swept along +Y: secs = [(y, half_width, height)], cross-section is a flat-bottomed half ellipse above z0."""
    bm = bmesh.new()
    rings = []
    for y, w, h in secs:
        rings.append([bm.verts.new((w * math.cos(math.pi * k / ns) * MM, y * MM, (z0 + h * math.sin(math.pi * k / ns) ** 0.7) * MM))
                      for k in range(ns + 1)])
    for a, b in zip(rings[:-1], rings[1:]):
        for k in range(ns + 1):
            j = (k + 1) % (ns + 1)
            bm.faces.new((a[k], a[j], b[j], b[k]))
    bm.faces.new(rings[0][::-1]); bm.faces.new(rings[-1])
    return finish_bm(bm, name, [mat], angle)


class ChickenHead:
    """amp pointer knob (generic, unbranded): satin-chrome base ring, black skirt, tapered chicken-head body with a
    bone pointer stripe.  Local frame: origin on the panel, +Z out of the panel, pointer along +Y."""
    SEC = [(-15.5, 3.0, 8.5), (-14.0, 7.0, 11.0), (-11.0, 11.5, 12.5), (-6.0, 13.8, 13.2), (0.0, 14.4, 13.4), (8.0, 11.8, 11.0),
           (16.0, 8.3, 8.2), (22.0, 5.4, 6.0), (26.0, 3.8, 4.4), (27.8, 2.8, 3.2), (28.6, 1.8, 2.2)]
    Z0 = 10.0
    RING_R = 31.0

    def __init__(self, mats, parent, ring=False):
        self.ring = ring
        self.root = empty('knob_chicken', 0, 0, 0)
        self.root.parent = parent
        self.spin = empty('spin_chicken', 0, 0, 0)
        self.spin.parent = self.root
        base = lathe('ck_base', [(0, 0.0), (18.6, 0.0), (18.9, 0.8), (18.5, 3.2), (17.6, 3.5), (0, 3.5)], [mats['chrome_sat']], 96, 40)
        skirt = lathe('ck_skirt', [(17.4, 3.4), (16.9, 4.2), (16.5, 9.4), (15.9, 10.5), (0, 10.5)], [mats['black']], 96, 40)
        plastic = mat_ink('ck_plastic', (0.030, 0.028, 0.026, 1), 0.26)
        body = loft('ck_body', self.SEC, plastic, self.Z0)
        ys = [s[0] for s in self.SEC]; hs = [s[2] for s in self.SEC]
        stripe_secs = [(y, 0.9, float(np.interp(y, ys, hs)) + 0.17) for y in np.linspace(5.0, 28.0, 12)]
        stripe = loft('ck_stripe', stripe_secs, mats['white'], self.Z0, ns=8, angle=70)
        screw = lathe('ck_screw', [(0, 0), (2.2, 0), (2.2, 0.8), (0, 0.8)], [mats['chrome_dark']], 24, 50)
        screw.location = (0, -9.5 * MM, (self.Z0 + 12.4) * MM)
        for o in (base, skirt, body, stripe, screw):
            o.parent = self.spin
        self.lit = new_obj('ck_ring_lit', bpy.data.meshes.new('ck_lit'), [mats['led_on']])
        self.unlit = new_obj('ck_ring_off', bpy.data.meshes.new('ck_off'), [mats['led_off']])
        for o in (self.lit, self.unlit):
            o.parent = self.root
        self.set(0.0)

    def set(self, v):
        ang = ANG0 + (ANG1 - ANG0) * v
        self.spin.rotation_euler = (0, 0, -math.radians(ang))
        if not self.ring:
            return
        for ob, bm in ((self.lit, arc_bm(self.RING_R, ANG0, ang, 0.7, 0.45) if v > 0.004 else None),
                       (self.unlit, arc_bm(self.RING_R, ang + 1.6, ANG1, 0.5, 0.45) if v < 0.996 else None)):
            ob.data.clear_geometry()
            if bm is not None:
                bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
                for f in bm.faces: f.smooth = True
                bm.to_mesh(ob.data); bm.free()


# ----------------------------------------------------------------- scene / render plumbing
def build_scene():
    """start from an empty factory scene."""
    bpy.ops.wm.read_factory_settings(use_empty=True)


def finish_scene(setup, frame_mm, res_px, scale, samples, view='AgX', env=0.0, env_kw=None):
    setup_world()
    setup_render((res_px, res_px), scale, samples, adaptive=True)
    sc = bpy.context.scene
    sc.render.film_transparent = True
    sc.render.threads = SPRITE_THREADS
    if view != 'AgX':
        sc.view_settings.view_transform = view
        sc.view_settings.look = 'None'
    sc.cycles.adaptive_min_samples = max(4, samples // 4)
    sc.render.image_settings.color_mode = 'RGBA'
    if setup == 'pedal':
        make_camera(dict(kind='ortho', loc=(0, 0, 0.5), rot=(0, 0, 0), ortho_scale=frame_mm * MM, k=1.0))
        build_lights(1.0, (0, 0, 0.03), front=False)
        if env:
            build_chrome_env(env, **(env_kw or {}))
    else:   # amp front panel, same placement as the amp ortho scene
        make_camera(dict(kind='ortho', loc=(0, -2.0, 0.13), rot=(90, 0, 0), ortho_scale=frame_mm * MM, k=3.2))
        build_lights(3.2, (0, 0, 0.13), front=True)


def grab(path, out_px):
    """load a rendered frame and downsample to out_px with premultiplied alpha."""
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    if im.size != (out_px, out_px):
        im = im.convert('RGBa').resize((out_px, out_px), Image.LANCZOS).convert('RGBA')
    a = np.asarray(im).copy()
    a[a[..., 3] < 10] = 0
    yy, xx = np.mgrid[0:out_px, 0:out_px]
    a[np.hypot(xx - (out_px - 1) / 2, yy - (out_px - 1) / 2) > out_px / 2] = 0   # all content sits inside the inscribed circle                # drop denoiser speckle in the empty background
    return Image.fromarray(a, 'RGBA')


def render_frames(name, n, out_px, frame_mm, setup, set_state, a, extra, make_parts, led=None, view='AgX', env=0.0, env_kw=None):
    """build the scene + parts, render n frames (set_state(i) poses the part), write <name>.png/<name>.json."""
    from PIL import Image
    t0 = time.time()
    ss = a.supersample
    build_scene()
    state = make_parts(led)
    finish_scene(setup, frame_mm, out_px * ss, a.scale, a.samples, view, env, env_kw)
    fd = os.path.join(a.out, '_frames_' + name)
    os.makedirs(fd, exist_ok=True)
    sheet = Image.new('RGBA', (out_px, out_px * n), (0, 0, 0, 0))
    for i in range(n):
        set_state(state, i, n)
        p = os.path.join(fd, 'f_%03d.png' % i)
        render_to(p)
        sheet.paste(grab(p, out_px), (0, out_px * i))
    shutil.rmtree(fd, ignore_errors=True)
    sheet.save(os.path.join(a.out, name + '.png'), optimize=True)
    meta = dict(name=name, file=name + '.png', frame_width=out_px, frame_height=out_px, frames=n, layout='vertical',
                sheet_width=out_px, sheet_height=out_px * n, pivot_px=[out_px / 2, out_px / 2],
                mm_per_px=frame_mm / out_px, frame_mm=frame_mm, view='top-down orthographic' if setup == 'pedal' else 'front-on orthographic (amp panel)',
                background='transparent (straight RGBA)', baked_shadow=False, supersample=ss,
                samples=a.samples, blender='bpy 4.2.0 / Cycles CPU')
    meta.update(extra)
    with open(os.path.join(a.out, name + '.json'), 'w') as f:
        json.dump(meta, f, indent=2)
    print('[sprite] %s: %d frames of %dx%d in %.1fs' % (name, n, out_px, out_px, time.time() - t0))


def rotary_extra(ring, ring_r_mm, px, mm):
    return dict(type='rotary', angle_min_deg=ANG0, angle_max_deg=ANG1, angle_zero='pointer up (12 o\'clock)', direction='clockwise positive',
                frame_for_value='frame = round(value * (frames - 1)); frame 0 = %d deg, last frame = +%d deg' % (ANG0, ANG1),
                ring_baked=ring, ring_radius_px=ring_r_mm / (mm / px), ring_note='value arc is not baked in (draw it in the UI); radius given for alignment')


# ----------------------------------------------------------------- part definitions
def part_knob_pedal(a):
    px, mm = PX_PEDAL_KNOB
    def make(_):
        return PedalKnob(make_mats(), ring=a.ring)
    def st(k, i, n):
        k.set(i / (n - 1))
    render_frames('knob_pedal', a.frames, px, mm, 'pedal', st, a, rotary_extra(a.ring, 16.4, px, mm), make)


def part_knob_amp(a):
    px, mm = PX_AMP_KNOB
    def make(_):
        mats = make_mats()
        face = empty('face', *FACE_AMP)
        face.rotation_euler = (math.pi / 2, 0, 0)
        return ChickenHead(mats, face, ring=a.ring)
    def st(k, i, n):
        k.set(i / (n - 1))
    render_frames('knob_amp', a.frames, px, mm, 'amp', st, a, rotary_extra(a.ring, ChickenHead.RING_R, px, mm), make)


def part_footswitch(a):
    px, mm = PX_FOOT
    def make(_):
        mats = make_mats()
        r = build_footswitch(0, 0, mats)
        # chrome collar around the cap so the pressed state reads in top-down view (cap sits below the collar rim)
        collar = lathe('fs_collar', [(11.5, 2.0), (11.5, 7.9), (11.9, 8.2), (12.6, 8.1), (12.9, 7.6), (12.9, 2.0)], [mats['chrome_dark']], 96, 40)
        collar.parent = r
        # pressed state only: the collar's inner lip overhangs the sunk cap, and a soft shadow falls on the cap under it
        lip = lathe('fs_lip', [(8.6, 7.5), (8.6, 8.2), (9.1, 8.5), (11.9, 8.3), (11.9, 7.5)], [mats['chrome_dark']], 96, 40)
        lip.parent = r
        ztop = 9.45 - FS_TRAVEL + 0.25
        shadow = lathe('fs_shadow', [(0, ztop), (10.8, ztop)], [mat_shadow_ring(4.5, 8.8, 0.88)], 96, 40)
        shadow.parent = r
        shadow.visible_shadow = False
        dark = mat_chrome('chrome_pressed', 0.30)
        dark.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (0.20, 0.205, 0.215, 1)
        return r, mats, dark, lip, shadow
    def st(s, i, n):
        # frame 0 = up (released), frame 1 = down (pressed).  Only the barrel + top move; the hex nut and collar are fixed.
        root, mats, dark, lip, shadow = s
        for ch in root.children:
            if ch.name.startswith(('fs_barrel', 'fs_top')):
                ch.location.z = (0.0 if i == 0 else -FS_TRAVEL) * MM
                ch.data.materials[0] = mats['chrome_sat'] if i == 0 else dark
        lip.hide_render = shadow.hide_render = (i == 0)
    render_frames('footswitch', 2, px, mm, 'pedal', st, a, dict(type='momentary', states=['up', 'down'], pressed_depth_mm=FS_TRAVEL,
                  note='down frame: cap %.1f mm below up, collar lip overhangs it, soft shadow ring and a darker cap, so it reads from straight above' % FS_TRAVEL),
                  make, env=ENV_FS)


def led_glow(colour, hexcol_, px_dir):
    """bake <name>_glow.png: 2 frames (off = empty, on = halo) of GLOW_PX x GLOW_PX at the LED's mm/px, same centre.
    Straight RGBA: constant saturated colour, alpha = halo intensity (core ~0.9 fading to 0 well inside the frame), so it
    composites normally under the LED sprite, or additively with premultiplied rgb = colour * alpha."""
    from PIL import Image
    mm_px = PX_LED[1] / PX_LED[0]
    ss = 4
    n = GLOW_PX * ss
    yy, xx = np.mgrid[0:n, 0:n]
    r_mm = np.hypot(xx - (n - 1) / 2, yy - (n - 1) / 2) / ss * mm_px
    disc = (r_mm < LED_LENS_R_MM * 1.1).astype(np.float64)
    fd = np.fft.rfft2(disc)
    fy, fx = np.fft.fftfreq(n)[:, None], np.fft.rfftfreq(n)[None, :]
    def blur(sig_mm):                                            # gaussian blur in the Fourier domain (disc is centred; no wrap at these sigmas)
        sg = sig_mm / mm_px * ss
        return np.fft.irfft2(fd * np.exp(-2 * (np.pi * sg) ** 2 * (fx ** 2 + fy ** 2)), s=(n, n)).real
    h = 2.2 * blur(1.2) + 1.6 * blur(3.0) + 1.0 * blur(6.5)
    h = 1.0 - np.exp(-1.9 * h)                                  # soft knee so the core saturates smoothly
    edge = np.clip((GLOW_PX / 2 * mm_px - r_mm) / (GLOW_PX * mm_px * 0.12), 0, 1)   # fade to 0 before the frame edge
    h = h * edge ** 1.5
    h = np.asarray(Image.fromarray(h.astype(np.float32), 'F').resize((GLOW_PX, GLOW_PX), Image.BOX))
    rgb = np.array([int(hexcol_[i:i + 2], 16) for i in (1, 3, 5)], np.uint8)
    on = np.zeros((GLOW_PX, GLOW_PX, 4), np.uint8)
    on[..., :3] = rgb
    on[..., 3] = np.clip(h * 0.97 * 255 + 0.5, 0, 255).astype(np.uint8)
    on[on[..., 3] < 2] = 0
    sheet = np.zeros((GLOW_PX * 2, GLOW_PX, 4), np.uint8)
    sheet[GLOW_PX:] = on
    name = 'led_%s_glow' % colour
    Image.fromarray(sheet, 'RGBA').save(os.path.join(px_dir, name + '.png'), optimize=True)
    meta = dict(name=name, file=name + '.png', frame_width=GLOW_PX, frame_height=GLOW_PX, frames=2, layout='vertical',
                sheet_width=GLOW_PX, sheet_height=GLOW_PX * 2, pivot_px=[GLOW_PX / 2, GLOW_PX / 2], mm_per_px=mm_px,
                frame_mm=GLOW_PX * mm_px, type='led_glow', states=['off', 'on'], colour=colour, glow_rgb_hex=hexcol_,
                background='transparent (straight RGBA)', pairs_with='led_%s.png' % colour,
                note='frame 0 (off) is fully transparent. Frame 1 is the halo: constant colour, alpha = intensity. Same mm/px '
                     'and centre as led_%s.png (64x64 px, 20 mm) but 128x128 px = 40 mm per frame, so centre it on the LED. '
                     'Draw it underneath the LED sprite (normal blend), or additively with rgb*alpha.' % colour)
    with open(os.path.join(px_dir, name + '.json'), 'w') as f:
        json.dump(meta, f, indent=2)
    print('[sprite] %s: glow layer 2 frames of %dx%d' % (name, GLOW_PX, GLOW_PX))


def part_led(a, colour):
    px, mm = PX_LED
    rgb, glow_hex = LED_COLOURS[colour]
    def make(_):
        mats = make_mats(rgb)
        lit, unlit = build_led(0, 0, True, mats), build_led(0, 0, False, mats)
        return lit, unlit
    def st(s, i, n):
        lit, unlit = s
        hide = (lambda r, h: [setattr(c, 'hide_render', h) for c in [r] + list(r.children)])
        hide(lit, i == 0); hide(unlit, i == 1)
        for o in bpy.data.objects:
            if o.name.startswith(('led_glow', 'led_pt')):
                o.hide_render = True      # the halo lives in the separate <name>_glow layer; the point light blows out the bezel
    render_frames('led_' + colour, 2, px, mm, 'pedal', st, a, dict(type='led', states=['off', 'on'], colour=colour,
                  glow_layer='led_%s_glow.png' % colour, note='no halo baked in; composite led_%s_glow.png underneath' % colour),
                  make, view='Standard', env=ENV_LED)
    led_glow(colour, glow_hex, a.out)


def part_toggle(a):
    px, mm = PX_TOGGLE
    TILTS = [-20.0, 0.0, 20.0]
    def make(_):
        return build_toggle(0, 0, 0, make_mats())
    def st(root, i, n):
        # flat horizontal chrome (washer, nut top, bush) mirrors the sky straight above the ortho camera: drop the rig's overhead
        # softbox so it reflects the dark world; the env strips are off-axis and only catch bevels, the lever and the nut flanks
        for o in bpy.data.objects:
            if o.name == 'top':
                o.hide_render = True
        for ch in root.children:
            if ch.name.startswith('tg_piv'):
                ch.rotation_euler = (math.radians(TILTS[i]), 0, 0)
    render_frames('toggle', 3, px, mm, 'pedal', st, a,
                  dict(type='toggle3', states=['lever toward top of panel', 'centre', 'lever toward bottom of panel'], tilt_deg=TILTS), make, env=ENV_TOGGLE, env_kw=dict(top_x=0.112, top_w=0.008))


PARTS = {'knob_pedal': part_knob_pedal, 'knob_amp': part_knob_amp, 'footswitch': part_footswitch,
         'toggle': part_toggle, 'led_orange': lambda a: part_led(a, 'orange'), 'led_bone': lambda a: part_led(a, 'bone'),
         'led_green': lambda a: part_led(a, 'green')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--part', default='all', choices=['all'] + list(PARTS))
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=int, default=100, help='render resolution percentage (40 = preview)')
    ap.add_argument('--samples', type=int, default=16)
    ap.add_argument('--supersample', type=int, default=2)
    ap.add_argument('--frames', type=int, default=128, help='rotary frames')
    ap.add_argument('--ring', action='store_true', help='also bake the common.py LED value ring into rotary frames')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for p in (list(PARTS) if a.part == 'all' else [a.part]):
        PARTS[p](a)


if __name__ == '__main__':
    main()
