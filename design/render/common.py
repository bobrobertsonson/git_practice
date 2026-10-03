"""
Shared procedural Blender (Cycles) toolkit for the Sawblade hardware renders: materials, parts, crust-stencil
artwork generator, lighting/camera rig and the `run(spec)` driver used by pedal_b2.py, pedal_tighten.py,
amp_saw.py and amp_body.py.

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


def mat_powder(art, ink_a_hex, ink_b_hex, coat=0.0065, gain_a=0.45):
    """Black powder-coated plate carrying the stencil artwork (R accent ink, G bone ink, B scratch-through)."""
    s = M('powder_coat')
    tc = s.n('ShaderNodeTexCoord')
    sep = s.n('ShaderNodeSeparateXYZ')
    s.l(tc, 'Normal', sep, 'Vector')                      # object-space normal: plate top is +Z even when the plate is rotated
    nz = (sep, 'Z')
    t = s.math('MULTIPLY', s.math('MULTIPLY', nz, s.math('SUBTRACT', 1.0, nz)), 4.0)
    ramp1 = s.ramp((t, 'Value'), ((0.45, 0.0), (0.85, 1.0)))
    patch = s.noise(900, 4, 0.6, (tc, 'Object'))
    patch2 = s.noise(90, 3, 0.55, (tc, 'Object'))
    mix = s.math('ADD', (patch, 'Fac'), (patch2, 'Fac'))
    pr = s.ramp((mix, 'Value'), ((1.04, 0.0), (1.22, 1.0)))
    wear = s.math('MULTIPLY', (ramp1, 'Color'), (pr, 'Color'), clamp=True)
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
    reveal = s.math('MULTIPLY', s.math('MULTIPLY', (sc, 'Blue'), 0.8), topm, clamp=True)
    wear2 = s.math('MAXIMUM', (wear, 'Value'), (reveal, 'Value'))
    ink_any = s.math('MAXIMUM', (ink_o, 'Value'), (ink_g, 'Value'))
    n_big = s.noise(40, 2, 0.5, (tc, 'Object'))
    n_fine = s.noise(2600, 2, 0.6, (tc, 'Object'))
    streak_m = s.n('ShaderNodeMapping'); streak_m.inputs['Scale'].default_value = (2.0, 70.0, 2.0)
    s.l(tc, 'Object', streak_m, 'Vector')
    streak = s.noise(18, 3, 0.55, (streak_m, 'Vector'))
    rr = s.math('ADD', 0.54, s.math('MULTIPLY', s.math('SUBTRACT', (n_big, 'Fac'), 0.5), 0.40))
    rr = s.math('ADD', rr, s.math('MULTIPLY', s.math('SUBTRACT', (streak, 'Fac'), 0.5), 0.22))
    rr = s.math('ADD', rr, s.math('MULTIPLY', s.math('SUBTRACT', (n_fine, 'Fac'), 0.5), 0.25))
    rr = s.math('ADD', rr, s.math('MULTIPLY', (ink_any, 'Value'), 0.18))
    # worn metal is rough/satin, never mirror-like: blend roughness toward 0.5 where paint is gone
    rw = s.math('ADD', s.math('MULTIPLY', rr, s.math('SUBTRACT', 1.0, (wear2, 'Value'))), s.math('MULTIPLY', (wear2, 'Value'), 0.5))
    c1 = s.n('ShaderNodeMix', data_type='RGBA')
    c1.inputs['A'].default_value = (coat, coat, coat * 1.05, 1)
    c1.inputs['B'].default_value = tuple(c * gain_a for c in hexcol(ink_a_hex)[:3]) + (1,)
    s.l(ink_o, 'Value', c1, 'Factor')
    c2 = s.n('ShaderNodeMix', data_type='RGBA')
    s.l(c1, 'Result', c2, 'A')
    c2.inputs['B'].default_value = tuple(c * 0.62 for c in hexcol(ink_b_hex)[:3]) + (1,)
    s.l(ink_g, 'Value', c2, 'Factor')
    c3 = s.n('ShaderNodeMix', data_type='RGBA')
    s.l(c2, 'Result', c3, 'A')
    c3.inputs['B'].default_value = (0.36, 0.37, 0.39, 1)
    s.l(wear2, 'Value', c3, 'Factor')
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
 'B': "11110 10001 10001 11110 10001 10001 11110", 'O': "01110 10001 10001 10001 10001 10001 01110",
 'D': "11110 10001 10001 10001 10001 10001 11110", 'Y': "10001 10001 01010 00100 00100 00100 00100",
 'J': "00111 00010 00010 00010 00010 10010 01100", '0': "01110 10001 10011 10101 11001 10001 01110",
 '3': "11110 00001 00001 01110 00001 00001 11110", '5': "11111 10000 11110 00001 00001 10001 01110",
 '1': "00100 01100 00100 00100 00100 00100 01110", 'R': "11110 10001 10001 11110 10100 10010 10001",
 'U': "10001 10001 10001 10001 10001 10001 01110", 'E': "11111 10000 10000 11110 10000 10000 11111",
 'V': "10001 10001 10001 10001 10001 01010 00100",
 ' ': "00000 " * 6 + "00000",
}

def oled_image(lines, W=None, H=28):
    W = W or max(98, 6 * max(len(l) for l in lines) + 8)
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


# ----------------------------------------------------------------- procedural face artwork (crust stencil)
# Channels (Non-Color): R = accent ink, G = bone ink, B = paint scratched through to raw aluminium.
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
        print("[render] stencil font fetch failed (%s); falling back to PIL default" % e)
        return None


class ArtCtx:
    """Drawing context handed to each piece's motif function (all inputs in mm, y up)."""
    def __init__(self, W, L, S, font_path, rng):
        self.W, self.L, self.S, self.font_path, self.rng = W, L, S, font_path, rng
        self.AW, self.AH = int(W * S), int(L * S)
    def P(self, x, y):
        return ((x + self.W / 2) * self.S, (self.L / 2 - y) * self.S)
    def new(self):
        from PIL import Image, ImageDraw
        im = Image.new('L', (self.AW, self.AH), 0)
        return im, ImageDraw.Draw(im)
    def font(self, sz):
        from PIL import ImageFont
        return ImageFont.truetype(self.font_path, sz) if self.font_path else ImageFont.load_default()
    def text_mask(self, text, cx, cy, width_mm, rot=0.0):
        """stencil lettering fitted to a width, rotated, as a float mask."""
        from PIL import Image, ImageDraw
        sz = 200
        bb = self.font(sz).getbbox(text)
        sz = max(8, int(sz * width_mm * self.S / (bb[2] - bb[0])))
        fo = self.font(sz); bb = fo.getbbox(text)
        im = Image.new('L', (bb[2] - bb[0] + 20, bb[3] - bb[1] + 20), 0)
        ImageDraw.Draw(im).text((10 - bb[0], 10 - bb[1]), text, fill=255, font=fo)
        im = im.rotate(rot, expand=True, resample=Image.BICUBIC)
        full = Image.new('L', (self.AW, self.AH), 0)
        x, y = self.P(cx, cy)
        full.paste(im, (int(x - im.width / 2), int(y - im.height / 2)))
        return np.asarray(full, dtype=np.float32) / 255.0


