#!/usr/bin/env python3
"""Sawblade workshop style kit: worn painted steel, brushed aluminium, dark workbench wood, sawdust, riveted plates,
plus the control kit (knobs, footswitches, LEDs, toggles, LCD / nixie readouts, buttons, badges) that every mockup
screen in design/mockups/workshop/ is built from.  Spec: docs/specs/v1_0-A-workshop_mockups.md.

Pillow + numpy only; no Blender, no system fonts, no randomness that is not seeded (every texture uses
numpy.random.default_rng(crc32(name))).  Same Pillow / FreeType / numpy -> bit-identical PNGs.

    python3 design/render/workshop_style.py --out DIR     # writes DIR/00_style_sheet.png
    python3 design/render/workshop_style.py --out DIR --font-dir ~/.cache/sawblade_fonts

Exit codes: 0 ok, 77 Pillow / numpy missing or a font could not be fetched / verified (never a silent fallback).

Conventions for screen authors
------------------------------
* All coordinates are LOGICAL pixels on a 1280 x 800 page (floats are fine).  The canvas is drawn at 2x (SCALE) and
  downsampled with LANCZOS in ``Canvas.finish()``.  A rect is a tuple ``(x0, y0, x1, y1)``; ``R(x, y, w, h)`` builds one.
* Text goes through ``text()`` only.  It sits on a flat well (``well()``, ``label_well()``, plates, buttons, LCD glass)
  and ``text()`` asserts the pixels under the ink box (+2 px) are one flat colour.  Draw textured things (wood, steel,
  sawdust, hatch, scratches) FIRST and the wells on top.
* Colours are token names from ``PAL`` ('bone', 'blade', ...) or RGB tuples.
* Every public function takes the ``Canvas`` as first argument and returns the rect(s) it used so you can lay out.

Public API (signatures)
-----------------------
Canvas / page:    Canvas(name), new_screen(name), Canvas.finish(), Canvas.crop2x(rect), darken(cv, rect, amount),
                  R(x,y,w,h), inset(rect,d), rect_w/rect_h, caption(cv, rect_or_xy, text)
Text:             text(cv, xy, s, style, bg=rect, fg=None, size=None, align='l', glow=0), text_width(s, style),
                  label_well(cv, xy, s, style='label', ...) -> rect, well(cv, rect, kind='well', ...) -> rect
Materials:        wood_tex / steel_tex / alu_tex(w, h, key) -> PIL image (2x), sawdust(cv, rect, n, key, ...),
                  hatch(cv, rect, ...), rivet(cv, x, y, r)
Frames:           panel(cv, rect, name), plate(cv, rect, title), nameplate(cv, rect, s), card(cv, rect, title, ...),
                  overlay_frame(cv, rect, title), inspector(cv, rect, title), top_bar(cv, state, y)
Controls:         knob, footswitch, led, toggle, rotary_selector, button, lcd, nixie, text_field, dropdown, checkbox,
                  badge, chip, tab, slider, progress, glyph_icon
Props:            sprite_image(name, width, crop=, round_px=), gaffer_tape, oled, empty_slot, bezier, cable
Contrast:         contrast(fg, bg), required_ratio(px, bold); every text() call appends to Canvas.log
"""
import argparse
import hashlib
import math
import os
import ssl
import sys
import urllib.request
import zlib
from dataclasses import dataclass, field

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError as e:  # pragma: no cover - depends on the machine
    print(f"workshop_style.py: SKIP, needs Pillow and numpy ({e})")
    sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
ASSETS = os.path.join(REPO, 'plugin', 'assets')
DEFAULT_FONT_DIR = os.path.expanduser('~/.cache/sawblade_fonts')

SCALE = 2                      # render at 2x, downsample to 1x with LANCZOS
S = SCALE
PAGE_W, PAGE_H = 1280, 800

# --------------------------------------------------------------------------------------------------------------------
# palette (docs/specs/v1_0-A-workshop_mockups.md, exact sRGB) and contrast maths
# --------------------------------------------------------------------------------------------------------------------
_HEX = {
    'bench_dark': '#0f0c09', 'bench': '#22180f', 'bench_hi': '#3a2a1b', 'sawdust': '#c9a36b',
    'steel': '#1f1d1a', 'steel_bare': '#8d8a83', 'rust': '#6b3a1c', 'alu': '#b9b5ab', 'alu_well': '#c4c0b6',
    'well': '#1a1714', 'well_raised': '#24201b', 'glass': '#0d0b08',
    'bone': '#e8e1d2', 'bone_dim': '#b3a995', 'bone_mute': '#9c927f', 'ink': '#14110d',
    'blade': '#ff6a1a', 'blade_hi': '#ff8a3d', 'body': '#7fb4ea', 'lcd_amber': '#ffb347',
    'ok': '#7fe08f', 'warn': '#ffb347', 'alert': '#ff6b5a',
}
PAL = {k: tuple(int(v[i:i + 2], 16) for i in (1, 3, 5)) for k, v in _HEX.items()}


def rgb(c):
    """Colour token name, '#rrggbb' or RGB tuple -> (r, g, b)."""
    if isinstance(c, str):
        if c.startswith('#'):
            return tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))
        return PAL[c]
    return tuple(int(v) for v in c[:3])


def token_name(c):
    """RGB tuple -> token name if it matches one (first in table order), else '#rrggbb'."""
    c = tuple(c)
    for k, v in PAL.items():
        if v == c:
            return k
    return '#%02x%02x%02x' % c


def luminance(c):
    v = []
    for x in rgb(c):
        x /= 255.0
        v.append(x / 12.92 if x <= 0.03928 else ((x + 0.055) / 1.055) ** 2.4)
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]


def contrast(fg, bg):
    """WCAG 2.x contrast ratio of two colours."""
    a, b = luminance(fg), luminance(bg)
    if a < b:
        a, b = b, a
    return (a + 0.05) / (b + 0.05)


def required_ratio(px, bold):
    """AA threshold: 3:1 for large text (>= 24 px, or >= 18.67 px bold), else 4.5:1."""
    return 3.0 if (px >= 24 or (bold and px >= 18.67)) else 4.5


# ratios the spec table claims (checked at import: the palette cannot drift silently)
_CLAIMED = [('bone', 'well', 13.7), ('bone_dim', 'well', 7.7), ('bone_mute', 'well', 5.8), ('ink', 'alu', 9.2),
            ('blade', 'well', 6.2), ('body', 'well', 8.2), ('lcd_amber', 'glass', 11.0), ('ok', 'well', 11.0),
            ('alert', 'well', 6.4)]
for _fg, _bg, _want in _CLAIMED:
    if contrast(PAL[_fg], PAL[_bg]) < _want - 0.05:
        raise ValueError('palette drift: %s on %s is %.2f, spec says %.1f' % (_fg, _bg, contrast(PAL[_fg], PAL[_bg]), _want))


# --------------------------------------------------------------------------------------------------------------------
# fonts (SIL OFL 1.1; fetched at run time, SHA-256 verified, never committed)
# --------------------------------------------------------------------------------------------------------------------
class FontError(RuntimeError):
    """A font is missing, unreachable or fails its SHA-256: callers exit 77."""


FONTS = {
    'blackops': ('Black Ops One', 'https://fonts.gstatic.com/s/blackopsone/v21/qWcsB6-ypo7xBdr6Xshe96H3WDw.ttf',
                 'bd8a70e63df108745316c6ad277874cbe139bbb90cbcaf705810ecc089fe59f8'),
    'allerta': ('Allerta Stencil', 'https://fonts.gstatic.com/s/allertastencil/v24/HTx0L209KT-LmIE9N7OR6eiycOeF-w.ttf',
                '036e8216a18f1b06036ac0081feed72bc63562ee70920a8151ca7c441f91c753'),
    'barlowc600': ('Barlow Condensed 600',
                   'https://fonts.gstatic.com/s/barlowcondensed/v13/HTxwL3I-JCGChYJ8VI-L6OO_au7B4873_3E.ttf',
                   '0d85af813fc3ed87db0c6265515689b2eef5cbaf7aab17922528dfc95a2cd73f'),
    'barlowc700': ('Barlow Condensed 700',
                   'https://fonts.gstatic.com/s/barlowcondensed/v13/HTxwL3I-JCGChYJ8VI-L6OO_au7B46r2_3E.ttf',
                   '7dde307fa887fc65ff5830cfada77a7decc5dae8d3c816c9d39ba3f1af1c4ed7'),
    'barlow500': ('Barlow 500', 'https://fonts.gstatic.com/s/barlow/v13/7cHqv4kjgoGqM7E3_-gc4A.ttf',
                  '91c841fdfa8e7b94ffedbb983a363947ba6ed720f3bbf0c71d48b618053655bc'),
    'barlow600': ('Barlow 600', 'https://fonts.gstatic.com/s/barlow/v13/7cHqv4kjgoGqM7E30-8c4A.ttf',
                  'c15439e7a03af5714282ec1780ff7b0214ec6a7db96300b54928dbcd2569ca0c'),
    'mono': ('Share Tech Mono', 'https://fonts.gstatic.com/s/sharetechmono/v16/J7aHnp1uDWRBEqV98dVQztYldFc7pA.ttf',
             '5f6b57538a1a35469a038dc3073003cebc4c101ad7b3c219e9555a3b3c0a81d2'),
}
_font_dir = DEFAULT_FONT_DIR
_fonts_ready = False
_font_cache = {}


def set_font_dir(path):
    """Select the directory fonts are cached in (default ~/.cache/sawblade_fonts)."""
    global _font_dir, _fonts_ready
    _font_dir = os.path.expanduser(path)
    _fonts_ready = False
    _font_cache.clear()


def _ssl_context():
    ctx = ssl.create_default_context()
    for var in ('SSL_CERT_FILE', 'REQUESTS_CA_BUNDLE', 'CURL_CA_BUNDLE', 'GIT_SSL_CAINFO'):
        p = os.environ.get(var)
        if p and os.path.exists(p):
            ctx.load_verify_locations(p)      # proxy CA, added on top of the system roots; verification stays on
            break
    return ctx


def ensure_fonts(font_dir=None):
    """Fetch (if missing) and SHA-256-verify every font; raise FontError on any failure.  Downloaded bytes are verified
    BEFORE they enter the cache (one retry on a mismatch), so a bad download never poisons the cache."""
    global _fonts_ready
    if font_dir:
        set_font_dir(font_dir)
    try:
        os.makedirs(_font_dir, exist_ok=True)
    except OSError as e:
        raise FontError(f'cannot create font dir {_font_dir}: {e}')
    for key, (name, url, sha) in sorted(FONTS.items()):
        path = os.path.join(_font_dir, url.rsplit('/', 1)[1])
        if not os.path.exists(path):
            data = None
            for _attempt in range(2):
                try:
                    with urllib.request.urlopen(url, timeout=60, context=_ssl_context()) as r:
                        data = r.read()
                except Exception as e:  # network down, proxy, TLS ...
                    raise FontError(f'cannot fetch {name} from {url}: {e}')
                if hashlib.sha256(data).hexdigest() == sha:
                    break
                data = None
            if data is None:
                raise FontError(f'{name}: downloaded bytes from {url} do not match sha256 {sha} (not cached)')
            tmp = path + '.part'
            try:
                with open(tmp, 'wb') as f:
                    f.write(data)
                os.replace(tmp, path)
            except OSError as e:
                raise FontError(f'cannot write {path}: {e}')
        try:
            with open(path, 'rb') as f:
                got = hashlib.sha256(f.read()).hexdigest()
        except OSError as e:
            raise FontError(f'cannot read {path}: {e}')
        if got != sha:
            raise FontError(f'{name}: {path} sha256 {got} != expected {sha}')
    _fonts_ready = True


def font(key, dev_px):
    """PIL font for a font key at a DEVICE pixel size (logical size * SCALE)."""
    if not _fonts_ready:
        ensure_fonts()
    k = (key, dev_px)
    if k not in _font_cache:
        path = os.path.join(_font_dir, FONTS[key][1].rsplit('/', 1)[1])
        _font_cache[k] = ImageFont.truetype(path, dev_px)
    return _font_cache[k]


_has_cache = {}


def _has_glyph(key, ch):
    k = (key, ch)
    if k not in _has_cache:
        f = font(key, 40)
        nd = f.getmask('￿')
        _has_cache[k] = not (bytes(f.getmask(ch)) == bytes(nd) and f.getbbox(ch) == f.getbbox('￿'))
    return _has_cache[k]


# text styles: font key, logical px size, tracking (em), default foreground token, bold (for the large-text rule)
STYLES = {
    'brand':    dict(font='blackops', size=22, track=0.04, fg='blade', bold=True),
    'title':    dict(font='blackops', size=30, track=0.03, fg='bone', bold=True),
    'section':  dict(font='allerta', size=13, track=0.08, fg='ink', caps=True),
    'section_mixed': dict(font='allerta', size=13, track=0.08, fg='ink'),
    'label':    dict(font='barlowc600', size=11, track=0.12, fg='bone_dim', caps=True),
    'label_b':  dict(font='barlowc700', size=11, track=0.12, fg='bone', caps=True, bold=True),
    'label_ink': dict(font='barlowc700', size=11, track=0.12, fg='ink', caps=True, bold=True),
    'button':   dict(font='barlowc700', size=13, track=0.12, fg='bone', caps=True, bold=True),
    'button_ink': dict(font='barlowc700', size=13, track=0.12, fg='ink', caps=True, bold=True),
    'body':     dict(font='barlow500', size=13, track=0.0, fg='bone'),
    'body_dim': dict(font='barlow500', size=13, track=0.0, fg='bone_dim'),
    'body_strong': dict(font='barlow600', size=14, track=0.0, fg='bone', bold=True),
    'body_ink': dict(font='barlow500', size=13, track=0.0, fg='ink'),
    'mono':     dict(font='mono', size=14, track=0.0, fg='bone'),
    'mono_dim': dict(font='mono', size=14, track=0.0, fg='bone_dim'),
    'lcd_unit': dict(font='mono', size=14, track=0.0, fg='lcd_amber'),
    'nixie':    dict(font='mono', size=40, track=0.02, fg='blade_hi'),
    'specimen': dict(font='barlow500', size=13, track=0.0, fg='ink'),
}
_FALLBACK = ('barlowc700', 'barlow600', 'barlow500')
# glyphs none of our fonts carry are drawn procedurally by text(): (advance, in cap heights)
_GLYPH_ADV = {'▶': 0.95, '▸': 0.8, '◀': 0.95, '▲': 1.1, '✓': 1.1, '✗': 1.0,
              '✕': 1.0, '║': 0.8, '→': 1.35}


def _cap_h(key, dev_px):
    f = font(key, dev_px)
    return -f.getbbox('H', anchor='ls')[1]


class FlatBackgroundError(AssertionError):
    """Text was about to be drawn over a textured / gradient area."""


# --------------------------------------------------------------------------------------------------------------------
# geometry helpers
# --------------------------------------------------------------------------------------------------------------------
def R(x, y, w, h):
    """(x, y, w, h) -> rect tuple (x0, y0, x1, y1)."""
    return (x, y, x + w, y + h)


def rect_w(r):
    return r[2] - r[0]


def rect_h(r):
    return r[3] - r[1]


def inset(r, d, dy=None):
    dy = d if dy is None else dy
    return (r[0] + d, r[1] + dy, r[2] - d, r[3] - dy)


