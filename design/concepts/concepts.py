#!/usr/bin/env python3
"""Sawblade v1.0 art-direction concepts: VERMIN rat (woodcut / engraving) V1-V3 and CAB spray-stencil wolf W1-W3.

    python3 concepts.py            # render every PNG into design/concepts/png/
    python3 concepts.py --check    # re-render in memory, compare with the committed PNGs, exit 1 on any difference
    python3 concepts.py --only V1 W2

Pillow + numpy only.  Silhouettes are hand-authored cubic Bezier paths (SVG-path strings) or tapered 'sweeps' along
hand-authored Bezier centre-lines; shading is procedural (flow-field hatching, cross-hatching, fur flicks).  Everything is
seeded from crc32(name), so output is bit-identical run to run.  Concept sketches only, not production art."""
import argparse
import io
import math
import os
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.path.join(HERE, 'png')
sys.path.insert(0, os.path.join(HERE, '..', 'render'))

INK = np.array((0xe4, 0xe0, 0xcc), np.float32)
ENAMEL = np.array((0x14, 0x16, 0x1a), np.float32)
GREEN = np.array((0x8f, 0xd1, 0x4f), np.float32)
SS = 3                                   # supersample factor for all drawing


def rng_for(name):
    return np.random.default_rng(zlib.crc32(name.encode()))


# ---------------------------------------------------------------------------------------------------------------------
# geometry: Bezier paths, sweeps, transforms
# ---------------------------------------------------------------------------------------------------------------------
def bez(p0, p1, p2, p3, n=20):
    t = np.linspace(0, 1, n)[:, None]
    p0, p1, p2, p3 = (np.asarray(p, float) for p in (p0, p1, p2, p3))
    return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t ** 2 * p2 + t ** 3 * p3


def parse(d, n=20):
    """Tiny SVG-path subset (absolute M L C Z) -> Nx2 polyline."""
    tok = d.replace(',', ' ').split()
    out, i, cmd, cur = [], 0, None, None
    while i < len(tok):
        if tok[i].isalpha():
            cmd = tok[i]
            i += 1
            continue
        v = [float(x) for x in tok[i:i + (6 if cmd == 'C' else 2)]]
        if cmd == 'M':
            cur = (v[0], v[1]); out.append(cur); i += 2; cmd = 'L'
        elif cmd == 'L':
            cur = (v[0], v[1]); out.append(cur); i += 2
        elif cmd == 'C':
            out.extend(map(tuple, bez(cur, v[0:2], v[2:4], v[4:6], n)[1:])); cur = (v[4], v[5]); i += 6
    return np.array(out, float)


def chain(pts, n=24):
    """[p0,c1,c2,p1,c1,c2,p2...] -> dense polyline."""
    segs = [bez(pts[i], pts[i + 1], pts[i + 2], pts[i + 3], n) for i in range(0, len(pts) - 3, 3)]
    return np.vstack([segs[0]] + [s[1:] for s in segs[1:]])


def _prof(knots, s):
    k = np.array(knots, float)
    w = np.interp(s, k[:, 0], k[:, 1])
    ker = np.ones(5) / 5
    return np.convolve(np.pad(w, 2, mode='edge'), ker, mode='valid')


def sweep(ctrl, wl, wr=None, n=24, cap=True):
    """Variable-width ribbon along a Bezier chain; wl/wr = [(s, halfwidth)] knots on the left/right of travel."""
    c = chain(ctrl, n)
    seg = np.hypot(*np.diff(c, axis=0).T)
    s = np.concatenate([[0], np.cumsum(seg)])
    s /= s[-1]
    t = np.gradient(c, axis=0)
    t /= np.maximum(np.hypot(t[:, 0], t[:, 1]), 1e-9)[:, None]
    nrm = np.stack([-t[:, 1], t[:, 0]], 1)
    a = _prof(wl, s)
    b = _prof(wr if wr is not None else wl, s)
    if cap:
        total = float(np.sum(seg))
        for w in (a, b):
            for end, ss in ((0, s), (1, 1 - s)):
                cl = max(w[0 if end == 0 else -1], 1.0) * 0.6 / total
                f = np.clip(ss / cl, 0, 1)
                w *= np.sqrt(1 - (1 - f) ** 2) if cl > 0 else 1
    return np.vstack([c + nrm * a[:, None], (c - nrm * b[:, None])[::-1]])


def centre(ctrl, n=24):
    return chain(ctrl, n)


class Xf:
    """local -> art transform: scale, rotate (deg), translate, optional x flip."""
    def __init__(self, ox, oy, s=1.0, rot=0.0, flip=False):
        self.o, self.s, self.flip = np.array((ox, oy), float), s, flip
        a = math.radians(rot)
        self.R = np.array(((math.cos(a), -math.sin(a)), (math.sin(a), math.cos(a))))

    def __call__(self, p):
        p = np.asarray(p, float).copy()
        if self.flip:
            p[..., 0] *= -1
        return (p * self.s) @ self.R.T + self.o


# ---------------------------------------------------------------------------------------------------------------------
# raster helpers
# ---------------------------------------------------------------------------------------------------------------------
def box1(a, r, axis):
    r = max(int(r), 1)
    p = np.pad(a, [(r, r) if i == axis else (0, 0) for i in range(2)], mode='edge')
    c = np.cumsum(p, axis=axis, dtype=np.float64)
    zero = np.zeros_like(np.take(c, [0], axis=axis))
    c = np.concatenate([zero, c], axis=axis)
    n = a.shape[axis]
    hi = np.take(c, range(2 * r + 1, 2 * r + 1 + n), axis=axis)
    lo = np.take(c, range(0, n), axis=axis)
    return ((hi - lo) / (2 * r + 1)).astype(np.float32)


def blur(a, sigma):
    """~Gaussian: three box passes per axis."""
    r = max(int(round(sigma * 0.9)), 1)
    for _ in range(3):
        a = box1(box1(a, r, 0), r, 1)
    return a


def to_mask(poly, W, H, polys=None):
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    for p in ([poly] if polys is None else polys):
        d.polygon([(float(x) * SS, float(y) * SS) for x, y in p], fill=255)
    return im


def noise(W, H, cell, rng, octaves=1):
    out = np.zeros((H, W), np.float32)
    for o in range(octaves):
        c = max(cell / (2 ** o), 2)
        gh, gw = int(H / c) + 3, int(W / c) + 3
        g = rng.random((gh, gw)).astype(np.float32)
        im = Image.fromarray(g).resize((int(gw * c), int(gh * c)), Image.BICUBIC)
        out += np.asarray(im)[:H, :W] / (2 ** o)
    out -= out.min()
    return out / max(out.max(), 1e-6)


class Flow:
    """Undirected direction field from guide points (x, y, theta_deg), inverse-distance weighted."""
    def __init__(self, guides):
        self.g = [(x, y, math.cos(2 * math.radians(a)), math.sin(2 * math.radians(a))) for x, y, a in guides]

    def __call__(self, x, y):
        sx = sy = 0.0
        for gx, gy, c, s in self.g:
            w = 1.0 / ((x - gx) ** 2 + (y - gy) ** 2 + 400.0)
            sx += w * c
            sy += w * s
        return 0.5 * math.atan2(sy, sx)


def line_poly(pts, widths):
    pts = np.asarray(pts, float)
    t = np.gradient(pts, axis=0)
    t /= np.maximum(np.hypot(t[:, 0], t[:, 1]), 1e-9)[:, None]
    n = np.stack([-t[:, 1], t[:, 0]], 1) * (np.asarray(widths)[:, None] * 0.5)
    return [(float(x), float(y)) for x, y in np.vstack([pts + n, (pts - n)[::-1]])]


def taper_stroke(d, p0, p1, w0, w1, fill, bow=0.0, n=8):
    """Tapered curved stroke, art coords."""
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    m = (p0 + p1) / 2 + np.array((-(p1 - p0)[1], (p1 - p0)[0])) * bow
    c = bez(p0, m, m, p1, n)
    w = np.linspace(w0, w1, n)
    d.polygon([(x * SS, y * SS) for x, y in line_poly(c, w)], fill=fill)


