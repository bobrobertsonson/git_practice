"""Gate matched to the reference (v0.4M Task B).

The gate is keyed on the DI and sits in front of the NAMs. Until stage 2 it is the fixed "DI floor + 4 dB / hold 40 ms /
release 150 ms / range -50 dB" gate (``space.gate_preset``); once the final chain is known, ``gate_sweep`` does a coordinate
descent on it (<= 15 renders of the chain's NAM cores, the linear part is the memoised cheap one): threshold = DI floor +
{4, 8, ..., 36} dB, then hold {2, 10, 40} ms, release {20, 80, 150, 250} ms, range {-50, -90} dB. Each step keeps the cell
whose inter-note level is closest to the reference's: the feel ``floor`` term of ``feel.py`` (output power in the DI's gap
regions re its active power, against the reference's), subject to

* the A-weighted LTAS error rising by at most ``LTAS_TOL_DB`` (0.05 dB) over the default cell, and
* the tightness term not getting worse than the default cell.

The default cell is a member of the grid, so the choice is never worse than not sweeping. Matched pair: the
reference's floor is measured at the same DI gaps. Soft target (no matched pair): the reference's own inter-note floor
(``reference_floor_db``: its own gap regions); a mix without real silence has none and the sweep is skipped (recorded).

Gap source (Task H.2): when the excerpt has less than 100 ms of DI gaps the sweep takes its gap regions from the full-length DI
(``gateSweep.gapSource: "fullDi"``, else ``"excerpt"``). Each cell then renders the chain over up to ``MAX_GAP_WINDOWS`` of the
longest full-DI gap windows (each from ``GAP_PREROLL_S`` before the gap so the chain and the gate carry the preceding note's
state) and the floor is the output power in those windows re the excerpt's active power; for a matched pair the reference's
floor is its power at the same windows re its active power in the excerpt.
"""
from __future__ import annotations

import numpy as np

from ..tonecheck.analysis import activity_mask, gap_regions
from . import feel as _feel
from . import loss as L
from .engine import RATE, Engine
from .screen import Scored
from .space import Space, gate_preset

GATE_OFFSETS_DB = (4.0, 8.0, 12.0, 16.0, 20.0, 24.0, 28.0, 32.0, 36.0)
GATE_HOLDS_MS = (2.0, 10.0, 40.0)
GATE_RELEASES_MS = (20.0, 80.0, 150.0, 250.0)
GATE_RANGES_DB = (-50.0, -90.0)
DEFAULT_CELL = (4.0, 40.0, 150.0, -50.0)       # (threshold offset dB, hold ms, release ms, range dB) = space.gate_preset
LTAS_TOL_DB = 0.05
TIGHT_TOL = 0.05      # normalised tightness term: a cell may be this much worse than the default cell's (noise level of the statistic)
MIN_IMPROVEMENT = 1e-3           # the floor term must fall by at least this to leave the current cell
THRESHOLD_MAX_DB = -6.0          # keep the swept threshold inside the schema range
MAX_GAP_WINDOWS = 6              # full-DI gap windows rendered per cell when the excerpt has no gaps (the longest ones)
MAX_GAP_WINDOW_S = 1.0           # ... of at most this length each
GAP_PREROLL_S = 0.5              # DI before each window that is rendered with it (chain / gate state, the preceding note's tail)


def cell_feasible(r: dict, base: dict) -> bool:
    """Acceptance rule of a gate cell against the default cell ``base``: its floor term is measurable, its LTAS error is at most
    ``LTAS_TOL_DB`` above the default's and its (normalised) tightness term at most ``TIGHT_TOL`` above the default's."""
    ok = r["floorTerm"] is not None and r["ltas"] <= base["ltas"] + LTAS_TOL_DB
    if ok and r["tight"] is not None and base["tight"] is not None:
        ok = r["tight"] <= base["tight"] + TIGHT_TOL
    return ok


def cell_gate(floor_db: float, offset_db: float, release_ms: float = DEFAULT_CELL[2], hold_ms: float = DEFAULT_CELL[1],
              range_db: float = DEFAULT_CELL[3]) -> dict:
    """The matcher's gate (``gate_preset``) with threshold = DI floor + ``offset_db`` and the given release / hold / range."""
    thr = min(round(max(floor_db, -90.0) + offset_db, 2), THRESHOLD_MAX_DB)
    return gate_preset(floor_db, {"thresholdDb": thr, "releaseMs": float(release_ms), "holdMs": float(hold_ms),
                                  "rangeDb": float(range_db)})


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


