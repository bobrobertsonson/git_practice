"""
Sawblade design B2 - "STOCKHOLM SYNDROME" boutique pedal, procedural Blender (Cycles) scene.

Fully procedural: every mesh, texture and the OLED font are generated here
(no downloaded models, textures or HDRIs).  Only Blender's built-in font is used.

Usage (bpy 4.2.0 from PyPI, see README.md):
    python pedal_b2.py --mode hero  --out /path/outside/repo
    python pedal_b2.py --mode ortho --out /path/outside/repo
    python pedal_b2.py --mode strip --out /path/outside/repo
    python pedal_b2.py --mode all   --out /path/outside/repo
    add  --scale 40  for a 40% size preview, --samples N to override sampling.
Units: scene is modelled in metres; helper constants are in millimetres.
"""
import argparse, math, os, sys, time
import bpy, bmesh
import numpy as np
from mathutils import Vector, Euler

MM = 1e-3
W_, L_, H_ = 120.0, 190.0, 55.0          # enclosure mm
Z = H_                                   # top face height (mm)


# ----------------------------------------------------------------- utilities
def lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

def hexcol(h, a=1.0):
    h = h.lstrip('#')
    return tuple(lin(int(h[i:i + 2], 16) / 255) for i in (0, 2, 4)) + (a,)

def new_obj(name, me, mats=()):
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    for m in mats:
        me.materials.append(m)
    return ob

def finish_bm(bm, name, mats=(), angle=35.0, smooth=True):
    """bmesh -> object; smooth shading with sharp edges above `angle`."""
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    if smooth:
        lim = math.radians(angle)
        for e in bm.edges:
            if len(e.link_faces) == 2 and e.calc_face_angle(0.0) > lim:
                e.smooth = False
        for f in bm.faces:
            f.smooth = True
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    return new_obj(name, me, mats)

def lathe(name, profile, mats, seg=96, angle=35.0):
    """profile: [(r_mm, z_mm)...] revolved around Z."""
    bm = bmesh.new()
    rings = []
    for r, z in profile:
        if r < 1e-6:
            rings.append([bm.verts.new((0, 0, z * MM))])
        else:
            rings.append([bm.verts.new((r * MM * math.cos(2 * math.pi * i / seg),
                                        r * MM * math.sin(2 * math.pi * i / seg), z * MM))
                          for i in range(seg)])
    for a, b in zip(rings[:-1], rings[1:]):
        for i in range(seg):
            j = (i + 1) % seg
            if len(a) == 1:
                bm.faces.new((a[0], b[i], b[j]))
            elif len(b) == 1:
                bm.faces.new((a[i], a[j], b[0]))
            else:
                bm.faces.new((a[i], a[j], b[j], b[i]))
    return finish_bm(bm, name, mats, angle)

def prism(name, pts, z0, z1, mats, angle=35.0, cap=True):
    bm = bmesh.new()
    lo = [bm.verts.new((x * MM, y * MM, z0 * MM)) for x, y in pts]
    hi = [bm.verts.new((x * MM, y * MM, z1 * MM)) for x, y in pts]
    n = len(pts)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((lo[i], lo[j], hi[j], hi[i]))
    if cap:
        bm.faces.new(hi)
        bm.faces.new(lo[::-1])
    return finish_bm(bm, name, mats, angle)

def circle_pts(r, n, a0=0.0):
    return [(r * math.cos(a0 + 2 * math.pi * i / n), r * math.sin(a0 + 2 * math.pi * i / n)) for i in range(n)]