def make_artwork(font_path, layout, W, L, S, motif, words, tapes=(), tally=(), seed=7):
    """motif(ctx) -> (accent_mask, bone_extra_mask or None) as float arrays.
    words: [(text, cx, cy, width_mm, rot_deg)] stencilled in bone with the accent ink mis-registered behind."""
    from PIL import Image, ImageDraw, ImageFilter
    rng = np.random.default_rng(seed)
    ctx = ArtCtx(W, L, S, font_path, rng)
    AW, AH = ctx.AW, ctx.AH
    q = max(0.3, S / 14.0)                                   # keeps grain sizes in mm independent of resolution

    def blur(a, r):
        return np.asarray(Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).filter(
            ImageFilter.GaussianBlur(max(0.3, r))), dtype=np.float32) / 255.0
    def noise(sigma):
        b = blur(rng.random((AH, AW), dtype=np.float32), sigma * q)
        return (b - b.min()) / (b.max() - b.min() + 1e-9)
    def arr(img):
        return np.asarray(img, dtype=np.float32) / 255.0
    def rough(m, r, amt, n):
        return np.clip((blur(m, r * q) - 0.5) * 3.0 + 0.5 + (n - 0.5) * amt, 0, 1) > 0.5

    n_fine = noise(1.2); n_mid = noise(5.0); n_big = noise(40)

    layer_a, extra = motif(ctx)
    word = np.zeros((AH, AW), np.float32)
    for (t, cx, cy, w, rot) in words:
        word = np.maximum(word, ctx.text_mask(t, cx, cy, w, rot))
    dx, dy = int(1.0 * S), int(0.7 * S)
    word_a = np.roll(np.roll(word, dx, axis=1), dy, axis=0)             # sloppy registration

    # clear zones keep controls and labels legible
    C = Image.new('L', (AW, AH), 0); dc = ImageDraw.Draw(C)
    for (x, y, rr_) in layout['circles']:
        px_, py_ = ctx.P(x, y); dc.ellipse((px_ - rr_ * S, py_ - rr_ * S, px_ + rr_ * S, py_ + rr_ * S), fill=255)
    for (x, y, w, h) in layout['rects']:
        px_, py_ = ctx.P(x, y); dc.rectangle((px_ - w / 2 * S, py_ - h / 2 * S, px_ + w / 2 * S, py_ + h / 2 * S), fill=255)
    clear = np.clip(blur(arr(C), 1.2 * S) * 1.6, 0, 1)

    # accent ink: rough photocopy edges, drop-outs, drips, overspray
    mo = rough(layer_a, 2.2, 0.8, n_fine).astype(np.float32)
    mo *= (0.15 + 0.85 * (n_mid > 0.30)).astype(np.float32) * (n_big * 0.5 + 0.7).clip(0, 1)
    mo *= (1 - 0.88 * clear)
    edge = (mo[:-1] > 0.5) & (mo[1:] < 0.5)
    ys, xs = np.nonzero(edge)
    sel = rng.random(len(ys)) < 0.0016 * (14.0 / S) ** 2 * 0.6
    D = Image.new('L', (AW, AH), 0); dd = ImageDraw.Draw(D)
    for y, x in zip(ys[sel], xs[sel]):
        ln = int(rng.uniform(1.0, 9.0) * S); w = int(rng.uniform(0.2, 0.45) * S) + 1
        dd.line((x, y, x, y + ln), fill=255, width=w)
        dd.ellipse((x - w * 0.9, y + ln - w * 0.6, x + w * 0.9, y + ln + w * 1.1), fill=255)
    mo = np.maximum(mo, arr(D) * (1 - 0.9 * clear) * 0.95)
    halo = blur(mo, 2.4 * S)
    dots = rng.random((AH, AW), dtype=np.float32) < (halo ** 1.4) * 0.55
    mo = np.maximum(mo, dots.astype(np.float32) * (1 - mo) * 0.85 * (1 - 0.9 * clear))
    mo *= (0.82 + 0.18 * n_fine)
    word_a_r = rough(word_a, 2.5, 1.0, noise(1.0)).astype(np.float32) * (0.2 + 0.8 * (n_mid > 0.2))
    mo = np.maximum(mo, word_a_r * 0.95)

    # bone ink: wordmark, motif extras, tape, halftone borders, tally, xerox speckle
    mg = rough(word, 2.0, 0.9, n_fine).astype(np.float32) * (0.12 + 0.88 * (noise(3.0) > 0.36))
    if extra is not None:
        mg = np.maximum(mg, rough(extra, 1.6, 0.9, n_fine).astype(np.float32) * (0.2 + 0.8 * (n_mid > 0.22)) * (1 - 0.9 * clear))
    G = Image.new('L', (AW, AH), 0); dg = ImageDraw.Draw(G)
    for (x, y, w, h, rot) in tapes:
        px_, py_ = ctx.P(x, y)
        hw, hh = w / 2 * S, h / 2 * S
        ep = [(-hw, -hh)]
        for t in np.linspace(-hw, hw, 40): ep.append((t, -hh + rng.uniform(-0.1, 0.1) * S))
        ep.append((hw + rng.uniform(0, 0.8) * S, -hh))
        for yy in np.linspace(-hh, hh, 6): ep.append((hw + rng.uniform(-0.4, 0.5) * S, yy))
        for t in np.linspace(hw, -hw, 40): ep.append((t, hh + rng.uniform(-0.1, 0.1) * S))
        for yy in np.linspace(hh, -hh, 6): ep.append((-hw + rng.uniform(-0.5, 0.4) * S, yy))
        ca, sa = math.cos(math.radians(rot)), math.sin(math.radians(rot))
        dg.polygon([(px_ + u * ca + v * sa, py_ - (u * sa - v * ca)) for u, v in ep], fill=150)
    mg = np.maximum(mg, arr(G) * (1 - 0.8 * clear) * (0.55 + 0.45 * n_mid))
    T = Image.new('L', (AW, AH), 0); dt = ImageDraw.Draw(T)
    for (tx, ty) in tally:
        x0, y0 = ctx.P(tx, ty)
        for k in range(4):
            dt.line((x0 + k * 0.9 * S, y0 - 3.5 * S, x0 + k * 0.9 * S + rng.uniform(-3, 3), y0 + 3.5 * S), fill=255, width=int(0.28 * S) + 1)
        dt.line((x0 - 0.6 * S, y0 + 2.4 * S, x0 + 3.5 * S, y0 - 2.4 * S), fill=255, width=int(0.28 * S) + 1)
    mg = np.maximum(mg, rough(arr(T), 1.5, 0.8, n_fine).astype(np.float32) * 0.9)
    yy, xx = np.mgrid[0:AH, 0:AW].astype(np.float32)
    u = (xx + yy) * 0.7071; v = (yy - xx) * 0.7071
    cell = 7.0 * max(1.0, S / 14.0) if S >= 14 else 7.0 * S / 14.0 * 1.6
    dist = np.sqrt(((u / cell) % 1 - 0.5) ** 2 + ((v / cell) % 1 - 0.5) ** 2)
    ymm = L / 2 - yy / S
    g = np.clip((ymm - (L / 2 - 6.5)) / 6.5, 0, 1) + np.clip((-(L / 2 - 4.0) - ymm) / 3.5, 0, 1)
    ht = (dist < 0.62 * np.sqrt(np.clip(g, 0, 1))).astype(np.float32) * 0.8 * (1 - 0.8 * clear)
    mg = np.maximum(mg, ht)
    mg = np.maximum(mg, ((n_fine > 0.83) & (rng.random((AH, AW)) < 0.8)).astype(np.float32) * (n_big > 0.5) * 0.5 * (1 - 0.9 * clear))
    mg *= (0.86 + 0.14 * n_fine)

    # scratches / chips: raw aluminium (kept sparse and rough so the rim reads as worn paint)
    B = Image.new('L', (AW, AH), 0); db = ImageDraw.Draw(B)
    area = W * L / (120.0 * 190.0)
    for k in range(int(110 * area ** 0.8) + 25):
        if rng.random() < 0.45:
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
    for k in range(int(70 * area ** 0.8) + 15):
        side = rng.integers(0, 4); t = rng.random(); off = rng.exponential(0.8) * S
        if side == 0: x, y = t * AW, off
        elif side == 1: x, y = t * AW, AH - off
        elif side == 2: x, y = off, t * AH
        else: x, y = AW - off, t * AH
        r = rng.uniform(0.25, 1.3) * S
        db.polygon([(x + r * rng.uniform(0.4, 1.2) * math.cos(a), y + r * rng.uniform(0.4, 1.2) * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 8, endpoint=False)], fill=255)
    mb = blur(arr(B), 0.6) * (0.5 + 0.5 * (n_mid > 0.2))

    out = np.zeros((AH, AW, 4), np.float32)
    out[..., 0] = np.clip(mo, 0, 1); out[..., 1] = np.clip(mg, 0, 1); out[..., 2] = np.clip(mb, 0, 1); out[..., 3] = 1.0
    return out