# ---------------------------------------------------------------------------------------------------------------------
# engraving canvas
# ---------------------------------------------------------------------------------------------------------------------
class Engrave:
    def __init__(self, w, h, name, mode='engrave'):
        self.w, self.h = w, h
        self.W, self.H = w * SS, h * SS
        self.ink = np.zeros((self.H, self.W), np.float32)
        self.rng = rng_for(name)
        self.mode = mode
        self.light = np.array((-0.55, -0.62, 0.56))
        self.light /= np.linalg.norm(self.light)

    # -- painter's-order part -----------------------------------------------------------------------------------
    def part(self, poly, flow, frame=0.0, sp=5.0, sigma=12.0, L=40.0, bias=0.0, cross=0.62, outline=3.6,
             fur=0, furlen=14.0, polys=None, fill=None, hatch=True, contrast=1.0):
        W, H = self.W, self.H
        m = to_mask(poly, W, H, polys)
        M = np.asarray(m, np.float32) / 255.0
        inside = M > 0.5
        if not inside.any():
            return
        ys, xs = np.nonzero(inside)
        x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
        pad = int(sigma * SS * 2) + 4
        x0, y0 = max(x0 - pad, 0), max(y0 - pad, 0)
        x1, y1 = min(x1 + pad, W - 1), min(y1 + pad, H - 1)
        Mc = M[y0:y1 + 1, x0:x1 + 1]
        hgt = blur(Mc, sigma * SS)
        hgt /= max(hgt[Mc > 0.5].max(), 1e-6)
        gy, gx = np.gradient(blur(Mc, sigma * SS * 0.6))
        k = sigma * SS * 3.0
        nx, ny = -gx * k, -gy * k
        nz = np.ones_like(nx)
        nn = np.sqrt(nx * nx + ny * ny + nz * nz)
        b = (nx * self.light[0] + ny * self.light[1] + nz * self.light[2]) / nn
        b = b * (0.35 + 0.65 * np.clip(hgt * 1.6, 0, 1) ** 0.8)
        lo, hi = np.percentile(b[Mc > 0.5], (4, 96))
        b = np.clip((b - lo) / max(hi - lo, 1e-6), 0, 1)
        b = np.clip((b - 0.5) * contrast + 0.5 + bias, 0, 1)
        tex = noise(Mc.shape[1], Mc.shape[0], 40 * SS, self.rng)
        b = np.clip(b + (tex - 0.5) * 0.18, 0, 1)

        if self.mode != 'engrave':
            self._flat_part(poly, polys, Mc, b, x0, y0, outline)
            return
        lay = Image.new('L', (W, H), 0)
        dl = ImageDraw.Draw(lay)
        if fill is not None:
            dl.bitmap((0, 0), m, fill=int(255 * fill))
        if hatch:
            self._hatch(dl, Mc, b, x0, y0, poly if polys is None else np.vstack(polys), flow, frame, sp, L, 0.0, 0.0, 1.0)
            if cross < 1.0:
                self._hatch(dl, Mc, b, x0, y0, poly if polys is None else np.vstack(polys), flow, frame + 58, sp * 1.15,
                            L * 0.8, 0.0, cross, cross_pass=True)
        la = np.asarray(lay, np.float32) / 255.0
        self.ink = self.ink * (1 - M) + np.maximum(la, 0) * M
        if outline:
            ol = Image.new('L', (W, H), 0)
            do = ImageDraw.Draw(ol)
            for p in ([poly] if polys is None else polys):
                pts = [(float(x) * SS, float(y) * SS) for x, y in p]
                do.line(pts + [pts[0]], fill=255, width=max(int(outline * SS), 1), joint='curve')
            self.ink *= 1 - np.asarray(ol, np.float32) / 255.0
            bl = Image.new('L', (W, H), 0)
            db = ImageDraw.Draw(bl)
            for p in ([poly] if polys is None else polys):
                pts = [(float(x) * SS, float(y) * SS) for x, y in p]
                db.line(pts + [pts[0]], fill=255, width=max(int(outline * 0.5 * SS), 1), joint='curve')
            self.ink = np.maximum(self.ink, np.asarray(bl, np.float32) / 255.0)
        if fur:
            self.edge_fur(poly if polys is None else np.vstack(polys), flow, fur, furlen)

    def _outline(self, polys, width, dashed):
        ol = Image.new('L', (self.W, self.H), 0)
        do = ImageDraw.Draw(ol)
        for p in polys:
            pts = [(float(x) * SS, float(y) * SS) for x, y in p]
            pts = pts + [pts[0]]
            wd = max(int(width * SS), 1)
            if dashed:                       # stencil: leave a bridge every few points so nothing floats free
                for i in range(0, len(pts) - 1, 16):
                    do.line(pts[i:i + 15], fill=255, width=wd, joint='curve')
            else:
                do.line(pts, fill=255, width=wd, joint='curve')
        self.ink *= 1 - np.asarray(ol, np.float32) / 255.0

    def _flat_part(self, poly, polys, Mc, b, x0, y0, outline):
        """flat bone fill + halftone dots in the shade (screen-print: fine dots; stencil: coarse dots, dashed cuts)."""
        stencil = self.mode == 'stencil'
        pitch = (11.0 if stencil else 7.5) * SS
        h, w = Mc.shape
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        u = (xx + x0 + yy + y0) / 1.4142
        v = (xx + x0 - yy - y0) / 1.4142
        d = np.hypot(u % pitch - pitch / 2, v % pitch - pitch / 2)
        dark = np.clip((0.55 - b) / 0.55, 0, 1)
        r = pitch * 0.5 * np.sqrt(dark) * (0.72 if stencil else 0.92)
        fillv = (d >= r).astype(np.float32)
        sl = (slice(y0, y0 + h), slice(x0, x0 + w))
        self.ink[sl] = self.ink[sl] * (1 - Mc) + fillv * Mc
        self._outline([poly] if polys is None else polys, outline * (0.95 if stencil else 0.8), stencil)

    def _hatch(self, dl, Mc, b, x0, y0, poly, flow, frame, sp, L, jit, thr, cross_pass=False):
        rng = self.rng
        a = math.radians(frame)
        ux, uy = math.cos(a), math.sin(a)
        vx, vy = -uy, ux
        pts = np.asarray(poly, float)
        us = pts @ np.array((ux, uy))
        vs = pts @ np.array((vx, vy))
        su = L * 0.62
        h, w = Mc.shape
        for v in np.arange(vs.min() - sp, vs.max() + sp, sp):
            u = us.min() - su * rng.random()
            while u < us.max() + su:
                uu = u + (rng.random() - 0.5) * su * 0.5
                vv = v + (rng.random() - 0.5) * sp * 0.25
                u += su
                sx, sy = uu * ux + vv * vx, uu * uy + vv * vy
                if not self._in(Mc, sx, sy, x0, y0):
                    continue
                self._stroke(dl, Mc, b, x0, y0, sx, sy, flow, frame, sp, L * (0.75 + 0.5 * rng.random()), thr, cross_pass)

    @staticmethod
    def _in(Mc, x, y, x0, y0):
        ix, iy = int(x * SS) - x0, int(y * SS) - y0
        return 0 <= ix < Mc.shape[1] and 0 <= iy < Mc.shape[0] and Mc[iy, ix] > 0.5

    def _stroke(self, dl, Mc, b, x0, y0, sx, sy, flow, frame, sp, length, thr, cross_pass):
        step = 2.0
        off = math.radians(frame) - 0.0
        pts = [(sx, sy)]
        for sgn in (1, -1):
            x, y = sx, sy
            pa = None
            for _ in range(int(length / 2 / step)):
                ang = flow(x, y)
                if cross_pass:
                    ang += math.radians(58)
                dx, dy = math.cos(ang), math.sin(ang)
                if pa is None:
                    pa = (dx * sgn, dy * sgn)
                if dx * pa[0] + dy * pa[1] < 0:
                    dx, dy = -dx, -dy
                pa = (dx, dy)
                x += dx * step
                y += dy * step
                if not self._in(Mc, x, y, x0, y0):
                    break
                (pts.append if sgn == 1 else lambda q: pts.insert(0, q))((x, y))
        if len(pts) < 3:
            return
        wd = []
        n = len(pts)
        for i, (x, y) in enumerate(pts):
            ix, iy = int(x * SS) - x0, int(y * SS) - y0
            bb = b[iy, ix]
            if cross_pass:
                bb = max(bb - thr, 0) / (1 - thr)
                wv = sp * 0.55 * bb
            else:
                wv = sp * 1.05 * max(bb - 0.06, 0) ** 0.8 * 1.15
            t = i / (n - 1)
            wd.append(wv * max(math.sin(math.pi * t), 0.0) ** 0.55)
        if max(wd) < 0.35:
            return
        dl.polygon([(x * SS, y * SS) for x, y in line_poly(pts, wd)], fill=255)

    def edge_fur(self, poly, flow, n, length):
        rng = self.rng
        lay = Image.new('L', (self.W, self.H), 0)
        dl = ImageDraw.Draw(lay)
        c = np.asarray(poly, float)
        cen = c.mean(0)
        for _ in range(n):
            i = int(rng.integers(0, len(c) - 1))
            p = c[i]
            tvec = c[(i + 1) % len(c)] - c[i - 1]
            tvec /= max(np.hypot(*tvec), 1e-6)
            out = np.array((tvec[1], -tvec[0]))
            if np.dot(out, p - cen) < 0:
                out = -out
            ang = flow(*p)
            fv = np.array((math.cos(ang), math.sin(ang)))
            if np.dot(fv, out) < 0:
                fv = -fv
            dirv = fv * 0.75 + out * 0.45
            dirv /= np.hypot(*dirv)
            ln = length * (0.6 + 0.8 * rng.random())
            s0 = p - dirv * ln * 0.55
            s1 = p + dirv * ln * 0.45
            pts = np.linspace(s0, s1, 6)
            dl.polygon([(x * SS, y * SS) for x, y in line_poly(pts, np.array((0.2, 1.7, 2.1, 1.6, 0.9, 0.1)))], fill=255)
        self.ink = np.maximum(self.ink, np.asarray(lay, np.float32) / 255.0)

    # -- solid shapes (bone) / dark cut-outs -----------------------------------------------------------------------
    def solid(self, poly, val=1.0, outline=0.0):
        m = np.asarray(to_mask(poly, self.W, self.H), np.float32) / 255.0
        self.ink = self.ink * (1 - m) + val * m
        if outline:
            self._outline([poly], outline, self.mode == 'stencil')

    def stroke_path(self, pts, widths, val=1.0):
        im = Image.new('L', (self.W, self.H), 0)
        ImageDraw.Draw(im).polygon([(x * SS, y * SS) for x, y in line_poly(pts, widths)], fill=255)
        m = np.asarray(im, np.float32) / 255.0
        self.ink = self.ink * (1 - m) + val * m

    def rim(self, polys, width=1.6, gap=2.6):
        """thin bone contour just outside the silhouette so it holds against the dark panel."""
        m = to_mask(polys[0], self.W, self.H, polys)
        a = m.filter(ImageFilter.MaxFilter(int(gap * 2 * SS) | 1))
        b = m.filter(ImageFilter.MaxFilter(int((gap + width) * 2 * SS) | 1))
        ring = (np.asarray(b, np.float32) - np.asarray(a, np.float32)) / 255.0
        self.ink = np.maximum(self.ink, ring)

    def finish(self, colour=INK):
        im = Image.fromarray((np.clip(self.ink, 0, 1) * 255 + 0.5).astype(np.uint8), 'L')
        im = im.resize((self.w, self.h), Image.BOX)
        a = np.asarray(im, np.float32)[..., None] / 255.0
        rgb = ENAMEL + a * (colour - ENAMEL)
        return Image.fromarray((rgb + 0.5).astype(np.uint8), 'RGB')


