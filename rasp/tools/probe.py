#!/usr/bin/env python3
"""Black-box measurement kit: match RASP to a reference plugin by ear AND by numbers.

This measures what a processor does to known test signals. It never looks at the
plugin's code. Workflow:

  1. python3 probe.py make probe.wav
  2. In your DAW (48 kHz), put the reference plugin on a track with probe.wav and
     bounce it to a stereo WAV, once per setting you want to match. Write the
     settings into the file name, e.g. ref_drive50_chop0.wav. For clean
     measurements, isolate one section at a time (see README).
  3. Render the same probe through RASP with matching settings:
       ./rasp_wav probe.wav rasp.wav drive=0.5 chop=0 thicken=0 flavour=0
  4. python3 probe.py analyze ref_drive50_chop0.wav --ref rasp.wav

Needs only numpy.
"""
import argparse
import sys
import wave

import numpy as np

SR = 48000
# Segment layout of the probe (seconds). The sync click at SYNC aligns renders.
SYNC = 0.5
SWEEP = (1.0, 5.0)            # log sweep 20 Hz..20 kHz at -30 dBFS
STAIR = (5.5, 10.7)           # 1 kHz, -50..0 dBFS in 2 dB steps of 200 ms
STEP_DB = np.arange(-50, 1, 2)
AR = (11.0, 14.0)             # 1 kHz: -30 dB / -6 dB / -30 dB, 1 s each
IMP = 14.5                    # single click, then silence for IR (comb, verb, delay)
DUCK = (21.0, 25.0)           # pink-ish noise 0.5 s on / 0.5 s off, -12 dBFS
LENGTH = 30.0


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def rms_db(x):
    return 10 * np.log10(np.mean(np.square(x)) + 1e-20)


def make_probe():
    n = int(LENGTH * SR)
    x = np.zeros(n)
    x[int(SYNC * SR)] = 0.5
    t0, t1 = SWEEP
    t = np.arange(int((t1 - t0) * SR)) / SR
    T, f0, f1 = t1 - t0, 20.0, 20000.0
    k = np.log(f1 / f0)
    sweep = np.sin(2 * np.pi * f0 * T / k * (np.exp(t / T * k) - 1))
    fade = np.minimum(1, np.minimum(t / 0.01, (T - t) / 0.01))
    x[int(t0 * SR):int(t0 * SR) + len(t)] = 10 ** (-30 / 20) * sweep * fade
    i = int(STAIR[0] * SR)
    step = int(0.2 * SR)
    ts = np.arange(step) / SR
    for lvl in STEP_DB:
        x[i:i + step] = 10 ** (lvl / 20) * np.sqrt(2) * np.sin(2 * np.pi * 1000 * ts) / np.sqrt(2)
        i += step
    i = int(AR[0] * SR)
    ts = np.arange(SR) / SR
    for lvl in (-30, -6, -30):
        x[i:i + SR] = 10 ** (lvl / 20) * np.sin(2 * np.pi * 1000 * ts)
        i += SR
    x[int(IMP * SR)] = 0.5
    rng = np.random.default_rng(7)
    noise = np.cumsum(rng.standard_normal(int(4 * SR))) * 0.02
    noise -= np.convolve(noise, np.ones(200) / 200, "same")
    noise *= 10 ** (-12 / 20) / np.sqrt(np.mean(noise ** 2))
    gate = (np.arange(len(noise)) // (SR // 2)) % 2 == 0
    x[int(DUCK[0] * SR):int(DUCK[1] * SR)] = noise * gate
    return x


def write_wav(path, x):
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def read_wav(path):
    with wave.open(path) as f:
        sr, ch, sw = f.getframerate(), f.getnchannels(), f.getsampwidth()
        raw = f.readframes(f.getnframes())
    if sr != SR:
        sys.exit(f"{path}: render at {SR} Hz (got {sr})")
    if sw == 2:
        x = np.frombuffer(raw, np.int16) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8) / 8388608.0
    elif sw == 4:
        x = np.frombuffer(raw, np.int32) / 2147483648.0
    else:
        sys.exit(f"{path}: unsupported sample width {sw}")
    x = x.reshape(-1, ch)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2]


def align(y):
    """Shift so the sync click lands at SYNC. Only for renders with an offset;
    don't use it on a render where the dry signal is muted (solo), because the
    first comb tap would be taken for the click."""
    m = np.abs(y).max(axis=1)
    hit = np.argmax(m[: int(1.5 * SR)] > 0.05)
    shift = hit - int(SYNC * SR)
    if shift > 0:
        y = y[shift:]
    elif shift < 0:
        y = np.vstack([np.zeros((-shift, 2)), y])
    return y, shift


def seg(y, a, b):
    return y[int(a * SR):int(b * SR)]