def art_to_image(rgba):
    h, w = rgba.shape[:2]
    img = bpy.data.images.new('face_art', w, h, alpha=True, float_buffer=False)
    img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(np.ascontiguousarray(rgba[::-1]).ravel())
    img.update()
    return img


# ---- small drawing helpers shared by the motif functions ------------------------------------------
def gear(ctx, d, cx, cy, r, teeth, hole=0.35, spokes=0, tooth=0.16):
    """filled stencil cog (mm)."""
    P = []
    for i in range(teeth):
        a0 = 2 * math.pi * i / teeth; da = 2 * math.pi / teeth
        for (f, rad) in ((0.0, 1.0 - tooth), (0.18, 1.0), (0.5, 1.0), (0.68, 1.0 - tooth)):
            a = a0 + f * da
            P.append((cx + rad * r * math.cos(a), cy - rad * r * math.sin(a)))
    d.polygon(P, fill=255)
    d.ellipse((cx - hole * r, cy - hole * r, cx + hole * r, cy + hole * r), fill=0)
    for k in range(spokes):
        a = 2 * math.pi * k / spokes
        d.line((cx, cy, cx + r * 0.95 * math.cos(a), cy - r * 0.95 * math.sin(a)), fill=0, width=int(r * 0.09) + 1)
    d.ellipse((cx - 0.12 * r, cy - 0.12 * r, cx + 0.12 * r, cy + 0.12 * r), fill=255)

def arr_of(im):
    return np.asarray(im, dtype=np.float32) / 255.0


# ----------------------------------------------------------------- parts
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

# ----------------------------------------------------------------- scene-level helpers
def build_enclosure(m_body, w, l, h, r=7.0, bev=1.3, nb=5, name='enclosure'):
    """Rounded-rect slab (footprint w x l, height h, mm) with bevelled top/bottom rims; top face at z=h."""
    prof = [(bev * (1 - math.sin(math.pi / 2 * k / nb)), bev * (1 - math.cos(math.pi / 2 * k / nb))) for k in range(nb + 1)]
    prof += [(bev * (1 - math.cos(math.pi / 2 * k / nb)), h - bev * (1 - math.sin(math.pi / 2 * k / nb))) for k in range(nb + 1)]
    bm = bmesh.new()
    rings = []
    for d, z in prof:
        pts = rrect(w / 2 - d, l / 2 - d, r - d, 10)
        rings.append([bm.verts.new((x * MM, y * MM, z * MM)) for x, y in pts])
    for a, b in zip(rings[:-1], rings[1:]):
        n = len(a)
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bm.faces.new(rings[-1])
    bm.faces.new(rings[0][::-1])
    return finish_bm(bm, name, [m_body], angle=40)

def build_oled(mats, x, y, s=1.0):
    root = empty('oled', x, y, Z)
    parts = []
    frame = rbox('oled_frame', 62, 21, 0.9, 2.2, [mats['black']], z0=0)
    glass_base = rbox('oled_well', 56.6, 16.4, 0.12, 0.8, [mats['glass_black']], z0=0.9)
    disp = plane('oled_display', 56, 16, [mats['oled']]); disp.location.z = 1.05 * MM
    cover = plane('oled_cover', 56.6, 16.4, [mats['glass']]); cover.location.z = 1.35 * MM
    cover.visible_shadow = False
    for p in (frame, glass_base, disp, cover):
        p.parent = root
    root.scale = (s, s, s)
    return root