def rrect(hx, hy, r, seg=10):
    pts = []
    for cx, cy, a0 in ((hx - r, hy - r, 0), (-hx + r, hy - r, 90), (-hx + r, -hy + r, 180), (hx - r, -hy + r, 270)):
        for k in range(seg + 1):
            a = math.radians(a0 + 90 * k / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def rbox(name, w, d, h, r, mats, z0=0.0, seg=5, angle=35.0):
    return prism(name, rrect(w / 2, d / 2, r, seg), z0, z0 + h, mats, angle)

def plane(name, w, h, mats=()):
    """quad in XY with UVs 0..1 (mm inputs)."""
    me = bpy.data.meshes.new(name)
    me.from_pydata([(-w / 2 * MM, -h / 2 * MM, 0), (w / 2 * MM, -h / 2 * MM, 0),
                    (w / 2 * MM, h / 2 * MM, 0), (-w / 2 * MM, h / 2 * MM, 0)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new()
    for i, c in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = c
    return new_obj(name, me, mats)

def at(ob, x, y, z=0.0, rz=0.0, parent=None):
    ob.location = (x * MM, y * MM, z * MM)
    ob.rotation_euler = (ob.rotation_euler.x, ob.rotation_euler.y, math.radians(rz))
    if parent is not None:
        ob.parent = parent
    return ob

def empty(name, x, y, z):
    e = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(e)
    e.location = (x * MM, y * MM, z * MM)
    return e


# ----------------------------------------------------------------- shader helpers
class M:
    """tiny node-builder"""
    def __init__(self, name):
        self.m = bpy.data.materials.new(name)
        self.m.use_nodes = True
        self.nt = self.m.node_tree
        self.nt.nodes.clear()
        self.out = self.n('ShaderNodeOutputMaterial')
    def n(self, typ, **kw):
        nd = self.nt.nodes.new(typ)
        for k, v in kw.items():
            setattr(nd, k, v)
        return nd
    def l(self, a, ao, b, bi):
        self.nt.links.new(a.outputs[ao], b.inputs[bi])
    def bsdf(self, **vals):
        b = self.n('ShaderNodeBsdfPrincipled')
        for k, v in vals.items():
            b.inputs[k.replace('_', ' ')].default_value = v
        self.l(b, 'BSDF', self.out, 'Surface')
        return b
    def math(self, op, a=None, b=None, clamp=False):
        n = self.n('ShaderNodeMath', operation=op, use_clamp=clamp)
        for i, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                n.inputs[i].default_value = v
            else:
                if not isinstance(v, tuple):
                    v = (v, 'Value')
                self.l(v[0], v[1], n, i)
        return n
    def ramp(self, fac, stops):
        r = self.n('ShaderNodeValToRGB')
        r.color_ramp.elements[0].position = stops[0][0]
        r.color_ramp.elements[0].color = (stops[0][1],) * 3 + (1,)
        r.color_ramp.elements[1].position = stops[1][0]
        r.color_ramp.elements[1].color = (stops[1][1],) * 3 + (1,)
        self.l(fac[0], fac[1], r, 'Fac')
        return r
    def noise(self, scale, detail=3.0, rough=0.5, coord=None, dist=0.0):
        n = self.n('ShaderNodeTexNoise')
        n.inputs['Scale'].default_value = scale
        n.inputs['Detail'].default_value = detail
        n.inputs['Roughness'].default_value = rough
        n.inputs['Distortion'].default_value = dist
        if coord is None:
            coord = self.n('ShaderNodeTexCoord')
            coord = (coord, 'Object')
        self.l(coord[0], coord[1], n, 'Vector')
        return n


def mat_powder(art):
    s = M('powder_coat')
    tc = s.n('ShaderNodeTexCoord')
    geo = s.n('ShaderNodeNewGeometry')
    sep = s.n('ShaderNodeSeparateXYZ')
    s.l(geo, 'Normal', sep, 'Vector')
    nz = (sep, 'Z')
    # edge wear: strongest mid-bevel (normal tilted ~45deg), patchy via noise
    t = s.math('MULTIPLY', s.math('MULTIPLY', nz, s.math('SUBTRACT', 1.0, nz)), 4.0)
    ramp1 = s.ramp((t, 'Value'), ((0.40, 0.0), (0.75, 1.0)))
    patch = s.noise(900, 4, 0.6, (tc, 'Object'))
    patch2 = s.noise(90, 3, 0.55, (tc, 'Object'))
    mix = s.math('ADD', (patch, 'Fac'), (patch2, 'Fac'))
    pr = s.ramp((mix, 'Value'), ((0.98, 0.0), (1.16, 1.0)))
    wear = s.math('MULTIPLY', (ramp1, 'Color'), (pr, 'Color'), clamp=True)
    # --- top-face artwork (planar projection from object space) ---
    mp = s.n('ShaderNodeMapping')
    mp.inputs['Scale'].default_value = (1 / (W_ * MM), 1 / (L_ * MM), 1)
    mp.inputs['Location'].default_value = (0.5, 0.5, 0)
    s.l(tc, 'Object', mp, 'Vector')
    tex = s.n('ShaderNodeTexImage'); tex.image = art
    tex.interpolation = 'Linear'; tex.extension = 'CLIP'
    s.l(mp, 'Vector', tex, 'Vector')
    sc = s.n('ShaderNodeSeparateColor'); s.l(tex, 'Color', sc, 'Color')
    topm = s.math('MULTIPLY', s.math('SUBTRACT', nz, 0.985, clamp=True), 66.0, clamp=True)
    ink_o = s.math('MULTIPLY', (sc, 'Red'), topm, clamp=True)
    ink_g = s.math('MULTIPLY', (sc, 'Green'), topm, clamp=True)
    reveal = s.math('MULTIPLY', (sc, 'Blue'), topm, clamp=True)
    wear2 = s.math('MAXIMUM', (wear, 'Value'), (reveal, 'Value'))
    ink_any = s.math('MAXIMUM', (ink_o, 'Value'), (ink_g, 'Value'))
    # roughness: blotchy smudges + streaks + fine grain
    n_big = s.noise(40, 2, 0.5, (tc, 'Object'))
    n_fine = s.noise(2600, 2, 0.6, (tc, 'Object'))
    streak_m = s.n('ShaderNodeMapping'); streak_m.inputs['Scale'].default_value = (2.0, 70.0, 2.0)
    s.l(tc, 'Object', streak_m, 'Vector')
    streak = s.noise(18, 3, 0.55, (streak_m, 'Vector'))
    rr = s.math('ADD', 0.50, s.math('MULTIPLY', s.math('SUBTRACT', (n_big, 'Fac'), 0.5), 0.40))
    rr = s.math('ADD', rr, s.math('MULTIPLY', s.math('SUBTRACT', (streak, 'Fac'), 0.5), 0.22))
    rr = s.math('ADD', rr, s.math('MULTIPLY', s.math('SUBTRACT', (n_fine, 'Fac'), 0.5), 0.25))
    rr = s.math('ADD', rr, s.math('MULTIPLY', (ink_any, 'Value'), 0.18))
    rw = s.math('ADD', rr, s.math('MULTIPLY', (wear2, 'Value'), -0.3))
    # colour: coat -> orange -> bone -> raw aluminium
    c1 = s.n('ShaderNodeMix', data_type='RGBA')
    c1.inputs['A'].default_value = (0.019, 0.019, 0.020, 1)
    c1.inputs['B'].default_value = tuple(c * 0.45 for c in hexcol('#ff6a1a')[:3]) + (1,)
    s.l(ink_o, 'Value', c1, 'Factor')
    c2 = s.n('ShaderNodeMix', data_type='RGBA')
    s.l(c1, 'Result', c2, 'A')
    c2.inputs['B'].default_value = tuple(c * 0.62 for c in hexcol('#e8e4d8')[:3]) + (1,)
    s.l(ink_g, 'Value', c2, 'Factor')
    c3 = s.n('ShaderNodeMix', data_type='RGBA')
    s.l(c2, 'Result', c3, 'A')
    c3.inputs['B'].default_value = (0.62, 0.63, 0.65, 1)
    s.l(wear2, 'Value', c3, 'Factor')
    # micro texture: orange-peel + fine grain, plus paint thickness from the artwork
    peel = s.noise(520, 3, 0.55, (tc, 'Object'))
    hh = s.math('ADD', s.math('MULTIPLY', (peel, 'Fac'), 0.6), s.math('MULTIPLY', (n_fine, 'Fac'), 0.4))
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.30
    bump.inputs['Distance'].default_value = 0.00025
    s.l(hh, 'Value', bump, 'Height')
    bump2 = s.n('ShaderNodeBump')
    bump2.inputs['Strength'].default_value = 0.6
    bump2.inputs['Distance'].default_value = 0.00008
    s.l(ink_any, 'Value', bump2, 'Height')
    s.l(bump, 'Normal', bump2, 'Normal')
    b = s.bsdf(Metallic=0.0, Specular_IOR_Level=0.10)
    s.l(c3, 'Result', b, 'Base Color')
    s.l(wear2, 'Value', b, 'Metallic')
    s.l(rw, 'Value', b, 'Roughness')
    s.l(bump2, 'Normal', b, 'Normal')
    return s.m

def mat_chrome(name='chrome', rough=0.06):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(1800, 3, 0.6, (tc, 'Object'))
    r = s.math('ADD', rough, s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.5), 0.05))
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.04
    bump.inputs['Distance'].default_value = 0.0001
    s.l(n, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=(0.80, 0.81, 0.83, 1), Metallic=1.0)
    s.l(r, 'Value', b, 'Roughness')
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_alu_brushed():
    s = M('alu_brushed_radial')
    tc = s.n('ShaderNodeTexCoord')
    wav = s.n('ShaderNodeTexWave', wave_type='RINGS', rings_direction='Z')
    wav.inputs['Scale'].default_value = 7000
    wav.inputs['Distortion'].default_value = 1.5
    wav.inputs['Detail'].default_value = 3
    wav.inputs['Detail Scale'].default_value = 3.0
    s.l(tc, 'Object', wav, 'Vector')
    fine = s.noise(4000, 3, 0.7, (tc, 'Object'))
    r = s.math('ADD', 0.30, s.math('MULTIPLY', s.math('SUBTRACT', (wav, 'Fac'), 0.5), 0.14))
    r = s.math('ADD', r, s.math('MULTIPLY', s.math('SUBTRACT', (fine, 'Fac'), 0.5), 0.08))
    tan = s.n('ShaderNodeTangent', direction_type='RADIAL', axis='Z')
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.08
    bump.inputs['Distance'].default_value = 0.00004
    s.l(wav, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=(0.86, 0.87, 0.88, 1), Metallic=1.0, Anisotropic=0.85)
    s.l(r, 'Value', b, 'Roughness')
    s.l(tan, 'Tangent', b, 'Tangent')
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_black_anodised(name='black_anodised'):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(1500, 3, 0.6, (tc, 'Object'))
    r = s.math('ADD', 0.34, s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.5), 0.2))
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.05
    bump.inputs['Distance'].default_value = 0.0001
    s.l(n, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=(0.022, 0.022, 0.024, 1), Metallic=0.75)
    s.l(r, 'Value', b, 'Roughness')
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_ink(name, col, rough=0.5):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(1200, 3, 0.6, (tc, 'Object'))
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.05
    bump.inputs['Distance'].default_value = 0.0001
    s.l(n, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=col, Roughness=rough, Specular_IOR_Level=0.35)
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_emit(name, col, strength):
    s = M(name)
    b = s.bsdf(Base_Color=(0.02, 0.02, 0.02, 1), Roughness=0.2, Emission_Color=col, Emission_Strength=strength)
    return s.m

