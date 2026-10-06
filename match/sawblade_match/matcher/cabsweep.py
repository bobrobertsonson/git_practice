"""Cab/IR breadth (v0.4M Task B): after stage 2, every cab of the pool is scored on the top refined candidates.

The cab is the last linear stage, so a candidate's NAM cores (memoised by the engine) are rendered once and each IR only
costs the two cheap linear renders + the full loss. ``cab_sweep`` is deliberately a separate function with the contract
"candidate + list of cabs -> one row per cab with its full loss": the analytic IR screen of Task B3 (thousands of IRs)
replaces it for large pools and keeps the same rows for the top-N IRs it passes on to full renders.
"""
from __future__ import annotations

from . import loss as L
from .engine import Engine
from .pool import Capture
from .screen import Scored
from .space import Space

TOP_PER_TOPOLOGY = 3        # refined candidates per topology that get the sweep


def feel_row(r: L.LossResult) -> dict:
    """The feel sub-terms of a result, normalised units, for the reports (None where a term was dropped)."""
    t = r.feel_terms or {}
    return {"feel": r.feel, "tight": t.get("tight"), "fizz": t.get("fizz"), "polish": t.get("polish")}


def cab_sweep(eng: Engine, cand: Scored, cabs: list[Capture], space: Space, ex, tgt: L.Target) -> list[dict]:
    """Full loss of the refined candidate ``cand`` (its stage-2 parameters) with each cab of ``cabs``, input order.
    Rows: ``{"cab": Capture, "result": LossResult}``."""
    v = cand.extra["params"]
    paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
    cores = [eng.core(cand.combo, v, p, ex.x) for p in paths]       # memo hits after stage 2
    ca, cb = cores[0], (cores[1] if len(cores) > 1 else None)
    eq = space.eq_gains(v)

    def job(cab: Capture) -> dict:
        y = ex.trim(eng.emulate(cand.combo.with_cab(cab), v, ca, cb, cand.align, cand.levels))
        return {"cab": cab, "result": L.evaluate(y, tgt, eq)}

    return eng.map(job, list(cabs))


def sweep_summary(cand: Scored, rows: list[dict]) -> dict:
    """JSON block of one swept candidate: every IR's loss (ascending), best / worst, whether the best differs from the
    candidate's own cab. ``changed`` is set by the caller once the cab was actually replaced."""
    cur = cand.combo.cab.key
    ranked = sorted(rows, key=lambda r: r["result"].total)
    irs = [{"cab": r["cab"].key, "toneId": r["cab"].tone_id, "title": r["cab"].title, "name": r["cab"].name,
            "loss": r["result"].total, "ltas": r["result"].ltas, **feel_row(r["result"]), "current": r["cab"].key == cur}
           for r in ranked]
    return {"topology": cand.topology, "boost": bool(cand.combo.boost), "currentCab": cur, "nCabs": len(rows),
            "best": irs[0] if irs else None, "worst": irs[-1] if irs else None, "irs": irs,
            "currentLoss": next((i["loss"] for i in irs if i["current"]), None), "changed": False}