def build_jewel(x, y, mats, s=1.0):
    """chrome-bezelled faceted jewel lamp (lit)."""
    root = empty('jewel', x, y, Z)
    bez = lathe('jewel_bezel', [(0, 0), (11.5, 0), (11.5, 2.2), (10.2, 3.0), (8.6, 2.6), (8.6, 1.0), (0, 1.0)], [mats['chrome']], 64, 40)
    gem = lathe('jewel_gem', [(0, 1.0), (8.6, 1.0), (8.6, 3.0), (6.0, 6.5), (2.5, 8.2), (0, 8.5)], [mats['jewel']], 12, 20)
    for p in (bez, gem): p.parent = root
    gl = plane('jewel_glow', 46, 46, [mats['glow']])
    gl.location = (0, 0, 0.04 * MM); gl.parent = root; gl.visible_shadow = False
    root.scale = (s, s, s)
    pl = bpy.data.lights.new('jewel_pt', 'POINT'); pl.energy = 0.5; pl.color = (1, 0.06, 0.02); pl.shadow_soft_size = 0.004
    po = bpy.data.objects.new('jewel_pt', pl); bpy.context.scene.collection.objects.link(po)
    po.location = (x * MM, y * MM, (Z + 12 * s) * MM); po.parent = None
    return root

def mat_tolex():
    s = M('tolex')
    tc = s.n('ShaderNodeTexCoord')
    vor = s.n('ShaderNodeTexVoronoi')
    vor.inputs['Scale'].default_value = 1500
    s.l(tc, 'Object', vor, 'Vector')
    n = s.noise(260, 3, 0.5, (tc, 'Object'))
    wrinkle = s.noise(55, 4, 0.6, (tc, 'Object'))
    h = s.math('ADD', s.math('MULTIPLY', (vor, 'Distance'), 0.8), s.math('MULTIPLY', (n, 'Fac'), 0.25))
    bump = s.n('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.85
    bump.inputs['Distance'].default_value = 0.0005
    s.l(h, 'Value', bump, 'Height')
    bump2 = s.n('ShaderNodeBump')
    bump2.inputs['Strength'].default_value = 0.25
    bump2.inputs['Distance'].default_value = 0.002
    s.l(wrinkle, 'Fac', bump2, 'Height'); s.l(bump, 'Normal', bump2, 'Normal')
    r = s.math('ADD', 0.52, s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.5), 0.25))
    b = s.bsdf(Base_Color=(0.004, 0.004, 0.0045, 1), Specular_IOR_Level=0.16, Sheen_Weight=0.0)
    s.l(r, 'Value', b, 'Roughness'); s.l(bump2, 'Normal', b, 'Normal')
    return s.m

def build_head_extras(W, D, H, mats):
    """strap handle + feet for an amp head; body spans x +-W/2, y -D/2..D/2 (mm), z 0..H."""
    cu = bpy.data.curves.new('handle', 'CURVE'); cu.dimensions = '3D'
    sp = cu.splines.new('POLY'); n = 24
    sp.points.add(n)
    for i in range(n + 1):
        t = i / n
        sp.points[i].co = ((-90 + 180 * t) * MM, 0, (H + 6 + 36 * math.sin(math.pi * t) ** 0.5) * MM, 1)
    cu.bevel_depth = 7 * MM; cu.bevel_resolution = 5; cu.use_fill_caps = True
    cu.materials.append(mats['leather'])
    h = new_obj('handle', cu)
    for sx in (-1, 1):
        mnt = lathe('handle_mount', [(0, 0), (13, 0), (13, 4), (0, 4)], [mats['chrome']], 24, 40)
        mnt.location = (sx * 90 * MM, 0, H * MM)
    return h


# ----------------------------------------------------------------- lights / camera / world
def softbox(name, w, h, loc, target, strength, tint=(1, 1, 1), gradient=True):
    pl = plane(name, w * 1000, h * 1000, [mat_emit_plane(strength, gradient, tint)])
    pl.location = loc
    d = Vector(target) - Vector(loc)
    pl.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    pl.visible_camera = False
    pl.visible_shadow = False
    return pl

def build_backdrop(k=1.0):
    s = M('backdrop')
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(300 / k, 3, 0.5, (tc, 'Object'))
    r = s.math('ADD', 0.62, s.math('MULTIPLY', s.math('SUBTRACT', (n, 'Fac'), 0.5), 0.25))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.08; bump.inputs['Distance'].default_value = 0.0004 * k
    s.l(n, 'Fac', bump, 'Height')
    ln = s.n('ShaderNodeVectorMath', operation='LENGTH')
    s.l(tc, 'Object', ln, 0)
    vig = s.math('ADD', 0.12, s.math('MULTIPLY', s.math('SUBTRACT', 1.0, s.math('DIVIDE', (ln, 'Value'), 1.5 * k), clamp=True), 0.88))
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
    prof = [(-3.0 * k, 0.0), (0.45 * k, 0.0)]
    Rr = 0.45 * k
    for i in range(1, 17):
        t = math.pi / 2 * i / 16
        prof.append((0.45 * k + Rr * math.sin(t), Rr * (1 - math.cos(t))))
    prof.append((0.9 * k, 2.5 * k))
    bm = bmesh.new()
    a = [(bm.verts.new((-3 * k, y, z)), bm.verts.new((3 * k, y, z))) for y, z in prof]
    for p, q in zip(a[:-1], a[1:]):
        bm.faces.new((p[0], p[1], q[1], q[0]))
    return finish_bm(bm, 'backdrop', [s.m], angle=80)

def setup_world():
    w = bpy.data.worlds.new('w'); bpy.context.scene.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes['Background']
    bg.inputs['Color'].default_value = hexcol('#5a534d')
    bg.inputs['Strength'].default_value = 0.10

def setup_render(res, scale, samples, adaptive=True):
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    cy = sc.cycles
    cy.device = 'CPU'
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 3
    cy.use_adaptive_sampling = adaptive
    cy.adaptive_threshold = 0.01
    cy.adaptive_min_samples = 24
    cy.samples = samples
    cy.light_sampling_threshold = 0.05
    cy.use_denoising = True
    cy.denoiser = 'OPENIMAGEDENOISE'
    cy.sample_clamp_indirect = 4.0
    cy.caustics_reflective = False
    cy.caustics_refractive = False
    cy.max_bounces = 6; cy.glossy_bounces = 4; cy.diffuse_bounces = 2; cy.transparent_max_bounces = 6
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
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.resolution_percentage = scale