def rect_c(r):
    return ((r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0)


def _dev(r):
    return tuple(int(round(v * S)) for v in r)


def _rng(name):
    return np.random.default_rng(zlib.crc32(name.encode('utf-8')))


# --------------------------------------------------------------------------------------------------------------------
# canvas
# --------------------------------------------------------------------------------------------------------------------
class Canvas:
    """A 2x RGB page.  ``name`` tags the contrast log.  ``log`` collects one dict per string drawn."""

    def __init__(self, name='page', w=PAGE_W, h=PAGE_H, fill='bench_dark'):
        self.name = name
        self.w, self.h = w, h
        self.im = Image.new('RGB', (w * S, h * S), rgb(fill))
        self.d = ImageDraw.Draw(self.im)
        self.log = []

    # -- output --------------------------------------------------------------------------------------------------
    def finish(self):
        """Downsample to the 1x page (LANCZOS), RGB."""
        return self.im.resize((self.w, self.h), Image.LANCZOS)

    def crop2x(self, rect):
        """The native 2x pixels of a logical rect (a true 2x zoom, no resampling)."""
        return self.im.crop(_dev(rect))

    # -- primitives (logical coords) -----------------------------------------------------------------------------
    def fill(self, rect, color, radius=0):
        x0, y0, x1, y1 = _dev(rect)
        if radius:
            self.d.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius=int(radius * S), fill=rgb(color))
        else:
            self.d.rectangle((x0, y0, x1 - 1, y1 - 1), fill=rgb(color))

    def outline(self, rect, color, width=1, radius=0):
        x0, y0, x1, y1 = _dev(rect)
        if radius:
            self.d.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius=int(radius * S), outline=rgb(color),
                                     width=int(width * S))
        else:
            self.d.rectangle((x0, y0, x1 - 1, y1 - 1), outline=rgb(color), width=int(width * S))

    def hline(self, x0, x1, y, color, alpha=1.0, w=1):
        self.blend((x0, y, x1, y + w), color, alpha)

    def vline(self, x, y0, y1, color, alpha=1.0, w=1):
        self.blend((x, y0, x + w, y1), color, alpha)

    def blend(self, rect, color, alpha):
        """Alpha-fill a rect (uniform, so it keeps flat areas flat)."""
        box = _dev(rect)
        box = (max(box[0], 0), max(box[1], 0), min(box[2], self.im.width), min(box[3], self.im.height))
        if box[2] <= box[0] or box[3] <= box[1]:
            return
        reg = self.im.crop(box)
        self.im.paste(Image.blend(reg, Image.new('RGB', reg.size, rgb(color)), alpha), box)

    def paint(self, box, painter, color, alpha=1.0, aa=4):
        """Anti-aliased shape.  ``painter(draw, k, ox, oy)`` draws white on black at k device px per logical px with
        the box origin at (ox, oy) logical; the mask is box-downsampled and painted in ``color``."""
        x0, y0, x1, y1 = box
        ox, oy = math.floor(x0 * S) / S, math.floor(y0 * S) / S
        wd = int(math.ceil((x1 - ox) * S)) + 1
        hd = int(math.ceil((y1 - oy) * S)) + 1
        m = Image.new('L', (wd * aa, hd * aa), 0)
        painter(ImageDraw.Draw(m), S * aa, ox, oy)
        m = m.resize((wd, hd), Image.BOX)
        if alpha < 1.0:
            m = m.point(lambda v: int(v * alpha + 0.5))
        self.im.paste(Image.new('RGB', (wd, hd), rgb(color)), (int(round(ox * S)), int(round(oy * S))), m)

    def poly(self, pts, color, alpha=1.0):
        """AA polygon from logical points."""
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]

        def pa(d, k, ox, oy):
            d.polygon([((x - ox) * k, (y - oy) * k) for x, y in pts], fill=255)
        self.paint((min(xs) - 1, min(ys) - 1, max(xs) + 1, max(ys) + 1), pa, color, alpha)

    def ellipse(self, cx, cy, rx, ry=None, color='bone', alpha=1.0, width=None):
        ry = rx if ry is None else ry

        def pa(d, k, ox, oy):
            bb = ((cx - rx - ox) * k, (cy - ry - oy) * k, (cx + rx - ox) * k, (cy + ry - oy) * k)
            if width:
                d.ellipse(bb, outline=255, width=max(1, int(width * k)))
            else:
                d.ellipse(bb, fill=255)
        self.paint((cx - rx - 1, cy - ry - 1, cx + rx + 1, cy + ry + 1), pa, color, alpha)

    def arc(self, cx, cy, r, a0, a1, width, color, alpha=1.0):
        """AA arc; angles in PIL convention (degrees, 0 = 3 o'clock, clockwise)."""
        def pa(d, k, ox, oy):
            d.arc(((cx - r - ox) * k, (cy - r - oy) * k, (cx + r - ox) * k, (cy + r - oy) * k), a0, a1, fill=255,
                  width=max(1, int(width * k)))
        self.paint((cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2), pa, color, alpha)

    def line(self, pts, color, width=1.0, alpha=1.0, round_caps=True):
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        pad = width + 1

        def pa(d, k, ox, oy):
            q = [((x - ox) * k, (y - oy) * k) for x, y in pts]
            d.line(q, fill=255, width=max(1, int(width * k)), joint='curve')
            if round_caps:
                for x, y in (q[0], q[-1]):
                    rr = width * k / 2
                    d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=255)
        self.paint((min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad), pa, color, alpha)

    def shadow(self, rect, radius=6, offset=(0, 4), blur=8, opacity=0.6, shape='rect'):
        """Soft cast shadow under a rect (or an ellipse inscribed in it)."""
        pad = blur * 3
        x0, y0, x1, y1 = rect
        box = (x0 + offset[0] - pad, y0 + offset[1] - pad, x1 + offset[0] + pad, y1 + offset[1] + pad)
        dx0, dy0 = int(round(box[0] * S)), int(round(box[1] * S))
        wd, hd = int(round((box[2] - box[0]) * S)), int(round((box[3] - box[1]) * S))
        m = Image.new('L', (wd, hd), 0)
        md = ImageDraw.Draw(m)
        bb = (pad * S, pad * S, pad * S + (x1 - x0) * S, pad * S + (y1 - y0) * S)
        if shape == 'ellipse':
            md.ellipse(bb, fill=255)
        else:
            md.rounded_rectangle(bb, radius=int(radius * S), fill=255)
        m = m.filter(ImageFilter.GaussianBlur(blur * S / 2.0)).point(lambda v: int(v * opacity))
        # clip to canvas
        cx0, cy0 = max(dx0, 0), max(dy0, 0)
        cx1, cy1 = min(dx0 + wd, self.im.width), min(dy0 + hd, self.im.height)
        if cx1 <= cx0 or cy1 <= cy0:
            return
        m = m.crop((cx0 - dx0, cy0 - dy0, cx1 - dx0, cy1 - dy0))
        self.im.paste(Image.new('RGB', m.size, (0, 0, 0)), (cx0, cy0), m)

    def texture(self, rect, tex, radius=0):
        """Paste a (2x) texture image into a rect, optionally with rounded corners."""
        box = _dev(rect)
        w, h = box[2] - box[0], box[3] - box[1]
        t = tex.crop((0, 0, w, h))
        if radius:
            self.im.paste(t, box[:2], _rr_mask(w, h, radius * S))
        else:
            self.im.paste(t, box[:2])

    def sprite(self, img, cx, cy, shadow=0.0):
        """Paste an RGBA sprite (already at 2x size) centred at logical (cx, cy)."""
        x = int(round(cx * S - img.width / 2.0))
        y = int(round(cy * S - img.height / 2.0))
        self.im.paste(img, (x, y), img)


_mask_cache = {}


def _rr_mask(w, h, r):
    k = (w, h, r)
    if k not in _mask_cache:
        m = Image.new('L', (w * 4, h * 4), 0)
        ImageDraw.Draw(m).rounded_rectangle((0, 0, w * 4 - 1, h * 4 - 1), radius=int(r * 4), fill=255)
        _mask_cache[k] = m.resize((w, h), Image.BOX)
    return _mask_cache[k]


# --------------------------------------------------------------------------------------------------------------------
# text
# --------------------------------------------------------------------------------------------------------------------
def _glyph_mask(ch, cap):
    """Procedural glyph mask (L image, advance px, ink top relative to baseline) for a cap height of ``cap`` device px."""
    k = 4
    adv = int(round(_GLYPH_ADV[ch] * cap))
    pad = 2
    wd, hd = adv + 2 * pad, int(cap * 1.3) + 2 * pad
    base = pad + int(cap * 1.15)               # baseline row inside the mask
    m = Image.new('L', (wd * k, hd * k), 0)
    d = ImageDraw.Draw(m)

    def P(x, y):                               # x in px from glyph origin, y in cap fractions above baseline
        return ((pad + x) * k, (base - y * cap) * k)
    c = cap
    if ch in ('▶', '▸'):
        w = 0.78 * c if ch == '▶' else 0.6 * c
        y0, y1 = (0.0, 1.0) if ch == '▶' else (0.1, 0.9)
        d.polygon([P(0.04 * c, y0), P(0.04 * c + w, (y0 + y1) / 2), P(0.04 * c, y1)], fill=255)
    elif ch == '◀':
        w = 0.78 * c
        d.polygon([P(0.04 * c + w, 0.0), P(0.04 * c, 0.5), P(0.04 * c + w, 1.0)], fill=255)
    elif ch == '▲':
        d.polygon([P(0.03 * c, 0.0), P(0.53 * c, 1.0), P(1.03 * c, 0.0)], fill=255)
        if cap >= 14:                          # '!' knocked out of the triangle
            d.rectangle([P(0.49 * c, 0.72)[0], P(0, 0.72)[1], P(0.57 * c, 0.3)[0], P(0, 0.3)[1]], fill=0)
            d.rectangle([P(0.49 * c, 0.2)[0], P(0, 0.2)[1], P(0.57 * c, 0.1)[0], P(0, 0.1)[1]], fill=0)
    elif ch == '✓':
        w = max(2.0, 0.16 * c) * k
        d.line([P(0.06 * c, 0.5), P(0.38 * c, 0.1), P(1.0 * c, 0.95)], fill=255, width=int(w), joint='curve')
    elif ch in ('✗', '✕'):
        w = max(2.0, 0.15 * c) * k
        d.line([P(0.1 * c, 0.9), P(0.9 * c, 0.1)], fill=255, width=int(w))
        d.line([P(0.1 * c, 0.1), P(0.9 * c, 0.9)], fill=255, width=int(w))
    elif ch == '║':
        w = max(1.5, 0.12 * c) * k
        for x in (0.22, 0.52):
            d.line([P(x * c, -0.05), P(x * c, 1.05)], fill=255, width=int(w))
    elif ch == '→':
        w = max(1.5, 0.12 * c) * k
        d.line([P(0.0, 0.5), P(1.25 * c, 0.5)], fill=255, width=int(w))
        d.polygon([P(1.3 * c, 0.5), P(0.95 * c, 0.85), P(0.95 * c, 0.15)], fill=255)
    return m.resize((wd, hd), Image.BOX), adv, pad, base


_gm_cache = {}


def _glyph(ch, cap):
    k = (ch, cap)
    if k not in _gm_cache:
        _gm_cache[k] = _glyph_mask(ch, cap)
    return _gm_cache[k]


def _pick_font(ch, style_font):
    """Font key that carries ``ch`` (style font, else the first fallback that has it)."""
    if ch == ' ' or _has_glyph(style_font, ch):
        return style_font
    for alt in _FALLBACK:
        if _has_glyph(alt, ch):
            return alt
    return style_font


def _layout(s, style_font, dev_px, trk_px):
    """-> (items, width, ink bbox (l, t, r, b) relative to the baseline origin, cap height).  Item = (kind, payload, x,
    fontkey); kind 't' = text run, 'g' = procedural glyph.  Untracked text is set in same-font runs (kerning kept);
    tracked text is set per character with ``trk_px`` between characters."""
    cap = _cap_h(style_font, dev_px)
    segs = []                                  # [fontkey or 'g', text]
    for ch in s:
        if ch in _GLYPH_ADV:
            segs.append(['g', ch])
            continue
        fk = _pick_font(ch, style_font)
        if trk_px == 0 and segs and segs[-1][0] == fk:
            segs[-1][1] += ch
        else:
            segs.append([fk, ch])
    items, x = [], 0.0
    l = t = 1e9
    r = b = -1e9
    for i, (fk, seg) in enumerate(segs):
        gap = trk_px if (trk_px and i < len(segs) - 1) else 0.0
        if fk == 'g':
            _m, adv, pad, base = _glyph(seg, cap)
            items.append(('g', seg, x, style_font))
            l, r = min(l, x), max(r, x + adv)
            t, b = min(t, -cap * 1.02), max(b, 0)
            x += adv + gap
            continue
        f = font(fk, dev_px)
        core = seg.strip(' ')
        if core:
            lead = f.getlength(seg[:len(seg) - len(seg.lstrip(' '))])
            bb = f.getbbox(core, anchor='ls')
            l, r = min(l, x + lead + bb[0]), max(r, x + lead + bb[2])
            t, b = min(t, bb[1]), max(b, bb[3])
        items.append(('t', seg, x, fk))
        x += f.getlength(seg) + gap
    if l > r:
        l = r = t = b = 0
    return items, x, (l, t, r, b), cap


def text_width(s, style='body', size=None, track=None):
    """Advance width of a string in logical px."""
    st = STYLES[style]
    px = size or st['size']
    trk = st['track'] if track is None else track
    s = s.upper() if st.get('caps') else s
    items, w, _bb, _cap = _layout(s, st['font'], int(round(px * S)), trk * px * S)
    return w / S


def _flat_colour(cv, box, what):
    """Return the single colour in a device box or raise FlatBackgroundError."""
    x0, y0, x1, y1 = box
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, cv.im.width), min(y1, cv.im.height)
    if x1 <= x0 or y1 <= y0:
        raise FlatBackgroundError(f'{cv.name}: text {what!r} is off the canvas')
    a = np.asarray(cv.im.crop((x0, y0, x1, y1)))
    first = a[0, 0]
    if not (a == first).all():
        bad = np.argwhere((a != first).any(axis=2))[0]
        raise FlatBackgroundError(f'{cv.name}: text {what!r} is not on a flat background '
                                  f'(logical box {x0 / S:.1f},{y0 / S:.1f},{x1 / S:.1f},{y1 / S:.1f}; first change at '
                                  f'{(x0 + bad[1]) / S:.1f},{(y0 + bad[0]) / S:.1f})')
    return tuple(int(v) for v in first)


def _paint_items(img, items, ox, base, dev_px, fill, cap):
    d = ImageDraw.Draw(img)
    for kind, payload, x, fk in items:
        if kind == 't':
            d.text((ox + x, base), payload, font=font(fk, dev_px), fill=fill, anchor='ls')
        else:
            m, adv, pad, bs = _glyph(payload, cap)
            gx = int(round(ox + x)) - pad
            gy = int(round(base)) - bs
            if isinstance(fill, tuple):
                img.paste(Image.new('RGB', m.size, fill), (gx, gy), m)
            else:
                img.paste(fill, (gx, gy), m)


