"""Phase 5.1a evaluation (runs in the venv): SDR, null test vs stock Python (shifts=0), determinism.

usage: eval_51a.py --data ~/sawblade-sep-data [--out DIR] [--engines eigen,blas,onnx] [--no-museval]
                   [--det A:B ...]   (A, B = stem directories under --out; reports bit-identical or the residual)

Expects stem WAVs in <out>/<engine>_<m>_<d>/ for engine in {eigen, blas, onnx} and the stock-Python
reference in <out>/py_<m>_<d>_shift0/ (scripts/run_python.py with no shift flags = demucs 4.0.1
apply_model(shifts=0, split=True, overlap=0.25)), for m in {4s, 6s}, d in {real7, synth}.
SDR: museval BSSEval v4 (1 s windows/hop, median over frames) and the global SDR over the whole track.
Null: 20*log10(rms(engine - python) / rms(python)), max abs diff, cross-correlation lag (samples).
"""
import argparse, sys, warnings, hashlib
from pathlib import Path
import numpy as np, soundfile as sf
warnings.filterwarnings("ignore")
ap = argparse.ArgumentParser()
ap.add_argument("--data", default=str(Path.home() / "sawblade-sep-data"))
ap.add_argument("--out", default=None)
ap.add_argument("--engines", default="eigen,blas,onnx")
ap.add_argument("--no-museval", action="store_true")
ap.add_argument("--det", nargs="*", default=[])
a = ap.parse_args()
data = Path(a.data); out = Path(a.out) if a.out else data / "out"
SR = 44100
ENG = a.engines.split(",")


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


def edir(impl, m, d):
    return out / (f"py_{m}_{d}_shift0" if impl == "py" else f"{impl}_{m}_{d}")


def run_one(d, m, est_dir):
    G = lambda s: rd(data / d / f"{s}.wav")
    E = lambda s: rd(est_dir / f"{s}.wav")
    if d == "real7":
        R = {"drums": G("drums"), "bass": G("bass"), "other": G("other"), "vocals": G("vocals")}
    else:
        pad, gtr = G("other"), G("guitar"); n0 = min(len(pad), len(gtr))
        R = {"drums": G("drums"), "bass": G("bass"), "vocals": G("vocals"), "other": pad[:n0] + gtr[:n0]}
    Es = {k: E(k) for k in R}
    if m == "6s":
        Es["other"] = E("other") + E("guitar") + E("piano")
    ms = [np.nan] * len(R)
    if not a.no_museval:
        try: ms = bss(list(R.values()), [Es[k] for k in R])
        except Exception as ex: print("museval failed:", ex, file=sys.stderr)
    return {k: (ms[i], gsdr(R[k], Es[k])) for i, k in enumerate(R)}


def xcorr_lag(x_, y_):
    n = min(len(x_), len(y_)); x = x_[:n].mean(1); y = y_[:n].mean(1)
    m = min(n, SR * 10); x, y = x[:m], y[:m]
    f = np.fft.rfft(x, 2 * m) * np.conj(np.fft.rfft(y, 2 * m)); c = np.fft.irfft(f)
    k = int(np.argmax(c)); return k if k < m else k - 2 * m


def resid(x, y):
    n = min(len(x), len(y)); x, y = x[:n], y[:n]; r = x - y
    return 20 * np.log10(np.sqrt((r ** 2).mean()) / (np.sqrt((y ** 2).mean()) + 1e-30) + 1e-30), np.abs(r).max()


for d in ("real7", "synth"):
    for m in ("4s", "6s"):
        print(f"\n### SDR (dB), dataset `{d}`, model htdemucs {m} (museval median / global)\n")
        res = {impl: run_one(d, m, edir(impl, m, d)) for impl in ["py"] + ENG}
        print("| stem group | Python shifts=0 | " + " | ".join(ENG) + " |")
        print("|---|---|" + "---|" * len(ENG))
        for k in res["py"]:
            print(f"| {k} | " + " | ".join(f"{res[i][k][0]:.2f} / {res[i][k][1]:.2f}" for i in ["py"] + ENG) + " |")
        stems = ["drums", "bass", "other", "vocals"] + (["guitar", "piano"] if m == "6s" else [])
        print(f"\n#### Null test `{d}` {m}: engine vs stock Python (shifts=0): residual dB / max abs diff / xcorr lag\n")
        print("| stem | " + " | ".join(ENG) + " |")
        print("|---|" + "---|" * len(ENG))
        for s in stems:
            cells = []
            for impl in ENG:
                x, y = rd(edir(impl, m, d) / f"{s}.wav"), rd(edir("py", m, d) / f"{s}.wav")
                db, mx = resid(x, y)
                cells.append(f"{db:.1f} / {mx:.1e} / {xcorr_lag(x, y)}")
            print(f"| {s} | " + " | ".join(cells) + " |")

if a.det:
    print("\n### Determinism / thread-count comparisons\n")
    print("| A vs B | stem | bit-identical | residual dB | max abs diff |")
    print("|---|---|---|---|---|")
    for pair in a.det:
        A, B = pair.split(":")
        for f in sorted((out / A).glob("*.wav")):
            same = hashlib.sha256(f.read_bytes()).digest() == hashlib.sha256((out / B / f.name).read_bytes()).digest()
            db, mx = resid(rd(f), rd(out / B / f.name))
            print(f"| {A} vs {B} | {f.stem} | {'yes' if same else 'NO'} | {'-' if same else f'{db:.1f}'} | {mx:.2e} |")