# ---------------------------------------------------------------------------------------------------------------------
# VERMIN: rat building blocks
# ---------------------------------------------------------------------------------------------------------------------
def ang_deg(v):
    return math.degrees(math.atan2(v[1], v[0]))


def flow_chain(ctrl, off=0.0, k=8):
    c = centre(ctrl, 12)
    idx = np.linspace(0, len(c) - 2, k).astype(int)
    return Flow([(c[i][0], c[i][1], ang_deg(c[i + 1] - c[i]) + off) for i in idx]), ang_deg(c[-1] - c[0]) + off


def limb(E, ctrl, wl, wr=None, off=0.0, sp=4.6, sigma=9.0, L=26.0, k=1.28, **kw):
    wl = [(a, b * k) for a, b in wl]
    wr = None if wr is None else [(a, b * k) for a, b in wr]
    fl, fr = flow_chain(ctrl, off)
    poly = sweep(ctrl, wl, wr)
    E.part(poly, fl, frame=fr, sp=sp, sigma=sigma, L=L, **kw)
    return poly


def claw(E, base, direction_deg, length, curl, width=3.4):
    """long curved bone claw tapering to a point; curl in degrees of bend over its length."""
    a = math.radians(direction_deg)
    p0 = np.array(base, float)
    d0 = np.array((math.cos(a), math.sin(a)))
    a1 = a + math.radians(curl)
    d1 = np.array((math.cos(a1), math.sin(a1)))
    p3 = p0 + d0 * length * 0.55 + d1 * length * 0.45
    ctrl = [p0, p0 + d0 * length * 0.4, p0 + d0 * length * 0.65 + d1 * length * 0.1, p3]
    poly = sweep(ctrl, [(0, width), (0.5, width * 0.62), (1, 0.15)])
    E.solid(poly, 1.0, outline=1.5)
    return p3


def paw(E, wrist, dirn, spread, flen, clen, curl, flow, n=4, wdt=4.6, claw_w=3.2):
    """fingers fanning from the wrist: dirn = centre direction (deg), spread = total fan angle."""
    for i in range(n):
        a = dirn + (i - (n - 1) / 2) * spread / max(n - 1, 1)
        ar = math.radians(a)
        d = np.array((math.cos(ar), math.sin(ar)))
        p0 = np.array(wrist, float)
        p1 = p0 + d * flen
        ctrl = [p0, p0 + d * flen * 0.3, p0 + d * flen * 0.7 + np.array((-d[1], d[0])) * 2, p1]
        poly = sweep(ctrl, [(0, wdt), (0.7, wdt * 0.78), (1, wdt * 0.6)])
        E.part(poly, flow, frame=a, sp=3.6, sigma=4.0, L=14, outline=1.7, cross=1.0, contrast=0.8, bias=0.15)
        claw(E, p1, a + 8, clen, curl, claw_w)


def nail(E, head, tip, f_in=0.28, f_out=0.74, hw=8.0, sw=3.6, tipf=0.16):
    head, tip = np.array(head, float), np.array(tip, float)
    d = tip - head
    ln = np.hypot(*d)
    u = d / ln
    nrm = np.array((-u[1], u[0]))
    pin, pout = head + d * f_in, head + d * f_out
    # puncture wounds (dark rings with cracks)
    for p, big in ((pin, 1.0), (pout, 0.8)):
        ell = [p + u * math.cos(t) * 5.5 * big + nrm * math.sin(t) * 11 * big for t in np.linspace(0, 2 * math.pi, 18)]
        E.solid(np.array(ell), 0.0)
        for k in range(7):
            a = (k / 7) * 2 * math.pi + 0.3
            q0 = p + u * math.cos(a) * 6 * big + nrm * math.sin(a) * 12 * big
            q1 = p + u * math.cos(a) * 11 * big + nrm * math.sin(a) * 22 * big
            im = Image.new('L', (E.W, E.H), 0)
            ImageDraw.Draw(im).polygon([(x * SS, y * SS) for x, y in line_poly(np.linspace(q0, q1, 4), np.array((1.8, 1.4, 0.9, 0.2)))], fill=255)
            E.ink = np.maximum(E.ink, np.asarray(im, np.float32) / 255.0 * 0.9)
    # visible shaft pieces: head -> pin (with head disc), pout -> tip (pointed)
    def shaft(a, b, w0, w1, point=False, halo=None):
        pts = np.linspace(a, b, 10)
        if halo is not None and E.mode == 'stencil':      # cut a gap round the nail, leaving the end that meets the body as the bridge
            E.solid(np.array(line_poly(pts[halo], np.full(len(pts[halo]), w0 * 2 + 9))), 0.0)
        ws = np.linspace(w0, w1, 10)
        if point:
            ws = np.concatenate([np.full(7, w0), np.linspace(w0, 0.2, 3)])
        poly = np.array(line_poly(pts, ws * 2))
        E.solid(poly, 1.0, outline=2.0)
        # dark centre groove for a rounded-iron look
        E.stroke_path(np.linspace(a + nrm * 0.5, b + nrm * 0.5, 8), np.full(8, 0.9), 0.0)
    shaft(head + u * 3, pin, sw, sw, halo=slice(0, 7))
    shaft(pout, tip, sw, sw, point=True, halo=slice(3, 10))
    # flat round head, seen at an angle
    cap = [head + nrm * math.cos(t) * hw + u * math.sin(t) * hw * 0.42 - u * 2 for t in np.linspace(0, 2 * math.pi, 24)]
    E.solid(np.array(cap), 1.0, outline=2.0)
    inner = [head + nrm * math.cos(t) * hw * 0.55 + u * math.sin(t) * hw * 0.22 - u * 3 for t in np.linspace(0, 2 * math.pi, 18)]
    E.solid(np.array(inner), 0.35)