def mat_led_off(name, col):
    s = M(name)
    b = s.bsdf(Base_Color=col, Roughness=0.12, Emission_Color=col, Emission_Strength=0.06)
    return s.m

def mat_glass_overlay():
    s = M('oled_glass')
    fr = s.n('ShaderNodeLayerWeight')
    fr.inputs['Blend'].default_value = 0.35
    tr = s.n('ShaderNodeBsdfTransparent')
    gl = s.n('ShaderNodeBsdfGlossy')
    gl.inputs['Roughness'].default_value = 0.03
    mx = s.n('ShaderNodeMixShader')
    f2 = s.math('MULTIPLY', (fr, 'Fresnel'), 1.0)
    s.l(f2, 'Value', mx, 'Fac')
    s.l(tr, 'BSDF', mx, 1)
    s.l(gl, 'BSDF', mx, 2)
    s.l(mx, 'Shader', s.out, 'Surface')
    return s.m

def mat_glow_decal(col, strength):
    s = M('glow_decal')
    tc = s.n('ShaderNodeTexCoord')
    g = s.n('ShaderNodeTexGradient', gradient_type='SPHERICAL')
    mp = s.n('ShaderNodeMapping')
    mp.inputs['Location'].default_value = (0.5, 0.5, 0)
    s.l(tc, 'UV', mp, 'Vector')
    mp.inputs['Scale'].default_value = (1, 1, 1)
    # centre at (0.5,0.5): shift by -0.5 via vector math
    vm = s.n('ShaderNodeVectorMath', operation='SUBTRACT')
    s.l(tc, 'UV', vm, 0)
    vm.inputs[1].default_value = (0.5, 0.5, 0)
    s.l(vm, 'Vector', g, 'Vector')
    inv = s.math('SUBTRACT', 1.0, s.math('MULTIPLY', (g, 'Fac'), 2.0), clamp=True)
    sq = s.math('POWER', (inv, 'Value'), 2.5)
    em = s.n('ShaderNodeEmission')
    em.inputs['Color'].default_value = col
    em.inputs['Strength'].default_value = strength
    tr = s.n('ShaderNodeBsdfTransparent')
    mx = s.n('ShaderNodeMixShader')
    s.l(sq, 'Value', mx, 'Fac')
    s.l(tr, 'BSDF', mx, 1)
    s.l(em, 'Emission', mx, 2)
    s.l(mx, 'Shader', s.out, 'Surface')
    return s.m

def mat_oled_display(img, W, H):
    s = M('oled_display')
    tc = s.n('ShaderNodeTexCoord')
    tex = s.n('ShaderNodeTexImage')
    tex.image = img
    tex.interpolation = 'Closest'
    tex.extension = 'EXTEND'
    s.l(tc, 'UV', tex, 'Vector')
    mp = s.n('ShaderNodeMapping')
    mp.inputs['Scale'].default_value = (W, H, 1)
    s.l(tc, 'UV', mp, 'Vector')
    fr = s.n('ShaderNodeVectorMath', operation='FRACTION')
    s.l(mp, 'Vector', fr, 0)
    sp = s.n('ShaderNodeSeparateXYZ')
    s.l(fr, 'Vector', sp, 'Vector')
    def axis(sock):
        d = s.math('MULTIPLY', s.math('ABSOLUTE', s.math('SUBTRACT', (sp, sock), 0.5)), 2.0)
        return s.math('SUBTRACT', 1.0, s.math('POWER', (d, 'Value'), 4.0))
    mask = s.math('MULTIPLY', (axis('X'), 'Value'), (axis('Y'), 'Value'))
    amber = s.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY')
    amber.inputs['Factor'].default_value = 1.0
    s.l(tex, 'Color', amber, 'A')
    amber.inputs['B'].default_value = (1.0, 0.62, 0.22, 1)
    em = s.n('ShaderNodeEmission')
    em.inputs['Strength'].default_value = 5.0
    mul = s.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY')
    mul.inputs['Factor'].default_value = 1.0
    s.l(amber, 'Result', mul, 'A')
    mm_ = s.n('ShaderNodeCombineXYZ')
    s.l(mask, 'Value', mm_, 'X'); s.l(mask, 'Value', mm_, 'Y'); s.l(mask, 'Value', mm_, 'Z')
    s.l(mm_, 'Vector', mul, 'B')
    s.l(mul, 'Result', em, 'Color')
    # off-pixels: very dark glossy
    b = s.bsdf(Base_Color=(0.004, 0.004, 0.005, 1), Roughness=0.25)
    add = s.n('ShaderNodeAddShader')
    s.l(b, 'BSDF', add, 0)
    s.l(em, 'Emission', add, 1)
    s.l(add, 'Shader', s.out, 'Surface')
    return s.m

def mat_emit_plane(strength, gradient=True, tint=(1, 1, 1)):
    s = M('softbox')
    em = s.n('ShaderNodeEmission')
    em.inputs['Strength'].default_value = strength
    if gradient:
        tc = s.n('ShaderNodeTexCoord')
        vm = s.n('ShaderNodeVectorMath', operation='SUBTRACT')
        s.l(tc, 'UV', vm, 0)
        vm.inputs[1].default_value = (0.5, 0.5, 0)
        g = s.n('ShaderNodeTexGradient', gradient_type='SPHERICAL')
        s.l(vm, 'Vector', g, 'Vector')
        f = s.math('SUBTRACT', 1.0, s.math('MULTIPLY', (g, 'Fac'), 1.1), clamp=True)
        f = s.math('ADD', 0.25, s.math('MULTIPLY', (f, 'Value'), 0.75))
        mul = s.n('ShaderNodeMath', operation='MULTIPLY')
        s.l(f, 'Value', mul, 0)
        mul.inputs[1].default_value = strength
        s.l(mul, 'Value', em, 'Strength')
    em.inputs['Color'].default_value = tint + (1,)
    s.l(em, 'Emission', s.out, 'Surface')
    return s.m


# ----------------------------------------------------------------- OLED bitmap font (5x7, procedural)
FONT = {
 'H': "10001 10001 10001 11111 10001 10001 10001", 'M': "10001 11011 10101 10101 10001 10001 10001",
 '-': "00000 00000 00000 11111 00000 00000 00000", '2': "01110 10001 00001 00010 00100 01000 11111",
 'w': "00000 00000 10001 10001 10101 10101 01010", 'C': "01110 10001 10000 10000 10000 10001 01110",
 'A': "01110 10001 10001 11111 10001 10001 10001", 'I': "01110 00100 00100 00100 00100 00100 01110",
 'N': "10001 11001 10101 10011 10001 10001 10001", 'S': "01111 10000 10000 01110 00001 00001 11110",
 'W': "10001 10001 10001 10101 10101 11011 10001", 's': "00000 00000 01111 10000 01110 00001 11110",
 't': "00100 00100 11111 00100 00100 00101 00010", 'd': "00001 00001 01101 10011 10001 10011 01101",
 '·': "00000 00000 00000 00100 00000 00000 00000", '4': "00010 00110 01010 10010 11111 00010 00010",
 '8': "01110 10001 10001 01110 10001 10001 01110", 'k': "10000 10000 10010 10100 11000 10100 10010",
 ' ': "00000 " * 6 + "00000",
}

