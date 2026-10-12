"""Reproduces the numbers of docs/reports/v0_4/a1_diagnosis.md (v0.4a, A.1). Needs tonerender (SAWBLADE_TONERENDER) and
the match package (`pip install -e match`). Usage:  python docs/reports/v0_4/a1_diagnosis.py [work_dir]

Nothing here is a capture: the probe's DI is tests/fixtures/di_riff.wav (a 4 s riff, wrapped to the 30 s DI slot)."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

from sawblade_match.calibrate import pedal_fit as PF

work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="a1diag"))
work.mkdir(parents=True, exist_ok=True)
np.set_printoptions(precision=1, suppress=True, linewidth=200)
DI = PF.REPO / "tests" / "fixtures" / "di_riff.wav"
probe, lay, slots = PF.build_probe(DI, PF.ProbeLayout())          # the full 45 s probe
wav = work / "probe.wav"
sf.write(str(wav), probe, PF.FS, subtype="FLOAT")


def render(preset):
    return PF.render(preset, wav, work / "out.wav", work)


def feat(y):
    return PF.features(y, probe, lay, slots)


def line(tag, t, extra=""):
    print(f"{tag}: legacy {t['harm_rms_db_legacy']:.2f}  fixed {t['harm_rms_db']:.2f}  even {t['harm_even_rms_db']:.2f}  "
          f"odd {t['harm_odd_rms_db']:.2f} {extra}")


KN = (7, 5, 8)                                                     # low, high, distortion; level 8
y1 = render(PF.hm_preset(*KN, 8.0, model_version=1))
y3 = render(PF.hm_preset(*KN, 8.0, model_version=3))
f1, f3 = feat(y1), feat(y3)
print("== v3 (reference) vs v1 (model), full probe, knobs", KN)
c = PF.compare(f3, f1)
line("v3 vs v1", c, f"ltas {c['ltas_rms_db']:.2f} dyn {c['dyn_db']:.2f}")
line("v3 vs v3", PF.compare(f3, feat(render(PF.hm_preset(*KN, 8.0, model_version=3)))))
for tag, f in (("v1", f1), ("v3", f3)):
    print(tag, "mean H2..H7 over the 16 slots (floor -70):", f["harm"].mean(axis=0))
    for li, lv in enumerate(PF.STEP_LEVELS_DB):
        print("   ", lv, "dBFS:", f["harm"][[fi * 4 + li for fi in range(4)]].mean(axis=0))
print("v1 even cells at the -70 floor:", float((f1["harm"][:, PF.EVEN_COLS] <= PF.HARM_FLOOR_DB + 1e-9).mean()))
d, df = f3["harm"] - f1["harm"], PF.harm_fixed(f3["harm"]) - PF.harm_fixed(f1["harm"])
print("v3 - v1 legacy: mean/H", d.mean(axis=0), "rms/H", np.sqrt((d ** 2).mean(axis=0)))
print("v3 - v1 fixed : mean/H", df.mean(axis=0), "rms/H", np.sqrt((df ** 2).mean(axis=0)))
for fl in (-30, -40, -50, -60, -70):
    print("floor", fl, {k: round(v, 2) for k, v in PF.harm_terms(f3["harm"], f1["harm"], fl).items()})
for li, lv in enumerate(PF.STEP_LEVELS_DB):
    sel = [fi * 4 + li for fi in range(4)]
    print("level", lv, "legacy rms %.1f fixed rms %.1f" % (PF._rms(d[sel]), PF._rms(df[sel])))
for kk in ((7, 5, 8), (2, 2, 3), (10, 10, 10), (7, 5, 4)):
    t = PF.harm_terms(f3["harm"], feat(render(PF.hm_preset(*kk, 8.0, model_version=1)))["harm"])
    print("v3@%s vs v1@%s" % (KN, kk), {k: round(v, 1) for k, v in t.items()})

print("\n== the 7.1 stock-capture profile (H2..H7 = -9 -18 -13 -22 -20 -21 dB, from the 7.1 report) vs the renders")
cap = np.tile([-9.0, -18.0, -13.0, -22.0, -20.0, -21.0], (16, 1))
for tag, f in (("v1", f1), ("v3", f3)):
    for fl in (-70, -50, -40, -30):
        print(tag, "floor", fl, {k: round(v, 1) for k, v in PF.harm_terms(cap, f["harm"], fl).items()})

print("\n== estimator floor: pure sines through harmonic_profile")
sig = np.zeros(lay.n_total)
for f0, lv, sl in slots:
    sig[sl] = PF._db(lv) * np.sin(2 * np.pi * f0 * np.arange(sl.stop - sl.start) / PF.FS)
print("max cell (floored at -70):", PF.harmonic_profile(sig, slots).max())
sl = slots[4][2]
seg = sig[sl]
a, b = int(0.35 * len(seg)), int(0.95 * len(seg))
z = seg[a:b] * np.hanning(b - a)
P = np.abs(np.fft.rfft(z, 1 << int(np.ceil(np.log2(4 * len(z)))))) ** 2
fr = np.fft.rfftfreq(1 << int(np.ceil(np.log2(4 * len(z)))), 1 / PF.FS)
band = lambda fc: P[np.abs(fr - fc) <= 12].sum() + 1e-30
print("raw H2..H7 of a -30 dBFS 220 Hz sine, dB re fundamental:",
      np.array([10 * np.log10(band(k * 220.0) / band(220.0)) for k in PF.HARMONICS]))

print("\n== gain and delay invariance (v3 as the reference, v1 as the model)")
base = PF.compare(f3, f1)
for g in (-20, -10, 10, 20):
    r = PF.compare(feat(y3 * 10 ** (g / 20)), f1)
    print(f"gain {g:+d} dB: d harm {r['harm_rms_db'] - base['harm_rms_db']:+.4f} d even {r['harm_even_rms_db'] - base['harm_even_rms_db']:+.4f} "
          f"d ltas {r['ltas_rms_db'] - base['ltas_rms_db']:+.4f} d dyn {r['dyn_db'] - base['dyn_db']:+.4f}")
for dl in (1, 77, 512, 2048):
    yd = np.concatenate([np.zeros(dl), y3])[:len(y3)]
    fd = feat(yd)
    r = PF.compare(fd, f1)
    un = PF.harm_terms(PF.harmonic_profile(np.concatenate([np.zeros(dl), y3])[:lay.n_total], slots), f1["harm"])
    print(f"delay {dl}: lag found {fd['lag']}, aligned d harm {r['harm_rms_db'] - base['harm_rms_db']:+.4f} "
          f"d ltas {r['ltas_rms_db'] - base['ltas_rms_db']:+.4f}; unaligned harm {un['harm_rms_db']:.4f} (base {base['harm_rms_db']:.4f})")

print("\n== is `level` a pure output gain? (level 8 -> L, rendered vs scaled by 10^(3 (L-8) / 20))")
KNOBS = {"hm": (7, 3, 6), "hmx": (6, 4, 7, 3, 8, 4), "eye": (7,), "muff": (6, 3, 7, 6), "ts": (7, 4)}
for name, spec in PF.PEDALS.items():
    ya = render(spec.preset(KNOBS[name], 8.0))
    for lv in (5.0, 2.0, 10.0):
        yb = render(spec.preset(KNOBS[name], lv))
        sc = 10 ** (3 * (lv - 8) / 20)
        print(name, f"level 8 -> {lv}: max rel err {np.max(np.abs(yb - ya * sc)) / np.max(np.abs(ya * sc)):.1e}")

print("\n== muff: sustain and crunch are redundant (iso gain/knee curve of modelVersion 1)")
spec = PF.PEDALS["muff"]


def muff(sustain, crunch):
    blk = spec.block((sustain, 4.0, 6.0, 6.0), 8.0)
    blk["params"]["crunch"] = crunch
    return feat(render(PF._preset(blk)))


ref = muff(7.0, 3.0)
for s, c in ((6.0, 6.5), (5.0, 8.99)):
    r = PF.compare(ref, muff(s, c))
    print(f"sustain {s} crunch {c}: ltas {r['ltas_rms_db']:.3f} harm {r['harm_rms_db']:.3f} dyn {r['dyn_db']:.3f} solved level {r['level']:.2f}")

print("\n== sensitivity: cost (ltas + 0.5 harm + 0.5 dyn) when one knob is moved by +-0.5 from the known-answer truth")
kprobe, klay, kslots = PF.build_probe(PF.KNOWN_DI, PF.KNOWN_LAYOUT)
kwav = work / "probe_known.wav"
sf.write(str(kwav), kprobe, PF.FS, subtype="FLOAT")
for name, spec in PF.PEDALS.items():
    truth, tl = PF.KNOWN_TRUTH[name]
    ev = PF.Evaluator(kwav, kprobe, klay, kslots, work, jobs=4, spec=spec)
    kref = PF.features(PF.render(spec.preset(truth, tl), kwav, work / "s.wav", work), kprobe, klay, kslots)
    for i, k in enumerate(spec.knobs):
        row = []
        for dlt in (-0.5, 0.5):
            v = list(truth)
            v[i] += dlt
            r = PF.compare(kref, ev.features(*v))
            row.append(f"{dlt:+.1f}: cost {r['cost']:.3f} (ltas {r['ltas_rms_db']:.2f} harm {r['harm_rms_db']:.2f} dyn {r['dyn_db']:.2f})")
        print(f"{name:5s} {k:10s} {' | '.join(row)}")