class Head:
    """Profile rat head in local units (skull radius ~1, nose at +x).  Draw order: ear, jaw, upper head, teeth, features."""
    UPPER = ("M 1.55 0.10 C 1.35 -0.18 1.05 -0.45 0.75 -0.62 C 0.45 -0.80 0.1 -0.98 -0.3 -0.92 C -0.75 -0.86 -1.05 -0.55 "
             "-1.05 -0.15 C -1.05 0.20 -0.85 0.40 -0.55 0.45 C -0.2 0.40 0.3 0.38 0.8 0.36 C 1.1 0.34 1.4 0.30 1.55 0.10")
    JAW = ("M -0.55 0.36 C -0.3 0.55 0.2 0.70 0.85 0.62 C 1.05 0.60 1.2 0.52 1.28 0.42 C 0.95 0.46 0.4 0.42 -0.1 0.40 "
           "C -0.3 0.38 -0.45 0.36 -0.55 0.36")
    EAR = ("M -0.45 -0.70 C -0.6 -1.3 -1.3 -1.45 -1.6 -1.0 C -1.85 -0.6 -1.5 -0.15 -1.0 -0.05 C -0.8 -0.3 -0.6 -0.5 -0.45 -0.70")
    EARIN = ("M -0.62 -0.66 C -0.75 -1.12 -1.2 -1.2 -1.42 -0.92 C -1.58 -0.65 -1.38 -0.3 -1.02 -0.2 C -0.85 -0.38 -0.7 -0.5 -0.62 -0.66")
    HINGE = np.array((-0.45, 0.40))

    def __init__(self, E, xf, jaw=30.0, name=''):
        self.E, self.xf, self.jaw = E, xf, jaw
        self.rot = xf.R
        self.ang = ang_deg(xf((1.0, 0.0)) - xf((0.0, 0.0)))

    def jawxf(self, p):
        a = math.radians(self.jaw)
        R = np.array(((math.cos(a), -math.sin(a)), (math.sin(a), math.cos(a))))
        p = np.asarray(p, float)
        return self.xf((p - self.HINGE) @ R.T + self.HINGE)

    def ear(self):
        E, xf = self.E, self.xf
        fl = Flow([(*xf((-1.3, -0.6)), self.ang + 100), (*xf((-0.9, -0.3)), self.ang + 60)])
        E.part(xf(parse(self.EAR)), fl, frame=self.ang + 90, sp=3.6, sigma=7.0, L=22, outline=3.0, cross=1.0)
        E.solid(xf(parse(self.EARIN)), 0.0)
        c = xf(parse(self.EARIN))
        E.part(c, fl, frame=self.ang + 80, sp=3.4, sigma=6.0, L=18, outline=1.6, cross=1.0, contrast=0.7, bias=-0.25)

    def draw(self, name_flip=False):
        E, xf = self.E, self.xf
        s = xf.s
        upper = xf(parse(self.UPPER))
        jaw = self.jawxf(parse(self.JAW))
        # mouth interior (dark), fan from the hinge across the gap
        up_gum = parse("M -0.55 0.45 C -0.2 0.40 0.3 0.38 0.8 0.36 C 1.1 0.34 1.4 0.30 1.55 0.10")
        jw_top = parse("M 1.28 0.42 C 0.95 0.46 0.4 0.42 -0.1 0.40 C -0.3 0.38 -0.45 0.36 -0.55 0.36")
        mouth = np.vstack([xf(up_gum), self.jawxf(jw_top)])
        E.solid(mouth, 0.0)
        # tongue
        tg = self.jawxf(parse("M -0.2 0.40 C 0.2 0.20 0.7 0.28 1.0 0.44 C 0.7 0.5 0.2 0.5 -0.2 0.40"))
        fl = Flow([(*xf((0.4, 0.3)), self.ang)])
        E.part(tg, fl, frame=self.ang, sp=3.2, sigma=5.0, L=14, outline=1.6, cross=1.0, contrast=0.7, bias=-0.15)
        # lower jaw
        fl_j = Flow([(*self.jawxf((0.3, 0.5)), self.ang + self.jaw), (*self.jawxf((1.0, 0.55)), self.ang + self.jaw - 6)])
        E.part(jaw, fl_j, frame=self.ang + self.jaw, sp=4.4, sigma=8.0, L=24, outline=3.4, fur=16, furlen=9)
        # lower incisors (point up/forward from the jaw tip)
        self.teeth_lower()
        # upper head
        pts = [(0.9, -0.1, 25), (0.2, -0.4, 5), (-0.5, -0.4, -10), (-0.5, 0.2, 15), (1.3, 0.1, 10)]
        fl = Flow([(*xf((x, y)), self.ang + a) for x, y, a in pts])
        E.part(upper, fl, frame=self.ang, sp=4.6, sigma=11.0, L=32, outline=3.6, fur=34, furlen=12)
        self.teeth_upper()
        self.features()

    def tooth(self, ctrl, w, xfun=None, ln=1.5):
        xfun = xfun or self.xf
        poly = sweep([xfun(p) for p in ctrl], [(0, w * self.xf.s), (0.55, w * 0.8 * self.xf.s), (1, 0.15)])
        self.E.solid(poly, 1.0, outline=1.8)

    def teeth_upper(self):
        # cheek-tooth row (small jagged blades along the gum line) + two long chisel incisors
        for x in np.linspace(0.15, 1.05, 6):
            y = 0.36 + (x - 0.15) * -0.03
            tri = self.xf(np.array(((x - 0.07, y - 0.02), (x + 0.07, y - 0.02), (x + 0.01, y + 0.13))))
            self.E.solid(tri, 1.0, outline=1.4)
        self.tooth([(1.40, 0.18), (1.46, 0.42), (1.44, 0.66), (1.34, 0.88)], 0.085)
        self.tooth([(1.28, 0.20), (1.32, 0.40), (1.30, 0.56), (1.22, 0.72)], 0.06)

    def teeth_lower(self):
        for x in np.linspace(0.1, 0.9, 5):
            tri = self.jawxf(np.array(((x - 0.07, 0.43), (x + 0.07, 0.43), (x + 0.01, 0.30))))
            self.E.solid(tri, 1.0, outline=1.4)
        self.tooth([(1.20, 0.46), (1.31, 0.28), (1.34, 0.08), (1.26, -0.10)], 0.075, xfun=self.jawxf)

    def features(self):
        E, xf = self.E, self.xf
        s = xf.s
        # eye: bone ring, dark pupil, spark; angry lid + brow wedge
        c = xf((0.38, -0.26))
        ring = [c + np.array((math.cos(t) * 0.17 * s, math.sin(t) * 0.12 * s)) for t in np.linspace(0, 2 * math.pi, 20)]
        E.solid(np.array(ring), 1.0, outline=1.6)
        pup = [c + np.array((math.cos(t) * 0.085 * s, math.sin(t) * 0.085 * s)) + np.array((0.02 * s, 0.01 * s)) for t in np.linspace(0, 2 * math.pi, 16)]
        E.solid(np.array(pup), 0.0)
        E.solid(np.array([c + np.array((-0.02 * s + math.cos(t) * 0.025 * s, -0.03 * s + math.sin(t) * 0.025 * s)) for t in np.linspace(0, 6.3, 8)]), 1.0)
        lid = np.linspace(xf((0.12, -0.50)), xf((0.66, -0.20)), 8)
        E.stroke_path(lid, np.linspace(1.2, 6.5, 8) * s / 62, 0.0)
        brow = np.linspace(xf((0.0, -0.68)), xf((0.70, -0.38)), 8)
        E.stroke_path(brow, np.full(8, 2.2 * s / 62), 1.0)
        # nose + nostril, muzzle wrinkles (snarl)
        nz = xf((1.46, 0.02))
        E.solid(np.array([nz + np.array((math.cos(t) * 0.10 * s, math.sin(t) * 0.08 * s)) for t in np.linspace(0, 6.3, 12)]), 0.0)
        for k in range(4):
            y = -0.40 + 0.0 * k
            a = xf((0.80 + 0.10 * k, -0.08 - 0.13 * k)); b = xf((1.22 + 0.04 * k, 0.04 - 0.10 * k))
            E.stroke_path(np.linspace(a, b, 8), np.linspace(0.6, 2.4, 8) * s / 62, 0.0)
        # whiskers
        rng = E.rng
        base = xf((1.22, 0.12))
        for k in range(7):
            a0 = self.ang + (-62 + k * 21) + (rng.random() - 0.5) * 6
            ln = (0.9 + 0.6 * rng.random()) * s
            ar = math.radians(a0)
            p0 = base + np.array((math.cos(ar), math.sin(ar))) * 0.05 * s
            p3 = base + np.array((math.cos(ar), math.sin(ar))) * ln
            mid = (p0 + p3) / 2 + np.array((-math.sin(ar), math.cos(ar))) * 0.15 * s * (1 if k % 2 else -1)
            pts = bez(p0, mid, mid, p3, 14)
            E.stroke_path(pts, np.linspace(1.9, 0.3, 14), 1.0)


