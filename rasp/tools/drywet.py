#!/usr/bin/env python3
"""What did a plugin chain do to a clean DI? Measure processed files against it.

For each processed file (rendered from the same DI, same start point):
  - offset: latency of the processed file vs the DI (ms)
  - level: integrated loudness change (LU) and peak
  - EQ: linear transfer DI -> processed per band, relative to 1 kHz (Welch
    cross-spectrum; distortion and compression show up as low coherence)
  - coherence: how much of the output is a linear function of the DI (%)
  - dynamics: spread of the 100 ms level while singing, in vs out (dB), and
    the in/out slope (1.0 = untouched, 0.25 = ~4:1 overall)
  - width: side/mid energy (dB; -inf = mono) and L/R correlation
  - space: energy after the voice stops (tail level, dB re the voice)

It does not try to name harmony intervals: on distorted screams, matching
shifted spectra found "+8 semitones" even in a render with no harmony at all.
Name the files by their settings instead (e.g. "Mantra - pair1 -12 lvl -10.wav").

  python3 tools/drywet.py --dry clean_di.wav processed/*.wav > results/drywet/session.txt

Needs numpy and ffmpeg.
"""
import argparse
import pathlib
import subprocess
import sys

import numpy as np

SR = 48000
BANDS = [63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000]


def decode(path, ch):
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-ac", str(ch), "-ar", str(SR), "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, ch).astype(np.float64)


def lag_of(ref, y, max_s=2.0):
    n = 1 << int(np.ceil(np.log2(len(ref) + len(y))))
    c = np.fft.irfft(np.fft.rfft(y, n) * np.conj(np.fft.rfft(ref, n)), n)
    m = int(max_s * SR)
    cand = np.concatenate([c[:m], c[-m:]])
    i = int(np.argmax(np.abs(cand)))
    return i if i < m else i - 2 * m


def shift(y, lag, n):
    out = np.zeros((n, y.shape[1]))
    if lag >= 0:
        seg = y[lag:lag + n]
        out[:len(seg)] = seg
    else:
        seg = y[:n + lag]
        out[-lag:-lag + len(seg)] = seg
    return out


def lufs(x):
    cmd = ["ffmpeg", "-v", "info", "-f", "f32le", "-ar", str(SR), "-ac", str(x.shape[1]), "-i", "-", "-af", "ebur128", "-f", "null", "-"]
    out = subprocess.run(cmd, input=x.astype(np.float32).tobytes(), capture_output=True).stderr.decode()
    vals = [l for l in out.splitlines() if l.strip().startswith("I:")]
    return float(vals[-1].split()[1]) if vals else float("nan")


def blocks_db(x, n=4800):
    b = x[: len(x) // n * n].reshape(-1, n)
    return 10 * np.log10((b ** 2).mean(axis=1) + 1e-12)


def transfer(x, y, nfft=8192):
    hop, w = nfft // 2, np.hanning(nfft)
    Sxx = 0; Sxy = 0; Syy = 0
    for s in range(0, len(x) - nfft, hop):
        if not np.any(x[s:s + nfft]):
            continue
        X = np.fft.rfft(x[s:s + nfft] * w); Y = np.fft.rfft(y[s:s + nfft] * w)
        Sxx = Sxx + np.abs(X) ** 2; Sxy = Sxy + np.conj(X) * Y; Syy = Syy + np.abs(Y) ** 2
    f = np.fft.rfftfreq(nfft, 1 / SR)
    H = Sxy / (Sxx + 1e-20)
    coh = np.abs(Sxy) ** 2 / (Sxx * Syy + 1e-30)
    return f, H, coh


def band(v, f, fc):
    sel = (f >= fc / 2 ** (1 / 3)) & (f < fc * 2 ** (1 / 3))
    return np.mean(v[sel])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry", required=True, help="the clean DI")
    ap.add_argument("files", nargs="+", help="processed versions of the same DI")
    args = ap.parse_args()

    dry = decode(args.dry, 1)[:, 0]
    act = blocks_db(dry) > np.percentile(blocks_db(dry), 95) - 20
    d_l = lufs(dry[:, None])
    print(f"dry: {pathlib.Path(args.dry).name}   {len(dry) / SR:.1f} s   {d_l:.1f} LUFS   singing {act.mean() * 100:.0f}% of the time")
    print()

    for path in args.files:
        if pathlib.Path(path).resolve() == pathlib.Path(args.dry).resolve():
            continue
        wet = decode(path, 2)
        lag = lag_of(dry, wet.mean(axis=1))
        n = len(dry)
        y = shift(wet, lag, n)
        mono = y.mean(axis=1)
        name = pathlib.Path(path).name
        print(f"== {name}")
        w_l = lufs(y)
        print(f"   offset {lag / SR * 1000:+.1f} ms   loudness {w_l - d_l:+.1f} LU   peak {20 * np.log10(np.abs(y).max() + 1e-12):.1f} dBFS")

        f, H, coh = transfer(dry, mono)
        ref = band(np.abs(H) ** 2, f, 1000)
        print("   EQ re 1 kHz " + " ".join(f"{b}:{10 * np.log10(band(np.abs(H) ** 2, f, b) / ref):+.1f}" for b in BANDS))
        print("   coherence % " + " ".join(f"{b}:{100 * band(coh, f, b):.0f}" for b in BANDS))

        din, dout = blocks_db(dry), blocks_db(mono)
        m = min(len(din), len(dout))
        a = act[:m]
        spread_in = np.percentile(din[:m][a], 90) - np.percentile(din[:m][a], 10)
        spread_out = np.percentile(dout[:m][a], 90) - np.percentile(dout[:m][a], 10)
        slope = np.polyfit(din[:m][a], dout[:m][a], 1)[0] if a.sum() > 10 else float("nan")
        print(f"   dynamics: level spread {spread_in:.1f} -> {spread_out:.1f} dB, in/out slope {slope:.2f}")

        L, R = y[:, 0], y[:, 1]
        mid, side = (L + R) / 2, (L - R) / 2
        sm = 10 * np.log10((side ** 2).sum() / ((mid ** 2).sum() + 1e-20) + 1e-20)
        corr = np.corrcoef(L, R)[0, 1] if np.std(R) > 0 else 1.0
        print(f"   width: side/mid {sm:.1f} dB, L/R correlation {corr:.2f}")

        quiet = ~act[:m]
        tail = 10 * np.log10(np.mean(10 ** (dout[:m][quiet] / 10)) / np.mean(10 ** (dout[:m][a] / 10)) + 1e-20) if quiet.any() else float("nan")
        print(f"   space: level in the gaps {tail:.1f} dB re singing")

        print()


if __name__ == "__main__":
    main()
