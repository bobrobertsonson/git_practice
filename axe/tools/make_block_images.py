#!/usr/bin/env python3
"""Draws the Anagram block images (200x200 PNG, on/off): an elevator door
frame with an up-arrow. Our own artwork; pure Python, no dependencies.
    python3 tools/make_block_images.py plugin/images"""
import struct, sys, zlib, pathlib

W = H = 200

def png(path, px):
    raw = b"".join(b"\x00" + bytes(c for p in px[y * W:(y + 1) * W] for c in p) for y in range(H))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    pathlib.Path(path).write_bytes(data)

def draw(on):
    bg, plate, steel = (20, 22, 25, 255), (34, 37, 42, 255), (70, 76, 86, 255)
    accent = (255, 176, 32, 255) if on else (110, 84, 40, 255)
    px = [bg] * (W * H)
    def rect(x0, y0, x1, y1, c):
        for y in range(max(0, y0), min(H, y1)):
            for x in range(max(0, x0), min(W, x1)):
                px[y * W + x] = c
    rect(10, 10, 190, 190, plate)
    # door frame and the two doors
    rect(40, 30, 160, 170, steel)
    rect(46, 36, 99, 170, bg)
    rect(101, 36, 154, 170, bg)
    # up arrow (the "elevator"), with an axe-head notch on its shaft
    for y in range(52, 100):
        half = (y - 52) * 26 // 48
        rect(100 - half, y, 100 + half + 1, y + 1, accent)
    rect(91, 100, 110, 150, accent)
    rect(110, 104, 122, 124, accent)       # the axe blade
    rect(10, 182, 190, 190, accent)        # floor light
    return px

out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "plugin/images")
out.mkdir(parents=True, exist_ok=True)
png(out / "block-on.png", draw(True))
png(out / "block-off.png", draw(False))
print("wrote", out / "block-on.png", out / "block-off.png")