def text(cv, xy, s, style='body', bg=None, fg=None, size=None, track=None, align='l', glow=0.0, bg_colour=None):
    """THE way to draw text.  ``xy`` = (x, y_centre) in logical px (``align`` l / c / r picks which edge x is); if
    ``xy`` is None the string is centred in ``bg``.  ``bg`` is the rect (logical) the text sits on: the ink box must be
    inside it and the pixels under the ink box + 2 px must be ONE flat colour or FlatBackgroundError is raised.  The
    measured (style, fg, bg colour, px size, bold) is appended to ``cv.log`` for the contrast check.  ``glow`` (0..0.2) adds
    the nixie bloom.  Returns the ink rect (logical)."""
    st = STYLES[style]
    px = size or st['size']
    dev_px = int(round(px * S))
    trk = st['track'] if track is None else track
    fgname = fg or st['fg']
    col = rgb(fgname)
    s2 = s.upper() if st.get('caps') else s
    items, width, (l, t, r, b), cap = _layout(s2, st['font'], dev_px, trk * dev_px)
    if xy is None:
        assert bg is not None, 'text(): xy=None needs bg'
        xy = (rect_c(bg)[0], rect_c(bg)[1])
        align = 'c'
    ax = xy[0] * S
    ox = ax - (width / 2.0 if align == 'c' else width if align == 'r' else 0.0)
    base = xy[1] * S + cap / 2.0
    ink = (ox + l, base + t, ox + r, base + b)
    if bg is not None:
        bd = _dev(bg)
        if ink[0] < bd[0] - 0.5 or ink[2] > bd[2] + 0.5 or ink[1] < bd[1] - 0.5 or ink[3] > bd[3] + 0.5:
            raise FlatBackgroundError(f'{cv.name}: text {s!r} ({ink[0] / S:.1f},{ink[1] / S:.1f},{ink[2] / S:.1f},'
                                      f'{ink[3] / S:.1f}) overflows its well {bg}')
    pad = 2 * S
    fbox = (int(math.floor(ink[0] - pad)), int(math.floor(ink[1] - pad)), int(math.ceil(ink[2] + pad)),
            int(math.ceil(ink[3] + pad)))
    bgc = _flat_colour(cv, fbox, s)
    eff_bg = bgc
    if glow:
        glow = min(glow, 0.2)
        gpad = 14 * S
        gb = (int(ink[0] - gpad), int(ink[1] - gpad), int(ink[2] + gpad), int(ink[3] + gpad))
        lay = Image.new('L', (gb[2] - gb[0], gb[3] - gb[1]), 0)
        _paint_items(lay, items, ox - gb[0], base - gb[1], dev_px, 255, cap)
        lay = lay.filter(ImageFilter.GaussianBlur(4.5 * S)).point(lambda v: min(255, int(v * 2.2)))
        lay = lay.point(lambda v: int(v * glow))
        cv.im.paste(Image.new('RGB', lay.size, col), (gb[0], gb[1]), lay)
        eff_bg = tuple(int(round(bgc[i] * (1 - glow) + col[i] * glow)) for i in range(3))
    _paint_items(cv.im, items, ox, base, dev_px, col, cap)
    cv.log.append(dict(screen=cv.name, style=style, fg=fgname, fg_rgb=col, bg_rgb=eff_bg, px=px,
                       bold=bool(st.get('bold')), text=s, ratio=contrast(col, eff_bg)))
    return (ink[0] / S, ink[1] / S, ink[2] / S, ink[3] / S)


def well(cv, rect, kind='well', radius=2, stamped=True, border=None):
    """A flat text well.  ``kind`` is a colour token (well, well_raised, glass, alu_well ...).  ``stamped`` draws the 1 px
    darker line above and lighter line below OUTSIDE the rect (text area stays flat).  Returns the rect."""
    x0, y0, x1, y1 = rect
    if stamped:
        light = kind in ('alu_well', 'alu')
        cv.blend((x0 - 1, y0 - 1, x1 + 1, y0), 'bench_dark', 0.55 if light else 0.7)
        cv.blend((x0 - 1, y1, x1 + 1, y1 + 1), 'bone', 0.28 if not light else 0.55)
        cv.blend((x0 - 1, y0, x0, y1), 'bench_dark', 0.35)
        cv.blend((x1, y0, x1 + 1, y1), 'bone', 0.12)
    cv.fill(rect, kind, radius)
    if border:
        cv.outline(rect, border, 1, radius)
    return rect


def label_well(cv, xy, s, style='label', fg=None, h=None, pad=6, kind='well', align='l', min_w=0, radius=2, size=None,
               border=None):
    """A well sized to the string plus the string.  ``xy`` = (x, y_centre) with ``align`` l / c / r for x.  Returns the well rect."""
    st = STYLES[style]
    px = size or st['size']
    w = max(text_width(s, style, size=size) + 2 * pad, min_w)
    h = h or int(px * 1.45 + 6)
    h = h + (h % 2)
    x = xy[0] - (w / 2.0 if align == 'c' else w if align == 'r' else 0)
    rect = (x, xy[1] - h / 2.0, x + w, xy[1] + h / 2.0)
    well(cv, rect, kind, radius, border=border)
    text(cv, None, s, style, bg=rect, fg=fg, size=size)
    return rect


def caption(cv, where, s, kind='well', style='label', fg=None):
    """Small caption well.  ``where`` is a rect (text centred) or an (x, y_centre) (left aligned, sized to the text)."""
    if len(where) == 4:
        well(cv, where, kind)
        text(cv, None, s, style, bg=where, fg=fg)
        return where
    return label_well(cv, where, s, style, fg=fg, kind=kind)


# --------------------------------------------------------------------------------------------------------------------
# sprites (Blender renders in plugin/assets, read-only input)
# --------------------------------------------------------------------------------------------------------------------
_asset_cache = {}


def _asset(name):
    if name not in _asset_cache:
        _asset_cache[name] = Image.open(os.path.join(ASSETS, name)).convert('RGBA')
    return _asset_cache[name]


_scaled_cache = {}


def _scale_rgba(img, w_dev, h_dev=None):
    """LANCZOS resize on premultiplied alpha (no dark fringe)."""
    h_dev = h_dev or int(round(img.height * w_dev / img.width))
    return img.convert('RGBa').resize((w_dev, h_dev), Image.LANCZOS).convert('RGBA')


def sprite_image(name, width, frame=None, frame_h=None, crop=None, round_px=0):
    """Asset PNG (or one frame of a vertical filmstrip) scaled to ``width`` logical px, RGBA at 2x.  ``crop`` = source-pixel
    box (x0, y0, x1, y1) cut out first (e.g. to trim the baked backdrop off cab_4x12.png); ``round_px`` = corner radius (source
    px) of an alpha mask applied after the crop."""
    k = (name, width, frame, crop, round_px)
    if k not in _scaled_cache:
        img = _asset(name)
        if frame is not None:
            img = img.crop((0, frame * frame_h, img.width, (frame + 1) * frame_h))
        if crop:
            img = img.crop(crop)
        if round_px:
            m = _rr_mask(img.width, img.height, round_px)
            img = img.copy()
            img.putalpha(m)
        _scaled_cache[k] = _scale_rgba(img, int(round(width * S)))
    return _scaled_cache[k]


def _knob_sprite(kind, value, size):
    name = 'knob_amp.png' if kind == 'amp' else 'knob_pedal.png'
    frame = int(round(max(0.0, min(1.0, value)) * 127))
    return sprite_image(name, size, frame, 128)


def _hue_shift(img, target_hue_deg, sat_scale=1.0):
    """Recolour an RGBA sprite: set hue of every pixel to target (degrees)."""
    a = img.split()[3]
    hsv = np.asarray(img.convert('RGB').convert('HSV')).copy()
    hsv[..., 0] = int(target_hue_deg / 360.0 * 255)
    hsv[..., 1] = np.clip(hsv[..., 1].astype(np.float32) * sat_scale, 0, 255).astype(np.uint8)
    out = Image.fromarray(hsv, 'HSV').convert('RGB')
    out.putalpha(a)
    return out