def oled_image(lines, W=98, H=28):
    px = np.zeros((H, W), np.float32)
    ys = (4, 16)
    for ln, y0 in zip(lines, ys):
        x0 = (W - (len(ln) * 6 - 1)) // 2
        for ci, ch in enumerate(ln):
            rows = FONT[ch].split()
            for ry, row in enumerate(rows):
                for rx, v in enumerate(row):
                    if v == '1':
                        px[H - 1 - (y0 + ry), x0 + ci * 6 + rx] = 1.0   # image origin bottom-left
    px *= 0.9
    rgba = np.zeros((H, W, 4), np.float32)
    rgba[..., 0] = rgba[..., 1] = rgba[..., 2] = px
    rgba[..., 3] = 1
    img = bpy.data.images.new('oled_px', W, H, alpha=False)
    img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(rgba.ravel())
    img.pack()
    return img


# ----------------------------------------------------------------- procedural top-face artwork
# "Swedish crust" stencil: giant stencilled saw blade, overspray, drips, xerox grain,
# torn tape, mis-registered second ink, scratched-through paint.
# Channels (Non-Color): R = burnt-orange ink, G = bone ink, B = paint scratched to raw aluminium.
S = 14                                   # texture pixels per mm
AW, AH = int(W_ * S), int(L_ * S)
FONT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/blackopsone/BlackOpsOne-Regular.ttf"

def fetch_font(cache_dir):
    """Black Ops One (SIL OFL 1.1) is fetched at run time and never committed."""
    path = os.path.join(cache_dir, "BlackOpsOne-Regular.ttf")
    if os.path.exists(path):
        return path
    try:
        import ssl, urllib.request
        ca = "/root/.ccr/ca-bundle.crt"
        ctx = ssl.create_default_context(cafile=ca) if os.path.exists(ca) else None
        os.makedirs(cache_dir, exist_ok=True)
        with urllib.request.urlopen(FONT_URL, timeout=30, context=ctx) as r, open(path, "wb") as f:
            f.write(r.read())
        return path
    except Exception as e:
        print("[pedal_b2] stencil font fetch failed (%s); falling back to PIL default" % e)
        return None

