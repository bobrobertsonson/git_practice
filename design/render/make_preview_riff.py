#!/usr/bin/env python3
"""Generates plugin/assets/preview_riff.wav: the built-in DI riff of the capture browser's PREVIEW.

6.0 s, 48 kHz, mono, 24-bit PCM, peak -12 dBFS. Synthesised from scratch (Karplus-Strong plucked
low-string model: palm-muted chugs on the open low E plus a few open power chords), so no
third-party audio is involved. Deterministic: seeded PRNG, pure Python, no numpy; two runs give
identical bytes.

    python3 design/render/make_preview_riff.py [--out plugin/assets/preview_riff.wav]
"""
import argparse
import math
import random
import wave
from pathlib import Path

RATE = 48000
SECONDS = 6.0
PEAK_DBFS = -12.0
SEED = 8128
BPM = 120.0

E2, B2, E3, G2, A2, D3 = 82.41, 123.47, 164.81, 98.0, 110.0, 146.83


def pluck(freq, seconds, rng, decay, bright, mute_ms=0.0):
    """One Karplus-Strong string. `decay` is the per-pass loop gain; `mute_ms` > 0 chokes the note."""
    n = max(2, round(RATE / freq))
    buf = [(rng.random() * 2.0 - 1.0) for _ in range(n)]
    # Pick position / brightness: one-pole low-pass of the excitation (smaller bright = darker).
    prev = 0.0
    for i in range(n):
        prev = prev + bright * (buf[i] - prev)
        buf[i] = prev
    mean = sum(buf) / n
    buf = [v - mean for v in buf]
    total = int(seconds * RATE)
    out = [0.0] * total
    idx = 0
    for t in range(total):
        nxt = (idx + 1) % n
        y = buf[idx]
        out[t] = y
        buf[idx] = decay * 0.5 * (y + buf[nxt])
        idx = nxt
    if mute_ms > 0.0:
        m = max(1, int(mute_ms * RATE / 1000.0))
        start = max(0, total - m)
        for i in range(start, total):
            out[i] *= 1.0 - (i - start) / m
    return out


def add(dst, src, at, gain):
    for i, v in enumerate(src):
        j = at + i
        if j >= len(dst):
            break
        dst[j] += gain * v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--out", type=Path, default=root / "plugin" / "assets" / "preview_riff.wav")
    args = ap.parse_args()

    rng = random.Random(SEED)
    total = int(SECONDS * RATE)
    mix = [0.0] * total
    eighth = 60.0 / BPM / 2.0  # 0.25 s

    # 24 eighth-note slots. "c" = palm-muted chug on E, "P" = open power chord (ring), "." = rest,
    # letters in lower case are a different root (g, a) used for the fills.
    pattern = "cc.cccc.cccP.cc.cc.cc.Pcg"
    pattern = pattern[:24]
    roots = {"c": [E2], "P": [E2, B2, E3], "g": [G2], "a": [A2]}
    for slot, ch in enumerate(pattern):
        if ch == ".":
            continue
        at = int(round(slot * eighth * RATE))
        vel = 0.85 + 0.15 * rng.random()
        if ch == "P":
            dur = eighth * 3.2 if slot + 1 < len(pattern) and pattern[slot + 1] == "." else eighth * 1.8
            for f in roots[ch]:
                add(mix, pluck(f * (1.0 + 0.0008 * (rng.random() - 0.5)), dur, rng, 0.9985, 0.55, mute_ms=30.0), at, vel * 0.55)
        else:
            dur = eighth * 0.55  # palm mute: short, damped
            for f in roots[ch]:
                add(mix, pluck(f, dur, rng, 0.994, 0.35, mute_ms=18.0), at, vel)

    # Last 40 ms to silence, so the loop end is clean.
    tail = int(0.04 * RATE)
    for i in range(tail):
        mix[total - tail + i] *= 1.0 - i / tail

    peak = max(abs(v) for v in mix)
    scale = (10.0 ** (PEAK_DBFS / 20.0)) / peak
    frames = bytearray()
    for v in mix:
        q = int(round(v * scale * 8388607.0))
        q = max(-8388608, min(8388607, q))
        frames += (q & 0xFFFFFF).to_bytes(3, "little")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(3)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {args.out} ({total} samples, peak {PEAK_DBFS} dBFS)")


if __name__ == "__main__":
    main()