# --------------------------------------------------------------------------------------------------------------------
# procedural textures (all deterministic, 2x device px)
# --------------------------------------------------------------------------------------------------------------------
def _noise(rng, h, w, cx, cy=None):
    """Smooth value noise, float32 (h, w), roughly unit variance; cx / cy = feature size in device px."""
    cy = cx if cy is None else cy
    gh, gw = int(h // cy) + 3, int(w // cx) + 3
    g = rng.standard_normal((gh, gw)).astype(np.float32)
    big = Image.fromarray(g, 'F').resize((int(gw * cx), int(gh * cy)), Image.BICUBIC)
    a = np.asarray(big)[:h, :w]
    return a * 1.6


def _fbm(rng, h, w, cells, weights):
    out = np.zeros((h, w), np.float32)
    for c, wt in zip(cells, weights):
        out += wt * _noise(rng, h, w, c)
    return out


def _lines_mask(rng, w, h, n, length, angle_mean, angle_sd, value, width=1, cluster=4):
    """Random thin scratches on an L image (device px)."""
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    i = 0
    while i < n:
        cx, cy = rng.uniform(0, w), rng.uniform(0, h)
        a0 = rng.uniform(0, math.pi)
        for _ in range(int(rng.integers(1, cluster + 1))):
            a = a0 + rng.normal(0, angle_sd)
            ln = rng.uniform(0.3, 1.0) * length
            px, py = cx + rng.normal(0, 12 * S), cy + rng.normal(0, 12 * S)
            d.line((px, py, px + ln * math.cos(a), py + ln * math.sin(a)), fill=int(rng.uniform(0.3, 1.0) * value),
                   width=width)
            i += 1
    return m


_tex_cache = {}


def steel_tex(w, h, key='steel', wear=1.0):
    """Worn painted steel, w x h LOGICAL px -> RGB image at 2x.  Mottled paint, scratches, edge chips that show bare steel,
    sparse rust at chips (denser at corners)."""
    ck = ('steel', w, h, key, wear)
    if ck in _tex_cache:
        return _tex_cache[ck]
    rng = _rng('steel:' + key)
    W, H = int(round(w * S)), int(round(h * S))
    base = np.array(PAL['steel'], np.float32)
    lum = 1.0 + 0.055 * _fbm(rng, H, W, [160 * S // 2, 40 * S // 2, 9], [0.7, 0.5, 0.35])
    lum += 0.012 * rng.standard_normal((H, W)).astype(np.float32)
    img = base[None, None, :] * lum[..., None]
    # scratches: thin, slightly lighter, in angle clusters
    scr = _lines_mask(rng, W, H, int(w * h / 1800 * wear) + 4, 70 * S, 0, 0.25, 60, 1, 5)
    scr = np.asarray(scr.filter(ImageFilter.GaussianBlur(0.45))).astype(np.float32) / 255.0
    img += scr[..., None] * 52.0
    # edge wear
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    dist = np.minimum(np.minimum(xx, W - 1 - xx), np.minimum(yy, H - 1 - yy)) / S
    dcx = np.minimum(xx, W - 1 - xx) / S
    dcy = np.minimum(yy, H - 1 - yy) / S
    dcorner = np.sqrt(dcx ** 2 + dcy ** 2)
    n1 = _noise(rng, H, W, 2.5 * S)
    n2 = _noise(rng, H, W, 9 * S)
    n3 = _noise(rng, H, W, 28 * S)
    nz = 0.75 * n1 + 0.55 * n2 + 0.35 * n3
    nz = (nz - nz.mean()) / nz.std()
    prox = np.exp(-dist / 3.0) * 1.5 + np.exp(-dist / 10.0) * 0.5 + np.exp(-dcorner / 22.0) * 0.9
    field = 0.5 * nz + 0.9 * prox * wear
    chip = (field > 1.5).astype(np.float32)
    ch_im = Image.fromarray((chip * 255).astype(np.uint8), 'L')
    alpha = np.asarray(ch_im.filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255.0
    rim = np.asarray(ch_im.filter(ImageFilter.GaussianBlur(1.6))).astype(np.float32) / 255.0
    bare = np.array(PAL['steel_bare'], np.float32)[None, None, :] * (0.72 + 0.14 * n1[..., None] * 0.5)
    img = img * (1 - 0.45 * np.clip(rim - alpha, 0, 1)[..., None])        # darker rim around chips
    img = img * (1 - alpha[..., None]) + bare * alpha[..., None]
    # paint wear halo: the paint thins before it chips (slightly lighter / browner just inside the edge)
    halo = np.clip((field - 1.25) / 0.35, 0, 1) * (1 - chip)
    img = img * (1 - 0.35 * halo[..., None]) + np.array(PAL['rust'], np.float32)[None, None, :] * 0.35 * halo[..., None]
    rust = (chip > 0) & (nz > 1.0) & (n1 > 0.2)
    r_im = Image.fromarray((rust * 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(0.7))
    ra = np.asarray(r_im).astype(np.float32)[..., None] / 255.0 * 0.85
    img = img * (1 - ra) + np.array(PAL['rust'], np.float32)[None, None, :] * ra
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    _tex_cache[ck] = out
    return out


def alu_tex(w, h, key='alu'):
    """Brushed aluminium: horizontal streaks, a few scuffs, darker edge band, 1 px top bevel highlight, edge nicks."""
    ck = ('alu', w, h, key)
    if ck in _tex_cache:
        return _tex_cache[ck]
    rng = _rng('alu:' + key)
    W, H = int(round(w * S)), int(round(h * S))
    base = np.array(PAL['alu'], np.float32)
    s1 = np.asarray(Image.fromarray(rng.standard_normal((H, W // 50 + 3)).astype(np.float32), 'F')
                    .resize(((W // 50 + 3) * 50, H), Image.BICUBIC))[:, :W]
    s2 = np.asarray(Image.fromarray(rng.standard_normal((H, W // 11 + 3)).astype(np.float32), 'F')
                    .resize(((W // 11 + 3) * 11, H), Image.BICUBIC))[:, :W]
    lum = 1.0 + 0.040 * s1 + 0.022 * s2 + 0.014 * _noise(rng, H, W, 120 * S // 2)
    img = base[None, None, :] * lum[..., None]
    # scuffs: short bright arcs
    m = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(m)
    for _ in range(int(w * h / 5200) + 2):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        r = rng.uniform(8, 44) * S
        a0 = rng.uniform(0, 360)
        d.arc((cx - r, cy - r, cx + r, cy + r), a0, a0 + rng.uniform(14, 60), fill=int(rng.uniform(50, 120)), width=1)
    scuff = np.asarray(m.filter(ImageFilter.GaussianBlur(0.5))).astype(np.float32) / 255.0
    img += scuff[..., None] * 38.0
    # a few dark scratches
    dk = _lines_mask(rng, W, H, int(w * h / 14000) + 1, 40 * S, 0, 0.4, 80, 1, 3)
    img -= (np.asarray(dk.filter(ImageFilter.GaussianBlur(0.5))).astype(np.float32) / 255.0)[..., None] * 26.0
    # darker edge band, bevel highlight / shadow
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    dist = np.minimum(np.minimum(xx, W - 1 - xx), np.minimum(yy, H - 1 - yy)) / S
    band = np.clip(1.0 - dist / 4.0, 0, 1)
    img *= (1.0 - 0.14 * band)[..., None]
    img[:S, :, :] = img[:S, :, :] * 0.5 + 255 * 0.5 * 0.9
    img[-S:, :, :] *= 0.78
    # nicks along the edges
    nk = Image.new('L', (W, H), 0)
    nd = ImageDraw.Draw(nk)
    for _ in range(int((w + h) / 40) + 2):
        side = int(rng.integers(0, 4))
        t = rng.uniform(0.05, 0.95)
        if side == 0:
            px, py = t * W, 0
        elif side == 1:
            px, py = t * W, H - 1
        elif side == 2:
            px, py = 0, t * H
        else:
            px, py = W - 1, t * H
        rr = rng.uniform(0.8, 2.0) * S
        nd.ellipse((px - rr, py - rr, px + rr, py + rr), fill=255)
    nka = np.asarray(nk.filter(ImageFilter.GaussianBlur(0.5))).astype(np.float32)[..., None] / 255.0
    img = img * (1 - 0.8 * nka) + np.array(PAL['steel'], np.float32)[None, None, :] * 1.6 * 0.8 * nka
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    _tex_cache[ck] = out
    return out


def plank_layout(h, key='floor'):
    """Plank boundaries (logical y) for a page of height ``h``; sawdust uses the seams."""
    rng = _rng('planks:' + key)
    ys = [0]
    while ys[-1] < h:
        ys.append(ys[-1] + int(rng.integers(74, 114)))
    return ys


def wood_tex(w, h, key='floor'):
    """Dark workbench wood: planks with seams, warped-sine grain, knots, saw-cut marks, oil darkening."""
    ck = ('wood', w, h, key)
    if ck in _tex_cache:
        return _tex_cache[ck]
    rng = _rng('wood:' + key)
    W, H = int(round(w * S)), int(round(h * S))
    bench = np.array(PAL['bench'], np.float32)
    hi = np.array(PAL['bench_hi'], np.float32)
    out = np.empty((H, W, 3), np.float32)
    ys = plank_layout(h, key)
    xs = np.arange(W, dtype=np.float32)[None, :]
    seam_dark = np.array(PAL['bench_dark'], np.float32)
    for pi in range(len(ys) - 1):
        r0, r1 = ys[pi] * S, min(ys[pi + 1] * S, H)
        if r0 >= H:
            break
        n = r1 - r0
        yl = np.arange(n, dtype=np.float32)[:, None]
        warp = np.asarray(Image.fromarray(rng.standard_normal((n // 18 + 3, W // 180 + 3)).astype(np.float32), 'F')
                          .resize(((W // 180 + 3) * 180, (n // 18 + 3) * 18), Image.BICUBIC))[:n, :W] * 9.0 * S / 2
        bend = np.zeros((n, W), np.float32)
        for _k in range(int(rng.integers(0, 3))):          # knots bend the grain locally
            kx, ky = rng.uniform(0, W), rng.uniform(0.2, 0.8) * n
            sig = rng.uniform(26, 46) * S
            bend += rng.uniform(-1, 1) * 30 * S * np.exp(-(((xs - kx) / (sig * 2.4)) ** 2 + ((yl - ky) / sig) ** 2))
        period = rng.uniform(5.5, 10.0) * S
        ph = 2 * math.pi * (yl + warp + bend) / period + rng.uniform(0, 6.28)
        g = 0.5 + 0.5 * np.sin(ph)
        g2 = 0.5 + 0.5 * np.sin(ph * 0.31 + 1.7 + 0.8 * np.sin(ph * 0.11))
        streak = np.asarray(Image.fromarray(rng.standard_normal((n, W // 70 + 3)).astype(np.float32), 'F')
                            .resize(((W // 70 + 3) * 70, n), Image.BICUBIC))[:, :W]
        tone = 1.0 + rng.normal(0, 0.10)
        mix = 0.30 * g ** 2 + 0.22 * g2 + 0.10 * np.clip(streak, -1, 1) * 1.0
        col = bench[None, None, :] * tone + (hi - bench)[None, None, :] * np.clip(mix, 0, 1)[..., None] * 2.0
        # oil darkening, large and soft
        col *= (0.86 + 0.16 * np.clip(_noise(rng, n, W, 260 * S // 2, 60 * S // 2), -1.2, 1.2))[..., None]
        # end joints (vertical seams)
        for _j in range(int(rng.integers(0, 2))):
            jx = int(rng.uniform(0.1, 0.9) * W)
            col[:, jx:jx + 2 * S // 1, :] = seam_dark * 0.7
            col[:, jx + 2 * S:jx + 3 * S, :] *= 1.35
        # seams top / bottom
        col[:2 * S, :, :] = seam_dark
        col[2 * S:3 * S, :, :] = col[2 * S:3 * S, :, :] * 1.5 + 4
        col[-1 * S:, :, :] *= 0.55
        out[r0:r1] = col
    # saw-cut marks: clusters of short parallel strokes, very faint
    m = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(m)
    for _ in range(int(w * h / 90000) + 3):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        a = rng.uniform(-0.35, 0.35) + math.pi / 2
        ln = rng.uniform(26, 60) * S
        for i in range(int(rng.integers(4, 9))):
            ox = cx + i * 3.2 * S
            d.line((ox, cy, ox + ln * math.cos(a), cy + ln * math.sin(a)), fill=int(rng.uniform(40, 90)), width=1)
    cut = np.asarray(m.filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255.0
    out *= (1 - 0.22 * cut)[..., None]
    out += cut[..., None] * 3.0
    arr = np.clip(out, 0, 255).astype(np.uint8)
    img = Image.fromarray(arr, 'RGB')
    _tex_cache[ck] = img
    return img


def sawdust(cv, rect, n, key, clumps=(), avoid=(), seams=None, seam_n=60, size_scale=1.0, alpha_scale=1.0):
    """Fine tan speckles (1-3 px, varied alpha) scattered over ``rect``, clumped around ``clumps`` = [(cx, cy, r, count)]
    and along ``seams`` (list of logical y) with ``seam_n`` particles per seam.  Particles inside any ``avoid`` rect
    are skipped, so call it BEFORE drawing wells / controls, or pass their rects.  Texture only: never under text."""
    rng = _rng('sawdust:' + key)
    x0, y0, x1, y1 = rect
    pts = [(rng.uniform(x0, x1), rng.uniform(y0, y1)) for _ in range(int(n))]
    for (cx, cy, r, cnt) in clumps:
        for _ in range(int(cnt)):
            a = rng.uniform(0, 2 * math.pi)
            rr = abs(rng.normal(0, r / 2.0))
            pts.append((cx + rr * math.cos(a) * 1.8, cy + rr * math.sin(a) * 0.6))
    if seams:
        for sy in seams:
            for _ in range(int(seam_n)):
                pts.append((rng.uniform(x0, x1), sy + abs(rng.normal(0, 2.2)) + 1.0))
    ov = Image.new('L', cv.im.size, 0)
    d = ImageDraw.Draw(ov)
    col_im = None
    for (px, py) in pts:
        if not (x0 <= px < x1 and y0 <= py < y1):
            continue
        if any(a[0] <= px < a[2] and a[1] <= py < a[3] for a in avoid):
            continue
        sz = rng.choice([1.0, 1.0, 1.0, 1.6, 2.2, 3.0]) * size_scale
        al = int(rng.uniform(40, 175) * alpha_scale)
        r = sz * S / 2.0
        d.ellipse((px * S - r, py * S - r, px * S + r, py * S + r), fill=min(255, al))
    ov = ov.filter(ImageFilter.GaussianBlur(0.35))
    cv.im.paste(Image.new('RGB', cv.im.size, PAL['sawdust']), (0, 0), ov)


def hatch(cv, rect, spacing=9, color='steel_bare', alpha=0.45, width=2.0, radius=0):
    """Diagonal-hatch overlay (bypass).  TEXTURE: draw before the wells / text of the card, never over them."""
    x0, y0, x1, y1 = rect
    wd, hd = int((x1 - x0) * S), int((y1 - y0) * S)
    m = Image.new('L', (wd * 2, hd * 2), 0)
    d = ImageDraw.Draw(m)
    k = 2 * S
    for off in range(-int(y1 - y0) - 10, int(x1 - x0) + 10, int(spacing)):
        d.line((off * k, (y1 - y0) * k, (off + (y1 - y0)) * k, 0), fill=255, width=max(1, int(width * k)))
    m = m.resize((wd, hd), Image.BOX).point(lambda v: int(v * alpha))
    if radius:
        mm = _rr_mask(wd, hd, radius * S)
        m = Image.fromarray((np.asarray(m).astype(np.float32) * np.asarray(mm).astype(np.float32) / 255).astype(np.uint8))
    cv.im.paste(Image.new('RGB', (wd, hd), rgb(color)), _dev(rect)[:2], m)


def rivet(cv, x, y, r=3.2, tone='alu'):
    """Domed rivet: cast shadow, shaded circle, highlight."""
    cv.ellipse(x + 0.7, y + 1.1, r + 0.6, r + 0.6, color='bench_dark', alpha=0.55)
    base = rgb(tone)
    cv.ellipse(x, y, r, color=tuple(int(v * 0.55) for v in base))
    cv.ellipse(x - 0.15, y - 0.2, r * 0.86, color=tuple(int(v * 0.92) for v in base))
    cv.ellipse(x - r * 0.28, y - r * 0.32, r * 0.46, color=tuple(min(255, int(v * 1.12) + 18) for v in base), alpha=0.9)
    cv.ellipse(x - r * 0.34, y - r * 0.38, r * 0.2, color=(255, 255, 255), alpha=0.7)


# --------------------------------------------------------------------------------------------------------------------
# frames: panel, plate, nameplate, card, overlay, inspector
# --------------------------------------------------------------------------------------------------------------------
def panel(cv, rect, name='panel', wear=1.0, radius=6, shadow=True, rivets=False):
    """Worn painted steel panel with cast shadow and a 1 px top highlight bevel.  Returns rect."""
    if shadow:
        cv.shadow(rect, radius, (0, 5), 10, 0.55)
    cv.texture(rect, steel_tex(int(rect_w(rect)), int(rect_h(rect)), name, wear), radius)
    x0, y0, x1, y1 = rect
    cv.blend((x0 + radius, y0, x1 - radius, y0 + 1), 'bone', 0.22)
    cv.blend((x0 + radius, y1 - 1, x1 - radius, y1), 'bench_dark', 0.6)
    if rivets:
        for (rx, ry) in ((x0 + 8, y0 + 8), (x1 - 8, y0 + 8), (x0 + 8, y1 - 8), (x1 - 8, y1 - 8)):
            rivet(cv, rx, ry, 3.0, 'steel_bare')
    return rect


def plate(cv, rect, title=None, kind='alu', rivets=True, title_style='section', title_align='l', right=None, key=None):
    """Riveted aluminium header plate (or steel for kind='steel'): cast shadow, brushed texture, domed rivets at the
    corners and every ~120 px along the long edges, title stamped into a flat alu_well strip.  Returns
    {'rect', 'title': strip rect, 'inner': area below the plate title strip}.  ``right`` = optional second string placed
    right-aligned in the same strip."""
    x0, y0, x1, y1 = rect
    w, h = rect_w(rect), rect_h(rect)
    cv.shadow(rect, 3, (0, 3), 7, 0.6)
    tex = alu_tex(int(w), int(h), key or ('plate%dx%d' % (w, h))) if kind == 'alu' else steel_tex(int(w), int(h), key or 'plateS')
    cv.texture(rect, tex, 3)
    strip = None
    if title is not None:
        sh = min(22, h - 10)
        mg = 18 if w > 140 else 9 if w > 100 else 5
        if w <= 100:
            title_align = 'c'
        strip = (x0 + mg, y0 + (h - sh) / 2.0, x1 - mg, y0 + (h - sh) / 2.0 + sh)
        well(cv, strip, 'alu_well', 2)
        text(cv, (strip[0] + 8, rect_c(strip)[1]) if title_align == 'l' else None, title, title_style, bg=strip,
             align='l')
        if right:
            text(cv, (strip[2] - 8, rect_c(strip)[1]), right, 'label_ink', bg=strip, align='r')
    if rivets:
        ins = 7.5 if h >= 22 else 5.5
        tone = 'alu' if kind == 'alu' else 'steel_bare'
        rr = 2.6 if h < 30 else 3.0
        pts = [(x0 + ins, y0 + ins), (x1 - ins, y0 + ins), (x0 + ins, y1 - ins), (x1 - ins, y1 - ins)]
        if h >= 44 and w > 240:                # tall plates: a rivet every ~120 px along both long edges
            n = int(w // 120)
            for i in range(1, n):
                xx = x0 + w * i / n
                pts += [(xx, y0 + ins), (xx, y1 - ins)]
        for (rx, ry) in pts:
            rivet(cv, rx, ry, rr, tone)
    return dict(rect=rect, title=strip, inner=(x0, y1, x1, y1))


def nameplate(cv, rect, s, style='section', size=None, fg='ink'):
    """Riveted aluminium nameplate with centred stamped text in a flat alu_well strip (covers baked art titles)."""
    x0, y0, x1, y1 = rect
    cv.shadow(rect, 3, (0, 2), 5, 0.6)
    cv.texture(rect, alu_tex(int(rect_w(rect)), int(rect_h(rect)), 'np%s%dx%d' % (s, rect_w(rect), rect_h(rect))), 3)
    strip = (x0 + 9, y0 + 4, x1 - 9, y1 - 4)
    well(cv, strip, 'alu_well', 2)
    text(cv, None, s, style, bg=strip, fg=fg, size=size)
    for (rx, ry) in ((x0 + 4.5, y0 + 4.5), (x1 - 4.5, y0 + 4.5), (x0 + 4.5, y1 - 4.5), (x1 - 4.5, y1 - 4.5)):
        rivet(cv, rx, ry, 1.9)
    return strip


def card(cv, rect, title, accent=None, bypassed=False, status=None, key=None):
    """Block card: worn steel body with a stamped alu_well title strip (and an optional status word in the strip's right
    corner).  ``bypassed`` lays the diagonal hatch over the body and stamps the word BYPASSED in the strip: draw the
    card's wells / controls AFTER this call.  ``accent`` = path colour token for a left edge bar.  Returns
    {'rect', 'title', 'body'}."""
    x0, y0, x1, y1 = rect
    cv.shadow(rect, 5, (0, 4), 8, 0.55)
    cv.texture(rect, steel_tex(int(rect_w(rect)), int(rect_h(rect)), key or ('card%s%dx%d' % (title, rect_w(rect), rect_h(rect))), 0.9), 5)
    strip = (x0 + 8, y0 + 8, x1 - 8, y0 + 8 + 20)
    body = (x0 + 8, strip[3] + 8, x1 - 8, y1 - 8)
    if bypassed:
        hatch(cv, (x0 + 3, strip[3] + 3, x1 - 3, y1 - 3), 10, 'steel_bare', 0.38, 2.0)
    well(cv, strip, 'alu_well', 2)
    text(cv, (strip[0] + 7, rect_c(strip)[1]), title, 'section', bg=strip)
    word = 'BYPASSED' if bypassed else status
    if word:
        text(cv, (strip[2] - 7, rect_c(strip)[1]), word, 'label_ink', bg=strip, align='r')
    if accent:
        cv.fill((x0, y0 + 5, x0 + 3, y1 - 5), accent)
    cv.blend((x0 + 5, y0, x1 - 5, y0 + 1), 'bone', 0.2)
    return dict(rect=rect, title=strip, body=body)


def overlay_frame(cv, rect, title, close=True, done=None, name='overlay'):
    """Riveted steel frame for overlays / dialogs: worn steel panel, riveted aluminium title plate with the title (and a
    close x button), optional primary DONE button bottom right.  Returns {'rect', 'title', 'content', 'close', 'done'}."""
    x0, y0, x1, y1 = rect
    panel(cv, rect, name, 1.3, 8, rivets=True)
    pl = plate(cv, (x0 + 14, y0 + 12, x1 - 14, y0 + 46), title, title_style='section')
    res = dict(rect=rect, title=pl['title'], content=(x0 + 18, y0 + 58, x1 - 18, y1 - 18), close=None, done=None)
    if close:
        cb = (x1 - 14 - 44, y0 + 18, x1 - 14 - 22, y0 + 40)
        # stamped close button sits in the title strip's right end: carve its own flat well
        well(cv, cb, 'ink', 2)
        cv.line([(cb[0] + 6, cb[1] + 6), (cb[2] - 6, cb[3] - 6)], 'bone', 1.8)
        cv.line([(cb[0] + 6, cb[3] - 6), (cb[2] - 6, cb[1] + 6)], 'bone', 1.8)
        res['close'] = cb
    if done:
        db = (x1 - 18 - 110, y1 - 18 - 34, x1 - 18, y1 - 18)
        button(cv, db, done, 'primary')
        res['done'] = db
        res['content'] = (x0 + 18, y0 + 58, x1 - 18, y1 - 18 - 34 - 10)
    return res


def gaffer_tape(cv, rect, label=None, fg=None, style='label', torn=True, angle=0.0):
    """Gaffer-tape strip with torn zig-zag ends.  With ``label`` the string is centred on the flat tape (the tape is the
    text well, axis-aligned).  Without it the strip is decoration (woven highlights, optional ``angle`` in degrees) that
    holds cables down and never carries text."""
    x0, y0, x1, y1 = rect
    h = y1 - y0
    zz = 3.0
    n = max(2, int(h // 4))
    left = [(x0 + (zz if i % 2 else 0), y0 + h * i / n) for i in range(n + 1)]
    right = [(x1 - (0 if i % 2 else zz), y0 + h * i / n) for i in range(n + 1)]
    poly = (left if torn else [(x0, y0), (x0, y1)]) + (right[::-1] if torn else [(x1, y1), (x1, y0)])
    if angle:
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        ca, sa = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        poly = [(cx + (px - cx) * ca - (py - cy) * sa, cy + (px - cx) * sa + (py - cy) * ca) for px, py in poly]
    cv.shadow(rect, 1, (0, 2), 3, 0.5)
    cv.poly(poly, 'well' if label else 'well_raised')
    if label:
        cv.blend((x0 + 6, y0, x1 - 6, y0 + 0.8), 'bone', 0.10)
        cv.blend((x0 + 6, y1 - 0.8, x1 - 6, y1), 'bench_dark', 0.7)
        text(cv, None, label, style, bg=(x0 + 5, y0 + 2, x1 - 5, y1 - 2), fg=fg)
    else:                                       # decoration only: cloth weave highlights
        for i in range(int((x1 - x0) // 3)):
            xx = x0 + 3 + i * 3
            pts = [(xx, y0 + 1), (xx + 0.0, y1 - 1)]
            if angle:
                pts = [(cx + (px - cx) * ca - (py - cy) * sa, cy + (px - cx) * sa + (py - cy) * ca) for px, py in pts]
            cv.line(pts, 'bone', 0.5, 0.07, False)
        for yy in (y0 + 0.6, y1 - 1.6):
            pts = [(x0 + 5, yy), (x1 - 5, yy)]
            if angle:
                pts = [(cx + (px - cx) * ca - (py - cy) * sa, cy + (px - cx) * sa + (py - cy) * ca) for px, py in pts]
            cv.line(pts, 'bone', 0.8, 0.12, False)
    return rect


def oled(cv, rect, lines=None, style='mono', size=11):
    """Flat OLED well (covers baked OLED art): glass colour, 1 px steel_bare bezel; optional centred text lines."""
    cv.fill(rect, 'steel_bare', 2)
    cv.fill(inset(rect, 1), 'glass', 1)
    if lines:
        n = len(lines)
        hh = rect_h(rect) / n
        for i, ln in enumerate(lines):
            text(cv, None, ln, style, bg=(rect[0] + 2, rect[1] + 1 + i * hh, rect[2] - 2, rect[1] + 1 + (i + 1) * hh), fg='lcd_amber', size=size)
    return rect


def empty_slot(cv, rect, label='+ PEDAL'):
    """Dashed-frame '+ PEDAL' slot on a flat well."""
    x0, y0, x1, y1 = rect
    cv.fill(rect, 'well', 8)
    cv.blend(rect, 'bench_dark', 0.0)
    d = 8
    for (a, b, c_, e) in ((x0 + 7, y0 + 3, x1 - 7, y0 + 3), (x0 + 7, y1 - 3, x1 - 7, y1 - 3)):
        xx = a
        while xx < c_:
            cv.fill((xx, b - 1, min(xx + d, c_), b + 1), 'steel_bare')
            xx += d * 1.8
    yy = y0 + 7
    while yy < y1 - 7:
        for xx in (x0 + 3, x1 - 3):
            cv.fill((xx - 1, yy, xx + 1, min(yy + d, y1 - 7)), 'steel_bare')
        yy += d * 1.8
    text(cv, None, label, 'button', bg=inset(rect, 8), fg='bone_dim')
    return rect


def bezier(p0, p1, p2, p3, n=40):
    """Sampled cubic Bezier through logical points."""
    out = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def cable(cv, pts, color='blade', width=6.0):
    """Instrument cable: soft shadow, dark sheath, a thin path-coloured stripe (decoration only)."""
    cv.line([(x + 1.5, y + 3) for x, y in pts], 'bench_dark', width + 2, 0.45)
    cv.line(pts, (46, 43, 39), width)
    cv.line(pts, (92, 88, 80), width * 0.3, 0.85)
    cv.line([(x, y) for x, y in pts], color, 1.6, 0.6)


INSPECTOR_RECT = (940, 58, 1280, 800)
RIG_RECT = (0, 58, 940, 800)


def inspector(cv, rect=INSPECTOR_RECT, title='INSPECTOR'):
    """The right-hand inspector: worn steel panel + riveted title plate.  Returns the content rect (below the plate)."""
    x0, y0, x1, y1 = rect
    panel(cv, rect, 'inspector', 1.4, 0, shadow=False)
    cv.blend((x0, y0, x0 + 1, y1), 'bench_dark', 0.9)
    cv.blend((x0 + 1, y0, x0 + 2, y1), 'bone', 0.08)
    pl = plate(cv, (x0 + 10, y0 + 10, x1 - 10, y0 + 42), title)
    return (x0 + 16, y0 + 52, x1 - 16, y1 - 12)


def darken(cv, rect, amount=0.35):
    """Multiply a rect by (1 - amount): the rig dimmed behind an overlay."""
    box = _dev(rect)
    box = (max(box[0], 0), max(box[1], 0), min(box[2], cv.im.width), min(box[3], cv.im.height))
    a = np.asarray(cv.im.crop(box)).astype(np.float32) * (1.0 - amount)
    cv.im.paste(Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8)), box[:2])


def new_screen(name, sawdust_n=900, floor_key='floor'):
    """A 1280 x 800 Canvas filled with the workbench-wood floor and a light dusting of sawdust along the plank seams."""
    cv = Canvas(name)
    cv.texture((0, 0, PAGE_W, PAGE_H), wood_tex(PAGE_W, PAGE_H, floor_key))
    if sawdust_n:
        seams = plank_layout(PAGE_H, floor_key)[1:-1]
        sawdust(cv, (0, 0, PAGE_W, PAGE_H), sawdust_n, name + ':floor', seams=seams, seam_n=70)
    return cv


# --------------------------------------------------------------------------------------------------------------------
# glyph icons (small stand-alone drawn icons)
# --------------------------------------------------------------------------------------------------------------------
def glyph_icon(cv, name, cx, cy, size, color='ink', hole='well_raised'):
    """Drawn icons (``hole`` = colour punched through a gear): gear, close, play, pause, rec, skew (OUT OF TRUE), levels, nc (non-commercial), warn, check, diamond."""
    h = size / 2.0
    if name == 'gear':
        pts = []
        n = 8
        for i in range(n * 2):
            a = math.pi * i / n
            r = h if i % 2 == 0 else h * 0.74
            for da in (-0.17, 0.17):
                pts.append((cx + r * math.cos(a + da * (1 if i % 2 == 0 else 1.6)),
                            cy + r * math.sin(a + da * (1 if i % 2 == 0 else 1.6))))
        cv.poly(pts, color)
        cv.ellipse(cx, cy, h * 0.58, color=color)
        cv.ellipse(cx, cy, h * 0.28, color=hole)
        return
    if name == 'play':
        cv.poly([(cx - h * 0.55, cy - h * 0.8), (cx + h * 0.85, cy), (cx - h * 0.55, cy + h * 0.8)], color)
    elif name == 'pause':
        cv.fill((cx - h * 0.7, cy - h * 0.75, cx - h * 0.15, cy + h * 0.75), color)
        cv.fill((cx + h * 0.15, cy - h * 0.75, cx + h * 0.7, cy + h * 0.75), color)
    elif name == 'rec':
        cv.ellipse(cx, cy, h * 0.75, color=color)
    elif name == 'stop':
        cv.fill((cx - h * 0.65, cy - h * 0.65, cx + h * 0.65, cy + h * 0.65), color)
    elif name == 'skew':      # out-of-true level: reference line + tilted bar + bubble
        cv.line([(cx - h, cy), (cx + h, cy)], color, 1.0, 0.45)
        cv.line([(cx - h * 0.9, cy + h * 0.45), (cx + h * 0.9, cy - h * 0.45)], color, 2.2)
        cv.ellipse(cx + h * 0.35, cy - h * 0.16 - 0.0, h * 0.28, color=color)
    elif name == 'levels':
        for i, (bh) in enumerate((0.4, 0.7, 1.0)):
            cv.fill((cx - h + i * h * 0.72, cy + h * 0.8 - bh * h * 1.6, cx - h + i * h * 0.72 + h * 0.5, cy + h * 0.8), color)
    elif name == 'nc':        # crossed circle
        cv.ellipse(cx, cy, h * 0.85, color=color, width=1.8)
        cv.line([(cx - h * 0.6, cy + h * 0.6), (cx + h * 0.6, cy - h * 0.6)], color, 1.8)
    elif name == 'warn':
        cv.poly([(cx - h, cy + h * 0.8), (cx, cy - h * 0.9), (cx + h, cy + h * 0.8)], color)
    elif name == 'check':
        cv.line([(cx - h * 0.7, cy), (cx - h * 0.15, cy + h * 0.6), (cx + h * 0.8, cy - h * 0.6)], color, 2.0)
    elif name == 'diamond':
        cv.poly([(cx, cy - h), (cx + h * 0.8, cy), (cx, cy + h), (cx - h * 0.8, cy)], color)
    elif name == 'eye':
        cv.ellipse(cx, cy, h, h * 0.55, color=color, width=1.6)
        cv.ellipse(cx, cy, h * 0.3, color=color)
    elif name == 'close':
        cv.line([(cx - h * 0.6, cy - h * 0.6), (cx + h * 0.6, cy + h * 0.6)], color, 1.8)
        cv.line([(cx - h * 0.6, cy + h * 0.6), (cx + h * 0.6, cy - h * 0.6)], color, 1.8)
    elif name == 'caret':
        cv.poly([(cx - h * 0.7, cy - h * 0.3), (cx + h * 0.7, cy - h * 0.3), (cx, cy + h * 0.5)], color)
    elif name == 'chev_l':
        cv.line([(cx + h * 0.35, cy - h * 0.7), (cx - h * 0.35, cy), (cx + h * 0.35, cy + h * 0.7)], color, 2.0)
    elif name == 'chev_r':
        cv.line([(cx - h * 0.35, cy - h * 0.7), (cx + h * 0.35, cy), (cx - h * 0.35, cy + h * 0.7)], color, 2.0)
    else:
        raise KeyError(name)


# --------------------------------------------------------------------------------------------------------------------
# buttons, fields, badges, chips
# --------------------------------------------------------------------------------------------------------------------
_BTN = {
    'primary':   dict(fill='blade', fg='ink', border='blade_hi', style='button_ink'),
    'secondary': dict(fill='well_raised', fg='bone', border='steel_bare', style='button'),
    'danger':    dict(fill='alert', fg='ink', border='alert', style='button_ink'),
}


def button(cv, rect, label, kind='secondary', state='normal', caption=None, icon=None):
    """Button.  kind: primary (blade fill, ink text) / secondary (well_raised, bone text, steel_bare border) / danger.
    state: normal / pressed (inset: darker top edge, label nudged 1 px) / disabled (muted text; ``caption`` says why, set
    in a well to the right of the button).  ``icon`` = glyph_icon name drawn left of the label.  Returns rect (extended to
    include the caption)."""
    spec = _BTN[kind]
    x0, y0, x1, y1 = rect
    fill, fgc, border = spec['fill'], spec['fg'], spec['border']
    if state == 'disabled':
        fill, fgc, border = 'well_raised', 'bone_mute', 'well'
    cv.shadow((x0, y0, x1, y1), 3, (0, 2), 4, 0.45)
    cv.fill(rect, border, 3)
    inner = (x0 + 1, y0 + 1, x1 - 1, y1 - 1)
    cv.fill(inner, fill, 2)
    if state == 'pressed':
        cv.blend((x0 + 1, y0 + 1, x1 - 1, y0 + 4), 'bench_dark', 0.55)       # inset: shadow on top, no bevel
        cv.blend((x0 + 1, y1 - 2, x1 - 1, y1 - 1), 'bone', 0.1)
        if kind == 'secondary':
            cv.fill((x0 + 4, y1 - 4.5, x1 - 4, y1 - 2.5), 'blade', 1)       # lit underline: pressed / open
        tbg = (x0 + 3, y0 + 5, x1 - 3, y1 - 3)
        dy = 1
    else:
        if state != 'disabled':
            cv.blend((x0 + 2, y0 + 1, x1 - 2, y0 + 2), 'bone', 0.18)           # top bevel
            cv.blend((x0 + 2, y1 - 2, x1 - 2, y1 - 1), 'bench_dark', 0.35)
        tbg = (x0 + 3, y0 + 4, x1 - 3, y1 - 4)
        dy = 0
    st = spec['style']
    tw = text_width(label, st)
    ix = 0
    if icon:
        ix = 8
    cx = (x0 + x1) / 2.0 + ix
    if icon:
        glyph_icon(cv, icon, cx - tw / 2.0 - 9, (y0 + y1) / 2.0 + dy, 11, fgc)
    text(cv, (cx, (y0 + y1) / 2.0 + dy), label, st, bg=tbg, fg=fgc, align='c')
    out = rect
    if state == 'disabled' and caption:
        cr = label_well(cv, (x1 + 8, (y0 + y1) / 2.0), 'DISABLED · ' + caption, 'label', fg='bone_mute')
        out = (x0, y0, cr[2], y1)
    elif state == 'disabled':
        cr = label_well(cv, (x1 + 8, (y0 + y1) / 2.0), 'DISABLED', 'label', fg='bone_mute')
        out = (x0, y0, cr[2], y1)
    return out


def text_field(cv, rect, s, style='mono', caret=False, disabled=False, placeholder=False, align='l'):
    """Text input: flat well_raised field with an inset border; ``caret`` draws the insertion bar after the text."""
    x0, y0, x1, y1 = rect
    cv.fill(rect, 'steel_bare', 3)
    cv.fill((x0 + 1, y0 + 1, x1 - 1, y1 - 1), 'well_raised', 2)
    cv.blend((x0 + 1, y0 + 1, x1 - 1, y0 + 3), 'bench_dark', 0.5)
    inner = (x0 + 3, y0 + 3, x1 - 3, y1 - 3)
    fgc = 'bone_mute' if (disabled or placeholder) else 'bone'
    tx = x0 + 8
    ink = text(cv, (tx, (y0 + y1) / 2.0), s, style, bg=inner, fg=fgc)
    if caret:
        cv.fill((ink[2] + 2, y0 + 5, ink[2] + 3.5, y1 - 5), 'blade')
    return rect


def dropdown(cv, rect, s, style='body', open_=False):
    """Dropdown: field with the selected text and a caret box on the right."""
    x0, y0, x1, y1 = rect
    text_field(cv, (x0, y0, x1 - 26, y1), s, style)
    cv.fill((x1 - 26, y0, x1, y1), 'steel_bare', 3)
    cv.fill((x1 - 25, y0 + 1, x1 - 1, y1 - 1), 'well_raised', 2)
    glyph_icon(cv, 'caret', x1 - 13, (y0 + y1) / 2.0, 10, 'bone')
    return rect


def checkbox(cv, x, y, on, label, style='body'):
    """Checkbox (x, y = left, centre).  The check mark is the state (not the colour).  Label sits in its own well."""
    b = (x, y - 8, x + 16, y + 8)
    cv.fill(b, 'steel_bare', 2)
    cv.fill((x + 1, y - 7, x + 15, y + 7), 'well_raised', 1)
    if on:
        glyph_icon(cv, 'check', x + 8, y, 12, 'ok')
    lr = label_well(cv, (x + 22, y), label, style, fg='bone')
    return (x, y - 9, lr[2], y + 9)


_BADGES = {
    'UNCAL':          dict(fill='warn', glyph='warn', fg='ink'),
    'NON-COMMERCIAL': dict(fill='bone_dim', glyph='nc', fg='ink'),
    'LEGACY LEVELS':  dict(fill='alu_well', glyph='levels', fg='ink'),
    'PREVIEW':        dict(fill='body', glyph='eye', fg='ink'),
    'REFINED':        dict(fill='ok', glyph='check', fg='ink'),
    'BETA':           dict(fill='blade_hi', glyph='diamond', fg='ink'),
    'OUT OF TRUE':    dict(fill='alert', glyph='skew', fg='ink'),
}


def badge(cv, xy, kind, h=20, align='l'):
    """Notched-tag badge stamped with its word + a glyph: UNCAL, NON-COMMERCIAL, LEGACY LEVELS, PREVIEW, REFINED, BETA,
    OUT OF TRUE.  ``xy`` = (x, y_centre).  Colour is never the only carrier (shape + glyph + word).  Returns rect."""
    spec = _BADGES[kind]
    tw = text_width(kind, 'label_ink')
    notch, ch = 5.0, 4.0
    w = notch + 6 + 12 + 5 + tw + 8 + ch
    x = xy[0] - (w if align == 'r' else w / 2 if align == 'c' else 0)
    y0, y1 = xy[1] - h / 2.0, xy[1] + h / 2.0
    x1 = x + w
    cy = xy[1]
    pts = [(x, y0), (x1 - ch, y0), (x1, y0 + ch), (x1, y1 - ch), (x1 - ch, y1), (x, y1), (x + notch, cy)]
    cv.shadow((x, y0, x1, y1), 2, (0, 2), 3, 0.5)
    cv.poly(pts, 'bench_dark')
    cv.poly([(x + 0.7, y0 + 0.7), (x1 - ch - 0.2, y0 + 0.7), (x1 - 0.7, y0 + ch + 0.2), (x1 - 0.7, y1 - ch - 0.2),
             (x1 - ch - 0.2, y1 - 0.7), (x + 0.7, y1 - 0.7), (x + notch + 0.5, cy)], spec['fill'])
    glyph_icon(cv, spec['glyph'], x + notch + 6 + 6, cy, 11, spec['fg'])
    tx = x + notch + 6 + 12 + 5
    box = (tx - 1, y0 + 3, x1 - ch - 1, y1 - 3)
    text(cv, (tx, cy), kind, 'label_ink', bg=box, fg=spec['fg'])
    return (x, y0, x1, y1)


_CHIP = {'ok': 'ok', 'warn': 'warn', 'alert': 'alert', 'neutral': 'bone_dim', 'blade': 'blade', 'body': 'body'}


def chip(cv, xy, s, kind='neutral', glyph=None, h=22, align='l', style='label_b'):
    """Status chip: well with a coloured 1 px border and a coloured word, optional drawn glyph (state = word + glyph)."""
    col = _CHIP[kind]
    tw = text_width(s, style)
    gw = 16 if glyph else 0
    w = tw + 14 + gw
    x = xy[0] - (w if align == 'r' else w / 2 if align == 'c' else 0)
    rect = (x, xy[1] - h / 2.0, x + w, xy[1] + h / 2.0)
    cv.fill(rect, col, 3)
    cv.fill(inset(rect, 1), 'well', 2)
    inner = inset(rect, 3)
    if glyph:
        glyph_icon(cv, glyph, x + 12, xy[1], 10, col)
    text(cv, (x + 7 + gw, xy[1]), s, style, bg=inner, fg=col)
    return rect


def tab(cv, rect, s, active=False, kind='path'):
    """Stamped tab: inactive = flat well; active = raised well_raised, blade top bar and a leading marker glyph."""
    x0, y0, x1, y1 = rect
    if active:
        cv.fill(rect, 'blade', 3)
        cv.fill((x0 + 1, y0 + 3, x1 - 1, y1), 'well_raised', 2)
        text(cv, None, '▸ ' + s, 'label_b', bg=(x0 + 3, y0 + 5, x1 - 3, y1 - 2))
    else:
        well(cv, (x0, y0 + 4, x1, y1), 'well', 2)
        text(cv, None, s, 'label', bg=(x0, y0 + 4, x1, y1))
    return rect


def slider(cv, rect, value, markers=None, key='slider'):
    """Aluminium slider rail with a thumb; ``markers`` = list of (value, label_char) tick flags (e.g. loop A / B)."""
    x0, y0, x1, y1 = rect
    cy = (y0 + y1) / 2.0
    rail = (x0, cy - 4, x1, cy + 4)
    cv.shadow(rail, 3, (0, 1), 3, 0.5)
    cv.texture(rail, alu_tex(int(rect_w(rail)), 8, key), 3)
    cv.fill((x0, cy - 4, x0 + (x1 - x0) * value, cy + 4), 'blade', 3)
    cv.blend((x0 + 1, cy - 4, x1 - 1, cy - 3), 'bone', 0.35)
    for (v, _lbl) in (markers or []):
        mx = x0 + (x1 - x0) * v
        cv.fill((mx - 1, y0, mx + 1, y1), 'bone')
    tx = x0 + (x1 - x0) * value
    cv.shadow((tx - 7, cy - 9, tx + 7, cy + 9), 3, (0, 2), 4, 0.6)
    cv.texture((tx - 7, cy - 9, tx + 7, cy + 9), alu_tex(14, 18, key + 'thumb'), 3)
    cv.fill((tx - 0.75, cy - 6, tx + 0.75, cy + 6), 'ink')
    return rect


def progress(cv, rect, frac, fill='blade'):
    """Progress bar on a flat well (no text on the bar; put the word in a neighbouring well)."""
    well(cv, rect, 'well', 3)
    cv.fill((rect[0] + 2, rect[1] + 2, rect[0] + 2 + (rect_w(rect) - 4) * frac, rect[3] - 2), fill, 2)
    return rect


# --------------------------------------------------------------------------------------------------------------------
# LCD (7-segment) and nixie
# --------------------------------------------------------------------------------------------------------------------
_SEG = {'0': 'abcdef', '1': 'bc', '2': 'abdeg', '3': 'abcdg', '4': 'bcfg', '5': 'acdfg', '6': 'acdefg', '7': 'abc',
        '8': 'abcdefg', '9': 'abcdfg', '-': 'g', '+': 'g', ' ': ''}
_SLANT = math.tan(math.radians(6.0))


def _seg_polys(h, cw, t):
    """Segment polygons of one cell, in cell units (x right, y down), unslanted.  cw = cell width, h = cell height."""
    g = t * 0.14
    hx0, hx1 = t * 0.5 + g, cw - t * 0.5 - g

    def hs(yc):
        return [(hx0 - t * 0.35, yc), (hx0 + t * 0.2, yc - t / 2), (hx1 - t * 0.2, yc - t / 2), (hx1 + t * 0.35, yc),
                (hx1 - t * 0.2, yc + t / 2), (hx0 + t * 0.2, yc + t / 2)]

    def vs(xc, y0, y1):
        return [(xc, y0 - t * 0.35), (xc + t / 2, y0 + t * 0.2), (xc + t / 2, y1 - t * 0.2), (xc, y1 + t * 0.35),
                (xc - t / 2, y1 - t * 0.2), (xc - t / 2, y0 + t * 0.2)]
    ym = h / 2.0
    return {'a': hs(t / 2), 'g': hs(ym), 'd': hs(h - t / 2),
            'f': vs(t / 2, t / 2 + g * 2, ym - g * 2), 'b': vs(cw - t / 2, t / 2 + g * 2, ym - g * 2),
            'e': vs(t / 2, ym + g * 2, h - t / 2 - g * 2), 'c': vs(cw - t / 2, ym + g * 2, h - t / 2 - g * 2)}


def lcd_width(value, unit='', digits=None, h=26):
    """Width of an lcd() glass for layout."""
    cells = [c for c in value if c != '.']
    n = digits if digits is not None else len(cells)
    cw = h * 0.56
    pitch = cw + h * 0.2
    w = 8 + n * pitch + h * 0.1
    if unit:
        w += 6 + text_width(unit, 'lcd_unit') + 2
    return w + 4


def lcd(cv, xy, value, unit='', digits=None, h=26, align='l', w=None):
    """7-segment LCD readout (h >= 14, spec rule 4): glass well (flat), lcd_amber digits slanted ~6 deg with faint ghost segments for the unlit
    ones (decoration, not text), the unit in Share Tech Mono to the right inside the same glass.  ``value`` is a string
    ('-33.1', '+12.5', '01:23.4', '142'); ``digits`` = number of digit cells (right-aligned, default = the string's).
    ``xy`` = (x, y_centre).  Returns the glass rect."""
    if h < 14:
        raise ValueError('lcd(): readouts must be >= 14 px (spec rule 4), got h=%s' % h)
    toks = []                                  # (char, dot_after)
    for ch in value:
        if ch == '.' and toks:
            toks[-1] = (toks[-1][0], True)
        else:
            toks.append((ch, False))
    n = digits if digits is not None else len(toks)
    cw = h * 0.56
    t = h * 0.14
    pitch = cw + h * 0.2
    wtot = lcd_width(value, unit, digits, h) if w is None else w
    x = xy[0] - (wtot if align == 'r' else wtot / 2.0 if align == 'c' else 0)
    rect = (x, xy[1] - h / 2.0 - 5, x + wtot, xy[1] + h / 2.0 + 5)
    # bezel + glass (flat)
    cv.shadow(rect, 3, (0, 2), 4, 0.5)
    cv.fill(rect, 'steel_bare', 3)
    cv.fill(inset(rect, 1), 'bench_dark', 3)
    glass = inset(rect, 2)
    cv.fill(glass, 'glass', 2)
    # the digit area must be flat before ghosts are drawn
    dx0 = x + 6
    dig_box = (dx0, rect[1] + 3, dx0 + n * pitch + h * 0.15, rect[3] - 3)
    bgc = _flat_colour(cv, _dev(dig_box), 'lcd ' + value)
    ghost, lit = Image.new('L', (int((dig_box[2] - dig_box[0] + 2) * S * 4), int((dig_box[3] - dig_box[1] + 2) * S * 4)), 0), None
    gm = ghost
    lm = Image.new('L', gm.size, 0)
    gd, ld = ImageDraw.Draw(gm), ImageDraw.Draw(lm)
    k = S * 4
    top = (dig_box[1] + dig_box[3]) / 2.0 - h / 2.0
    padn = n - len(toks)
    polys = _seg_polys(h, cw, t)

    def put(d, cell, segs, ox):
        for sname in segs:
            pts = []
            for (px, py) in polys[sname]:
                sx = px + (h - py) * _SLANT
                pts.append(((ox + cell * pitch + sx) * k, (py + top - dig_box[1] + 1) * k))
            d.polygon(pts, fill=255)
    ox = dx0 - dig_box[0] + 1
    for cell in range(n):
        put(gd, cell, 'abcdefg', ox)
        gd.ellipse(((ox + cell * pitch + cw + t * 0.3) * k, (top + h - dig_box[1] + 1 - t * 0.9) * k,
                    (ox + cell * pitch + cw + t * 1.1) * k, (top + h - dig_box[1] + 1 - t * 0.1) * k), fill=255)
    for i, (ch, dot) in enumerate(toks):
        cell = padn + i
        if cell < 0:
            continue
        if ch in _SEG:
            put(ld, cell, _SEG[ch], ox)
            if ch == '+':                      # vertical bar of the plus
                put_pts = [((ox + cell * pitch + cw / 2 - t / 2 + (h - yy) * _SLANT) * k, (yy + top - dig_box[1] + 1) * k)
                           for yy in (h * 0.28, h * 0.28)]
                x_c = cw / 2
                poly = [(x_c - t / 2, h * 0.28), (x_c + t / 2, h * 0.28), (x_c + t / 2, h * 0.72), (x_c - t / 2, h * 0.72)]
                ld.polygon([((ox + cell * pitch + px + (h - py) * _SLANT) * k, (py + top - dig_box[1] + 1) * k)
                            for px, py in poly], fill=255)
        elif ch == ':':
            for yc in (h * 0.32, h * 0.68):
                cx_ = ox + cell * pitch + cw / 2 + (h - yc) * _SLANT
                ld.ellipse(((cx_ - t * 0.55) * k, (yc + top - dig_box[1] + 1 - t * 0.55) * k,
                            (cx_ + t * 0.55) * k, (yc + top - dig_box[1] + 1 + t * 0.55) * k), fill=255)
        if dot:
            ld.ellipse(((ox + cell * pitch + cw + t * 0.3) * k, (top + h - dig_box[1] + 1 - t * 0.9) * k,
                        (ox + cell * pitch + cw + t * 1.1) * k, (top + h - dig_box[1] + 1 - t * 0.1) * k), fill=255)
    sz = (int((dig_box[2] - dig_box[0] + 2) * S), int((dig_box[3] - dig_box[1] + 2) * S))
    gm = gm.resize(sz, Image.BOX).point(lambda v: int(v * 0.11))
    lm = lm.resize(sz, Image.BOX)
    pos = (int(round((dig_box[0] - 1) * S)), int(round((dig_box[1] - 1) * S)))
    cv.im.paste(Image.new('RGB', sz, PAL['lcd_amber']), pos, gm)
    cv.im.paste(Image.new('RGB', sz, PAL['lcd_amber']), pos, lm)
    cv.log.append(dict(screen=cv.name, style='lcd', fg='lcd_amber', fg_rgb=PAL['lcd_amber'], bg_rgb=bgc,
                       px=h, bold=False, text=value, ratio=contrast(PAL['lcd_amber'], bgc)))
    if unit:
        ux = dig_box[2] + 4
        text(cv, (ux, xy[1] + 1), unit, 'lcd_unit', bg=(ux - 1, rect[1] + 3, rect[2] - 4, rect[3] - 3))
    return rect


def nixie(cv, rect, s, size=40, caption_text=None):
    """Big BLEND readout: glass well (flat) with Share Tech Mono digits in blade_hi and a soft glow (blurred copy over
    the same flat glass; no mesh).  Returns rect."""
    cv.shadow(rect, 4, (0, 2), 5, 0.5)
    cv.fill(rect, 'steel_bare', 4)
    cv.fill(inset(rect, 1), 'bench_dark', 4)
    cv.fill(inset(rect, 2), 'glass', 3)
    text(cv, None, s, 'nixie', bg=inset(rect, 3), size=size, glow=0.2)
    return rect


# --------------------------------------------------------------------------------------------------------------------
# knob, footswitch, LED, toggle, rotary selector
# --------------------------------------------------------------------------------------------------------------------
def knob(cv, cx, cy, size, kind='pedal', value=0.5, path='blade', label=None, readout=None, track=True):
    """Knob from the Blender filmstrip (frame = round(value * 127)) with a drop shadow, a value arc in the path colour
    (``path`` = colour token; the arc is drawn here as the sidecar says) on a faint track, a label well under it and an
    optional LCD readout ``readout=(value_str, unit)`` under the label.  ``size`` = sprite width in logical px.  Returns
    the bounding rect including label + readout."""
    spr = _knob_sprite(kind, value, size)
    cv.shadow((cx - size * 0.42, cy - size * 0.4, cx + size * 0.42, cy + size * 0.4), 99, (size * 0.03, size * 0.07),
              size * 0.18, 0.7, 'ellipse')
    if track:
        ring = (62.0 if kind == 'amp' else 52.48) / 64.0 * size / 2.0
        wdt = max(1.8, size * 0.04)
        cv.arc(cx, cy, ring, 135, 405, wdt, 'bench_dark', 0.8)
        cv.arc(cx, cy, ring, 135, 405, wdt * 0.5, 'steel_bare', 0.35)
        if value > 0.005:
            cv.arc(cx, cy, ring, 135, 135 + 270 * value, wdt, path)
    cv.sprite(spr, cx, cy)
    bottom = cy + size / 2.0
    top = cy - size / 2.0
    right = cx + size / 2.0
    if label:
        lr = label_well(cv, (cx, bottom + 8), label, 'label', h=16, pad=5, align='c')
        bottom = lr[3]
        top = min(top, lr[1])
        right = max(right, lr[2])
    if readout:
        rr = lcd(cv, (cx, bottom + 4 + 13), readout[0], readout[1] if len(readout) > 1 else '',
                 digits=readout[2] if len(readout) > 2 else None, h=14, align='c')
        bottom = rr[3]
    return (cx - size / 2.0, top, right, bottom)


def footswitch(cv, cx, cy, size=56, down=False, label=None, word=None):
    """Footswitch from the Blender sprite (frame 0 up / frame 1 pressed) with a shadow.  ``label`` / ``word`` (e.g. ACTIVE)
    go in a well under it.  Returns bounding rect."""
    spr = sprite_image('footswitch.png', size, 1 if down else 0, 128)
    cv.shadow((cx - size * 0.38, cy - size * 0.38, cx + size * 0.38, cy + size * 0.38), 99, (0, size * (0.03 if down else 0.07)),
              size * 0.14, 0.6 if not down else 0.35, 'ellipse')
    cv.sprite(spr, cx, cy)
    bottom = cy + size / 2.0
    s = ' '.join(x for x in (label, word) if x)
    if s:
        lr = label_well(cv, (cx, bottom + 7), s, 'label_b', h=15, pad=5, align='c')
        bottom = lr[3]
    return (cx - size / 2.0, cy - size / 2.0, cx + size / 2.0, bottom)


def led(cv, cx, cy, on, size=22, hue='orange', word=True, word_side='r'):
    """LED sprite (off / on), hue-rotated variants 'orange' / 'ok' (green) / 'alert' (red); the word ON / OFF follows in a
    well so state is never colour alone.  Returns the bounding rect."""
    key = ('led', hue, on, size)
    if key not in _scaled_cache:
        spr = sprite_image('led_orange.png', size * 2, 1 if on else 0, 64)
        if hue == 'ok':
            spr = _hue_shift(spr, 125)
        elif hue == 'alert':
            spr = _hue_shift(spr, 358)
        _scaled_cache[key] = spr
    spr = _scaled_cache[key]
    cv.sprite(spr, cx, cy)
    r = (cx - size / 2.0, cy - size / 2.0, cx + size / 2.0, cy + size / 2.0)
    if word:
        lr = label_well(cv, (cx + size / 2.0 + 5, cy), 'ON' if on else 'OFF', 'label_b', h=16, pad=5,
                        fg='ok' if on else 'bone_mute' if False else None)
        r = (r[0], r[1], lr[2], r[3])
    return r


def _chrome_gradient(w, h, light=1.0):
    ys = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    v = 90 + 140 * (0.5 + 0.5 * np.cos((ys - 0.3) * 5.5)) * light
    return np.repeat(np.repeat(v[..., None], w, 1), 3, 2)


def toggle(cv, cx, cy, pos, labels, size=34, key='tg'):
    """Mini toggle: chrome hex nut + bushing + bat lever (procedural, the asset set has no toggle strip).  ``pos`` = index
    into ``labels`` (2 or 3 throws: left / [up] / right).  Every label is in a well; the selected one gets a blade border and
    a leading marker.  Returns the bounding rect."""
    n = len(labels)
    r = size / 2.0
    # hex nut
    cv.shadow((cx - r * 0.9, cy - r * 0.9, cx + r * 0.9, cy + r * 0.9), 99, (1, 3), 5, 0.6, 'ellipse')
    hexp = [(cx + r * 0.95 * math.cos(math.radians(30 + 60 * i)), cy + r * 0.95 * math.sin(math.radians(30 + 60 * i))) for i in range(6)]
    cv.poly(hexp, (70, 70, 68))
    # chrome gradient clip to hex via mask
    wd, hd = int(2 * r * S) + 2, int(2 * r * S) + 2
    g = Image.fromarray(np.clip(_chrome_gradient(wd, hd), 0, 255).astype(np.uint8), 'RGB')
    mm = Image.new('L', (wd * 4, hd * 4), 0)
    ImageDraw.Draw(mm).polygon([((x - (cx - r) + 0.5) * S * 4 - 0, (y - (cy - r) + 0.5) * S * 4) for x, y in
                                [(cx + r * 0.9 * math.cos(math.radians(30 + 60 * i)), cy + r * 0.9 * math.sin(math.radians(30 + 60 * i))) for i in range(6)]],
                               fill=255)
    cv.im.paste(g, (int(round((cx - r) * S)), int(round((cy - r) * S))), mm.resize((wd, hd), Image.BOX))
    cv.ellipse(cx, cy, r * 0.5, color=(60, 60, 58))
    cv.ellipse(cx, cy, r * 0.44, color=(170, 170, 166))
    cv.ellipse(cx - r * 0.1, cy - r * 0.12, r * 0.28, color=(225, 225, 220), alpha=0.8)
    # lever
    if n == 2:
        ang = -38 if pos == 0 else 38
    else:
        ang = (-48, 0, 48)[pos] if n == 3 else (-48 + 96 * pos / max(1, n - 1))
    a = math.radians(ang)
    L = r * 1.45
    tipx, tipy = cx + L * math.sin(a), cy - L * math.cos(a) * 0.55 - r * 0.15
    nx, ny = math.cos(a), math.sin(a)
    w0 = r * 0.2
    cv.poly([(cx - nx * w0 + 0.9, cy - ny * w0 + 1.4), (cx + nx * w0 + 0.9, cy + ny * w0 + 1.4),
             (tipx + nx * w0 * 0.55 + 0.9, tipy + ny * w0 * 0.55 + 1.4), (tipx - nx * w0 * 0.55 + 0.9, tipy - ny * w0 * 0.55 + 1.4)], 'bench_dark', 0.6)
    cv.poly([(cx - nx * w0, cy - ny * w0), (cx + nx * w0, cy + ny * w0), (tipx + nx * w0 * 0.55, tipy + ny * w0 * 0.55),
             (tipx - nx * w0 * 0.55, tipy - ny * w0 * 0.55)], (176, 176, 172))
    cv.line([(cx - nx * w0 * 0.3, cy - ny * w0 * 0.3), (tipx - nx * w0 * 0.2, tipy - ny * w0 * 0.2)], (240, 240, 235), 1.0, 0.9)
    cv.ellipse(tipx, tipy, w0 * 0.85, color=(205, 205, 200))
    cv.ellipse(tipx - w0 * 0.25, tipy - w0 * 0.25, w0 * 0.3, color=(255, 255, 255), alpha=0.8)
    # labels
    out = [cx - r, cy - r, cx + r, cy + r]
    ly = cy + r * 0.1
    spots = []
    if n == 2:
        spots = [('r', cx - r - 8, ly), ('l', cx + r + 8, ly)]
    elif n == 3:
        spots = [('r', cx - r - 8, ly), ('c', cx, cy - r - 12), ('l', cx + r + 8, ly)]
    else:
        spots = [('l', cx + r + 8, cy - r + 11 * i) for i in range(n)]
    for i, (al, px, py) in enumerate(spots[:n]):
        sel = (i == pos)
        s = labels[i]
        al2 = {'r': 'r', 'l': 'l', 'c': 'c'}[al]
        tw = text_width(s, 'label_b' if sel else 'label')
        w = tw + 12 + (10 if sel else 0)
        x = px - (w if al2 == 'r' else w / 2 if al2 == 'c' else 0)
        wr = (x, py - 8, x + w, py + 8)
        if sel:
            cv.fill(wr, 'blade', 3)
            well(cv, inset(wr, 1), 'well', 2, stamped=False)
            text(cv, (wr[0] + 6, py), '▸ ' + s, 'label_b', bg=inset(wr, 2))
        else:
            well(cv, wr, 'well', 2)
            text(cv, None, s, 'label', bg=wr)
        out[0], out[1], out[2], out[3] = min(out[0], wr[0]), min(out[1], wr[1]), max(out[2], wr[2]), max(out[3], wr[3])
    return tuple(out)


def rotary_selector(cv, cx, cy, size, options, index, path='blade', R_label=None, kind='amp'):
    """Pointer knob with stamped option labels around it (2-5 positions spread over -135..+135 deg, the knob sprite's own
    pointer shows the selection).  Option labels sit in wells; the selected one has a blade border + marker.  Returns the
    bounding rect."""
    n = len(options)
    angs = [(-135 + 270 * i / (n - 1)) if n > 1 else 0 for i in range(n)]
    spr = _knob_sprite(kind, index / max(1, n - 1), size)
    cv.shadow((cx - size * 0.42, cy - size * 0.4, cx + size * 0.42, cy + size * 0.4), 99, (size * 0.03, size * 0.07),
              size * 0.18, 0.7, 'ellipse')
    # stamped tick marks
    Rl = R_label if R_label is not None else size / 2.0 + 6
    for a in angs:
        ar = math.radians(a)
        cv.line([(cx + (size / 2.0 + 0.5) * math.sin(ar), cy - (size / 2.0 + 0.5) * math.cos(ar)),
                 (cx + (size / 2.0 + 5) * math.sin(ar), cy - (size / 2.0 + 5) * math.cos(ar))], 'bone_dim', 1.4)
    cv.sprite(spr, cx, cy)
    out = [cx - size / 2.0, cy - size / 2.0, cx + size / 2.0, cy + size / 2.0]
    for i, (s, a) in enumerate(zip(options, angs)):
        ar = math.radians(a)
        sx, sy = math.sin(ar), -math.cos(ar)
        sel = (i == index)
        st = 'label_b' if sel else 'label'
        tw = text_width(s, st)
        w = tw + 12 + (10 if sel else 0)
        px, py = cx + (size / 2.0 + 9) * sx, cy + (size / 2.0 + 9) * sy
        if abs(sx) > 0.3:
            x = px if sx > 0 else px - w
        else:
            x = px - w / 2.0
        y = py + (-9 if sy < -0.6 else 9 if sy > 0.6 else 0)
        wr = (x, y - 8, x + w, y + 8)
        if sel:
            cv.fill(wr, path, 3)
            well(cv, inset(wr, 1), 'well', 2, stamped=False)
            text(cv, (wr[0] + 6, y), '▸ ' + s, st, bg=inset(wr, 2))
        else:
            well(cv, wr, 'well', 2)
            text(cv, None, s, st, bg=wr)
        out = [min(out[0], wr[0]), min(out[1], wr[1]), max(out[2], wr[2]), max(out[3], wr[3])]
    return tuple(out)


# --------------------------------------------------------------------------------------------------------------------
# top bar (all screens)
# --------------------------------------------------------------------------------------------------------------------
@dataclass
class TopBarState:
    """State of the 58 px top bar (spec 02).  Defaults = the plain bar of screen 01."""
    preset: str = 'BARBARIC · MATCHED v2'
    ab: dict = None                  # None or {'a': 'GRAVE DIRT · MATCHED v2', 'b': 'THRASH TIGHT', 'active': 'a'}
    uncal: bool = False              # UNCAL chip
    out_of_true: bool = False        # OUT OF TRUE chip
    out_of_true_db: float = 4.5
    woodshed_open: bool = False      # WOODSHED button pressed
    match_pct: int = None            # MATCH running: LCD shows the percentage
    lat: int = 92
    cpu: int = 18
    mode: str = 'LIVE'               # 'LIVE' | 'STUDIO'
    rig_active: bool = True          # RIG button pressed (rig screen visible)


BAR_H = 58


def top_bar(cv, state=None, y=0, width=PAGE_W):
    """Draw the 58 px top bar (brushed-aluminium rail with rivets at both ends; every label in a flat well) at logical
    ``y``.  ``state`` = TopBarState (or a dict of its fields).  Returns a dict of element rects: brand, preset, ab_a, ab_b,
    rig, woodshed, gear, status, mode, match, forger, chip_uncal, chip_oot, rail."""
    st = state if isinstance(state, TopBarState) else TopBarState(**(state or {}))
    rail = (0, y, width, y + BAR_H)
    cv.shadow(rail, 0, (0, 4), 8, 0.7)
    cv.texture(rail, alu_tex(width, BAR_H, 'topbar'))
    cv.blend((0, y + BAR_H - 1, width, y + BAR_H), 'bench_dark', 0.9)
    for (rx, ry) in ((9, y + 9), (9, y + BAR_H - 10), (width - 9, y + 9), (width - 9, y + BAR_H - 10)):
        rivet(cv, rx, ry, 2.6)
    cy = y + BAR_H / 2.0
    out = {'rail': rail}
    x = 24
    # brand mark
    bw = text_width('SAWBLADE', 'brand', size=20) + 16
    out['brand'] = brand = (x, cy - 17, x + bw, cy + 17)
    well(cv, brand, 'well', 3)
    text(cv, None, 'SAWBLADE', 'brand', bg=brand, size=20)
    x = brand[2] + 12
    # preset selector / A-B slot names
    b0 = (x, cy - 14, x + 22, cy + 14)
    button(cv, b0, '', 'secondary')
    glyph_icon(cv, 'chev_l', (b0[0] + b0[2]) / 2.0, cy, 10, 'bone')
    x = b0[2] + 3
    pw = 204
    pr = (x, cy - 19, x + pw, cy + 19)
    if st.ab:
        a_act = st.ab.get('active', 'a') == 'a'
        well(cv, pr, 'well', 3)
        ra = (pr[0] + 3, pr[1] + 3, pr[2] - 3, cy - 0.5)
        rb = (pr[0] + 3, cy + 0.5, pr[2] - 3, pr[3] - 3)
        text(cv, (ra[0] + 4, rect_c(ra)[1]), 'A ▸ ' + st.ab['a'], 'body_strong' if a_act else 'body_dim', bg=ra, size=13)
        text(cv, (rb[0] + 4, rect_c(rb)[1]), 'B ▸ ' + st.ab['b'], 'body_strong' if not a_act else 'body_dim', bg=rb, size=13)
    else:
        well(cv, pr, 'well', 3)
        text(cv, (pr[0] + 10, cy), st.preset, 'body_strong', bg=pr)
    out['preset'] = pr
    x = pr[2] + 3
    b1 = (x, cy - 14, x + 22, cy + 14)
    button(cv, b1, '', 'secondary')
    glyph_icon(cv, 'chev_r', (b1[0] + b1[2]) / 2.0, cy, 10, 'bone')
    x = b1[2] + 8
    # A/B footswitch pair
    for i, (k, nm) in enumerate((('ab_a', 'A'), ('ab_b', 'B'))):
        active = st.ab is not None and st.ab.get('active', 'a') == ('a' if nm == 'A' else 'b')
        fx = x + 25 + i * 48
        if st.ab is None:
            r = footswitch(cv, fx, cy - 7, 30, False, None, None)
            lr = label_well(cv, (fx, cy + 18), nm, 'label_b', h=13, pad=6, align='c')
        else:
            footswitch(cv, fx, cy - 7, 30, active)
            lr = label_well(cv, (fx, cy + 18), nm + (' ACTIVE' if active else ''), 'label_b', h=13, pad=4, align='c')
        out[k] = (fx - 15, cy - 22, fx + 15, lr[3])
    x += 104
    # RIG / WOODSHED / gear buttons
    rw = text_width('RIG', 'button') + 18
    out['rig'] = r = (x, cy - 17, x + rw, cy + 17)
    button(cv, r, 'RIG', 'secondary', 'pressed' if st.rig_active and not st.woodshed_open else 'normal')
    x = r[2] + 5
    ww = text_width('WOODSHED', 'button') + 18
    out['woodshed'] = r = (x, cy - 17, x + ww, cy + 17)
    button(cv, r, 'WOODSHED', 'secondary', 'pressed' if st.woodshed_open else 'normal')
    x = r[2] + 5
    out['gear'] = r = (x, cy - 17, x + 30, cy + 17)
    button(cv, r, '', 'secondary')
    glyph_icon(cv, 'gear', (r[0] + r[2]) / 2.0, cy, 15, 'bone')
    left_end = r[2]
    # right side, laid out right to left
    x = width - 24
    fw = text_width('NAM FORGER', 'button_ink') + 20
    out['forger'] = r = (x - fw, cy - 17, x, cy + 17)
    button(cv, r, 'NAM FORGER', 'primary')
    x = r[0] - 6
    if st.match_pct is not None:
        mw = 118
        out['match'] = r = (x - mw, cy - 17, x, cy + 17)
        button(cv, r, '', 'secondary', 'pressed')
        text(cv, (r[0] + 12, cy + 1), 'MATCH', 'button', bg=(r[0] + 5, r[1] + 6, r[0] + 62, r[3] - 3))
        lcd(cv, (r[2] - 6, cy + 1), '%d' % st.match_pct, '%', digits=2, h=14, align='r')
    else:
        mw = text_width('MATCH', 'button') + 20
        out['match'] = r = (x - mw, cy - 17, x, cy + 17)
        button(cv, r, 'MATCH', 'secondary')
    x = r[0] - 8
    # LIVE / STUDIO chip
    if st.mode == 'LIVE':
        cr = chip(cv, (x, cy), 'LIVE', 'ok', glyph=None, h=24, align='r')
    else:
        cr = chip(cv, (x, cy), 'STUDIO', 'body', h=24, align='r')
    out['mode'] = cr
    x = cr[0] - 8
    # status LCD (LAT / CPU, two rows in one glass)
    sw = 104
    sr = (x - sw, cy - 21, x, cy + 21)
    cv.shadow(sr, 3, (0, 2), 4, 0.5)
    cv.fill(sr, 'steel_bare', 3)
    cv.fill(inset(sr, 1), 'bench_dark', 3)
    cv.fill(inset(sr, 2), 'glass', 2)
    r1 = (sr[0] + 4, sr[1] + 4, sr[2] - 4, cy)
    r2 = (sr[0] + 4, cy, sr[2] - 4, sr[3] - 4)
    text(cv, (r1[0] + 4, rect_c(r1)[1]), 'LAT %d smp' % st.lat, 'lcd_unit', bg=r1, size=14)
    text(cv, (r2[0] + 4, rect_c(r2)[1]), 'CPU %d %%' % st.cpu, 'lcd_unit', bg=r2, size=14)
    out['status'] = sr
    x = sr[0] - 10
    # chips: UNCAL / OUT OF TRUE (the room between left_end and x)
    both = st.uncal and st.out_of_true
    if st.out_of_true:
        s = 'OUT OF TRUE' if both else 'OUT OF TRUE · +%.1f dB' % st.out_of_true_db
        out['chip_oot'] = cr = chip(cv, (x, cy), s, 'alert', glyph='skew', h=24, align='r')
        x = cr[0] - 8
    if st.uncal:
        s = 'UNCAL' if both else 'UNCAL · interface not calibrated'
        out['chip_uncal'] = cr = chip(cv, (x, cy), s, 'warn', glyph='warn', h=24, align='r')
        x = cr[0] - 8
    assert x >= left_end + 4 or not (st.uncal or st.out_of_true), f'top bar chips collide with the left cluster (x={x:.0f}, left_end={left_end:.0f})'
    return out


# --------------------------------------------------------------------------------------------------------------------
# style sheet (00)
# --------------------------------------------------------------------------------------------------------------------
def _section(cv, rect, title):
    panel(cv, rect, 'ss:' + title, 1.0, 6)
    pl = plate(cv, (rect[0] + 8, rect[1] + 8, rect[2] - 8, rect[1] + 38), title)
    return (rect[0] + 12, rect[1] + 46, rect[2] - 12, rect[3] - 10)


_TEXT_ON = {   # token -> (background token) used for the swatch ratio column
    'bone': 'well', 'bone_dim': 'well', 'bone_mute': 'well', 'ink': 'alu_well', 'blade': 'well', 'blade_hi': 'glass',
    'body': 'well', 'lcd_amber': 'glass', 'ok': 'well', 'warn': 'well', 'alert': 'well',
}


def render_style_sheet():
    """The 1280 x 800 style sheet (00_style_sheet)."""
    cv = new_screen('00_style_sheet', 700)
    top = plate(cv, (12, 10, 1268, 50), 'SAWBLADE · WORKSHOP STYLE', right='1280 x 800 · 2x render · Pillow + numpy')
    # ---- column A: palette, materials, type ------------------------------------------------------------------
    ax0, ax1 = 12, 436
    pal = _section(cv, (ax0, 60, ax1, 372), 'PALETTE')
    names = list(PAL.keys())
    colw = (pal[2] - pal[0]) / 2.0
    for i, nme in enumerate(names):
        col, row = divmod(i, 12)
        x0 = pal[0] + col * colw
        y0 = pal[1] + row * 21.4
        sw = (x0, y0 + 1, x0 + 22, y0 + 18)
        cv.fill(sw, PAL[nme], 2)
        cv.outline(sw, 'bench_dark', 1, 2)
        lab = (x0 + 26, y0 + 1, x0 + colw - 4, y0 + 19)
        well(cv, lab, 'well', 2)
        s = '%s  %s' % (nme, _HEX[nme])
        text(cv, (lab[0] + 5, rect_c(lab)[1]), s, 'label', bg=lab, size=11, fg='bone')
        if nme in _TEXT_ON:
            rr = contrast(PAL[nme], PAL[_TEXT_ON[nme]])
            text(cv, (lab[2] - 4, rect_c(lab)[1]), '%.1f:1' % rr, 'label', bg=lab, size=11, fg=nme if _TEXT_ON[nme] != 'alu_well' else 'bone_dim', align='r')
    mat = _section(cv, (ax0, 382, ax1, 556), 'MATERIALS')
    mw = (mat[2] - mat[0] - 8 * 4) / 5.0
    mats = ['STEEL', 'ALU', 'WOOD', 'SAWDUST', 'PLATE']
    for i, nm in enumerate(mats):
        x0 = mat[0] + i * (mw + 8)
        sr = (x0, mat[1], x0 + mw, mat[1] + 84)
        if i == 0:
            cv.texture(sr, steel_tex(int(mw), 84, 'sheet_steel', 1.6), 4)
        elif i == 1:
            cv.texture(sr, alu_tex(int(mw), 84, 'sheet_alu'), 3)
        elif i == 2:
            cv.texture(sr, wood_tex(int(mw), 84, 'sheet_wood'), 3)
        elif i == 3:
            cv.texture(sr, wood_tex(int(mw), 84, 'sheet_wood2'), 3)
            sawdust(cv, sr, 40, 'sheet_dust', clumps=[(sr[0] + mw * 0.5, sr[1] + 72, 30, 420)], seams=[sr[1] + 1], seam_n=40)
        else:
            cv.texture(sr, steel_tex(int(mw), 84, 'sheet_plate', 0.5), 4)
            plate(cv, (sr[0] + 4, sr[1] + 16, sr[2] - 4, sr[1] + 64), 'PLATE', key='sheet_pl')
        lw = (x0, mat[1] + 90, x0 + mw, mat[1] + 106)
        well(cv, lw, 'well', 2)
        text(cv, None, nm, 'label', bg=lw, size=10) if False else text(cv, None, nm, 'label', bg=lw)
    note = (mat[0], mat[1] + 112, mat[2], mat[1] + 130)
    well(cv, note, 'well', 2)
    text(cv, (note[0] + 6, rect_c(note)[1]), 'Texture is never under text: text sits on flat wells.', 'body_dim', bg=note)
    ty = _section(cv, (ax0, 566, ax1, 790), 'TYPE')
    spec = [('brand', 'SAWBLADE', 'Black Ops One 22 px  brand / titles'),
            ('section', 'Section stencil', 'Allerta Stencil 13 px, tracking .08'),
            ('label_b', 'Label caps bold', 'Barlow Condensed 700 11 px, .12'),
            ('label', 'Label caps', 'Barlow Condensed 600 11 px, .12'),
            ('body_strong', 'Body strong 14', 'Barlow 600 (500 for body 13)'),
            ('body', 'Body text sentence case', 'Barlow 500 13 px'),
            ('mono', 'Readout 0123 -33.1 dB', 'Share Tech Mono 14 px')]
    for i, (stn, sample, desc) in enumerate(spec):
        y = ty[1] + i * 24 + 10
        r1 = (ty[0], y - 10, ty[0] + 190, y + 10)
        well(cv, r1, 'alu_well' if stn in ('section',) else 'well', 2)
        text(cv, (r1[0] + 6, y), sample, stn, bg=r1, fg='ink' if stn == 'section' else None)
        r2 = (r1[2] + 6, y - 10, ty[2], y + 10)
        well(cv, r2, 'well', 2)
        text(cv, (r2[0] + 6, y), desc, 'label', bg=r2)
    # ---- column B: controls ----------------------------------------------------------------------------------
    bx0, bx1 = 446, 858
    kit = _section(cv, (bx0, 60, bx1, 438), 'KNOBS \u00b7 FOOTSWITCH \u00b7 LEDS')
    for i, (v, rd, kd, pc) in enumerate(((0.0, ('-90.0', 'dB', 4), 'amp', 'blade'), (0.5, ('0.0', 'dB', 4), 'amp', 'body'),
                                         (1.0, ('+12.0', 'dB', 4), 'amp', 'blade'))):
        knob(cv, kit[0] + 56 + i * 124, kit[1] + 40, 66, kd, v, pc, 'GAIN %d' % int(v * 100), rd)
    for i, v in enumerate((0.0, 0.5, 1.0)):
        knob(cv, kit[0] + 34 + i * 70, kit[1] + 178, 46, 'pedal', v, 'blade' if i != 1 else 'body', 'TONE')
    footswitch(cv, kit[0] + 262, kit[1] + 170, 52, False, 'A', 'UP')
    footswitch(cv, kit[0] + 330, kit[1] + 170, 52, True, 'B', 'DOWN')
    for i, (on, hue) in enumerate(((False, 'orange'), (True, 'orange'), (True, 'ok'), (True, 'alert'))):
        led(cv, kit[0] + 16 + i * 92, kit[1] + 240, on, 22, hue)
    toggle(cv, kit[0] + 84, kit[1] + 294, 0, ['OFF', 'ON'])
    toggle(cv, kit[0] + 250, kit[1] + 296, 1, ['LIVE', 'MID', 'STUDIO'], key='tg3')
    btn = _section(cv, (bx0, 448, bx1, 790), 'BUTTONS · FIELDS')
    by = btn[1] + 2
    button(cv, (btn[0], by, btn[0] + 110, by + 30), 'PRIMARY', 'primary')
    button(cv, (btn[0] + 120, by, btn[0] + 230, by + 30), 'SECONDARY', 'secondary')
    button(cv, (btn[0] + 240, by, btn[0] + 340, by + 30), 'DANGER', 'danger')
    by += 40
    button(cv, (btn[0], by, btn[0] + 110, by + 30), 'PRIMARY', 'primary', 'pressed')
    button(cv, (btn[0] + 120, by, btn[0] + 230, by + 30), 'SECONDARY', 'secondary', 'pressed')
    button(cv, (btn[0] + 240, by, btn[0] + 340, by + 30), 'DANGER', 'danger', 'pressed')
    by += 40
    button(cv, (btn[0], by, btn[0] + 110, by + 30), 'MATCH', 'secondary', 'disabled', caption='needs a DI take')
    by += 36
    button(cv, (btn[0], by, btn[0] + 110, by + 30), 'START', 'primary', 'disabled', caption='no song loaded')
    by += 44
    text_field(cv, (btn[0], by, btn[0] + 150, by + 28), '+12.5', 'mono', caret=True)
    dropdown(cv, (btn[0] + 160, by, btn[0] + 340, by + 28), '4i4 3rd gen \u2014 INST', 'body')
    by += 36
    text_field(cv, (btn[0], by, btn[0] + 150, by + 28), 'search presets', 'body', placeholder=True)
    checkbox(cv, btn[0] + 166, by + 7, True, 'AUTO-REFINE')
    checkbox(cv, btn[0] + 166, by + 25, False, 'KEEP KEYS')
    by += 38
    tab(cv, (btn[0], by, btn[0] + 70, by + 26), 'CHAIN', True)
    tab(cv, (btn[0] + 76, by, btn[0] + 130, by + 26), 'EQ', False)
    tab(cv, (btn[0] + 136, by, btn[0] + 190, by + 26), 'CAB', False)
    slider(cv, (btn[0] + 205, by, btn[0] + 340, by + 26), 0.45, [(0.2, 'A'), (0.7, 'B')])
    by += 38
    progress(cv, (btn[0], by, btn[0] + 230, by + 14), 0.41)
    label_well(cv, (btn[0] + 240, by + 7), 'epoch 41 / 100', 'label')
    # ---- column C: readouts, badges, frames ------------------------------------------------------------------
    cx0, cx1 = 868, 1268
    rd = _section(cv, (cx0, 60, cx1, 292), 'READOUTS · BADGES')
    y = rd[1] + 16
    lcd(cv, (rd[0], y), '-33.1', 'dB', 5, 26)
    lcd(cv, (rd[0] + 150, y), '2.0', ':1', 4, 26)
    lcd(cv, (rd[0] + 270, y), '+120', 'ms', 4, 26)
    y += 40
    lcd(cv, (rd[0], y), '01:23.4', '/ 04:12.0', 6, 22)
    nixie(cv, (rd[0] + 200, y - 18, rd[0] + 376, y + 22), '79 / 21', 34)
    y += 40
    bx = rd[0]
    for kind in ('UNCAL', 'NON-COMMERCIAL', 'LEGACY LEVELS'):
        b = badge(cv, (bx, y + 8), kind)
        bx = b[2] + 8
    y += 26
    bx = rd[0]
    for kind in ('PREVIEW', 'REFINED', 'BETA', 'OUT OF TRUE'):
        b = badge(cv, (bx, y + 8), kind)
        bx = b[2] + 8
    y += 26
    bx = rd[0]
    for (s, k, g) in (('LIVE', 'ok', None), ('STUDIO', 'body', None), ('UNCAL', 'warn', 'warn')):
        b = chip(cv, (bx, y + 8), s, k, g)
        bx = b[2] + 8
    fr = _section(cv, (cx0, 302, cx1, 790), 'FRAMES')
    y = fr[1]
    plate(cv, (fr[0], y, fr[2], y + 34), 'RIVETED PLATE · HEADER', right='alu')
    y += 46
    nameplate(cv, (fr[0], y, fr[0] + 150, y + 28), 'THE SAW MILL')
    nameplate(cv, (fr[0] + 160, y, fr[0] + 270, y + 28), 'TS-STYLE')
    y += 42
    c1 = card(cv, (fr[0], y, fr[0] + 186, y + 112), 'SAW HEAD', 'blade', status='ON')
    well(cv, (c1['body'][0], c1['body'][1], c1['body'][2], c1['body'][1] + 20), 'well')
    text(cv, None, '@marrow_amps · cc-by', 'label', bg=(c1['body'][0], c1['body'][1], c1['body'][2], c1['body'][1] + 20))
    c2 = card(cv, (fr[0] + 198, y, fr[2], y + 112), 'TS-STYLE', 'body', bypassed=True)
    well(cv, (c2['body'][0], c2['body'][1], c2['body'][2], c2['body'][1] + 20), 'well')
    text(cv, None, 'BYPASSED · hatch', 'label', bg=(c2['body'][0], c2['body'][1], c2['body'][2], c2['body'][1] + 20))
    y += 126
    pr = (fr[0], y, fr[2], fr[3] - 2)
    panel(cv, pr, 'sheet_panel', 1.5, 6, rivets=True)
    rotary_selector(cv, pr[0] + 84, pr[1] + 70, 54, ['AUTO', 'L', 'R', 'MIX'], 0, 'blade')
    rotary_selector(cv, pr[0] + 290, pr[1] + 70, 54, ['MUTE', 'GHOST', 'FULL'], 1, 'body')
    toggle(cv, pr[0] + 100, pr[1] + 170, 1, ['OFF', 'ON'], 40, key='tg4')
    label_well(cv, (pr[0] + 168, pr[1] + 170), 'CALIBRATED LEVELS', 'label')
    return cv


# --------------------------------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------------------------------
def save_png(img, path):
    img.save(path, 'PNG', optimize=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(REPO, 'design', 'mockups', 'workshop', 'png'),
                    help='output directory for 00_style_sheet.png')
    ap.add_argument('--font-dir', default=DEFAULT_FONT_DIR, help='font cache directory (fetched + SHA-256 verified)')
    a = ap.parse_args(argv)
    try:
        ensure_fonts(a.font_dir)
    except FontError as e:
        print(f'workshop_style.py: SKIP, {e}', file=sys.stderr)
        return 77
    os.makedirs(a.out, exist_ok=True)
    cv = render_style_sheet()
    save_png(cv.finish(), os.path.join(a.out, '00_style_sheet.png'))
    fails = [e for e in cv.log if e['ratio'] < required_ratio(e['px'], e['bold']) - 1e-9]
    for e in fails:
        print('CONTRAST FAIL', e['text'], e['style'], round(e['ratio'], 2))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