def make_artwork(font_path, layout, seed=7):
    from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
    rng = np.random.default_rng(seed)
    P = lambda x, y: ((x + W_ / 2) * S, (L_ / 2 - y) * S)

    def blur(a, r):
        return np.asarray(Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).filter(
            ImageFilter.GaussianBlur(r)), dtype=np.float32) / 255.0

    def noise(sigma):
        b = blur(rng.random((AH, AW), dtype=np.float32), sigma)
        return (b - b.min()) / (b.max() - b.min() + 1e-9)

    def arr(img):
        return np.asarray(img, dtype=np.float32) / 255.0

    def rough(m, r, amt, n):                          # rough, photocopied edges
        return np.clip((blur(m, r) - 0.5) * 3.0 + 0.5 + (n - 0.5) * amt, 0, 1) > 0.5

    n_fine = noise(1.2); n_mid = noise(5.0); n_big = noise(40)

    # --- stencilled blade (orange) ----------------------------------------------------
    O = Image.new('L', (AW, AH), 0); d = ImageDraw.Draw(O)
    cx, cy = P(16, -6)
    def pol(a, r): return (cx + r * S * math.cos(math.radians(a)), cy - r * S * math.sin(math.radians(a)))
    pts = []
    nt = 16
    for t in range(nt):
        a0 = t * 360 / nt
        pts += [pol(a0, 78), pol(a0 + 2, 91)]
        pts += [pol(a0 + 2 + 18 * u, 91 - 13 * u ** 0.6) for u in np.linspace(0.1, 1.0, 7)]
    d.polygon(pts, fill=255)
    d.ellipse((cx - 70 * S, cy - 70 * S, cx + 70 * S, cy + 70 * S), fill=0)       # gullet ring
    for ro, ri in ((60, 53), (41, 37), (25, 21), (9, 6)):
        d.ellipse((cx - ro * S, cy - ro * S, cx + ro * S, cy + ro * S), fill=255)
        d.ellipse((cx - ri * S, cy - ri * S, cx + ri * S, cy + ri * S), fill=0)
    d.ellipse((cx - 2.5 * S, cy - 2.5 * S, cx + 2.5 * S, cy + 2.5 * S), fill=255)
    for rr_, ra, rb in ((87, 66, 92), (60, 50, 64), (41, 34, 45), (25, 18, 29), (9, 3, 12)):   # stencil bridges
        for k in range(5):
            a = rng.uniform(0, 360)
            d.line((*pol(a, ra), *pol(a, rb)), fill=0, width=int(rng.uniform(0.9, 1.5) * S))
    # bridges through the toothed band, radial
    for k in range(6):
        a = k * 60 + 17
        d.line((*pol(a, 71), *pol(a, 93)), fill=0, width=int(1.1 * S))
    layer_o = arr(O)

    # --- wordmark (bone) with orange mis-registered twin -------------------------------
    f = None
    def mkfont(sz):
        return ImageFont.truetype(font_path, sz) if font_path else ImageFont.load_default()
    def line_mask(text, cy_mm, width_mm, rot):
        sz = 200
        fo = mkfont(sz)
        bb = fo.getbbox(text)
        sz = int(sz * width_mm * S / (bb[2] - bb[0]))
        fo = mkfont(sz); bb = fo.getbbox(text)
        im = Image.new('L', (bb[2] - bb[0] + 20, bb[3] - bb[1] + 20), 0)
        ImageDraw.Draw(im).text((10 - bb[0], 10 - bb[1]), text, fill=255, font=fo)
        im = im.rotate(rot, expand=True, resample=Image.BICUBIC)
        full = Image.new('L', (AW, AH), 0)
        x, y = P(0, cy_mm)
        full.paste(im, (int(x - im.width / 2), int(y - im.height / 2)))
        return arr(full)
    w1 = line_mask("STOCKHOLM", 85, 100, 2.2)
    w2 = line_mask("SYNDROME", 73.2, 88, 2.2)
    word = np.maximum(w1, w2)
    dx, dy = int(1.0 * S), int(0.7 * S)
    word_o = np.roll(np.roll(word, dx, axis=1), dy, axis=0)           # sloppy registration

    # --- clear zones (keep controls + labels legible) ------------------------------------
    C = Image.new('L', (AW, AH), 0); dc = ImageDraw.Draw(C)
    for (x, y, rr_) in layout['circles']:
        px_, py_ = P(x, y); dc.ellipse((px_ - rr_ * S, py_ - rr_ * S, px_ + rr_ * S, py_ + rr_ * S), fill=255)
    for (x, y, w, h) in layout['rects']:
        px_, py_ = P(x, y); dc.rectangle((px_ - w / 2 * S, py_ - h / 2 * S, px_ + w / 2 * S, py_ + h / 2 * S), fill=255)
    clear = blur(arr(C), 1.2 * S)
    clear = np.clip(clear * 1.6, 0, 1)

    # --- orange: rough edges, xerox drop-outs, overspray, drips --------------------------
    mo = rough(layer_o, 2.2, 0.8, n_fine).astype(np.float32)
    mo *= (0.15 + 0.85 * (n_mid > 0.30)).astype(np.float32) * (n_big * 0.5 + 0.7).clip(0, 1)
    mo *= (1 - 0.88 * clear)
    # drips from lower edges
    edge = (mo[:-1] > 0.5) & (mo[1:] < 0.5)
    ys, xs = np.nonzero(edge)
    sel = rng.random(len(ys)) < 0.0016
    D = Image.new('L', (AW, AH), 0); dd = ImageDraw.Draw(D)
    for y, x in zip(ys[sel], xs[sel]):
        ln = int(rng.uniform(1.0, 9.0) * S); w = int(rng.uniform(0.2, 0.45) * S) + 1
        dd.line((x, y, x, y + ln), fill=255, width=w)
        dd.ellipse((x - w * 0.9, y + ln - w * 0.6, x + w * 0.9, y + ln + w * 1.1), fill=255)
    mo = np.maximum(mo, arr(D) * (1 - 0.9 * clear) * 0.95)
    # overspray halo
    halo = blur(mo, 2.4 * S)
    dots = rng.random((AH, AW), dtype=np.float32) < (halo ** 1.4) * 0.55
    mo = np.maximum(mo, dots.astype(np.float32) * (1 - mo) * 0.85 * (1 - 0.9 * clear))
    mo *= (0.82 + 0.18 * n_fine)
    word_o_r = rough(word_o, 2.5, 1.0, noise(1.0)).astype(np.float32) * (0.2 + 0.8 * (n_mid > 0.2))
    mo = np.maximum(mo, word_o_r * 0.95)

    # --- bone: wordmark, tape residue, halftone borders, tally marks, xerox speckle -----------
    mg = rough(word, 2.0, 0.9, n_fine).astype(np.float32) * (0.12 + 0.88 * (noise(3.0) > 0.36))
    G = Image.new('L', (AW, AH), 0); dg = ImageDraw.Draw(G)
    for (x, y, w, h, rot) in ((-47, 91, 44, 8, -33), (48, -91, 40, 8, -30), (53, 4, 36, 7.5, 84)):
        pts = []
        px_, py_ = P(x, y)
        hw, hh = w / 2 * S, h / 2 * S
        edge_pts = [(-hw, -hh)]
        for t in np.linspace(-hw, hw, 40):
            edge_pts.append((t, -hh + rng.uniform(-0.1, 0.1) * S))
        edge_pts.append((hw + rng.uniform(0, 0.8) * S, -hh))
        for yy in np.linspace(-hh, hh, 6):
            edge_pts.append((hw + rng.uniform(-0.4, 0.5) * S, yy))
        for t in np.linspace(hw, -hw, 40):
            edge_pts.append((t, hh + rng.uniform(-0.1, 0.1) * S))
        for yy in np.linspace(hh, -hh, 6):
            edge_pts.append((-hw + rng.uniform(-0.5, 0.4) * S, yy))
        ca, sa = math.cos(math.radians(rot)), math.sin(math.radians(rot))
        dg.polygon([(px_ + u * ca + v * sa, py_ - (u * sa - v * ca)) for u, v in edge_pts], fill=150)
    tape = arr(G) * (1 - 0.8 * clear) * (0.55 + 0.45 * n_mid)
    mg = np.maximum(mg, tape)
    # tally marks between the footswitches
    T = Image.new('L', (AW, AH), 0); dt = ImageDraw.Draw(T)
    for grp in range(2):
        x0, y0 = P(-9 + grp * 10, -80)
        for k in range(4):
            dt.line((x0 + k * 0.9 * S, y0 - 3.5 * S, x0 + k * 0.9 * S + rng.uniform(-3, 3), y0 + 3.5 * S), fill=255, width=int(0.28 * S) + 1)
        dt.line((x0 - 0.6 * S, y0 + 2.4 * S, x0 + 3.5 * S, y0 - 2.4 * S), fill=255, width=int(0.28 * S) + 1)
    mg = np.maximum(mg, rough(arr(T), 1.5, 0.8, n_fine).astype(np.float32) * 0.9)
    # halftone gradient borders (photocopy edge): top and bottom
    yy, xx = np.mgrid[0:AH, 0:AW].astype(np.float32)
    u = (xx + yy) * 0.7071; v = (yy - xx) * 0.7071
    cell = 7.0
    fu = (u / cell) % 1 - 0.5; fv = (v / cell) % 1 - 0.5
    dist = np.sqrt(fu ** 2 + fv ** 2)
    ymm = L_ / 2 - yy / S
    g = np.clip((ymm - 88.5) / 6.5, 0, 1) + np.clip((-91.0 - ymm) / 3.5, 0, 1)
    ht = (dist < 0.62 * np.sqrt(np.clip(g, 0, 1))).astype(np.float32) * 0.8
    ht *= (1 - 0.8 * clear)
    mg = np.maximum(mg, ht)
    # xerox speckle
    mg = np.maximum(mg, ((n_fine > 0.83) & (rng.random((AH, AW)) < 0.8)).astype(np.float32) * (n_big > 0.5) * 0.5 * (1 - 0.9 * clear))
    mg *= (0.86 + 0.14 * n_fine)

    # --- scratches / chips: raw aluminium ---------------------------------------------------
    B = Image.new('L', (AW, AH), 0); db = ImageDraw.Draw(B)
    for k in range(110):
        if rng.random() < 0.45:                       # near an edge
            side = rng.integers(0, 4); t = rng.random(); off = rng.exponential(1.3) * S
            if side == 0: x, y = t * AW, off
            elif side == 1: x, y = t * AW, AH - off
            elif side == 2: x, y = off, t * AH
            else: x, y = AW - off, t * AH
        else:
            x, y = rng.random() * AW, rng.random() * AH
        a = rng.uniform(0, 2 * math.pi)
        for seg in range(rng.integers(1, 4)):
            ln = rng.uniform(0.8, 12) * S
            nx, ny = x + ln * math.cos(a), y + ln * math.sin(a)
            db.line((x, y, nx, ny), fill=int(rng.uniform(90, 230)), width=int(rng.uniform(1, 2.2)))
            x, y = nx, ny; a += rng.normal(0, 0.5)
    for k in range(70):                               # chips on edges / corners
        side = rng.integers(0, 4); t = rng.random(); off = rng.exponential(0.8) * S
        if side == 0: x, y = t * AW, off
        elif side == 1: x, y = t * AW, AH - off
        elif side == 2: x, y = off, t * AH
        else: x, y = AW - off, t * AH
        r = rng.uniform(0.25, 1.3) * S
        pts = [(x + r * rng.uniform(0.4, 1.2) * math.cos(a), y + r * rng.uniform(0.4, 1.2) * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 8, endpoint=False)]
        db.polygon(pts, fill=255)
    mb = blur(arr(B), 0.6) * (0.5 + 0.5 * (n_mid > 0.2))

    out = np.zeros((AH, AW, 4), np.float32)
    out[..., 0] = np.clip(mo, 0, 1)
    out[..., 1] = np.clip(mg, 0, 1)
    out[..., 2] = np.clip(mb, 0, 1)
    out[..., 3] = 1.0
    return out

def art_to_image(rgba):
    img = bpy.data.images.new('top_art', AW, AH, alpha=True, float_buffer=False)
    img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(np.ascontiguousarray(rgba[::-1]).ravel())        # Blender rows start at the bottom
    img.update()
    return img


