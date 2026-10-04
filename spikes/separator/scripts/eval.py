"""SDR + C++-vs-Python null test (runs in the venv). Prints markdown tables.

usage: eval.py --data ~/sawblade-sep-data --out ~/sawblade-sep-data/out [--no-museval]

Expects, per dataset d in {real7, synth} and model m in {4s, 6s}, stem WAVs in
  <out>/cpp_<m>_<d>/  <out>/py_<m>_<d>_shift0/  <out>/py_<m>_<d>_shift4033/
SDR: museval BSSEval v4 (museval.metrics.bss_eval, 1 s windows/hop, median over frames, nan ignored);
also the global SDR 10*log10(sum s^2 / sum (s-s_hat)^2) over the whole track, both channels.
"""
import argparse, sys, warnings
from pathlib import Path
import numpy as np, soundfile as sf
warnings.filterwarnings("ignore")
ap = argparse.ArgumentParser()
ap.add_argument("--data", default=str(Path.home() / "sawblade-sep-data"))
ap.add_argument("--out", default=None)
ap.add_argument("--no-museval", action="store_true")
a = ap.parse_args()
data = Path(a.data); out = Path(a.out) if a.out else data / "out"
SR = 44100


def edir(impl, m, d):
    return out / (f"cpp_{m}_{d}" if impl == "cpp" else f"py_{m}_{d}_{impl[3:]}")


def rd(p):
    x, sr = sf.read(p, dtype="float64", always_2d=True); assert sr == SR; return x


def gsdr(ref, est):
    n = min(len(ref), len(est)); ref, est = ref[:n], est[:n]
    return 10 * np.log10(np.sum(ref ** 2) / max(np.sum((ref - est) ** 2), 1e-20))


def bss(refs, ests):
    import museval.metrics as mm
    n = min(min(len(r) for r in refs), min(len(e) for e in ests))
    R = np.stack([r[:n] for r in refs]); E = np.stack([e[:n] for e in ests])
    sdr, *_ = mm.bss_eval(R, E, window=SR, hop=SR, compute_permutation=False)
    return np.nanmedian(sdr, axis=1)


def run_one(d, m, est_dir):
    G = lambda s: rd(data / d / f"{s}.wav")
    E = lambda s: rd(est_dir / f"{s}.wav")
    if d == "real7":
        R = {"drums": G("drums"), "bass": G("bass"), "other": G("other"), "vocals": G("vocals")}
        if m == "4s":
            Es = {k: E(k) for k in R}
            extra = {}
        else:
            Es = {"drums": E("drums"), "bass": E("bass"), "vocals": E("vocals"),
                  "other": E("other") + E("guitar") + E("piano")}
            n = min(len(E("other")), len(R["other"]))
            extra = {"other (6s 'other' stem only)": gsdr(R["other"], E("other")),
                     "guitar (vs GT other; not a pure-guitar GT)": gsdr(R["other"], E("guitar"))}
    else:
        pad, gtr = G("other"), G("guitar")
        n0 = min(len(pad), len(gtr))
        R = {"drums": G("drums"), "bass": G("bass"), "vocals": G("vocals"), "other": pad[:n0] + gtr[:n0]}
        if m == "4s":
            Es = {k: E(k) for k in R}
            extra = {}
        else:
            Es = {"drums": E("drums"), "bass": E("bass"), "vocals": E("vocals"),
                  "other": E("other") + E("guitar") + E("piano")}
            extra = {"guitar (6s guitar stem vs GT guitar)": gsdr(gtr, E("guitar")),
                     "pad (6s other+piano vs GT pad)": gsdr(pad, E("other") + E("piano"))}
    keys = list(R)
    ms = [np.nan] * len(keys)
    if not a.no_museval:
        try: ms = bss([R[k] for k in keys], [Es[k] for k in keys])
        except Exception as ex: print("museval failed:", ex, file=sys.stderr)
    rows = {k: (ms[i], gsdr(R[k], Es[k])) for i, k in enumerate(keys)}
    return rows, extra


def xcorr_lag(a_, b_):
    n = min(len(a_), len(b_)); x = a_[:n].mean(1); y = b_[:n].mean(1)
    m = min(n, SR * 10); x, y = x[:m], y[:m]
    f = np.fft.rfft(x, 2 * m) * np.conj(np.fft.rfft(y, 2 * m)); c = np.fft.irfft(f)
    k = int(np.argmax(c)); return k if k < m else k - 2 * m


def null(d, m, a_dir, b_dir, stems):
    rows = []
    for s in stems:
        x, y = rd(a_dir / f"{s}.wav"), rd(b_dir / f"{s}.wav")
        lag = xcorr_lag(x, y); n = min(len(x), len(y)); x, y = x[:n], y[:n]
        r = x - y
        rows.append((s, 20 * np.log10(np.sqrt((r ** 2).mean()) / np.sqrt((y ** 2).mean()) + 1e-30), np.abs(r).max(), lag, len(x), len(y)))
    return rows


for d in ("real7", "synth"):
    for m in ("4s", "6s"):
        print(f"\n### SDR (dB), dataset `{d}`, model htdemucs {m}\n")
        res = {}
        for impl in ("cpp", "py_shift0", "py_shift4033"):
            res[impl] = run_one(d, m, edir(impl, m, d))
        print("| stem group | C++ museval | C++ global | Py shifts=0 museval | Py shifts=0 global | Py shift=4033 museval | Py shift=4033 global |")
        print("|---|---|---|---|---|---|---|")
        for k in res["cpp"][0]:
            cells = []
            for impl in res:
                ms_, gl = res[impl][0][k]; cells += [f"{ms_:.2f}", f"{gl:.2f}"]
            print(f"| {k} | " + " | ".join(cells) + " |")
        for k in res["cpp"][1]:
            print(f"| {k} (global only) | - | {res['cpp'][1][k]:.2f} | - | {res['py_shift0'][1][k]:.2f} | - | {res['py_shift4033'][1][k]:.2f} |")
        stems = ["drums", "bass", "other", "vocals"] + (["guitar", "piano"] if m == "6s" else [])
        for ref_impl, lab in (("py_cppmatch", "Python with C++ settings: same time shift (4033) and zero-padded short chunks"), ("py_shift4033", "Python with the same time shift as C++ (offset 4033)"), ("py_shift0", "Python shifts=0 (spec setting)")):
            print(f"\n#### Null test `{d}` {m}: C++ vs {lab}\n")
            print("| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |")
            print("|---|---|---|---|---|")
            for s, rdb, mx, lag, n1, n2 in null(d, m, edir("cpp", m, d), edir(ref_impl, m, d), stems):
                print(f"| {s} | {rdb:.1f} | {mx:.2e} | {lag} | {n1} / {n2} |")
