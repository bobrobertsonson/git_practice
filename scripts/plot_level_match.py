#!/usr/bin/env python3
"""Phase 10.1 report plot: output loudness vs blend for the linear and constant-loudness laws.

For each preset given, renders the DI through tonerender at blend 0, 0.1, ..., 1 under both laws
(levelMatch auto), measures BS.1770 integrated loudness of the output in numpy, and writes one PNG
plus a JSON with the curves and the trims/make-up read from the first --report. Needs numpy, scipy,
matplotlib. Usage:
  scripts/plot_level_match.py --tonerender build/cli/tonerender --di tests/fixtures/di_riff.wav \
      --out docs/specs/plots/phase10_1 preset1.json [preset2.json ...]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

LAWS = ("linear", "constantLoudness")
BLENDS = [round(i / 10, 1) for i in range(11)]


def k_weighting(fs: float):
    """ITU-R BS.1770-4 K-weighting (pre-filter high shelf + RLB high-pass), designed for `fs`."""
    # Stage 1: high shelf, +4 dB above ~1.5 kHz (design from the standard's analogue prototype).
    f0, g_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / fs)
    vh = 10 ** (g_db / 20)
    vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    b = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a = [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    # Stage 2: RLB high-pass at 38 Hz.
    f0, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * f0 / fs)
    a0 = 1 + k / q + k * k
    b2 = [1 / a0, -2 / a0, 1 / a0]
    a2 = [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    return (b, a), (b2, a2)


def integrated_lufs(x: np.ndarray, fs: float) -> float:
    (b1, a1), (b2, a2) = k_weighting(fs)
    y = lfilter(b2, a2, lfilter(b1, a1, x.astype(np.float64)))
    blk = int(round(0.4 * fs))
    hop = blk // 4
    if len(y) < blk:
        return float("nan")
    n = (len(y) - blk) // hop + 1
    z = np.array([np.mean(y[i * hop:i * hop + blk] ** 2) for i in range(n)])
    lk = -0.691 + 10 * np.log10(np.maximum(z, 1e-30))
    keep = lk > -70
    if not keep.any():
        return float("nan")
    rel = -0.691 + 10 * np.log10(z[keep].mean()) - 10
    keep &= lk > rel
    if not keep.any():
        return float("nan")
    return float(-0.691 + 10 * np.log10(z[keep].mean()))


def read_wav(path: Path) -> tuple[np.ndarray, float]:
    with wave.open(str(path)) as w:
        fs, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2 ** 31
    elif sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        x = (a[:, 0].astype(np.int32) | (a[:, 1].astype(np.int32) << 8) | (a[:, 2].astype(np.int8).astype(np.int32) << 16)).astype(np.float64) / 2 ** 23
    elif sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 2 ** 15
    else:
        raise SystemExit(f"unsupported sample width {sw}")
    x = x.reshape(-1, ch)[:, 0]
    return x, float(fs)


def absolutize(obj, base: Path):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "file" and isinstance(v, str) and not os.path.isabs(v):
                obj[k] = str((base / v).resolve())
            else:
                absolutize(v, base)
    elif isinstance(obj, list):
        for v in obj:
            absolutize(v, base)
    return obj


def render(tonerender: Path, preset: dict, di: Path, tmp: Path, tag: str, block: int) -> tuple[float, dict]:
    pj = tmp / f"{tag}.json"
    out = tmp / f"{tag}.wav"
    rep = tmp / f"{tag}.report.json"
    pj.write_text(json.dumps(preset))
    cmd = [str(tonerender), "--preset", str(pj), "--in", str(di), "--out", str(out), "--report", str(rep), "--block", str(block)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"tonerender failed ({r.returncode}) for {tag}:\n{r.stdout}\n{r.stderr}")
    x, fs = read_wav(out)
    return integrated_lufs(x, fs), json.loads(rep.read_text())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tonerender", required=True, type=Path)
    ap.add_argument("--di", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="output prefix (PNG + JSON)")
    ap.add_argument("--block", type=int, default=256)
    ap.add_argument("presets", nargs="+", type=Path)
    args = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results = {}
    fig, axes = plt.subplots(1, len(args.presets), figsize=(5.2 * len(args.presets), 4.0), squeeze=False)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for ax, pp in zip(axes[0], args.presets):
            base = json.loads(pp.read_text())
            absolutize(base, pp.parent)
            name = base.get("name", pp.stem)
            res = {"preset": str(pp), "name": name, "curves": {}, "levelMatch": None, "makeupDb": None}
            for law in LAWS:
                ys = []
                for b in BLENDS:
                    p = json.loads(json.dumps(base))
                    p["blend"] = b
                    p["blendLaw"] = law
                    p["levelMatch"] = {"mode": "auto"}
                    p["align"] = p.get("align", {"mode": "auto"})
                    lufs, rep = render(args.tonerender, p, args.di, tmp, f"{pp.stem}_{law}_{b}", args.block)
                    ys.append(lufs)
                    if res["levelMatch"] is None:
                        res["levelMatch"] = rep.get("levelMatch")
                        res["makeupDb"] = (rep.get("blend") or {}).get("makeupDb")
                        res["reportBlend"] = rep.get("blend")
                res["curves"][law] = ys
                ax.plot(BLENDS, ys, marker="o", label="linear" if law == "linear" else "constant loudness")
            # Reference: the untrimmed linear law (levelMatch off), to show what the trims do.
            ys = []
            for b in BLENDS:
                p = json.loads(json.dumps(base))
                p["blend"] = b
                p["blendLaw"] = "linear"
                p["levelMatch"] = {"mode": "off"}
                lufs, _ = render(args.tonerender, p, args.di, tmp, f"{pp.stem}_off_{b}", args.block)
                ys.append(lufs)
            res["curves"]["linear_noTrim"] = ys
            ax.plot(BLENDS, ys, marker=".", linestyle="--", color="gray", label="linear, level match off")
            ax.set_title(name, fontsize=10)
            ax.set_xlabel("blend (0 = A, 1 = B)")
            ax.set_ylabel("output loudness (LUFS)")
            ax.grid(True, alpha=0.3)
            ax.legend(fontsize=8)
            results[pp.stem] = res
    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(args.out) + ".png", dpi=130)
    Path(str(args.out) + ".json").write_text(json.dumps(results, indent=2))
    for k, v in results.items():
        print(k, json.dumps(v["levelMatch"]), "makeup", v["makeupDb"])
        for law, ys in v["curves"].items():
            print(f"  {law:14s}", " ".join(f"{y:6.2f}" for y in ys), f"  spread {max(ys) - min(ys):.2f} LU")
    return 0


if __name__ == "__main__":
    sys.exit(main())