# ----------------------------------------------------------------- parts
def build_enclosure(m_body):
    R, bev, nb = 7.0, 1.3, 5
    prof = []
    for k in range(nb + 1):                      # bottom bevel
        t = math.pi / 2 * k / nb
        prof.append((bev * (1 - math.sin(t)), bev * (1 - math.cos(t))))
    prof = [(d, z) for d, z in prof]
    top = [(bev * (1 - math.cos(math.pi / 2 * k / nb)), H_ - bev * (1 - math.sin(math.pi / 2 * k / nb))) for k in range(nb + 1)]
    prof = prof + top[::-1][::-1]
    bm = bmesh.new()
    rings = []
    pts0 = None
    for d, z in [(p[0], p[1]) for p in prof]:
        pts = rrect(W_ / 2 - d, L_ / 2 - d, R - d, 10)
        rings.append([bm.verts.new((x * MM, y * MM, z * MM)) for x, y in pts])
    for a, b in zip(rings[:-1], rings[1:]):
        n = len(a)
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bm.faces.new(rings[-1])
    bm.faces.new(rings[0][::-1])
    # recompute: profile order must be bottom->top
    return finish_bm(bm, 'enclosure', [m_body], angle=40)

def add_text(body, size, x, y, mat, width=None, rot=0.0, extrude=0.06, embolden=0.0, sx=1.0):
    cu = bpy.data.curves.new('t_' + body, 'FONT')
    cu.body = body
    cu.size = size * MM
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    cu.extrude = extrude * MM
    cu.offset = embolden * MM
    cu.materials.append(mat)
    ob = new_obj('t_' + body, cu)
    bpy.context.view_layer.update()
    if width:
        sx = width * MM / max(ob.dimensions.x, 1e-9)
    ob.scale = (sx, 1.0, 1.0)
    ob.location = (x * MM, y * MM, (Z + 0.01) * MM)
    ob.rotation_euler = (0, 0, math.radians(rot))
    return ob

def arc_bm(R, a0, a1, rt, z, rings_per_deg=0.6, cap=True):
    """tube along arc (deg clockwise from +Y); radii in mm."""
    n = max(2, int(abs(a1 - a0) * rings_per_deg))
    ns = 10
    bm = bmesh.new()
    rings = []
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        rad = Vector((math.sin(a), math.cos(a), 0))
        c = rad * R
        ring = []
        for k in range(ns):
            t = 2 * math.pi * k / ns
            p = c + rad * (rt * math.cos(t)) + Vector((0, 0, rt * math.sin(t) + 0.0))
            ring.append(bm.verts.new((p.x * MM, p.y * MM, (p.z + z) * MM)))
        rings.append(ring)
    for a, b in zip(rings[:-1], rings[1:]):
        for k in range(ns):
            j = (k + 1) % ns
            bm.faces.new((a[k], b[k], b[j], a[j]))
    if cap:
        bm.faces.new(rings[0][::-1]); bm.faces.new(rings[-1])
    return bm

class Knob:
    def __init__(self, name, x, y, value, mats):
        self.value = value
        self.root = empty('knob_' + name, x, y, Z)
        self.spin = empty('spin_' + name, 0, 0, 0)
        self.spin.parent = self.root
        sp = self.spin
        # knurled black skirt
        N = 120
        star = []
        for i in range(2 * N):
            a = math.pi * i / N
            r = 12.55 if i % 2 == 0 else 12.15
            star.append((r * math.cos(a), r * math.sin(a)))
        sk = prism('skirt_' + name, star, 0.0, 8.5, [mats['black']], angle=50)
        sh = lathe('shoulder_' + name, [(12.05, 8.45), (11.95, 8.85), (11.55, 9.0), (0, 9.0)], [mats['black']], 96)
        # machined aluminium cap, slight dish + chamfers
        cap = lathe('cap_' + name, [(0, 9.0), (10.05, 9.0), (10.25, 9.3), (10.25, 12.4), (10.0, 12.9),
                                    (9.3, 13.15), (6.0, 13.05), (3.0, 12.95), (0, 12.9)], [mats['alu']], 128, angle=40)
        # indicator: raised white line, local +Y
        ind = prism('ind_' + name, [(-0.45, 3.0), (0.45, 3.0), (0.45, 9.4), (-0.45, 9.4)], 12.95, 13.12, [mats['white']], angle=60)
        for o in (sk, sh, cap, ind):
            o.parent = sp
        # LED ring (fixed, not spinning), radius 16 mm
        self.lit = new_obj('led_lit_' + name, bpy.data.meshes.new('lit_' + name), [mats['led_on']])
        self.unlit = new_obj('led_off_' + name, bpy.data.meshes.new('off_' + name), [mats['led_off']])
        for o in (self.lit, self.unlit):
            o.parent = self.root
        self.set(value)

    def set(self, v):
        self.value = v
        ang = -135 + 270 * v
        self.spin.rotation_euler = (0, 0, -math.radians(ang))
        R, rt, zc = 16.4, 0.5, 0.45
        for ob, bm in ((self.lit, arc_bm(R, -135, ang, rt, zc) if v > 0.004 else None),
                       (self.unlit, arc_bm(R, ang + 1.6, 135, rt * 0.7, zc) if v < 0.996 else None)):
            ob.data.clear_geometry()
            if bm is not None:
                bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
                for f in bm.faces: f.smooth = True
                bm.to_mesh(ob.data); bm.free()

def build_toggle(x, y, tilt, mats):
    root = empty('toggle', x, y, Z)
    parts = []
    parts.append(lathe('tg_washer', [(0, 0), (8.0, 0), (8.0, 0.8), (0, 0.8)], [mats['chrome']], 64, 40))
    nut = prism('tg_nut', circle_pts(6.4, 6, math.pi / 6), 0.8, 3.6, [mats['chrome']], 40)
    mod = nut.modifiers.new('b', 'BEVEL'); mod.width = 0.45 * MM; mod.segments = 3; mod.limit_method = 'ANGLE'
    parts += [nut]
    parts.append(lathe('tg_bush', [(0, 3.5), (3.9, 3.5), (3.9, 7.4), (3.4, 7.9), (0, 7.9)], [mats['chrome']], 48, 40))
    for p in parts: p.parent = root
    piv = empty('tg_piv', 0, 0, 7.9); piv.parent = root
    piv.rotation_euler = (math.radians(tilt), 0, 0)
    bat = lathe('tg_bat', [(0, -1.0), (1.8, -0.8), (2.5, 0.2), (2.2, 1.4), (1.9, 2.2), (1.55, 6), (1.2, 11), (0.95, 15.5),
                           (0.8, 17.0), (0.45, 17.6), (0, 17.75)], [mats['chrome']], 40, 55)
    bat.parent = piv
    return root

