"""Pre-screen recall check (spec 3.3 A): does the per-capture pre-screen keep the captures the full search picks?

Reads a finished run (``result.json``: stage-1 ranking of the full pair search and the refined candidates), recomputes the
pre-screen on the same excerpt/target for several N (top N per gear class), and reports

* recall@M: fraction of the full search's top-M stage-1 combos (per topology) whose every NAM survives the pre-screen;
* whether the selected best combo's captures all survive;
* capture recall: fraction of the distinct NAM captures in the top-30 that survive.

``python -m sawblade_match.matcher.recall --run RUN_DIR --di DI.wav --ref REF [--matched left] --pool POOL --ns 2,3,4,6``
(the run must have used the full pair search, i.e. no pre-screen).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from ..tonecheck.analysis import activity_mask  # noqa: F401
from .engine import RATE, Engine, to48
from .pool import default_cab, load_pool
from .prescreen import prescreen
from .reference import build_target, load_reference, make_excerpt
from .run import gate_envelope_floor_db
from .space import gate_preset


def _keys(captures: dict) -> set[str]:
    return {f"{c['toneId']}/{c['modelId']}" for k, c in captures.items() if c and k != "cab"}


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="sawblade-match-recall")
    p.add_argument("--run", required=True)
    p.add_argument("--di", required=True)
    p.add_argument("--ref", required=True)
    p.add_argument("--matched", choices=["left", "right", "mono"])
    p.add_argument("--pool", required=True)
    p.add_argument("--ns", default="2,3,4,6")
    p.add_argument("--threads", type=int, default=4)
    a = p.parse_args(argv)
    res = json.loads((Path(a.run) / "result.json").read_text())
    if res["stage1"].get("prescreen", {}).get("appliedToBlendSingle"):
        print("error: the run used a pre-screen; recall needs a full-search run", file=sys.stderr)
        return 3
    pool = load_pool(a.pool)
    ref = load_reference(a.ref, channel="auto", matched=a.matched)
    di, fs = sf.read(a.di, dtype="float32")
    di48 = to48(di if di.ndim == 1 else di[:, 0], fs)
    window = (int(res["excerpt"]["startS"] * RATE), int(res["excerpt"]["endS"] * RATE))
    ex = make_excerpt(di48, window[1] / RATE - window[0] / RATE, window=window, ref=ref)
    off = res.get("offsetRefinement", {}).get("excerptStarterRender", {}).get("offset")
    tgt = build_target(ref, ex, offset_samples=off) if ref.matched_sig is not None else build_target(ref, ex)
    eng = Engine(res["gate"], a.threads)
    top = {t: lst for t, lst in res["stage1"]["top"].items() if t in ("blend", "single")}
    best = _keys(res["best"]["captures"])
    allcaps = {k for lst in top.values() for e in lst[:30] for k in _keys(e["captures"])}
    out = {"run": a.run, "fullPairs": res["stage1"]["fullPairs"], "perN": {}}
    try:
        for n in [int(x) for x in a.ns.split(",")]:
            kp, ka, info = prescreen(eng, pool, ex, tgt, default_cab(pool.cabs), n, lambda *_: None)
            kept = {c.key for c in kp} | {c.key for c in ka}
            row = {"pedalsKept": len(kp), "ampsKept": len(ka),
                   "pairsAfter": (len(kp) + 1) * len(ka),
                   "bestComboKept": best <= kept, "captureRecallTop30": len(allcaps & kept) / max(len(allcaps), 1)}
            for t, lst in top.items():
                for m in (5, 10, 30):
                    sel = lst[:m]
                    row[f"{t}RecallAt{m}"] = sum(_keys(e["captures"]) <= kept for e in sel) / max(len(sel), 1)
            out["perN"][n] = row
            print(n, json.dumps(row))
    finally:
        eng.close()
    (Path(a.run) / "prescreen_recall.json").write_text(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
