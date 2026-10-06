"""Gate matched to the reference (v0.4M Task B).

The gate is keyed on the DI and sits in front of the NAMs. Until stage 2 it is the fixed "DI floor + 4 dB / 150 ms" gate
(``space.gate_preset``); once the final chain is known, ``gate_sweep`` renders it with threshold = DI floor + {4, 8, 12, 16, 20} dB
x release {80, 150, 250} ms (15 NAM renders per path, the linear part is the memoised cheap one) and picks the cell whose
inter-note level is closest to the reference's: the feel ``floor`` term of ``feel.py`` (output power in the DI's gap regions
re its active power, against the reference's), subject to

* the A-weighted LTAS error rising by at most ``LTAS_TOL_DB`` (0.05 dB) over the default cell, and
* the tightness term not getting worse than the default cell.

The default cell (4 dB, 150 ms) is a member of the grid, so the choice is never worse than not sweeping. Matched pair: the
reference's floor is measured at the same DI gaps. Soft target (no matched pair): the reference's own inter-note floor
(``reference_floor_db``: its own gap regions); a mix without real silence has none and the sweep is skipped (recorded).
"""
from __future__ import annotations

import numpy as np

from ..tonecheck.analysis import activity_mask, gap_regions
from . import feel as _feel
from . import loss as L
from .engine import RATE, Engine
from .screen import Scored
from .space import Space, gate_preset

GATE_OFFSETS_DB = (4.0, 8.0, 12.0, 16.0, 20.0)
GATE_RELEASES_MS = (80.0, 150.0, 250.0)
DEFAULT_CELL = (4.0, 150.0)
LTAS_TOL_DB = 0.05
TIGHT_TOL = 1e-9
MIN_IMPROVEMENT = 1e-3           # the floor term must fall by at least this to leave the default cell
THRESHOLD_MAX_DB = -6.0          # keep the swept threshold inside the schema range


def cell_gate(floor_db: float, offset_db: float, release_ms: float) -> dict:
    """The matcher's gate (``gate_preset``) with threshold = DI floor + ``offset_db`` and the given release."""
    thr = min(round(max(floor_db, -90.0) + offset_db, 2), THRESHOLD_MAX_DB)
    return gate_preset(floor_db, {"thresholdDb": thr, "releaseMs": float(release_ms)})


def reference_floor_db(sig: np.ndarray) -> float | None:
    """The reference's own inter-note floor (soft target): power in its real-silence gaps (``gap_regions``: 10 ms RMS below
    -50 dBFS for >= 120 ms) re its active power, dB. None when it has less than 100 ms of such gaps (a full mix has none)."""
    x = np.asarray(sig, dtype=np.float64)
    gaps = [(a, b) for a, b in gap_regions(x, RATE) if b > a]
    if sum(b - a for a, b in gaps) < _feel.FLOOR_MIN_S * RATE:
        return None
    mask, _, _ = activity_mask(x, RATE)
    if not mask.any():
        return None
    g = float(np.mean(np.concatenate([x[a:b] for a, b in gaps]) ** 2))
    a = float(np.mean(x[mask] ** 2))
    return float(max(10.0 * np.log10(max(g, 1e-30)) - 10.0 * np.log10(max(a, 1e-30)), _feel.LEVEL_FLOOR_DB))


def render_gate(eng: Engine, cand: Scored, ex, gate: dict) -> np.ndarray:
    """Excerpt output of the candidate's chain (its stage-2 parameters) with ``gate`` in front of the NAMs."""
    v = cand.extra["params"]
    paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
    cores = [eng.core(cand.combo, v, p, ex.x, gate=gate) for p in paths]
    return ex.trim(eng.emulate(cand.combo, v, cores[0], cores[1] if len(cores) > 1 else None, cand.align, cand.levels))


def gate_sweep(eng: Engine, cand: Scored, space: Space, ex, tgt: L.Target, floor_db: float,
               ref_floor_db: float | None = None) -> dict:
    """Sweep the 15 gates on ``cand`` (a refined candidate). ``tgt`` must carry the feel target (the sweep measures the
    feel ``floor`` and tightness terms even when the search itself runs with the feel term ablated). ``ref_floor_db``:
    the reference's own floor, used when there is no matched pair. Returns the record written to ``result.json ->
    gateSweep``: the whole grid, the default cell, the pick (``picked``) and the gate to use (``gate``)."""
    ft = tgt.feel
    base_gate = cell_gate(floor_db, *DEFAULT_CELL)
    out: dict = {"diNoiseFloorDb": floor_db, "offsetsDb": list(GATE_OFFSETS_DB), "releasesMs": list(GATE_RELEASES_MS),
                 "ltasToleranceDb": LTAS_TOL_DB, "gate": base_gate, "changed": False, "skipped": None}
    if ft is None:
        out["skipped"] = "no feel target"
        return out
    paired = ft.mode == "paired"
    out["mode"] = ft.mode
    if not ft.plan.gap_ok:
        out["skipped"] = f"less than {int(_feel.FLOOR_MIN_S * 1000)} ms of DI gaps in the excerpt"
        return out
    ref_floor = ft.ref.floor if paired else ref_floor_db
    if ref_floor is None:
        out["skipped"] = ("the reference's gaps could not be measured in the matched channel" if paired else
                          "the reference has no real-silence gaps of its own (a mix); nothing to match the floor to")
        return out
    out["floorRefDb"] = float(ref_floor)
    out["floorRefSource"] = "matched reference at the DI's gaps" if paired else "the reference's own inter-note gaps"
    eq = space.eq_gains(cand.extra["params"])
    cells = [(o, r) for o in GATE_OFFSETS_DB for r in GATE_RELEASES_MS]

    def job(cell):
        g = cell_gate(floor_db, *cell)
        y = render_gate(eng, cand, ex, g)
        r = L.evaluate(y, tgt, eq)
        fo = _feel.floor_db(np.asarray(y, dtype=np.float64), ft.plan)
        t = (r.feel_terms or {}).get("tight")
        return {"offsetDb": cell[0], "releaseMs": cell[1], "thresholdDb": g["thresholdDb"], "loss": r.total, "ltas": r.ltas,
                "tight": t, "floorDbOut": fo, "floorTerm": None if fo is None else _feel.floor_term(fo, ref_floor)}

    grid = eng.map(job, cells)
    base = next(r for r in grid if (r["offsetDb"], r["releaseMs"]) == DEFAULT_CELL)
    for r in grid:
        ok = r["floorTerm"] is not None and r["ltas"] <= base["ltas"] + LTAS_TOL_DB
        if ok and r["tight"] is not None and base["tight"] is not None:
            ok = r["tight"] <= base["tight"] + TIGHT_TOL
        r["feasible"] = bool(ok)
    out.update(grid=grid, baseline=base, picked=base)
    feas = [r for r in grid if r["feasible"]]
    if feas and base["floorTerm"] is not None:
        best = min(feas, key=lambda r: (r["floorTerm"], r["thresholdDb"], abs(r["releaseMs"] - DEFAULT_CELL[1])))
        if best["floorTerm"] < base["floorTerm"] - MIN_IMPROVEMENT:
            out["picked"] = best
            out["changed"] = True
            out["gate"] = cell_gate(floor_db, best["offsetDb"], best["releaseMs"])
    return out