def build_footswitch(x, y, mats):
    """Flatter boutique stomp cap: knurled satin-chrome barrel, ridged brushed top, hex nut."""
    root = empty('footswitch', x, y, Z)
    nut = prism('fs_nut', circle_pts(11.9, 6, math.pi / 6), 0, 2.2, [mats['chrome']], 40)
    mod = nut.modifiers.new('b', 'BEVEL'); mod.width = 0.5 * MM; mod.segments = 3; mod.limit_method = 'ANGLE'
    N = 90
    star = []
    for i in range(2 * N):
        a = math.pi * i / N
        r = 11.0 if i % 2 == 0 else 10.65
        star.append((r * math.cos(a), r * math.sin(a)))
    barrel = prism('fs_barrel', star, 2.0, 8.6, [mats['chrome_sat']], angle=50)
    prof = [(0, 9.45)]
    for r in np.linspace(0.8, 9.2, 28):               # concentric ridges
        prof.append((float(r), 9.45 + 0.17 * abs(math.sin(math.pi * r / 1.05)) - 0.01 * r))
    prof += [(9.7, 9.35), (10.3, 9.15), (10.85, 8.8), (11.0, 8.5), (11.0, 8.3)]
    top = lathe('fs_top', prof, [mats['chrome_sat']], 96, 38)
    for p in (nut, barrel, top): p.parent = root
    return root

def build_led(x, y, lit, mats):
    root = empty('led', x, y, Z)
    bz = lathe('led_bezel', [(0, 0), (3.2, 0), (3.2, 1.0), (2.5, 1.2), (2.2, 1.0), (0, 1.0)], [mats['chrome_dark']], 40, 40)
    dome = lathe('led_dome', [(0, 0.9), (2.2, 0.9), (2.1, 1.7), (1.5, 2.3), (0.7, 2.55), (0, 2.6)],
                 [mats['led_red_on'] if lit else mats['led_red_off']], 40, 50)
    for p in (bz, dome): p.parent = root
    if lit:
        gl = plane('led_glow', 20, 20, [mats['glow']])
        gl.location = (x * MM, y * MM, (Z + 0.04) * MM)
        gl.visible_shadow = False
        pl = bpy.data.lights.new('led_pt', 'POINT')
        pl.energy = 0.12; pl.color = (1, 0.1, 0.05); pl.shadow_soft_size = 0.002
        po = bpy.data.objects.new('led_pt', pl)
        bpy.context.scene.collection.objects.link(po)
        po.location = (x * MM, y * MM, (Z + 4) * MM)
    return root

def build_screw(x, y, mats):
    head = lathe('screw', [(0, 0), (2.1, 0), (2.1, 0.35), (1.9, 0.75), (1.2, 1.0), (0, 1.05)], [mats['chrome']], 32, 60)
    head.location = (x * MM, y * MM, Z * MM)
    # cross recess = two thin dark slots
    for rz in (0, 90):
        sl = prism('slot', [(-1.3, -0.22), (1.3, -0.22), (1.3, 0.22), (-1.3, 0.22)], 0.93, 1.07, [mats['slot']], 60)
        sl.parent = head
        sl.rotation_euler = (0, 0, math.radians(rz + 15))
    return head

def build_oled(mats, x, y):
    frame = rbox('oled_frame', 62, 21, 0.9, 2.2, [mats['black']], z0=0)
    at(frame, x, y, Z)
    glass_base = rbox('oled_well', 56.6, 16.4, 0.12, 0.8, [mats['glass_black']], z0=0.9)
    at(glass_base, x, y, Z)
    disp = plane('oled_display', 56, 16, [mats['oled']])
    disp.location = (x * MM, y * MM, (Z + 1.05) * MM)
    cover = plane('oled_cover', 56.6, 16.4, [mats['glass']])
    cover.location = (x * MM, y * MM, (Z + 1.35) * MM)
    cover.visible_shadow = False


# ----------------------------------------------------------------- lights / camera / world
def softbox(name, w, h, loc, target, strength, tint=(1, 1, 1), gradient=True):
    pl = plane(name, w * 1000, h * 1000, [mat_emit_plane(strength, gradient, tint)])
    pl.location = loc
    d = Vector(target) - Vector(loc)
    pl.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    pl.visible_camera = False
    pl.visible_shadow = False
    return pl

def build_backdrop():
    s = M('backdrop')
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(300, 3, 0.5, (tc, 'Object'))
    r = s.math('ADD', 0.62, s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.5), 0.25))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.08; bump.inputs['Distance'].default_value = 0.0004
    s.l(n, 'Fac', bump, 'Height')
    ln = s.n('ShaderNodeVectorMath', operation='LENGTH')
    s.l(tc, 'Object', ln, 0)
    vig = s.math('ADD', 0.12, s.math('MULTIPLY', s.math('SUBTRACT', 1.0, s.math('DIVIDE', (ln, 'Value'), 1.5), clamp=True), 0.88))
    vig = s.math('POWER', (vig, 'Value'), 1.6)
    mc = s.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY')
    mc.inputs['Factor'].default_value = 1.0
    mc.inputs['A'].default_value = hexcol('#4e4843')
    cmb = s.n('ShaderNodeCombineXYZ')
    for sk in 'XYZ': s.l(vig, 'Value', cmb, sk)
    s.l(cmb, 'Vector', mc, 'B')
    b = s.bsdf(Specular_IOR_Level=0.3)
    s.l(mc, 'Result', b, 'Base Color')
    s.l(r, 'Value', b, 'Roughness'); s.l(bump, 'Normal', b, 'Normal')
    prof = [(-3.0, 0.0), (0.45, 0.0)]
    Rr = 0.45
    for k in range(1, 17):
        t = math.pi / 2 * k / 16
        prof.append((0.45 + Rr * math.sin(t), Rr * (1 - math.cos(t))))
    prof.append((0.9, 2.5))
    bm = bmesh.new()
    a = [(bm.verts.new((-3, y, z)), bm.verts.new((3, y, z))) for y, z in prof]
    for p, q in zip(a[:-1], a[1:]):
        bm.faces.new((p[0], p[1], q[1], q[0]))
    ob = finish_bm(bm, 'backdrop', [s.m], angle=80)
    ob.visible_shadow = True
    return ob

def setup_world():
    w = bpy.data.worlds.new('w'); bpy.context.scene.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes['Background']
    bg.inputs['Color'].default_value = hexcol('#5a534d')
    bg.inputs['Strength'].default_value = 0.10

def setup_render(mode, scale, samples):
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    cy = sc.cycles
    cy.device = 'CPU'
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 3
    cy.use_adaptive_sampling = mode != 'strip'
    cy.adaptive_threshold = 0.01
    cy.adaptive_min_samples = 24
    cy.samples = samples
    cy.light_sampling_threshold = 0.05
    cy.use_denoising = True
    cy.denoiser = 'OPENIMAGEDENOISE'
    cy.sample_clamp_indirect = 4.0
    cy.sample_clamp_direct = 0.0
    cy.caustics_reflective = False
    cy.caustics_refractive = False
    cy.max_bounces = 6
    cy.glossy_bounces = 4
    cy.diffuse_bounces = 2
    cy.transparent_max_bounces = 6
    cy.filter_width = 1.1
    sc.render.film_transparent = False
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGB'
    sc.render.image_settings.color_depth = '8'
    sc.view_settings.view_transform = 'AgX'
    try:
        sc.view_settings.look = 'AgX - Medium High Contrast'
    except Exception:
        pass
    sc.display_settings.display_device = 'sRGB'
    res = {'hero': (1600, 1200), 'ortho': (1200, 1800), 'strip': (128, 128)}[mode]
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.resolution_percentage = scale