class _Window:
    """A DI segment standing in for an excerpt (no lead): ``eng.core`` renders ``x``, ``trim`` is the identity."""

    def __init__(self, x: np.ndarray):
        self.x = np.ascontiguousarray(x, dtype=np.float32)
        self.lead, self.n = 0, len(self.x)

    def trim(self, y):
        return y


def full_gap_windows(di: np.ndarray) -> list[tuple[int, int, int]]:
    """(render start, gap start, gap end) of the longest full-DI gap regions (``gap_regions``), each gap cut to
    ``MAX_GAP_WINDOW_S``, the render start ``GAP_PREROLL_S`` earlier. Empty when the DI has under ``FLOOR_MIN_S`` of gaps."""
    gaps = [(a, min(b, a + int(MAX_GAP_WINDOW_S * RATE))) for a, b in gap_regions(np.asarray(di, dtype=np.float64), RATE) if b > a]
    if sum(b - a for a, b in gaps) < _feel.FLOOR_MIN_S * RATE:
        return []
    gaps = sorted(gaps, key=lambda g: g[0] - g[1])[:MAX_GAP_WINDOWS]
    return [(max(0, a - int(GAP_PREROLL_S * RATE)), a, b) for a, b in sorted(gaps)]


def _gap_power(sig_at, wins) -> float | None:
    """Mean power over the gap parts of the windows; ``sig_at(s, e)`` returns the signal on [s, e) (None: unavailable)."""
    parts = []
    for s, a, b in wins:
        seg = sig_at(s, b)
        if seg is None or len(seg) < b - s:
            return None
        parts.append(np.asarray(seg[a - s:b - s], dtype=np.float64))
    return float(np.mean(np.concatenate(parts) ** 2)) if parts else None


def render_gate(eng: Engine, cand: Scored, ex, gate: dict) -> np.ndarray:
    """Excerpt output of the candidate's chain (its stage-2 parameters) with ``gate`` in front of the NAMs."""
    v = cand.extra["params"]
    paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
    cores = [eng.core(cand.combo, v, p, ex.x, gate=gate) for p in paths]
    y = ex.trim(eng.emulate(cand.combo, v, cores[0], cores[1] if len(cores) > 1 else None, cand.align, cand.levels))
    return eng.apply_comp(y, cand.extra.get("busComp"), cand.combo.cab)      # studio processing, when the matcher added it