def torso(E, ctrl, wl, wr, **kw):
    return limb(E, ctrl, wl, wr, sp=5.2, sigma=16.0, L=40, **kw)


def tail(E, ctrl, w0=11.0):
    fl, fr = flow_chain(ctrl, 90.0, k=14)
    poly = sweep(ctrl, [(0, w0), (0.5, w0 * 0.55), (1, 1.5)], n=30)
    E.part(poly, fl, frame=fr, sp=3.6, sigma=4.0, L=16, outline=2.6, cross=1.0, bias=0.1, contrast=0.9)
    return poly


def mx(pts, cx=300.0):
    return [(2 * cx - x, y) for x, y in pts]


class FrontHead:
    """Frontal snarling rat head (symmetric): local units, skull half-width ~1, nose near (0, 0.45)."""
    def __init__(self, E, ox, oy, s, jaw=0.0):
        self.E, self.o, self.s, self.jaw = E, np.array((ox, oy), float), s, jaw

    def X(self, p):
        p = np.asarray(p, float)
        return p * self.s + self.o

    def sym(self, d):
        """right-half path string 'M 0 y ... C ... 0 y' -> closed symmetric outline (right then mirrored left)."""
        r = parse(d)
        l = r[::-1].copy()
        l[:, 0] *= -1
        return np.vstack([r, l[1:]])

    def draw(self):
        E, X, s = self.E, self.X, self.s
        nose = X((0, 0.40))
        rad = lambda x, y: ang_deg(np.array((x, y)) - np.array((0, 0.40)))
        guides = [(*X((x, y)), rad(x, y) + 180) for x, y in ((0.5, 0.1), (-0.5, 0.1), (0.7, -0.4), (-0.7, -0.4), (0, -0.6),
                                                          (0.9, 0.0), (-0.9, 0.0), (0.3, 0.4), (-0.3, 0.4))]
        fl = Flow(guides)
        # ears (behind)
        for sg in (1, -1):
            ear = np.array([X((sg * (0.98 + 0.44 * math.cos(t)), -0.80 + 0.46 * math.sin(t))) for t in np.linspace(0, 6.283, 40)])
            fe = Flow([(*X((sg * 1.1, -0.8)), 90 + sg * 20)])
            E.part(ear, fe, frame=90, sp=3.8, sigma=8.0, L=24, outline=3.4, cross=1.0)
            inn = np.array([X((sg * (1.00 + 0.31 * math.cos(t)), -0.78 + 0.35 * math.sin(t))) for t in np.linspace(0, 6.283, 32)])
            E.solid(inn, 0.0)
            E.part(inn, fe, frame=80, sp=3.4, sigma=6.0, L=18, outline=1.6, cross=1.0, contrast=0.7, bias=-0.3)
        # lower jaw (drops with mouth open)
        j = self.jaw
        jawpoly = self.sym(f"M 0 {0.92 + j} C 0.30 {0.92 + j} 0.62 {0.72 + j * 0.4} 0.62 0.60 C 0.62 {0.9 + j} 0.38 {1.30 + j} 0 {1.30 + j}")
        mouth = self.sym(f"M 0 0.55 C 0.30 0.55 0.58 0.48 0.60 0.58 C 0.60 {0.86 + j} 0.34 {0.92 + j} 0 {0.92 + j}")
        E.solid(X(mouth), 0.0)
        tg = self.sym(f"M 0 {0.80 + j} C 0.22 {0.78 + j} 0.34 {0.9 + j} 0.30 {0.98 + j}")
        tgp = X(self.sym(f"M 0 {0.82 + j * 0.9} C 0.2 {0.8 + j * 0.9} 0.32 {0.9 + j * 0.9} 0.3 {1.12 + j}"))
        E.part(X(jawpoly), Flow([(*X((0, 1.1 + j)), 90)]), frame=90, sp=4.4, sigma=9.0, L=26, outline=3.6, fur=20, furlen=10)
        # lower incisors
        for sg in (1, -1):
            self.tooth([(sg * 0.12, 0.98 + j), (sg * 0.13, 0.86 + j), (sg * 0.12, 0.74 + j), (sg * 0.10, 0.62 + j)], 0.08)
        # head
        head = self.sym("M 0 -0.98 C 0.55 -0.98 0.98 -0.62 1.02 -0.15 C 1.05 0.22 0.78 0.46 0.60 0.58 C 0.45 0.66 0.22 0.62 0 0.60")
        E.part(X(head), fl, frame=90, sp=4.6, sigma=11.0, L=30, outline=3.8, fur=44, furlen=12)
        # upper cheek teeth + incisors
        for sg in (1, -1):
            for x in (0.22, 0.34, 0.46):
                tri = X(np.array(((sg * (x - 0.05), 0.60), (sg * (x + 0.05), 0.60), (sg * x, 0.73))))
                E.solid(tri, 1.0, outline=1.3)
            self.tooth([(sg * 0.10, 0.60), (sg * 0.11, 0.80), (sg * 0.10, 1.00), (sg * 0.09, 1.16 + j * 0.2)], 0.095)
        # nose + nostrils
        E.solid(np.array([nose + np.array((math.cos(t) * 0.20 * s, math.sin(t) * 0.14 * s)) for t in np.linspace(0, 6.3, 20)]), 0.0)
        for sg in (1, -1):
            E.solid(np.array([nose + np.array((sg * 0.09 * s + math.cos(t) * 0.05 * s, math.sin(t) * 0.07 * s)) for t in np.linspace(0, 6.3, 10)]), 1.0)
        # muzzle wrinkles
        for sg in (1, -1):
            for k in range(4):
                a = X((sg * (0.30 + 0.07 * k), 0.12 - 0.16 * k)); b = X((sg * (0.58 + 0.06 * k), 0.30 - 0.14 * k))
                E.stroke_path(np.linspace(a, b, 8), np.linspace(0.6, 2.4, 8), 0.0)
        # eyes: angry slanted lids
        for sg in (1, -1):
            c = X((sg * 0.50, -0.22))
            ring = np.array([c + np.array((math.cos(t) * 0.20 * s, math.sin(t) * 0.12 * s)) for t in np.linspace(0, 6.3, 20)])
            E.solid(ring, 1.0, outline=1.8)
            E.solid(np.array([c + np.array((math.cos(t) * 0.085 * s, math.sin(t) * 0.085 * s)) for t in np.linspace(0, 6.3, 14)]), 0.0)
            E.solid(np.array([c + np.array((-0.03 * s * sg + math.cos(t) * 0.022 * s, -0.03 * s + math.sin(t) * 0.022 * s)) for t in np.linspace(0, 6.3, 8)]), 1.0)
            E.stroke_path(np.linspace(X((sg * 0.84, -0.48)), X((sg * 0.20, -0.20)), 8), np.linspace(2.0, 8.0, 8), 0.0)   # heavy lid
            E.stroke_path(np.linspace(X((sg * 0.90, -0.62)), X((sg * 0.22, -0.34)), 8), np.full(8, 2.4), 1.0)            # brow
        # whiskers
        rng = E.rng
        for sg in (1, -1):
            for k in range(6):
                a0 = (-24 + k * 12) + (0 if sg == 1 else 0)
                ar = math.radians(a0 if sg == 1 else 180 - a0)
                p0 = X((sg * 0.40, 0.42 + 0.02 * k))
                ln = (1.2 + 0.9 * rng.random()) * s
                p3 = p0 + np.array((math.cos(ar), math.sin(ar))) * ln
                mid = (p0 + p3) / 2 + np.array((0, 0.18 * s * (1 if k % 2 else -1)))
                E.stroke_path(bez(p0, mid, mid, p3, 14), np.linspace(1.9, 0.3, 14), 1.0)

    def tooth(self, ctrl, w):
        poly = sweep([self.X(p) for p in ctrl], [(0, w * self.s), (0.6, w * 0.8 * self.s), (1, 0.15)])
        self.E.solid(poly, 1.0, outline=1.8)