def make_camera(cfg):
    """cfg: dict(kind='persp'|'ortho', loc=(m), target=(m), lens, fstop, ortho_scale, rot=(deg))."""
    cam = bpy.data.cameras.new('cam')
    co = bpy.data.objects.new('cam', cam)
    bpy.context.scene.collection.objects.link(co)
    bpy.context.scene.camera = co
    co.location = cfg['loc']
    if cfg['kind'] == 'persp':
        cam.lens = cfg.get('lens', 85)
        tgt = bpy.data.objects.new('tgt', None)
        bpy.context.scene.collection.objects.link(tgt)
        tgt.location = cfg['target']
        cam.dof.use_dof = True
        cam.dof.focus_object = tgt
        cam.dof.aperture_fstop = cfg.get('fstop', 9.0)
        tr = co.constraints.new('TRACK_TO'); tr.target = tgt
        tr.track_axis = 'TRACK_NEGATIVE_Z'; tr.up_axis = 'UP_Y'
    else:
        cam.type = 'ORTHO'
        cam.ortho_scale = cfg['ortho_scale']
        co.rotation_euler = tuple(math.radians(a) for a in cfg.get('rot', (0, 0, 0)))
    cam.clip_start, cam.clip_end = 0.01 * cfg.get('k', 1.0), 12.0
    return co

def build_lights(k=1.0, c=(0, 0, 0.03), front=False):
    c = Vector(c)
    P = lambda x, y, z: (c.x + x * k, c.y + y * k, c.z + (z - 0.03) * k + 0.0)
    softbox('key', 0.95 * k, 0.75 * k, P(-0.55, -0.40, 0.62), c, 11.0, (1.0, 0.95, 0.9))
    softbox('rim', 0.14 * k, 0.9 * k, P(0.50, 0.55, 0.28), c, 70.0, (1.0, 0.97, 0.95), gradient=False)
    softbox('rim2', 0.14 * k, 0.9 * k, P(-0.55, 0.40, 0.25), c, 30.0, (0.95, 0.97, 1.0), gradient=False)
    softbox('fill', 0.8 * k, 0.8 * k, P(0.6, -0.5, 0.25), c, 0.9, (0.95, 0.97, 1.0))
    softbox('top', 0.9 * k, 0.9 * k, P(0.0, 0.0, 0.95), c, 0.8, (1, 0.98, 0.95))
    if front:        # head front views: a soft panel behind the camera so chrome/metal reads
        softbox('front', 0.9 * k, 0.6 * k, P(0.1, -1.1, 0.10), c, 1.4, (1, 0.98, 0.95))


# ----------------------------------------------------------------- piece runner
def render_to(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)