def analyze(path, do_align=False):
    y = read_wav(path)
    shift = 0
    if do_align:
        y, shift = align(y)
    mono = y.mean(axis=1)
    r = {"offset_samples": shift}

    # Small-signal frequency response from the sweep (output / input level).
    x = make_probe()
    sx, sy = seg(x, *SWEEP), seg(mono, *SWEEP)
    X, Y = np.fft.rfft(sx), np.fft.rfft(sy)
    f = np.fft.rfftfreq(len(sx), 1 / SR)
    resp = {}
    for fc in (50, 100, 200, 400, 800, 1600, 3200, 6400, 12800):
        band = (f > fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        resp[fc] = 10 * np.log10(np.sum(np.abs(Y[band]) ** 2) / np.sum(np.abs(X[band]) ** 2) + 1e-20)
    ref = resp[800]
    r["freq_response_db_rel_800Hz"] = {k: round(float(v - ref), 1) for k, v in resp.items()}
    r["gain_at_800Hz_db"] = round(float(ref), 1)

    # Static curve and harmonics from the staircase (skip 50 ms settling per step).
    curve, harm = [], None
    for i, lvl in enumerate(STEP_DB):
        a = STAIR[0] + i * 0.2 + 0.05
        s = seg(mono, a, a + 0.15)
        curve.append((int(lvl), round(float(rms_db(s)) + 3.01, 1)))
        if lvl == -12:
            w = s * np.blackman(len(s))
            S = np.abs(np.fft.rfft(w))
            fs = np.fft.rfftfreq(len(w), 1 / SR)
            h = [S[np.argmin(np.abs(fs - 1000 * k))] for k in range(1, 8)]
            harm = {f"H{k}": round(float(db(h[k - 1] / h[0])), 1) for k in range(2, 8)}
    r["level_in_out_dbfs"] = curve[::3]
    r["harmonics_at_-12dBFS_rel_H1"] = harm

    # Attack / release: time for the output envelope to settle after each step.
    env = np.sqrt(np.convolve(mono ** 2, np.ones(48) / 48, "same"))
    def settle(a, b):
        e = db(seg(env, a, b))
        final = np.median(e[-int(0.2 * SR):])
        i0 = int(0.002 * SR)             # skip the step itself
        start = e[i0]
        if abs(start - final) < 1:
            return None                  # no gain change: nothing to time
        target = final + (start - final) * np.exp(-1)
        idx = i0 + np.argmax((e[i0:] - target) * np.sign(start - final) <= 0)
        return round(idx / SR * 1000, 1)
    r["attack_ms_(-30->-6dB)"] = settle(AR[0] + 1.0, AR[0] + 2.0)
    r["release_ms_(-6->-30dB)"] = settle(AR[0] + 2.0, AR[0] + 3.0)

    # Impulse response: early taps per side (thicken), RT60, delay echo.
    ir = seg(y, IMP, IMP + 6.0)
    taps = {}
    for c, name in ((0, "L"), (1, "R")):
        lo = int(0.001 * SR)             # skip the direct click
        e = np.abs(ir[lo:int(0.04 * SR), c])
        # The strongest local peaks, at least 0.5 ms apart, listed in time order.
        pk = [i for i in range(1, len(e) - 1) if e[i] > e.max() * 0.1 and e[i] >= e[i - 1] and e[i] > e[i + 1]]
        chosen = []
        for i in sorted(pk, key=lambda i: -e[i]):
            if all(abs(i - j) > 0.0005 * SR for j in chosen):
                chosen.append(i)
            if len(chosen) == 6:
                break
        taps[name] = [round((i + lo) / SR * 1000, 1) for i in sorted(chosen)]
    r["early_taps_ms"] = taps
    tail = np.sum(ir[int(0.05 * SR):] ** 2, axis=1)
    edc = np.cumsum(tail[::-1])[::-1]
    edc_db = 10 * np.log10(edc / (edc[0] + 1e-20) + 1e-20)
    i5, i25 = np.argmax(edc_db < -5), np.argmax(edc_db < -25)
    r["rt60_s_(T20)"] = round((i25 - i5) / SR * 3, 2) if i25 > i5 else None
    env_ir = np.sqrt(np.convolve(np.sum(ir ** 2, axis=1), np.ones(240) / 240, "same"))
    lo = int(0.06 * SR)
    j = lo + np.argmax(env_ir[lo:int(1.6 * SR)])
    r["strongest_echo_ms"] = round(j / SR * 1000, 1)

    # Ducking: effect level while the noise plays vs. in the gaps right after.
    on, off = [], []
    for k in range(4):
        a = DUCK[0] + k * 1.0
        on.append(rms_db(seg(mono, a + 0.2, a + 0.45)))
        off.append(rms_db(seg(mono, a + 0.55, a + 0.8)))
    r["gap_level_vs_note_db"] = round(float(np.mean(off) - np.mean(on)), 1)
    return r


def show(results, names):
    keys = list(results[0].keys())
    for k in keys:
        vals = [res.get(k) for res in results]
        if isinstance(vals[0], dict):
            print(f"{k}:")
            for sub in vals[0]:
                print(f"  {str(sub):>10}  " + "  ".join(f"{str(v.get(sub)):>14}" for v in vals))
        elif isinstance(vals[0], list):
            print(f"{k}:")
            for row in zip(*vals):
                print("            " + "  ".join(f"{str(v):>14}" for v in row))
        else:
            print(f"{k}: " + "  ".join(f"{str(v):>14}" for v in vals))
    print("columns: " + "  |  ".join(names))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make", help="write the probe signal")
    m.add_argument("out")
    a = sub.add_parser("analyze", help="measure a render of the probe")
    a.add_argument("render")
    a.add_argument("--ref", help="a second render to compare against (e.g. RASP's)")
    a.add_argument("--align", action="store_true", help="re-align on the sync click if the bounce is offset")
    args = ap.parse_args()
    if args.cmd == "make":
        write_wav(args.out, make_probe())
        print(f"wrote {args.out} ({LENGTH:.0f} s, {SR} Hz mono)")
    else:
        names = [args.render] + ([args.ref] if args.ref else [])
        show([analyze(p, args.align) for p in names], names)


if __name__ == "__main__":
    main()