def vermin_rear(name, mode):
    """(shared pose for V1-V3) rearing up, facing the viewer: head and torso frontal, arms spread with claws toward the viewer, tail looped up behind."""
    E = Engrave(600, 820, name, mode)
    tctrl = [(310, 640), (400, 720), (520, 730), (548, 640), (566, 560), (540, 470), (500, 462), (466, 456), (452, 500), (474, 520)]
    tail(E, tctrl, 13.0)
    # hind feet + legs
    for m in (False, True):
        f = (lambda p: mx(p)) if m else (lambda p: p)
        limb(E, f([(236, 560), (196, 600), (180, 650), (196, 720)]), [(0, 40), (0.4, 36), (1, 16)], [(0, 40), (0.4, 36), (1, 16)] if False else None, sp=5.0, sigma=14, L=34)
        limb(E, f([(196, 720), (186, 745), (160, 762), (124, 772)]), [(0, 16), (1, 12)], sp=4.0)
        paw(E, tuple(f([(124, 772)])[0]), 195 if not m else -15, 58, 28, 26, -22 if not m else 22,
            Flow([(*f([(124, 772)])[0], 195 if not m else -15)]), n=4, wdt=4.6, claw_w=3.4)
    # torso: pear shape, narrow neck
    tor = [(300, 270), (300, 380), (300, 520), (300, 650)]
    torso(E, tor, [(0, 62), (0.25, 92), (0.65, 124), (1, 96)], [(0, 62), (0.25, 92), (0.65, 124), (1, 96)], off=0.0)
    # arms spread, forearms up, claws toward the viewer
    for m in (False, True):
        f = (lambda p: mx(p)) if m else (lambda p: p)
        limb(E, f([(236, 340), (180, 360), (130, 400), (112, 360)]), [(0, 32), (0.5, 22), (1, 14)], sp=4.4, sigma=10, L=28)
        w = f([(112, 360)])[0]
        limb(E, f([(112, 360), (98, 330), (96, 300), (100, 270)]), [(0, 14), (1, 11)], sp=3.8)
        w = f([(100, 270)])[0]
        paw(E, tuple(w), -95 if not m else -85, 74, 30, 34, 40 if not m else -40, Flow([(*w, -90)]), n=4, wdt=5.2, claw_w=3.8)
    FrontHead(E, 300, 188, 118, jaw=0.30).draw()
    # nails
    nail(E, (170, 470), (420, 560), 0.30, 0.74)
    nail(E, (440, 380), (210, 450), 0.30, 0.72)
    nail(E, (330, 300), (240, 360), 0.36, 0.64, hw=7)
    nail(E, (160, 600), (290, 670), 0.30, 0.70)
    nail(E, (460, 580), (330, 520), 0.32, 0.72)
    return E


VSTYLE = {'V1': 'stencil', 'V2': 'engrave', 'V3': 'print'}
SCENES_V = {n: (lambda n=n: vermin_rear(n, VSTYLE[n])) for n in VSTYLE}


def frame_panel(E):
    """thin engraved border so the art panel reads as a printed face."""
    W, H = E.W, E.H
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    for o, w in ((10, 2), (17, 1)):
        d.rectangle((o * SS, o * SS, W - o * SS, H - o * SS), outline=255, width=w * SS)
    E.ink = np.maximum(E.ink, np.asarray(im, np.float32) / 255.0)


def render_v(name):
    E = SCENES_V[name]()
    if E.mode == 'stencil':
        from types import SimpleNamespace
        P = Image.fromarray((np.clip(E.ink, 0, 1) * 255 + 0.5).astype(np.uint8), 'L')
        return spray(SimpleNamespace(P=P), name, INK, base_tolex)
    if E.mode == 'print':
        # mis-registered second pass: a keyline copy of the silhouette, offset like a slipped screen
        sil = blur((E.ink > 0.3).astype(np.float32), 5 * SS) > 0.6          # opening: whiskers and fine lines drop out
        bl = blur(sil.astype(np.float32), 8 * SS)
        ring = ((bl > 0.10) & (bl < 0.30)).astype(np.float32)
        ring = np.roll(ring, (6 * SS, 8 * SS), axis=(0, 1))
        E.ink = np.maximum(E.ink, ring * (1 - (E.ink > 0.3)))
    frame_panel(E)
    return E.finish()



# ---------------------------------------------------------------------------------------------------------------------
# CAB: spray-stencil wolf
# ---------------------------------------------------------------------------------------------------------------------
WS = 700                                  # panel size, px
SW = SH = WS                              # current spray canvas (set by spray())


class Stencil:
    """The stencil sheet: paint passes where the sheet has been cut away.  add() = material removed from the sheet
    (paint goes through), cut() = material left in the sheet (a bridge / island holder, no paint)."""
    def __init__(self, name):
        self.rng = rng_for(name)
        self.W = WS * SS
        self.P = Image.new('L', (self.W, self.W), 0)
        self.d = ImageDraw.Draw(self.P)

    def add(self, poly, fill=255):
        self.d.polygon([(float(x) * SS, float(y) * SS) for x, y in poly], fill=fill)

    def cut(self, poly):
        self.add(poly, 0)

    def region(self, polys, erode=0.0):
        im = Image.new('L', (self.W, self.W), 0)
        d = ImageDraw.Draw(im)
        for p in polys:
            d.polygon([(float(x) * SS, float(y) * SS) for x, y in p], fill=255)
        if erode:
            im = im.filter(ImageFilter.MinFilter(int(erode * 2 * SS) | 1))
        return np.asarray(im, np.float32) / 255.0

    def blades(self, reg, flow, sv, su, length, wmax, frame=0.0, jit=0.35, skip=0.0, maxfrac=1.0):
        """tapered fur cuts along a flow field, clipped to region array `reg`; the lattice gaps are the bridges."""
        rng = self.rng
        ys, xs = np.nonzero(reg > 0.5)
        if not len(xs):
            return
        pts = np.stack([xs[::97], ys[::97]], 1) / SS
        a = math.radians(frame)
        ux, uy = math.cos(a), math.sin(a)
        vx, vy = -uy, ux
        us, vs = pts @ np.array((ux, uy)), pts @ np.array((vx, vy))
        if len(us) == 0:
            return
        inside = lambda x, y: 0 <= int(x * SS) < self.W and 0 <= int(y * SS) < self.W and reg[int(y * SS), int(x * SS)] > 0.5
        for v in np.arange(vs.min() - sv, vs.max() + sv, sv):
            u = us.min() - su * rng.random()
            while u < us.max() + su:
                uu = u + (rng.random() - 0.5) * su * jit
                vv = v + (rng.random() - 0.5) * sv * jit * 0.6
                u += su
                sx, sy = uu * ux + vv * vx, uu * uy + vv * vy
                if not inside(sx, sy) or rng.random() < skip:
                    continue
                ln = length * (0.7 + 0.6 * rng.random())
                path = [(sx, sy)]
                for sgn in (1, -1):
                    x, y, pa = sx, sy, None
                    for _ in range(int(ln / 2 / 2.0)):
                        ang = flow(x, y)
                        dx, dy = math.cos(ang), math.sin(ang)
                        if pa is None:
                            pa = (dx * sgn, dy * sgn)
                        if dx * pa[0] + dy * pa[1] < 0:
                            dx, dy = -dx, -dy
                        pa = (dx, dy)
                        x += dx * 2.0
                        y += dy * 2.0
                        if not inside(x, y):
                            break
                        (path.append if sgn == 1 else lambda q: path.insert(0, q))((x, y))
                if len(path) < 4:
                    continue
                n = len(path)
                w = [wmax * max(math.sin(math.pi * i / (n - 1)), 0.0) ** 0.7 for i in range(n)]
                self.cut(line_poly(path, w))


