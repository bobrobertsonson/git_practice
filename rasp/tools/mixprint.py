#!/usr/bin/env python3
"""Learn a producer's vocal treatment from raw vocal stems plus the final mix.

The band in the mix is uncorrelated with the raw vocals, so averaging the
cross-spectra over the whole song isolates the part of the mix that is a
(linear) function of each vocal stem. For every stem that gives:
  - its level and left/right placement in the mix,
  - the EQ curve applied to it,
  - the room/reverb/delay it was put in (from the long impulse response),
  - coherence: how much of the mix, per band, the vocals explain.

Compression, distortion and MP3 encoding are nonlinear, so they show up as
lower coherence rather than in the curves. EQ, level and pan come out
reliably. The space figures are rough: where coherence is low, estimation
noise forms a floor under the impulse response that reads as extra reverb.
In a synthetic check a planted -10 dB echo read as -3.4 dB wet/dry, so treat
wet/dry, RT60 and echo levels as upper bounds.

  python3 tools/mixprint.py --mix final.mp3 stems/*.wav
  python3 tools/mixprint.py --mix final.mp3 stems/*.wav --compare rasp_render.wav --compare-stem stems/main.wav

--compare runs the same measurement on a RASP render of one stem and prints the
EQ difference between the producer's treatment of that stem and RASP's.

Needs numpy and ffmpeg.
"""
import argparse
import pathlib
import subprocess
import sys

import numpy as np

SR = 44100
# Below this coherence the band, not the vocal, sets the cross-spectrum and the
# EQ estimate is noise (on KnockedLoose: +30 dB "boosts" at 63 Hz at 6%).
MIN_COH = 0.12
BANDS = [63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000]


def blabel(b):
    return f"{b}" if b < 1000 else (f"{b // 1000}k" if b % 1000 == 0 else f"{b / 1000:.1f}k")


def decode(path, channels):
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-ac", str(channels), "-ar", str(SR), "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, channels).astype(np.float64)


def find_lag(ref, y, max_s=10.0):
    """Samples to shift y by so it lines up with ref (via FFT cross-correlation)."""
    n = 1 << int(np.ceil(np.log2(len(ref) + len(y))))
    c = np.fft.irfft(np.fft.rfft(y, n) * np.conj(np.fft.rfft(ref, n)), n)
    m = int(max_s * SR)
    cand = np.concatenate([c[:m], c[-m:]])
    i = int(np.argmax(np.abs(cand)))
    return i if i < m else i - 2 * m


def place_in_mix(stem, mix_mono):
    """Find where a stem sits in the mix, searching the whole song, and return
    it on the mix's timeline. Exports often start at different bars (a verse
    stem, a full-length double), so each stem is aligned on its own.
    Returns (stem on mix timeline, offset in samples, peak/noise confidence)."""
    n = 1 << int(np.ceil(np.log2(len(stem) + len(mix_mono))))
    c = np.fft.irfft(np.fft.rfft(mix_mono, n) * np.conj(np.fft.rfft(stem, n)), n)
    valid = np.concatenate([c[:len(mix_mono)], c[n - len(stem):]])  # lags 0..mix, then negative
    i = int(np.argmax(np.abs(valid)))
    lag = i if i < len(mix_mono) else i - len(valid)
    conf = float(np.abs(valid[i]) / (np.percentile(np.abs(valid), 99.9) + 1e-12))
    out = np.zeros(len(mix_mono))
    if lag >= 0:
        seg = stem[:len(mix_mono) - lag]
        out[lag:lag + len(seg)] = seg
    else:
        seg = stem[-lag:-lag + len(mix_mono)]
        out[:len(seg)] = seg
    return out, lag, conf


def align(y, lag, length):
    out = np.zeros((length, y.shape[1]))
    if lag >= 0:
        seg = y[lag:lag + length]
        out[:len(seg)] = seg
    else:
        seg = y[:length + lag]
        out[-lag:-lag + len(seg)] = seg
    return out