def make_camera(mode, strip_center=None):
    cam = bpy.data.cameras.new('cam')
    co = bpy.data.objects.new('cam', cam)
    bpy.context.scene.collection.objects.link(co)
    bpy.context.scene.camera = co
    if mode == 'hero':
        cam.lens = 85
        co.location = (0.20, -0.44, 0.59)
        tgt = empty('tgt', 0, -8, Z)
        cam.dof.use_dof = True
        cam.dof.focus_object = tgt
        cam.dof.aperture_fstop = 9.0
        tr = co.constraints.new('TRACK_TO'); tr.target = tgt
        tr.track_axis = 'TRACK_NEGATIVE_Z'; tr.up_axis = 'UP_Y'
    else:
        cam.type = 'ORTHO'
        if mode == 'ortho':
            cam.ortho_scale = 0.205
            co.location = (0, 0, 0.5)
        else:
            cam.ortho_scale = 0.039
            co.location = (strip_center[0] * MM, strip_center[1] * MM, 0.5)
        co.rotation_euler = (0, 0, 0)
    cam.clip_start, cam.clip_end = 0.01, 5.0
    return co

def build_lights(mode):
    c = (0, 0, 0.03)
    softbox('key', 0.95, 0.75, (-0.55, -0.40, 0.62), c, 11.0, (1.0, 0.95, 0.9))
    softbox('rim', 0.14, 0.9, (0.50, 0.55, 0.28), c, 70.0, (1.0, 0.97, 0.95), gradient=False)
    softbox('rim2', 0.14, 0.9, (-0.55, 0.40, 0.25), c, 30.0, (0.95, 0.97, 1.0), gradient=False)
    softbox('fill', 0.8, 0.8, (0.6, -0.5, 0.25), c, 0.9, (0.95, 0.97, 1.0))
    softbox('top', 0.9, 0.9, (0.0, 0.0, 0.95), (0, 0, 0), 0.8, (1, 0.98, 0.95))


# ----------------------------------------------------------------- scene
KNOBS = [("LOW", -36, 26, 0.95), ("HIGH", 0, 26, 0.92), ("DIST", 36, 26, 0.80),
         ("IN GAIN", -36, -15, 0.50), ("OUT", 0, -15, 0.55), ("MIX", 36, -15, 1.00)]
LABEL_DY = -18.0
TOGGLE_Y, TOGGLE_LABEL_Y = -47.5, -58.5
FOOT_Y, FOOT_LED_Y = -80.0, -64.5
OLED_Y = 56.0

def art_layout():
    circles = [(x, y, 19.5) for _, x, y, _ in KNOBS]
    circles += [(x, TOGGLE_Y, 11.0) for x in (-36, 0, 36)]
    circles += [(x, FOOT_Y, 14.5) for x in (-30, 30)]
    circles += [(x, FOOT_LED_Y, 6.0) for x in (-30, 30)]
    circles += [(sx * 53.2, sy * 88.2, 4.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, y + LABEL_DY, 20, 5.5) for _, x, y, _ in KNOBS]
    rects += [(x, TOGGLE_LABEL_Y, 14, 5.5) for x in (-36, 0, 36)]
    rects += [(0, OLED_Y, 68, 25)]
    return dict(circles=circles, rects=rects)

def build_scene(mode, font_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    art = art_to_image(make_artwork(fetch_font(font_dir), art_layout()))
    mats = dict(
        powder=mat_powder(art), chrome=mat_chrome(), chrome_dark=mat_chrome('chrome_dark', 0.25),
        chrome_sat=mat_chrome('chrome_satin', 0.22),
        alu=mat_alu_brushed(), black=mat_black_anodised(),
        white=mat_ink('inlay_white', hexcol('#e8e4d8'), 0.45),
        led_on=mat_emit('led_on', (1.0, 0.20, 0.0, 1), 6.0),
        led_off=mat_led_off('led_off', hexcol('#5a1c05')),
        led_red_on=mat_emit('led_red_on', (1.0, 0.02, 0.01, 1), 9.0),
        led_red_off=mat_led_off('led_red_off', (0.22, 0.01, 0.008, 1)),
        glow=mat_glow_decal((1.0, 0.07, 0.03, 1), 0.6),
        glass=mat_glass_overlay(), glass_black=mat_black_anodised('oled_well'),
        slot=mat_ink('slot', (0.01, 0.01, 0.01, 1), 0.6))
    mats['oled'] = mat_oled_display(oled_image(["HM-2w CHAINSAW", "std \u00b7 A2 \u00b7 48k"]), 98, 28)
    enc = build_enclosure(mats['powder'])
    setup_world()
    build_backdrop()
    ink_w = mat_ink('print_white', hexcol('#e8e4d8'), 0.5)
    knobs = []
    if mode == 'strip':
        knobs.append(Knob('DIST', 0, 0, 0.0, mats))
    else:
        for name, x, y, v in KNOBS:
            knobs.append(Knob(name, x, y, v, mats))
            add_text(name, 3.4, x, y + LABEL_DY, ink_w, embolden=0.03)
        for (nm, x, tilt) in (("SLOT", -36, 20), ("SIZE", 0, 0), ("NORM", 36, -20)):
            build_toggle(x, TOGGLE_Y, tilt, mats)
            add_text(nm, 3.4, x, TOGGLE_LABEL_Y, ink_w, embolden=0.03)
        build_footswitch(-30, FOOT_Y, mats); build_footswitch(30, FOOT_Y, mats)
        build_led(-30, FOOT_LED_Y, True, mats); build_led(30, FOOT_LED_Y, False, mats)
        for sx in (-1, 1):
            for sy in (-1, 1):
                build_screw(sx * 53.2, sy * 88.2, mats)
        build_oled(mats, 0, OLED_Y)
    return knobs

def render_to(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='hero', choices=['hero', 'ortho', 'strip', 'all'])
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=int, default=100)
    ap.add_argument('--samples', type=int, default=0)
    ap.add_argument('--frames', type=int, default=64)
    ap.add_argument('--font-dir', default=os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))
    ap.add_argument('--dump-art', action='store_true', help='only write the top-face artwork PNG to --out')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.dump_art:
        from PIL import Image
        rgba = make_artwork(fetch_font(a.font_dir), art_layout())
        Image.fromarray((rgba[..., :3] * 255).astype(np.uint8)).save(os.path.join(a.out, 'art_channels.png'))
        return
    modes = ['hero', 'ortho', 'strip'] if a.mode == 'all' else [a.mode]
    for mode in modes:
        t0 = time.time()
        knobs = build_scene(mode, a.font_dir)
        samples = a.samples or {'hero': 48, 'ortho': 40, 'strip': 16}[mode]
        setup_render(mode, a.scale, samples)
        make_camera(mode, (0, 0))
        build_lights(mode)
        if mode == 'strip':
            fd = os.path.join(a.out, 'strip_frames'); os.makedirs(fd, exist_ok=True)
            n = a.frames
            for i in range(n):
                knobs[0].set(i / (n - 1))
                render_to(os.path.join(fd, 'f_%02d.png' % i))
            from PIL import Image
            sheet = Image.new('RGB', (128, 128 * n))
            for i in range(n):
                sheet.paste(Image.open(os.path.join(fd, 'f_%02d.png' % i)).convert('RGB'), (0, 128 * i))
            sheet.save(os.path.join(a.out, 'knob_strip.png'))
        else:
            render_to(os.path.join(a.out, {'hero': 'hero_3q.png', 'ortho': 'top_ortho.png'}[mode]))
        print('[pedal_b2] %s done in %.1fs (samples=%d)' % (mode, time.time() - t0, samples))

if __name__ == '__main__':
    main()
