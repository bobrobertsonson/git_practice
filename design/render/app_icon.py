#!/usr/bin/env python3
"""Sawblade app icon: the 16-tooth saw blade motif of pedal_b2.py (concentric rings, stencil bridges) in saw
orange with bone ring highlights on the near-black ground, inside the macOS rounded square (corner radius 22.4 %
of the square's side, 10 % transparent margin on every side).

Pillow + numpy only; no Blender, no fonts, no randomness: the output is deterministic.

    python3 design/render/app_icon.py                 # render plugin/assets/icon/icon_{16..1024}.png
    python3 design/render/app_icon.py --out DIR       # render somewhere else
    python3 design/render/app_icon.py --check         # re-render to a temp dir and compare with the committed
                                                      # files (exit 0 same, 1 different, 77 Pillow/numpy missing)

--check compares the decoded RGBA pixels, not the PNG bytes, so a different zlib or Pillow build that encodes
the same image does not fail it.
"""
import argparse
import math
import os
import sys
import tempfile

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError as e:  # pragma: no cover - depends on the machine
    print(f"app_icon.py: SKIP, needs Pillow and numpy ({e})")
    sys.exit(77)

SIZES = (16, 32, 64, 128, 256, 512, 1024)
SS = 4                       # supersampling factor
MARGIN = 0.10                # transparent margin around the square, fraction of the canvas
RADIUS = 0.224               # corner radius, fraction of the square's side
ORANGE = (0xff, 0x6a, 0x1a)
BONE = (0xe8, 0xe1, 0xd2)
GROUND = (0x0b, 0x0a, 0x09)
GROUND_TOP = (0x1a, 0x16, 0x12)
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'plugin', 'assets', 'icon')


def blade_masks(n, cx, cy, k):
    """(orange, bone) 'L' masks at n x n. Units are pedal_b2's motif units (outer tooth radius 91), scaled by k px."""
    def pol(a, r):
        return (cx + r * k * math.cos(math.radians(a)), cy - r * k * math.sin(math.radians(a)))

    def ring(d, ro, ri, fill=255):
        d.ellipse((cx - ro * k, cy - ro * k, cx + ro * k, cy + ro * k), fill=fill)
        d.ellipse((cx - ri * k, cy - ri * k, cx + ri * k, cy + ri * k), fill=0)

    def disc(d, r, fill):
        d.ellipse((cx - r * k, cy - r * k, cx + r * k, cy + r * k), fill=fill)

    orange = Image.new('L', (n, n), 0)
    bone = Image.new('L', (n, n), 0)
    d = ImageDraw.Draw(orange)
    pts = []
    for t in range(16):
        a0 = t * 360 / 16
        pts += [pol(a0, 78), pol(a0 + 2, 91)]
        pts += [pol(a0 + 2 + 18 * u, 91 - 13 * u ** 0.6) for u in np.linspace(0.1, 1.0, 7)]
    d.polygon(pts, fill=255)
    disc(d, 70, 0)                       # hollow the blade: toothed band only
    ring(d, 60, 53)                      # concentric rings
    ring(d, 25, 21)
    b = ImageDraw.Draw(bone)
    ring(b, 74, 72.2)                    # thin bone line inside the toothed band (first: ring() hollows its inside)
    ring(b, 41, 37)                      # bone highlights
    ring(b, 9, 6)
    disc(b, 2.5, 255)
    # stencil bridges: cuts through the rings (fixed angles), so the rings read as stencilled
    for (ra, rb, angs) in ((50, 64, (35, 125, 215, 305)), (34, 45, (80, 170, 260, 350)), (18, 29, (20, 110, 200, 290))):
        for a in angs:
            for img in (orange, bone):
                ImageDraw.Draw(img).line((*pol(a, ra), *pol(a, rb)), fill=0, width=max(1, int(round(1.4 * k))))
    for i in range(6):
        a = i * 60 + 17
        for img in (orange, bone):
            ImageDraw.Draw(img).line((*pol(a, 71), *pol(a, 93)), fill=0, width=max(1, int(round(1.1 * k))))
    return orange, bone


def render(size):
    n = size * SS
    side = n * (1.0 - 2 * MARGIN)
    x0 = n * MARGIN
    cx = cy = n / 2.0
    # rounded square
    tile = Image.new('L', (n, n), 0)
    ImageDraw.Draw(tile).rounded_rectangle((x0, x0, x0 + side - 1, x0 + side - 1), radius=RADIUS * side, fill=255)
    # ground: vertical gradient inside the square
    yy = np.linspace(0.0, 1.0, n, dtype=np.float32)[:, None]
    t = np.clip((yy - MARGIN) / (1.0 - 2 * MARGIN), 0.0, 1.0)
    ground = np.empty((n, n, 3), np.float32)
    for c in range(3):
        ground[..., c] = (GROUND_TOP[c] * (1.0 - t) + GROUND[c] * t) * np.ones((1, n), np.float32)
    # warm glow behind the blade
    xs = (np.arange(n, dtype=np.float32) - cx) / (side * 0.5)
    r2 = xs[None, :] ** 2 + xs[:, None] ** 2
    glow = np.exp(-r2 * 2.2)[..., None] * 0.30
    rgb = ground * (1.0 - glow) + np.array(ORANGE, np.float32) * glow * 0.55
    # blade
    k = side * 0.5 * 0.86 / 91.0
    orange, bone = blade_masks(n, cx, cy, k)
    mo = np.asarray(orange, np.float32)[..., None] / 255.0
    mb = np.asarray(bone, np.float32)[..., None] / 255.0
    rgb = rgb * (1.0 - mo) + np.array(ORANGE, np.float32) * mo
    rgb = rgb * (1.0 - mb) + np.array(BONE, np.float32) * mb
    alpha = np.asarray(tile, np.float32)
    rgba = np.concatenate([np.clip(rgb, 0, 255), alpha[..., None]], axis=2).round().astype(np.uint8)
    img = Image.fromarray(rgba, 'RGBA')
    # premultiplied downsample: no dark fringe on the transparent corners
    return img.convert('RGBa').resize((size, size), Image.LANCZOS).convert('RGBA')


def render_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for s in SIZES:
        render(s).save(os.path.join(out_dir, f'icon_{s}.png'), 'PNG', optimize=True)


def check():
    committed = os.path.normpath(DEFAULT_OUT)
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        render_all(tmp)
        for s in SIZES:
            name = f'icon_{s}.png'
            path = os.path.join(committed, name)
            if not os.path.exists(path):
                print(f'MISSING {path}')
                bad += 1
                continue
            a = np.asarray(Image.open(path).convert('RGBA'))
            b = np.asarray(Image.open(os.path.join(tmp, name)).convert('RGBA'))
            if a.shape != b.shape or not np.array_equal(a, b):
                print(f'DIFFERS {name}')
                bad += 1
    print('app icon: ' + ('all 7 PNGs match a fresh render' if bad == 0 else f'{bad} differ'))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=DEFAULT_OUT, help='output directory (default plugin/assets/icon)')
    ap.add_argument('--check', action='store_true', help='compare a fresh render with the committed PNGs')
    a = ap.parse_args()
    if a.check:
        return check()
    render_all(a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