def transfer(X, Y, nfft):
    """Multi-input Welch estimate. X: (n, k) stems, Y: (n, c) outputs.
    Returns H (bins, k, c) and per-output multiple coherence (bins, c)."""
    k, c = X.shape[1], Y.shape[1]
    hop = nfft // 2
    win = np.hanning(nfft)
    bins = nfft // 2 + 1
    Sxx = np.zeros((bins, k, k), complex)
    Sxy = np.zeros((bins, k, c), complex)
    Syy = np.zeros((bins, c))
    for s in range(0, len(X) - nfft, hop):
        if not np.any(X[s:s + nfft]):
            continue
        Fx = np.fft.rfft(X[s:s + nfft] * win[:, None], axis=0)      # (bins, k)
        Fy = np.fft.rfft(Y[s:s + nfft] * win[:, None], axis=0)      # (bins, c)
        Sxx += np.conj(Fx)[:, :, None] * Fx[:, None, :]
        Sxy += np.conj(Fx)[:, :, None] * Fy[:, None, :]
        Syy += np.abs(Fy) ** 2
    reg = 1e-6 * np.trace(Sxx, axis1=1, axis2=2).real[:, None, None] / k * np.eye(k)[None]
    H = np.linalg.solve(Sxx + reg, Sxy)
    # Multiple coherence: share of output power explained by all inputs together.
    explained = np.einsum("bkc,bkj,bjc->bc", np.conj(H), Sxx, H).real
    coh = np.clip(explained / (Syy + 1e-30), 0, 1)
    return H, coh


def band_db(Hmag2, f, fc):
    sel = (f >= fc / 2 ** (1 / 3)) & (f < fc * 2 ** (1 / 3))
    return 10 * np.log10(np.mean(Hmag2[sel]) + 1e-30)


def space(h):
    """Pre-delay, wet/dry and RT60 from an impulse response h (n, 2)."""
    e = np.sum(h ** 2, axis=1)
    peak = int(np.argmax(e[: int(0.05 * SR)]))
    direct = e[max(0, peak - int(0.002 * SR)):peak + int(0.005 * SR)].sum()
    late = e[peak + int(0.01 * SR):]
    wet_db = 10 * np.log10(late.sum() / (direct + 1e-30) + 1e-30)
    # Pre-delay: first point after the direct sound where the tail rises above
    # -20 dB of its own maximum (smoothed over 2 ms).
    sm = np.convolve(late, np.ones(int(0.002 * SR)) / (0.002 * SR), "same")
    above = np.nonzero(sm > sm.max() * 0.01)[0]
    pre_ms = (above[0] + int(0.01 * SR)) / SR * 1000 if len(above) else None
    edc = np.cumsum(late[::-1])[::-1]
    edc_db = 10 * np.log10(edc / (edc[0] + 1e-30) + 1e-30)
    i5, i25 = np.argmax(edc_db < -5), np.argmax(edc_db < -25)
    rt60 = (i25 - i5) / SR * 3 if i25 > i5 else None
    # Strongest discrete echo between 60 ms and 1 s (a delay line).
    env = np.convolve(e, np.ones(int(0.005 * SR)), "same")
    lo, hi = peak + int(0.06 * SR), min(len(env), peak + SR)
    echo_i = lo + int(np.argmax(env[lo:hi]))
    echo_rel = 10 * np.log10(env[echo_i] / (env[peak] + 1e-30) + 1e-30)
    lr = 10 * np.log10(np.sum(h[peak + int(0.01 * SR):, 0] ** 2) / (np.sum(h[peak + int(0.01 * SR):, 1] ** 2) + 1e-30) + 1e-30)
    return {"wet_db": wet_db, "predelay_ms": pre_ms, "rt60_s": rt60,
            "echo_ms": (echo_i - peak) / SR * 1000, "echo_db": echo_rel, "tail_lr_db": lr}