def run(spec):
    """spec keys: name, kind('pedal'|'head'), plate=(w,l,thick or height), body=(w,d,h) for heads, S (art px/mm),
    art(font)->rgba, ink_a, ink_b (hex), populate(mats, mode)->knobs, oled_lines, hero/ortho camera dicts,
    ortho_res, k, center, strip(bool)"""
    global W_, L_, H_, Z
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=['hero', 'ortho', 'strip', 'open', 'all'])
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=int, default=100)
    ap.add_argument('--samples', type=int, default=0)
    ap.add_argument('--frames', type=int, default=64)
    ap.add_argument('--font-dir', default=os.path.join(os.path.expanduser('~'), '.cache', 'pedal_b2_fonts'))
    ap.add_argument('--dump-art', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    kind = spec['kind']
    W_, L_, H_ = spec['plate'][0], spec['plate'][1], spec['plate'][2]
    Z = H_
    font = fetch_font(a.font_dir)
    if a.dump_art:
        from PIL import Image
        rgba = spec['art'](font)
        Image.fromarray((rgba[..., :3] * 255).astype(np.uint8)).save(os.path.join(a.out, spec['name'] + '_art_channels.png'))
        return
    modes = spec.get('modes', ['hero', 'ortho'] + (['strip'] if spec.get('strip') else [])) if a.mode == 'all' else [a.mode]
    for mode in modes:
        t0 = time.time()
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if mode == 'strip':
            art = bpy.data.images.new('blank', 4, 4, alpha=True); art.colorspace_settings.name = 'Non-Color'
        else:
            art = art_to_image(spec['art'](font))
        mats = dict(
            powder=mat_powder(art, spec['ink_a'], spec['ink_b'], gain_a=spec.get('ink_gain', 0.45)), chrome=mat_chrome(), chrome_dark=mat_chrome('chrome_dark', 0.25),
            chrome_sat=mat_chrome('chrome_satin', 0.22), alu=mat_alu_brushed(), black=mat_black_anodised(),
            white=mat_ink('inlay_white', hexcol('#e8e4d8'), 0.45),
            led_on=mat_emit('led_on', spec.get('led_rgb', (1.0, 0.20, 0.0, 1)), 6.0), led_off=mat_led_off('led_off', hexcol(spec.get('led_off', '#5a1c05'))),
            led_red_on=mat_emit('led_red_on', (1.0, 0.02, 0.01, 1), 9.0), led_red_off=mat_led_off('led_red_off', (0.22, 0.01, 0.008, 1)),
            jewel=mat_emit('jewel', spec.get('jewel_rgb', (1.0, 0.10, 0.02, 1)), 4.0), rubber=mat_black_anodised('rubber'), paper=mat_paper('cone_paper', hexcol('#6b4a2c')), paper_dark=mat_paper('dust_cap', hexcol('#3a2616')), label=mat_ink('spk_label', hexcol('#d9d2b8'), 0.6), label_ink=mat_ink('spk_label_ink', (0.01, 0.01, 0.01, 1), 0.6), mic_black=mat_black_anodised('mic_black'), mic_mesh=mat_mesh_grey(), baffle=mat_baffle(), art=art, leather=mat_tolex(),
            glow=mat_glow_decal((1.0, 0.07, 0.03, 1), 0.6), glass=mat_glass_overlay(), glass_black=mat_black_anodised('oled_well'),
            slot=mat_ink('slot', (0.01, 0.01, 0.01, 1), 0.6))
        oimg = oled_image(spec.get('oled_lines', ["HM-2w CHAINSAW", "std · A2 · 48k"]))
        mats['oled'] = mat_oled_display(oimg, oimg.size[0], 28)
        k = spec.get('k', 1.0)
        root = None
        if kind in ('pedal', 'cab'):
            pass
        if kind == 'pedal':
            build_enclosure(mats['powder'], W_, L_, H_, r=spec.get('corner', 7.0), bev=spec.get('bevel', 1.0), nb=5)
            root = None
        elif kind == 'head':
            bw, bd, bh = spec['body']
            tol = mat_tolex()
            body = build_enclosure(tol, bw, bd, bh, r=6.0, bev=3.0, nb=5, name='tolex_body')
            body.rotation_euler = (0, 0, 0)
            build_head_extras(bw, bd, bh, mats)
            build_head_details(bw, bd, bh, mats)
            root = empty('face_root', 0, -bd / 2 + H_ - 0.5, bh / 2)
            root.rotation_euler = (math.pi / 2, 0, 0)
            before = set(bpy.data.objects)
            build_enclosure(mats['powder'], W_ - 1.0, L_ - 1.0, H_, r=3.0, bev=0.8, nb=4, name='faceplate')
            piping_loop(W_ / 2 + 1.5, L_ / 2 + 1.5, 6.0, 3.5, 2.6, mats['black'])
        if kind == 'cab':
            knobs = spec['build'](mats, mode); root = None
        else:
            knobs = spec['populate'](mats, mode)
        if root is not None:
            for ob in list(bpy.data.objects):
                if ob not in before and ob is not root and ob.parent is None and ob.type != 'CAMERA':
                    ob.parent = root
        setup_world(); build_backdrop(k)
        samples = a.samples or {'hero': 40, 'ortho': 40, 'strip': 16, 'open': 40}[mode]
        res = {'hero': (1600, 1200), 'ortho': spec.get('ortho_res', (1200, 1800)), 'strip': (128, 128), 'open': spec.get('ortho_res', (1200, 1800))}[mode]
        setup_render(res, a.scale, samples, adaptive=(mode != 'strip'))
        if mode == 'strip':
            make_camera(dict(kind='ortho', loc=(0, 0, 0.5), ortho_scale=0.039))
        else:
            make_camera(spec['hero'] if mode == 'hero' else spec['ortho'])
        build_lights(k, spec.get('center', (0, 0, 0.03)), front=(mode in ('ortho', 'open') and kind in ('head', 'cab')))
        tag = spec['name']
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
            sheet.save(os.path.join(a.out, tag + '_knob_strip.png'))
        else:
            render_to(os.path.join(a.out, spec.get('fnames', {}).get(mode, '%s_%s.png' % (tag, 'hero_3q' if mode == 'hero' else 'ortho'))))
        print('[render] %s %s done in %.1fs (samples=%d)' % (tag, mode, time.time() - t0, samples))




# ----------------------------------------------------------------- real-cab details (corner caps, piping, stitching, speakers, mic)
def tube_path(name, pts, r, mat, closed=True, ns=8, ref=(0, 0, 1), angle=60):
    """tube swept along a 3D polyline (mm)."""
    P = [Vector(p) for p in pts]
    n = len(P)
    bm = bmesh.new()
    rings = []
    for i in range(n):
        a = P[(i - 1) % n] if (closed or i > 0) else P[i]
        b = P[(i + 1) % n] if (closed or i < n - 1) else P[i]
        t = (b - a).normalized()
        rv = Vector(ref)
        if abs(t.dot(rv)) > 0.95:
            rv = Vector((0, 1, 0))
        n1 = t.cross(rv).normalized(); n2 = t.cross(n1).normalized()
        ring = []
        for k in range(ns):
            ang = 2 * math.pi * k / ns
            q = P[i] + n1 * (r * math.cos(ang)) + n2 * (r * math.sin(ang))
            ring.append(bm.verts.new((q.x * MM, q.y * MM, q.z * MM)))
        rings.append(ring)
    last = n if closed else n - 1
    for i in range(last):
        a, b = rings[i], rings[(i + 1) % n]
        for k in range(ns):
            j = (k + 1) % ns
            bm.faces.new((a[k], a[j], b[j], b[k]))
    return finish_bm(bm, name, [mat], angle)

def stitch_line(name, p0, p1, nrm, mat, pitch=6.5, length=3.6, w=0.9, h=0.45):
    """dashed thread stitching along a seam (mm, world coordinates); nrm = surface normal."""
    p0, p1, nrm = Vector(p0), Vector(p1), Vector(nrm).normalized()
    d = p1 - p0; L = d.length; t = d.normalized()
    s = t.cross(nrm).normalized()
    bm = bmesh.new()
    cnt = int(L / pitch)
    for i in range(cnt + 1):
        c = p0 + t * (i * pitch)
        vs = []
        for sz in (-1, 1):
            for sy in (-1, 1):
                for sx in (-1, 1):
                    q = c + t * (sx * length / 2) + s * (sy * w / 2) + nrm * (sz * h / 2 + h / 2 - 0.1)
                    vs.append(bm.verts.new((q.x * MM, q.y * MM, q.z * MM)))
        # vs index = sz*4 + sy*2 + sx  (with -1/1 mapped 0/1)
        for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
            bm.faces.new([vs[k] for k in f])
    return finish_bm(bm, name, [mat], angle=50)

def corner_caps(bw, bd, bh, mats, front=True, size=28.0):
    """chrome-satin corner protectors at the body corners (mm)."""
    for sx in (-1, 1):
        for sy in (-1, 1):
            if sy < 0 and not front:
                continue
            for zc in (1, -1):
                cx = sx * (bw / 2 - size / 2 + 1.6); cy = sy * (bd / 2 - size / 2 + 1.6)
                cz = size / 2 - 1.6 if zc > 0 else bh - size / 2 + 1.6
                cap = rbox('corner_cap', size, size, size, 2.0, [mats['chrome_sat']], z0=-size / 2, seg=3)
                cap.location = (cx * MM, cy * MM, cz * MM)
                m = cap.modifiers.new('b', 'BEVEL'); m.width = 2.2 * MM; m.segments = 3; m.limit_method = 'ANGLE'

def build_head_details(bw, bd, bh, mats):
    thread = mat_ink('thread', (0.12, 0.115, 0.10, 1), 0.8)
    corner_caps(bw, bd, bh, mats, front=False, size=26.0)
    stitch_line('st_top', (-(bw / 2 - 18), -bd / 2 + 16, bh), (bw / 2 - 18, -bd / 2 + 16, bh), (0, 0, 1), thread)
    stitch_line('st_top_b', (-(bw / 2 - 18), bd / 2 - 16, bh), (bw / 2 - 18, bd / 2 - 16, bh), (0, 0, 1), thread)
    for sx in (-1, 1):
        stitch_line('st_side', (sx * bw / 2, -bd / 2 + 16, 18), (sx * bw / 2, -bd / 2 + 16, bh - 18), (sx, 0, 0), thread)
        stitch_line('st_side_t', (sx * bw / 2 - sx * 18, -bd / 2 + 60, bh), (sx * bw / 2 - sx * 18, bd / 2 - 60, bh), (0, 0, 1), thread)

def piping_loop(hx, hy, r_corner, z, rt, mat, name='piping'):
    pts = [(x, y, z) for x, y in rrect(hx, hy, r_corner, 8)]
    return tube_path(name, pts, rt, mat, closed=True, ns=8)

def mat_baffle():
    s = M('baffle')
    tc = s.n('ShaderNodeTexCoord')
    n = s.noise(900, 3, 0.6, (tc, 'Object'))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.2; bump.inputs['Distance'].default_value = 0.0002
    s.l(n, 'Fac', bump, 'Height')
    b = s.bsdf(Base_Color=(0.008, 0.008, 0.009, 1), Roughness=0.7, Specular_IOR_Level=0.2)
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_paper(name, col, rough=0.8):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    fib = s.noise(3500, 5, 0.7, (tc, 'Object'), dist=1.5)
    mc = s.n('ShaderNodeMix', data_type='RGBA')
    mc.inputs['A'].default_value = tuple(c * 0.55 for c in col[:3]) + (1,)
    mc.inputs['B'].default_value = col
    s.l(fib, 'Fac', mc, 'Factor')
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.5; bump.inputs['Distance'].default_value = 0.0002
    s.l(fib, 'Fac', bump, 'Height')
    b = s.bsdf(Roughness=rough, Specular_IOR_Level=0.2, Sheen_Weight=0.25)
    s.l(mc, 'Result', b, 'Base Color'); s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_mesh_grey(name='mic_mesh'):
    s = M(name)
    tc = s.n('ShaderNodeTexCoord')
    wv = s.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='X')
    wv.inputs['Scale'].default_value = 2600
    wv2 = s.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='Y')
    wv2.inputs['Scale'].default_value = 2600
    s.l(tc, 'Object', wv, 'Vector'); s.l(tc, 'Object', wv2, 'Vector')
    h = s.math('ADD', (wv, 'Fac'), (wv2, 'Fac'))
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.9; bump.inputs['Distance'].default_value = 0.0003
    s.l(h, 'Value', bump, 'Height')
    b = s.bsdf(Base_Color=(0.42, 0.43, 0.45, 1), Metallic=1.0, Roughness=0.38)
    s.l(bump, 'Normal', b, 'Normal')
    return s.m