def gate_sweep(eng: Engine, cand: Scored, space: Space, ex, tgt: L.Target, floor_db: float,
               ref_floor_db: float | None = None, ref_clean: bool = True, full: dict | None = None) -> dict:
    """Coordinate-descent sweep of the gate on ``cand`` (a refined candidate): threshold offset (9 values) at the default hold /
    release / range, then hold, then release, then range, each step keeping the best cell under the acceptance rule (module
    docstring). ``tgt`` must carry the feel target (the sweep measures the feel ``floor`` and tightness terms even when the
    search itself runs with the feel term ablated). ``ref_floor_db``: the reference's own floor, used when there is no matched
    pair. ``full``: ``{"di": full-length DI at 48 kHz, "ref": the matched reference channel (or None), "offset": reference index of
    DI sample 0}``, used for the gap regions when the excerpt has none (module docstring). Returns the record written to ``result.json -> gateSweep``: every rendered cell (``grid``), the ``steps``, the default
    cell (``baseline``), the pick (``picked``) and the gate to use (``gate``)."""
    ft = tgt.feel
    base_gate = cell_gate(floor_db, DEFAULT_CELL[0])
    out: dict = {"diNoiseFloorDb": floor_db, "method": "coordinate descent: threshold, hold, release, range",
                 "offsetsDb": list(GATE_OFFSETS_DB), "holdsMs": list(GATE_HOLDS_MS), "releasesMs": list(GATE_RELEASES_MS),
                 "rangesDb": list(GATE_RANGES_DB), "ltasToleranceDb": LTAS_TOL_DB, "gate": base_gate, "changed": False,
                 "skipped": None, "gapSource": "excerpt"}
    if ft is None:
        out["skipped"] = "no feel target"
        return out
    paired = ft.mode == "paired"
    out["mode"] = ft.mode
    if "floor" in getattr(ft, "off", ()):      # the target switched the floor term off (e.g. the matched channel is a mix)
        out["skipped"] = ft.dropped.get("floor") or "floor term is off for this reference"
        return out
    if not paired and not ref_clean:
        out["skipped"] = "reference is not a clean guitar track"
        return out
    wins: list = []
    if not ft.plan.gap_ok:
        wins = full_gap_windows(full["di"]) if full is not None else []
        if not wins:
            out["skipped"] = (f"less than {int(_feel.FLOOR_MIN_S * 1000)} ms of DI gaps in the excerpt" +
                              (" or in the full DI" if full is not None else ""))
            return out
        out["gapSource"] = "fullDi"
        out["gapWindows"] = [{"startS": s / RATE, "gapStartS": a / RATE, "gapEndS": b / RATE} for s, a, b in wins]
    if wins:                   # gaps from the full DI: the reference's floor at the same windows re its active power in the excerpt
        act = tgt.matched[ft.plan.active_idx] if paired and getattr(tgt, "matched", None) is not None else None
        if paired:
            rg = None
            if full.get("ref") is not None and act is not None and len(act):
                off, rsig = int(full.get("offset", 0)), full["ref"]
                rg = _gap_power(lambda s, e: rsig[s + off:e + off] if s + off >= 0 else None, wins)
            ref_floor = None if rg is None else float(max(
                10.0 * np.log10(max(rg, 1e-30)) - 10.0 * np.log10(max(float(np.mean(np.asarray(act, np.float64) ** 2)), 1e-30)),
                _feel.LEVEL_FLOOR_DB))
        else:
            ref_floor = ref_floor_db
    else:
        ref_floor = ft.ref.floor if paired else ref_floor_db
    if ref_floor is None:
        out["skipped"] = ("the reference's gaps could not be measured in the matched channel" if paired else
                          "the reference has no real-silence gaps of its own (a mix); nothing to match the floor to")
        return out
    out["floorRefDb"] = float(ref_floor)
    out["floorRefSource"] = "matched reference at the DI's gaps" if paired else "the reference's own inter-note gaps"
    if wins:
        out["floorRefSource"] += " (full-length DI gap windows)" if paired else ""
    eq = space.eq_gains(cand.extra["params"])

    def job(cell):
        g = cell_gate(floor_db, cell[0], cell[2], cell[1], cell[3])
        y = render_gate(eng, cand, ex, g)
        r = L.evaluate(y, tgt, eq)
        if wins:               # floor from the full-DI gap windows, re the excerpt's active power
            fo = None
            act_p = float(np.mean(np.asarray(y, dtype=np.float64)[ft.plan.active_idx] ** 2)) if len(ft.plan.active_idx) else 0.0
            gp = _gap_power(lambda s_, e_: render_gate(eng, cand, _Window(full["di"][s_:e_]), g), wins)
            if gp is not None and act_p > 0:
                fo = float(max(10.0 * np.log10(max(gp, 1e-30)) - 10.0 * np.log10(act_p), _feel.LEVEL_FLOOR_DB))
        else:
            fo = _feel.floor_db(np.asarray(y, dtype=np.float64), ft.plan)
        t = (r.feel_terms or {}).get("tight")
        return {"offsetDb": cell[0], "holdMs": cell[1], "releaseMs": cell[2], "rangeDb": cell[3],
                "thresholdDb": g["thresholdDb"], "loss": r.total, "ltas": r.ltas, "tight": t, "floorDbOut": fo,
                "floorTerm": None if fo is None else _feel.floor_term(fo, ref_floor)}

    done: dict = {}

    def run(cells):
        new = [c for c in cells if c not in done]
        for c, row in zip(new, eng.map(job, new)):
            done[c] = row
        return [done[c] for c in cells]

    run([DEFAULT_CELL])
    base = done[DEFAULT_CELL]

    def feasible(r):
        return cell_feasible(r, base)

    cur, steps = DEFAULT_CELL, []
    axes = (("thresholdOffsetDb", 0, GATE_OFFSETS_DB), ("holdMs", 1, GATE_HOLDS_MS), ("releaseMs", 2, GATE_RELEASES_MS),
            ("rangeDb", 3, GATE_RANGES_DB))
    for name, i, values in axes:
        rows = run([tuple(v if k == i else c for k, c in enumerate(cur)) for v in values])
        for r in rows:
            r["feasible"] = bool(feasible(r))
        feas = [r for r in rows if r["feasible"]]
        pick = cur
        if feas and done[cur]["floorTerm"] is not None:
            best = min(feas, key=lambda r: (r["floorTerm"], r["thresholdDb"], -r["holdMs"]))
            if best["floorTerm"] < done[cur]["floorTerm"] - MIN_IMPROVEMENT:
                pick = (best["offsetDb"], best["holdMs"], best["releaseMs"], best["rangeDb"])
        steps.append({"axis": name, "tried": list(values), "picked": pick[i], "changed": pick != cur})
        cur = pick
    base["feasible"] = True
    picked = done[cur]
    out.update(grid=list(done.values()), steps=steps, baseline=base, picked=picked, renders=len(done))
    if cur != DEFAULT_CELL:
        out["changed"] = True
        out["gate"] = cell_gate(floor_db, cur[0], cur[2], cur[1], cur[3])
    return out