def eq_curve(H, f, k):
    """Per-band magnitude of stem k summed over L+R, relative to 1 kHz."""
    m2 = np.abs(H[:, k, 0]) ** 2 + np.abs(H[:, k, 1]) ** 2
    ref = band_db(m2, f, 1000)
    return {fc: band_db(m2, f, fc) - ref for fc in BANDS}, ref


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mix", required=True)
    ap.add_argument("stems", nargs="+")
    ap.add_argument("--compare", help="a RASP render (stereo) of one stem")
    ap.add_argument("--compare-stem", help="which stem the render was made from")
    args = ap.parse_args()

    names = [pathlib.Path(s).stem for s in args.stems]
    raw = [decode(s, 1)[:, 0] for s in args.stems]
    mix = decode(args.mix, 2)
    mono = mix.mean(axis=1)
    cols = []
    print("where each stem starts in the mix (confidence: correlation peak over the")
    print("99.9th percentile; below ~1.5 the placement is a guess and the stem's row is noise)")
    for nm, x in zip(names, raw):
        col, lag, conf = place_in_mix(x, mono)
        cols.append(col)
        print(f"  {nm[:22]:<23}{lag / SR:+9.3f} s   confidence {conf:5.1f}")
    X = np.stack(cols, axis=1)
    Y = mix
    n = len(mix)

    # Short window: EQ, level, pan, coherence. Long window: the space.
    H, coh = transfer(X, Y, 1 << 13)
    f = np.fft.rfftfreq(1 << 13, 1 / SR)
    cm = coh.mean(axis=1)
    band_coh = {b: float(np.mean(cm[(f >= b / 2 ** (1 / 3)) & (f < b * 2 ** (1 / 3))])) for b in BANDS}
    print("\nper stem: level and placement in the mix (at 1 kHz), then EQ relative to 1 kHz")
    print(f"(bands where the vocals explain under {MIN_COH:.0%} of the mix show '-': the band dominates there)")
    print(f"{'stem':<16}{'level':>7}{'pan L/R':>9}  " + "".join(f"{blabel(b):>6}" for b in BANDS))
    for i, nm in enumerate(names):
        curve, ref = eq_curve(H, f, i)
        l = band_db(np.abs(H[:, i, 0]) ** 2, f, 1000)
        r = band_db(np.abs(H[:, i, 1]) ** 2, f, 1000)
        print(f"{nm[:15]:<16}{ref:7.1f}{l - r:+9.1f}  "
              + "".join(f"{curve[b]:6.1f}" if band_coh[b] >= MIN_COH else f"{'-':>6}" for b in BANDS))
    print("\nhow much of the mix the vocals explain (coherence, %):")
    print("          " + "".join(f"{blabel(b):>6}" for b in BANDS))
    print("          " + "".join(f"{100 * band_coh[b]:6.0f}" for b in BANDS))

    Hl, _ = transfer(X, Y, 1 << 16)
    print("\nspace each stem was put in (from a 1.5 s impulse response; upper bounds, see --help):")
    print(f"{'stem':<16}{'wet/dry':>8}{'predelay':>10}{'RT60':>7}{'echo at':>9}{'echo':>7}{'tail L/R':>9}")
    for i, nm in enumerate(names):
        h = np.fft.irfft(Hl[:, i, :], axis=0)
        h = np.roll(h, int(0.02 * SR), axis=0)           # keep a little pre-ringing visible
        sp = space(h)
        pre = f"{sp['predelay_ms']:.0f} ms" if sp["predelay_ms"] is not None else "-"
        rt = f"{sp['rt60_s']:.2f} s" if sp["rt60_s"] is not None else "-"
        print(f"{nm[:15]:<16}{sp['wet_db']:+7.1f}dB{pre:>10}{rt:>7}{sp['echo_ms']:7.0f}ms{sp['echo_db']:+6.1f}dB{sp['tail_lr_db']:+8.1f}dB")

    if args.compare:
        k = [str(pathlib.Path(s)) for s in args.stems].index(str(pathlib.Path(args.compare_stem)))
        render = decode(args.compare, 2)
        x = raw[k][:, None]                               # the render follows the raw stem's timeline
        lag_r = find_lag(raw[k], render.mean(axis=1))
        Yr = align(render, lag_r, len(raw[k]))
        Hr, _ = transfer(x, Yr, 1 << 13)
        prod, _ = eq_curve(H, f, k)
        rasp, _ = eq_curve(Hr, f, 0)
        print(f"\nEQ difference for {names[k]}: producer minus RASP (positive = producer has more), dB")
        print("          " + "".join(f"{blabel(b):>6}" for b in BANDS))
        print("          " + "".join(f"{prod[b] - rasp[b]:6.1f}" for b in BANDS))


if __name__ == "__main__":
    main()
