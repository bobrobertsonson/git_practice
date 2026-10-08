"""Pre-screen recall check (spec 3.3 A): does the per-capture pre-screen keep the captures the full search picks?

Reads a finished run (``result.json``: stage-1 ranking of the full pair search and the refined candidates), recomputes the
pre-screen on the same excerpt/target for several N (top N per gear class), and reports

* recall@M: fraction of the full search's top-M stage-1 combos (per topology) whose every NAM survives the pre-screen;
* whether the selected best combo's captures all survive;
* capture recall: fraction of the distinct NAM captures in the top-30 that survive.

``python -m sawblade_match.matcher.recall --run RUN_DIR --di DI.wav --ref REF [--matched left] --pool POOL --ns 2,3,4,6``
(the run must have used the full pair search, i.e. no pre-screen).

Phase 6b additions:

* ``--quick`` also scores the quick mode's pre-screen (blend-aware, on the coarse excerpt, 4 pedals per class and the pair
  cap spent on amps) next to the plain per-class pre-screen at the same cost;
* ``--old-pool`` restricts the pool to the captures the full-search run actually used (read from its stage-1 pair list), so
  a pool that has grown since is compared like for like;
* ``--quick-run QUICK_DIR`` (no rendering): recall of the full search's top-M combos against the pairs a finished quick run
  pre-screened and then rendered at full length (``coarse`` stage), i.e. the whole quick screening funnel.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from ..tonecheck.analysis import activity_mask  # noqa: F401
from .calibration import di_rule_from_run, options_from_run, pick_di_channel
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
    p.add_argument("--quick", action="store_true", help="also evaluate the quick mode's blend-aware pre-screen")
    p.add_argument("--coarse-s", type=float, help="window of the quick pre-screen (default: Plan.quick().coarse_s)")
    p.add_argument("--old-pool", action="store_true", help="restrict the pool to the captures of the full-search run")
    p.add_argument("--quick-run", help="a finished --quick run dir: funnel recall of the full search's top-M (no rendering)")
    a = p.parse_args(argv)
    res = json.loads((Path(a.run) / "result.json").read_text())
    if res["stage1"].get("prescreen", {}).get("appliedToBlendSingle"):
        print("error: the run used a pre-screen; recall needs a full-search run", file=sys.stderr)
        return 3
    pool = load_pool(a.pool)
    if a.old_pool:
        used = {k for pr in res["stage1"]["sampledPairs"] for k in [*pr[0], pr[1]]}
        pool.pedals = [c for c in pool.pedals if c.key in used]
        pool.amps = [c for c in pool.amps if c.key in used]
        print(f"old pool: {len(pool.pedals)} pedals, {len(pool.amps)} amps")
    ref = load_reference(a.ref, channel="auto", matched=a.matched)
    di, fs = sf.read(a.di, dtype="float32")
    di48 = to48(pick_di_channel(di, di_rule_from_run(res))[0], fs)         # the run's DI-channel rule
    window = (int(res["excerpt"]["startS"] * RATE), int(res["excerpt"]["endS"] * RATE))
    ex = make_excerpt(di48, window[1] / RATE - window[0] / RATE, window=window, ref=ref)
    off = res.get("offsetRefinement", {}).get("excerptStarterRender", {}).get("offset")
    tgt = build_target(ref, ex, offset_samples=off) if ref.matched_sig is not None else build_target(ref, ex)
    eng = Engine(res["gate"], a.threads, calibration=options_from_run(res, {}))
    top = {t: lst for t, lst in res["stage1"]["top"].items() if t in ("blend", "single")}
    best = _keys(res["best"]["captures"])
    allcaps = {k for lst in top.values() for e in lst[:30] for k in _keys(e["captures"])}
    out = {"run": a.run, "fullPairs": res["stage1"]["fullPairs"], "perN": {}, "oldPool": a.old_pool}

    def recall_row(kept, extra=None):
        row = {"bestComboKept": best <= kept, "captureRecallTop30": len(allcaps & kept) / max(len(allcaps), 1)}
        for t, lst in top.items():
            for m in (5, 10, 30):
                sel = lst[:m]
                row[f"{t}RecallAt{m}"] = sum(_keys(e["captures"]) <= kept for e in sel) / max(len(sel), 1)
        return {**(extra or {}), **row}

    if a.quick_run:
        q = json.loads((Path(a.quick_run) / "result.json").read_text())
        pre = q["stage1"].get("prescreen", {})
        kept = set(pre.get("keptPedals", [])) | set(pre.get("keptAmps", []))
        final_pairs = {frozenset([*pr[0], pr[1]]) for pr in q["stage1"]["sampledPairs"]}
        rows = {"prescreen": recall_row(kept, {"pedalsKept": len(pre.get("keptPedals", [])),
                                               "ampsKept": len(pre.get("keptAmps", []))})}
        # funnel: a combo survives when each of its paths (pedals + amp) is one of the pairs rendered at full length
        def pair_ok(cap):
            for path in (("a_pedal", "a_amp"), ("b_pedal", "b_amp")) if "a_amp" in cap else (("pedal", "amp"),):
                ks = [cap[k] for k in path if cap.get(k)]
                if ks and frozenset(f"{c['toneId']}/{c['modelId']}" for c in ks) not in final_pairs:
                    return False
            return True
        funnel = {}
        for t, lst in top.items():
            for m in (5, 10, 30):
                sel = lst[:m]
                funnel[f"{t}RecallAt{m}"] = sum(pair_ok(e["captures"]) for e in sel) / max(len(sel), 1)
        rows["funnel"] = {"pairsFullLength": len(final_pairs), "bestComboKept": pair_ok(res["best"]["captures"]), **funnel}
        out["quickRun"] = {"dir": a.quick_run, "rows": rows}
        print(json.dumps(rows, indent=1))
        (Path(a.run) / "quick_funnel_recall.json").write_text(json.dumps(out, indent=2))
        return 0
    try:
        for n in [int(x) for x in a.ns.split(",")]:
            kp, ka, info = prescreen(eng, pool, ex, tgt, default_cab(pool.cabs), n, lambda *_: None)
            kept = {c.key for c in kp} | {c.key for c in ka}
            row = recall_row(kept, {"pedalsKept": len(kp), "ampsKept": len(ka), "pairsAfter": (len(kp) + 1) * len(ka)})
            out["perN"][n] = row
            print(n, json.dumps(row))
        if a.quick:
            from .excerpt import select_excerpt
            from .prescreen import auto_n_amps
            from .run import Plan
            qp = Plan.quick()
            cs = a.coarse_s or qp.coarse_s
            a0, b0, _ = select_excerpt(ex.x[ex.lead:], RATE, cs)
            cex = make_excerpt(di48, cs, lead_s=0.2, window=(ex.start + a0, ex.start + b0), ref=ref)
            ctgt = build_target(ref, cex, offset_samples=off) if ref.matched_sig is not None else build_target(ref, cex)
            cp, ca_ = len({p.kind for p in pool.pedals}), len({x.kind for x in pool.amps})
            n_a = auto_n_amps(cp, ca_, qp.prescreen_n_ped + qp.blend_extra_ped, qp.cap_pairs, qp.blend_extra_amp)
            for ba in (False, True):       # same pedal/amp counts: plain (singles only, quota incl. extras) vs blend-aware
                kw = dict(n_pedals_per_class=qp.prescreen_n_ped + (0 if ba else qp.blend_extra_ped),
                          blend_aware=ba, n_blend_pedals=qp.blend_extra_ped if ba else 0,
                          n_blend_amps=qp.blend_extra_amp if ba else 0)
                kp, ka, info = prescreen(eng, pool, cex, ctgt, default_cab(pool.cabs),
                                         n_a + (0 if ba else qp.blend_extra_amp), lambda *_: None, **kw)
                kept = {c.key for c in kp} | {c.key for c in ka}
                key = ("quickBlendAware" if ba else "quickPlainSameBudget") + f"@{cs:g}s"
                row = recall_row(kept, {"pedalsKept": len(kp), "ampsKept": len(ka), "pairsAfter": (len(kp) + 1) * len(ka)})
                out["perN"][key] = row
                print(key, json.dumps(row))
    finally:
        eng.close()
    (Path(a.run) / "prescreen_recall.json").write_text(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