def wolf_profile(st, xf, jaw, fang=1.0, howl=True):
    """Hand-authored wolf head + ruff in local px (nose at +x) -> stencil add/cut.  Returns (head_poly, ruff_poly)."""
    UP = ("M -70 -160 C -10 -190 70 -180 120 -140 C 160 -115 200 -108 236 -104 C 262 -100 272 -84 262 -70 C 252 -56 226 -52 190 -50 "
          "C 150 -46 110 -40 72 -26 C 30 -8 -20 -4 -64 -16 C -100 -40 -108 -110 -70 -160")
    LOW = "M 60 -18 C 120 -14 190 -30 242 -34 C 252 -26 246 -4 222 4 C 180 18 120 34 60 22 C 38 14 38 -8 60 -18"
    RUFF = ("M -100 -60 L -165 -30 L -125 -22 L -185 40 L -140 36 L -200 120 L -145 108 L -205 200 L -125 178 L -140 250 "
            "L -50 215 L -40 275 L 30 220 L 70 260 L 85 195 L 135 215 L 120 150 L 170 150 L 135 95 L 175 80 L 125 30 L 150 20 "
            "L 80 -10 L 20 -20 L -60 -25")
    EAR = "M -46 -150 C -70 -220 -95 -270 -122 -310 C -60 -300 10 -250 56 -150"
    EAR2 = "M -22 -158 C -38 -206 -54 -236 -74 -258 C -42 -250 -10 -222 14 -158"
    hinge = np.array((60.0, -15.0))
    a = math.radians(jaw)
    Rj = np.array(((math.cos(a), -math.sin(a)), (math.sin(a), math.cos(a))))
    J = lambda p: xf((np.asarray(p, float) - hinge) @ Rj.T + hinge)
    ruff, up, low, ear = xf(parse(RUFF) * np.array((0.8, 0.72))), xf(parse(UP)), J(parse(LOW)), xf(parse(EAR) * np.array((0.9, 0.8)) + np.array((0, -30 * 0.2)))
    for p in (ruff, up, low, ear):
        st.add(p)
    st.cut(xf(parse(EAR2) * np.array((0.9, 0.8)) + np.array((0, -30 * 0.2))))                                    # ear hollow (outer rim stays as the bridge)
    # fangs: upper hang from the lip, lower rise from the jaw
    st.add(xf(np.array(((178, -52), (216, -50), (206 + 8 * fang, -50 + 50 * fang)))))
    st.add(xf(np.array(((124, -44), (150, -42), (142, -42 + 28 * fang)))))
    st.add(J(np.array(((206, -28), (236, -34), (230, -34 - 50 * fang)))))
    # eye (two halves with a bridge), brow, nostril
    st.cut(xf(parse("M 92 -122 C 108 -132 124 -134 134 -130 L 131 -112 C 118 -108 104 -110 92 -122")))
    st.cut(xf(parse("M 142 -129 C 156 -127 170 -118 180 -106 C 168 -104 154 -106 142 -112")))
    st.cut(xf(parse("M 76 -150 C 110 -160 150 -156 188 -132 C 150 -142 112 -146 76 -150")))
    st.cut(xf(parse("M 250 -96 C 258 -98 264 -92 262 -84 C 256 -86 250 -88 250 -96")))
    st.cut(xf(parse("M 196 -102 C 216 -98 234 -96 248 -98 L 248 -104 C 232 -104 214 -106 196 -102")))   # muzzle seam
    # fur cuts
    rot = ang_deg(xf((1, 0)) - xf((0, 0)))
    fx = -1 if xf.flip else 1
    ex = st.region([up], 6.0)
    for sx, sy, ang in ((0, 0, 0),):
        pass
    headflow = Flow([(*xf((x, y)), rot + (a if not xf.flip else -a)) for x, y, a in
                     ((200, -80, 0), (140, -100, 8), (40, -110, 28), (-30, -90, 60), (-20, -40, 40))]) if not xf.flip else \
        Flow([(*xf((x, y)), ang_deg(xf((1, 0)) - xf((0, 0))) + (-a)) for x, y, a in
              ((200, -80, 0), (140, -100, 8), (40, -110, 28), (-30, -90, 60), (-20, -40, 40))])
    st.blades(ex, headflow, 17, 62, 54, 3.2, frame=rot, skip=0.12)
    rr = st.region([ruff], 7.0)
    down = ang_deg(xf((-0.45, 1.0)) - xf((0, 0)))
    ruffflow = Flow([(*xf((x, y)), down) for x, y in ((-60, 60), (-100, 150), (20, 150), (80, 100), (-40, 220))])
    st.blades(rr, ruffflow, 19, 56, 58, 4.8, frame=down + 90, skip=0.1)
    return up


def wolf_front(st, cx, cy, s, jaw=1.0):
    """frontal snarling wolf head, authored as a right half and mirrored; centre (cx, cy), scale s."""
    def half(pts):
        r = np.asarray(pts, float)
        l = r[::-1].copy()
        l[:, 0] *= -1
        return np.vstack([r, l[1:]])

    def X(p):
        return np.asarray(p, float) * s + np.array((cx, cy))

    HEAD = parse("M 0 -215 C 30 -220 52 -226 66 -236 L 128 -340 C 166 -310 200 -270 218 -224 C 244 -170 258 -122 250 -80 "
                 "L 292 -60 L 246 -20 L 284 22 L 232 42 L 258 104 L 204 102 L 206 168 L 150 152 L 112 222 L 62 190 L 0 232")
    st.add(X(half(HEAD)))
    # inner ears (hollow, rim remains)
    st.cut(X(half(parse("M 80 -238 L 124 -304 C 152 -284 178 -262 192 -236 C 160 -250 120 -252 80 -238"))))
    # eyes: slanted almond, bridged
    for sg in (1, -1):
        m = lambda pts: X(np.array([(sg * x, y) for x, y in pts]))
        st.cut(m(parse("M 52 -92 C 80 -112 112 -126 142 -144 L 150 -120 C 126 -96 90 -74 56 -76").tolist()))
        st.add(m([(98, -112), (114, -106), (108, -92), (92, -98)]))                         # glint island on a bridge
        st.cut(m(parse("M 48 -150 C 90 -160 140 -190 170 -216 C 130 -184 90 -170 48 -150").tolist()))   # heavy brow
    # nose block with seam, muzzle cut lines
    st.cut(X(half(parse("M 0 -26 C 26 -26 50 -14 54 6 C 40 26 18 36 0 36"))))
    st.add(X(half(parse("M 0 -12 C 18 -12 34 -2 36 8 C 26 20 12 26 0 26"))))
    st.cut(X(half(parse('M 6 -170 C 18 -130 26 -90 28 -44 L 14 -44 C 10 -90 6 -130 0 -170'))))   # nose-bridge seam
    # open mouth void with fangs
    j = jaw
    st.cut(X(half(parse(f"M 0 70 C 60 62 130 72 176 98 C 156 {134 + 20 * j} 138 {168 + 20 * j} 96 {184 + 20 * j} "
                        f"C 60 {194 + 20 * j} 20 {196 + 20 * j} 0 {196 + 20 * j}"))))
    for sg in (1, -1):
        m = lambda pts: X(np.array([(sg * x, y) for x, y in pts]))
        st.add(m([(52, 64), (84, 66), (70, 150)]))                                   # upper fang
        st.add(m([(110, 74), (132, 80), (124, 120)]))
        st.add(m([(20, 66), (40, 66), (30, 110)]))
        st.add(m([(46, 190 + 20 * j), (78, 186 + 20 * j), (64, 130 + 12 * j)]))      # lower fang
        st.add(m([(96, 178 + 20 * j), (120, 168 + 20 * j), (112, 140 + 12 * j)]))
    # fur blades
    reg = st.region([X(half(HEAD))], 7.0)
    # remove the mouth void and eye zones from the blade region
    holes = Image.new('L', (st.W, st.W), 0)
    ImageDraw.Draw(holes).polygon([(float(x) * SS, float(y) * SS) for x, y in X(half(parse("M 0 40 C 60 40 180 60 200 100 L 160 220 L 0 230")))], fill=255)
    reg = reg * (1 - np.asarray(holes.filter(ImageFilter.MaxFilter(9)), np.float32) / 255.0)
    guides = [(*X((x, y)), math.degrees(math.atan2(y - 10, x))) for x, y in
              ((130, -120), (-130, -120), (180, -20), (-180, -20), (100, -190), (-100, -190), (150, 60), (-150, 60), (60, -60), (-60, -60))]
    st.blades(reg, Flow(guides), 17, 58, 52, 3.4, frame=0, skip=0.15)