def mat_grille(art, W, H, ink_a_hex, ink_b_hex, pitch=1.6):
    """woven dark salt-and-pepper cloth; stencil paint (art R/G) sits on the thread crests so the weave shows through."""
    s = M('grille_cloth')
    tc = s.n('ShaderNodeTexCoord')
    sc_ = 2 * math.pi / (pitch * MM)
    wx = s.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='X'); wx.inputs['Scale'].default_value = sc_ / 2
    wy = s.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='Y'); wy.inputs['Scale'].default_value = sc_ / 2
    s.l(tc, 'Object', wx, 'Vector'); s.l(tc, 'Object', wy, 'Vector')
    # over/under checker so the weave reads as cloth, not stripes
    ck = s.n('ShaderNodeTexChecker'); ck.inputs['Scale'].default_value = 1.0 / (pitch * MM) / 2.0
    s.l(tc, 'Object', ck, 'Vector')
    weave = s.math('ADD', s.math('MULTIPLY', (wx, 'Fac'), (ck, 'Fac')), s.math('MULTIPLY', (wy, 'Fac'), s.math('SUBTRACT', 1.0, (ck, 'Fac'))))
    # salt-and-pepper per thread cell
    sc2 = s.n('ShaderNodeVectorMath', operation='SCALE'); sc2.inputs['Scale'].default_value = 1.0 / (pitch * MM)
    s.l(tc, 'Object', sc2, 0)
    fl = s.n('ShaderNodeVectorMath', operation='FLOOR'); s.l(sc2, 'Vector', fl, 0)
    wn = s.n('ShaderNodeTexWhiteNoise', noise_dimensions='3D'); s.l(fl, 'Vector', wn, 'Vector')
    salt = s.ramp((wn, 'Value'), ((0.80, 0.0), (0.84, 1.0)))
    pep = s.ramp((wn, 'Value'), ((0.10, 1.0), (0.16, 0.0)))
    base = s.math('ADD', 0.010, s.math('SUBTRACT', s.math('MULTIPLY', (salt, 'Color'), 0.07), s.math('MULTIPLY', (pep, 'Color'), 0.006)))
    base = s.math('MULTIPLY', (base, 'Value'), s.math('ADD', 0.6, s.math('MULTIPLY', (weave, 'Value'), 0.8)))
    cloth = s.n('ShaderNodeCombineXYZ')
    for sk in 'XYZ': s.l(base, 'Value', cloth, sk)
    # paint
    mp = s.n('ShaderNodeMapping')
    mp.inputs['Scale'].default_value = (1 / (W * MM), 1 / (H * MM), 1); mp.inputs['Location'].default_value = (0.5, 0.5, 0)
    s.l(tc, 'Object', mp, 'Vector')
    tex = s.n('ShaderNodeTexImage'); tex.image = art; tex.interpolation = 'Linear'; tex.extension = 'CLIP'
    s.l(mp, 'Vector', tex, 'Vector')
    sp = s.n('ShaderNodeSeparateColor'); s.l(tex, 'Color', sp, 'Color')
    crest = s.ramp((weave, 'Value'), ((0.15, 0.45), (0.75, 1.0)))
    pa = s.math('MULTIPLY', (sp, 'Red'), (crest, 'Color'), clamp=True)
    pb = s.math('MULTIPLY', (sp, 'Green'), (crest, 'Color'), clamp=True)
    c1 = s.n('ShaderNodeMix', data_type='RGBA'); s.l(cloth, 'Vector', c1, 'A')
    c1.inputs['B'].default_value = tuple(c * 0.75 for c in hexcol(ink_a_hex)[:3]) + (1,)
    s.l(pa, 'Value', c1, 'Factor')
    c2 = s.n('ShaderNodeMix', data_type='RGBA'); s.l(c1, 'Result', c2, 'A')
    c2.inputs['B'].default_value = tuple(c * 0.62 for c in hexcol(ink_b_hex)[:3]) + (1,)
    s.l(pb, 'Value', c2, 'Factor')
    bump = s.n('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.9; bump.inputs['Distance'].default_value = 0.0006
    s.l(weave, 'Value', bump, 'Height')
    b = s.bsdf(Roughness=0.92, Specular_IOR_Level=0.1, Sheen_Weight=0.35, Sheen_Roughness=0.6)
    s.l(c2, 'Result', b, 'Base Color'); s.l(bump, 'Normal', b, 'Normal')
    return s.m

def build_speaker(cx, cy, z0, mats, label="V-30 TYPE 16 OHM"):
    """generic 12-inch driver in local panel coordinates; z0 = baffle front face (mm)."""
    root = empty('speaker', cx, cy, z0)
    parts = []
    flange = lathe('spk_flange', [(143, 0), (163, 0), (163, 2.2), (143, 2.2), (143, 0)], [mats['black']], 128, 40)
    parts.append(flange)
    for k in range(8):                                   # mounting bolts
        a = 2 * math.pi * (k + 0.5) / 8
        bolt = lathe('bolt', [(0, 2.2), (3.4, 2.2), (3.4, 3.4), (0, 3.4)], [mats['chrome_dark']], 12, 50)
        bolt.location = (math.cos(a) * 153 * MM, math.sin(a) * 153 * MM, 0); parts.append(bolt)
    sur = [(142, -0.5), (140.5, 1.8), (136, 3.2), (131, 1.8), (128.5, -0.5)]
    surround = lathe('spk_surround', sur, [mats['rubber']], 128, 40)
    cone = [(128.5, -0.5)]
    for r in np.arange(126.0, 35.0, -2.0):
        t = (128 - r) / 92.0
        cone.append((float(r), -1.0 - 48.0 * t ** 0.9 + 0.8 * math.sin((128 - r) * 0.5)))
    cone.append((34.0, -49.5))
    cone_o = lathe('spk_cone', cone, [mats['paper']], 128, 30)
    cap = [(34.0, -49.5), (33.5, -46.0)]
    for r in np.linspace(32.0, 0.0, 12):
        cap.append((float(r), -46.0 + 11.0 * (1 - (r / 34.0) ** 2) ** 0.8))
    cap_o = lathe('spk_dustcap', cap, [mats['paper_dark']], 64, 60)
    parts += [surround, cone_o, cap_o]
    lab = rbox('spk_label', 46, 9, 0.3, 1.0, [mats['label']], z0=2.2, seg=2)
    lab.location = (0, -153 * MM, 0)
    parts.append(lab)
    tx = add_text(label, 2.6, 0, -153, mats['label_ink'], extrude=0.05)
    tx.location = (0, -153 * MM, 2.6 * MM)
    parts.append(tx)
    for p in parts: p.parent = root
    return root

def build_mic(mats):
    """generic unbranded dynamic instrument mic (grey mesh head, black body), axis along local +Z, tip at z=0."""
    prof_head = [(0, 0.0), (6.5, 0.0), (9.4, 0.8), (10.0, 3.0), (10.0, 18.0), (9.0, 20.0), (7.6, 21.0)]
    head = lathe('mic_head', prof_head, [mats['mic_mesh']], 48, 50)
    ring = lathe('mic_ring', [(7.6, 21.0), (10.6, 21.0), (10.6, 24.0), (7.6, 24.0), (7.6, 21.0)], [mats['chrome_sat']], 48, 50)
    body_prof = [(7.4, 24.0), (7.8, 30.0), (8.0, 70.0), (7.8, 110.0), (7.6, 150.0), (7.8, 168.0), (9.0, 172.0), (0, 172.0)]
    body = lathe('mic_body', body_prof, [mats['mic_black']], 48, 40)
    tail = lathe('mic_tail', [(9.0, 172.0), (9.4, 174.0), (9.4, 190.0), (6.0, 194.0), (0, 194.0)], [mats['mic_black']], 48, 40)
    root = empty('mic', 0, 0, 0)
    for p in (head, ring, body, tail): p.parent = root
    return root


# ----------------------------------------------------------------- amp head control panel (shared by SAW and BODY)
AMP_KNOBS = [("GAIN", -180, 0.55), ("LOW", -108, 0.60), ("MID", -36, 0.45), ("HIGH", 36, 0.70), ("LEVEL", 108, 0.50), ("PRESENCE", 180, 0.80)]
AMP_KY, AMP_KS, AMP_LABEL_DY = 80.0, 1.6, -33.0
AMP_JEWEL, AMP_TOGGLE, AMP_OLED = (258.0, 96.0), (258.0, 52.0), (-252.0, 78.0)

def amp_layout():
    circles = [(x, AMP_KY, 33.0) for _, x, _ in AMP_KNOBS]
    circles += [(AMP_JEWEL[0], AMP_JEWEL[1], 17.0), (AMP_TOGGLE[0], AMP_TOGGLE[1], 14.0)]
    circles += [(sx * 285.0, sy * 118.0, 6.0) for sx in (-1, 1) for sy in (-1, 1)]
    rects = [(x, AMP_KY + AMP_LABEL_DY, 40, 8.0) for _, x, _ in AMP_KNOBS]
    rects += [(AMP_TOGGLE[0], AMP_TOGGLE[1] - 27, 24, 7.0), (AMP_OLED[0], AMP_OLED[1], 74, 29)]
    return dict(circles=circles, rects=rects)

def populate_amp(mats, mode):
    ink_w = mat_ink('print_white', hexcol('#e8e4d8'), 0.5)
    knobs = []
    for name, x, v in AMP_KNOBS:
        k = Knob(name, x, AMP_KY, v, mats); k.root.scale = (AMP_KS,) * 3; knobs.append(k)
        add_text(name, 5.4, x, AMP_KY + AMP_LABEL_DY, ink_w, embolden=0.04)
    build_jewel(AMP_JEWEL[0], AMP_JEWEL[1], mats, 1.0)
    t = build_toggle(AMP_TOGGLE[0], AMP_TOGGLE[1], 20, mats); t.scale = (1.35,) * 3
    add_text("STBY", 4.6, AMP_TOGGLE[0], AMP_TOGGLE[1] - 27, ink_w, embolden=0.04)
    build_oled(mats, AMP_OLED[0], AMP_OLED[1], 1.1)
    for sx in (-1, 1):
        for sy in (-1, 1):
            h = build_screw(sx * 285.0, sy * 118.0, mats); h.scale = (2.2,) * 3
    return knobs

def amp_spec(name, art, ink_a, ink_b, oled_lines, ink_gain=0.45, led_rgb=(1.0, 0.2, 0.0, 1), led_off='#5a1c05', jewel_rgb=(1.0, 0.10, 0.02, 1)):
    return dict(jewel_rgb=jewel_rgb, 
        name=name, kind='head', plate=(600.0, 260.0, 8.0), body=(600.0, 250.0, 260.0), art=art, ink_a=ink_a, ink_b=ink_b,
        populate=populate_amp, oled_lines=oled_lines, ink_gain=ink_gain, led_rgb=led_rgb, led_off=led_off,
        k=3.2, center=(0, 0, 0.13),
        hero=dict(kind='persp', loc=(0.70, -1.30, 0.74), target=(0, 0, 0.12), lens=70, k=3.2),
        ortho=dict(kind='ortho', loc=(0, -2.0, 0.13), rot=(90, 0, 0), ortho_scale=0.62, k=3.2), ortho_res=(1600, 700))