def spray(st, name, colour, base):
    global SW, SH
    SW, SH = st.P.size[0] // SS, st.P.size[1] // SS
    """paint passes through the stencil: soft edges, edge breakup, overspray haze + speckle, drips, wear."""
    rng = rng_for(name + '_spray')
    P = np.asarray(st.P.resize((SW, SH), Image.BOX), np.float32) / 255.0
    nz = noise(SW, SH, 3.0, rng, 2)
    nz2 = noise(SW, SH, 22.0, rng, 2)
    soft = blur(P, 1.1)
    cov = np.clip((soft + (nz - 0.5) * 0.30 + (nz2 - 0.5) * 0.25 - 0.5) / 0.30 + 0.5, 0, 1)
    # drips: lowest painted pixel of a tall paint run in a column
    drip = Image.new('L', (SW * SS, SH * SS), 0)
    dd = ImageDraw.Draw(drip)
    cols = np.arange(8, SW - 8, 5)
    cand = []
    for x in cols:
        col = cov[:, x] > 0.6
        ends = np.nonzero(col[:-1] & ~col[1:])[0]
        for y in ends:
            run = 0
            while y - run >= 0 and col[y - run]:
                run += 1
            if run > 14:
                cand.append((x, y))
    rng.shuffle(cand)
    used = []
    for x, y in cand:
        if len(used) >= 15 or any(abs(x - ux) < 22 for ux, _ in used):
            continue
        used.append((x, y))
        ln = float(rng.uniform(20, 120))
        w = float(rng.uniform(1.6, 3.4))
        path = [(x + math.sin(i * 0.7 + x) * 0.5, y - 2 + i * ln / 14) for i in range(15)]
        ws = [w * (1.0 - 0.35 * i / 14) for i in range(15)]
        dd.polygon([(px * SS, py * SS) for px, py in line_poly(path, ws)], fill=255)
        bx, by = path[-1]
        r = w * 1.35
        dd.ellipse(((bx - r) * SS, (by - r) * SS, (bx + r) * SS, (by + r * 1.5) * SS), fill=255)
    dr = np.asarray(drip.resize((SW, SH), Image.BOX), np.float32) / 255.0
    cov = np.maximum(cov, dr)
    # overspray haze + speckle
    haze = blur(P, 11.0)
    haze2 = blur(P, 4.0)
    speck = (rng.random((SH, SW)) < (haze * 0.38 + haze2 * 0.22) * (1 - cov)).astype(np.float32) * 0.85
    alpha = np.maximum(cov, np.maximum(speck, haze * 0.12))
    # uneven paint + wear scuffs
    alpha *= 0.80 + 0.20 * noise(SW, SH, 9.0, rng, 2)
    wear = noise(SW, SH, 14.0, rng, 3)
    alpha *= 1 - np.clip((wear - 0.80) / 0.10, 0, 1) * 0.6
    alpha *= 1 - (rng.random((SH, SW)) < 0.012).astype(np.float32)
    # panel
    img, bump, metal = base(rng)
    a3 = (alpha * (metal if metal is not None else 1.0))[..., None]
    paint = colour * (0.80 + 0.40 * bump[..., None])
    out = img * (1 - a3) + paint * a3
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), 'RGB')


def base_tolex(rng):
    g = blur(rng.random((SH, SW)).astype(np.float32), 1.0)
    g = (g - g.mean()) / g.std()
    gy, gx = np.gradient(g)
    bump = np.clip(0.5 + (-gx - gy) * 0.5, 0, 1)
    low = noise(SW, SH, 90.0, rng, 2)
    lum = 20 + 7 * bump * 2 + 6 * low
    edge = np.zeros((SH, SW), np.float32)
    yy, xx = np.mgrid[0:SH, 0:SW]
    vig = 1 - 0.35 * (((xx - SW / 2) ** 2 + (yy - SH / 2) ** 2) / (SW / 2) ** 2)
    img = (lum * vig)[..., None] * np.array((1.0, 1.0, 1.04), np.float32)
    return img, bump, None


def base_grille(rng):
    """perforated steel grille: staggered round holes; paint sits only on the metal."""
    pitch, rad = 14.0, 3.9
    yy, xx = np.mgrid[0:SH, 0:SW].astype(np.float32)
    row = np.floor(yy / (pitch * 0.866))
    xs = xx + (row % 2) * pitch / 2
    cx = (np.floor(xs / pitch) + 0.5) * pitch
    cy = (row + 0.5) * pitch * 0.866
    d = np.hypot(xs - cx, yy - cy)
    metal = np.clip((d - rad) / 1.6 + 0.5, 0, 1)
    low = noise(SW, SH, 120.0, rng, 2)
    shade = 0.8 + 0.4 * np.clip(1 - np.abs(d - rad - 1.5) / 2.0, 0, 1) * 0.5 + 0.1 * low
    lum = 58 * shade * metal + 6 * (1 - metal)
    img = lum[..., None] * np.array((0.92, 0.96, 1.03), np.float32)
    bump = 0.5 * metal + 0.25 * noise(SW, SH, 2.0, rng)
    return img, bump, 0.15 + 0.85 * metal


def base_cloth(rng):
    """woven speaker-cloth in a dark wool-grey."""
    yy, xx = np.mgrid[0:SH, 0:SW].astype(np.float32)
    p = 4.2
    warp = 0.5 + 0.5 * np.sin(2 * math.pi * xx / p)
    weft = 0.5 + 0.5 * np.sin(2 * math.pi * yy / p)
    over = (np.floor(xx / p) + np.floor(yy / p)) % 2
    tex = np.where(over > 0, warp, weft) * 0.7 + 0.3 * noise(SW, SH, 1.5, rng)
    low = noise(SW, SH, 100.0, rng, 2)
    lum = 18 + 22 * tex + 7 * low
    img = lum[..., None] * np.array((1.0, 0.98, 0.95), np.float32)
    return img, tex, None


def wolf_w1():
    st = Stencil('W1')
    wolf_profile(st, Xf(300, 440, 1.05, -36), jaw=24, fang=1.0)
    return spray(st, 'W1', INK, base_tolex)


def wolf_w2():
    st = Stencil('W2')
    wolf_front(st, 350, 400, 0.95, jaw=1.0)
    return spray(st, 'W2', GREEN, base_grille)


def wolf_w3():
    st = Stencil('W3')
    wolf_profile(st, Xf(500, 420, 1.0, 4, flip=True), jaw=28, fang=1.4)
    return spray(st, 'W3', INK, base_cloth)


SCENES_W = {'W1': wolf_w1, 'W2': wolf_w2, 'W3': wolf_w3}


def render_all(only=None):
    """-> {filename: PIL.Image}"""
    out = {}
    v, w = {}, {}
    for n in SCENES_V:
        if only and n not in only:
            continue
        im = render_v(n)
        v[n] = im
        out[f'{n}.png'] = im
        out[f'{n}_300.png'] = im.resize((300, 410), Image.LANCZOS)
    for n, fn in SCENES_W.items():
        if only and n not in only:
            continue
        w[n] = fn()
        out[f'{n}.png'] = w[n]
    if v and len(v) == len(SCENES_V):
        out['sheet_vermin.png'] = sheet(v, out, 'V')
    if w and len(w) == len(SCENES_W):
        out['sheet_wolf.png'] = sheet(w, out, 'W')
    return out


def sheet(panels, out, kind):
    import workshop_style as ws
    names = sorted(panels)
    gap, top = 30, 70
    bg = tuple(int(x) for x in ENAMEL * 0.7)
    ph = panels[names[0]].height
    if kind == 'V':
        H = top + ph + 20 + 410 + 70
        W = sum(panels[n].width for n in names) + gap * (len(names) + 1)
    else:
        H = top + ph + 40
        W = sum(panels[n].width for n in names) + gap * (len(names) + 1)
    im = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(im)
    f = ws.font('barlowc700', 34)
    fs = ws.font('barlow500', 20)
    x = gap
    sub = {'V1': 'spray stencil', 'V2': 'woodcut / engraving', 'V3': 'flat screen-print',
           'W1': 'howling bust, bone on tolex', 'W2': 'front snarl, green on perforated grille', 'W3': 'snarling profile, bone on cloth'}
    for n in names:
        p = panels[n]
        d.text((x, 14), n, font=f, fill=tuple(int(c) for c in INK))
        d.text((x + 72, 26), sub[n], font=fs, fill=tuple(int(c * 0.8) for c in INK))
        im.paste(p, (x, top))
        if kind == 'V':
            sm = out[f'{n}_300.png']
            im.paste(sm, (x + (p.width - 300) // 2, top + ph + 20))
            d.text((x + (p.width - 300) // 2, top + ph + 20 + 414), '300 px wide', font=fs, fill=tuple(int(c * 0.8) for c in INK))
        x += p.width + gap
    return im


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=False, compress_level=9)
    return b.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='compare a fresh render with the committed PNGs; exit 1 on any diff')
    ap.add_argument('--only', nargs='*', help='render only these scenes (no sheets), e.g. V1 W2')
    ap.add_argument('--out', default=PNG)
    a = ap.parse_args()
    try:
        out = render_all(a.only)
    except Exception as e:  # font failure etc.
        if type(e).__name__ == 'FontError':
            print('font error:', e, file=sys.stderr)
            return 77
        raise
    bad = 0
    for name, im in sorted(out.items()):
        path = os.path.join(a.out, name)
        if a.check:
            if not os.path.exists(path):
                print('MISSING', name); bad += 1; continue
            old = Image.open(path).convert('RGB')
            if old.size != im.size or not np.array_equal(np.asarray(old), np.asarray(im)):
                print('DIFF   ', name); bad += 1
            else:
                print('ok     ', name, im.size)
        else:
            os.makedirs(a.out, exist_ok=True)
            with open(path, 'wb') as f:
                f.write(png_bytes(im))
            print('wrote', path, im.size)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
