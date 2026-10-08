"""Dynamics sweep of a matched preset (v0.4M Task F.4).

    python -m sawblade_match.matcher.dynsweep --result <run>/result.json --di <di.wav> [--json out.json]

Renders the winning preset over the full DI at input offsets -12, -6, 0 and +6 dB (the DI is scaled before the chain), twice:
dynamics as matched (the RECORD set, what the matcher scored) and bypassed (gate and bus comp both disabled), plus the LIVE
set (``dynamicsMode: "live"``: what a rig plays; for a match preset the derived floor-following expander, bus comp off): 12 renders. Per render: integrated
LUFS (BS.1770), the median 400 ms crest factor (``feel.crest_values``: the feel term's own active windows of the DI) and the
inter-note floor (``feel.floor_db`` at the DI's gaps; null when the DI has too little gap). Per adjacent step the slope is
dLUFS_out / dB_in (1.0 = the output follows the input; a gate shows a knee at the low step, a compressor flattens it). The
table prints matched vs bypassed and the largest |slope difference|. Exit 0 unless the inputs are unreadable (2). Deterministic.
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

import numpy as np

from ..tonecheck.analysis import activity_mask, detect_onsets, gap_regions
from . import feel as FEEL
from .dynformat import format_table, _f      # noqa: F401  (format_table is re-exported)
from .engine import RATE, Engine, to48
from .loudness import integrated_lufs
from .calibration import di_rule_from_run, options_from_run, pick_di_channel
from .pathcheck import PathcheckError, load_run
from .run import _finite_or_none

OFFSETS_DB = (-12.0, -6.0, 0.0, 6.0)


def bypassed(preset: dict) -> dict:
    """The preset with the gate and the bus compressor disabled (everything else unchanged)."""
    p = copy.deepcopy(preset)
    p["gate"] = {**(p.get("gate") or {}), "enabled": False}
    if p.get("busComp"):
        p["busComp"] = {**p["busComp"], "enabled": False}
    return p


def _measure(y: np.ndarray, fs: int, plan) -> dict:
    y48 = to48(y, fs) if fs != RATE else np.asarray(y, dtype=np.float32)
    y64 = np.asarray(y48, dtype=np.float64)
    if len(y64) < plan.n:
        y64 = np.concatenate([y64, np.zeros(plan.n - len(y64))])
    y64 = y64[:plan.n]
    lufs = float(integrated_lufs(np.asarray(y, dtype=np.float64), fs))
    cr = FEEL.crest_values(y64, plan) if len(plan.crest_starts) else np.zeros(0)
    fl = FEEL.floor_db(y64, plan) if plan.gap_ok else None
    return {"lufs": lufs, "crestDb": float(np.median(cr)) if len(cr) else None, "floorDb": None if fl is None else float(fl)}


def slopes(lufs: list[float], offsets=OFFSETS_DB) -> list[float | None]:
    out = []
    for i in range(len(offsets) - 1):
        a, b = lufs[i], lufs[i + 1]
        out.append(float((b - a) / (offsets[i + 1] - offsets[i])) if np.isfinite(a) and np.isfinite(b) else None)
    return out


def dynsweep(result_path, di_path, engine: Engine | None = None, offsets=OFFSETS_DB, di_channel: str | None = None) -> dict:
    res, preset, pname, x, fs = load_run(Path(result_path), di_path)
    x, _ = pick_di_channel(x, di_channel or di_rule_from_run(res))      # the run's rule (louder channel by default)
    di48 = to48(x, fs)
    di64 = di48.astype(np.float64)
    mask, _, _ = activity_mask(di64, RATE)
    plan = FEEL._Plan(len(di48), mask, detect_onsets(di64, RATE), gap_regions(di64, RATE))
    own = engine is None
    eng = engine or Engine(None, 1, calibration=options_from_run(res, preset))
    sets: dict = {}
    try:
        for name, p, dyn in (("matched", preset, "record"), ("bypassed", bypassed(preset), "record"), ("live", preset, "live")):
            rows = []
            for off in offsets:
                xs = np.ascontiguousarray(x * np.float32(10 ** (off / 20)), dtype=np.float32)
                y, _ = eng.render(p, xs, fs, dynamics=dyn)
                rows.append({"inputDb": float(off), **_measure(y, fs, plan)})
            sets[name] = {"rows": rows, "slopes": slopes([r["lufs"] for r in rows], offsets)}
    finally:
        if own:
            eng.close()
    sm, sb = sets["matched"]["slopes"], sets["bypassed"]["slopes"]
    diffs = [None if a is None or b is None else a - b for a, b in zip(sm, sb)]
    ok = [abs(d) for d in diffs if d is not None]
    g, bc = preset.get("gate") or {}, preset.get("busComp") or {}
    from ..core import resolve_dynamics
    try:
        lv = resolve_dynamics({**preset, "dynamicsMode": "live"})
    except Exception as e:                                 # reporting only: say so, leave the live-set fields empty (None)
        print(f"dynsweep: cannot resolve the live dynamics set ({type(e).__name__}: {e}); live gate / bus comp fields are null",
              file=sys.stderr)
        lv = None
    sl = sets["live"]["slopes"]
    ld = [abs(a - b) for a, b in zip(sl, sb) if a is not None and b is not None]
    return _finite_or_none({
        "schema": "sawblade.dynsweep", "version": 1, "result": str(result_path), "preset": str(Path(result_path).parent / pname),
        "di": str(di_path), "inputOffsetsDb": list(offsets), "gateEnabled": bool(g.get("enabled")),
        "busCompEnabled": bool(bc.get("enabled")), **sets,
        "liveGateEnabled": None if lv is None else bool(lv["gate"].get("enabled")),
        "liveBusCompEnabled": None if lv is None else bool(lv["busComp"].get("enabled")),
        "liveMaxAbsSlopeDiff": max(ld) if ld else None, "slopeDiff": diffs,
        "maxAbsSlopeDiff": max(ok) if ok else None, "randomness": "none (deterministic)"})


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="dynsweep", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--result", required=True)
    ap.add_argument("--di", required=True)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)
    try:
        r = dynsweep(args.result, args.di)
    except PathcheckError as e:
        print(f"dynsweep: {e}", file=sys.stderr)
        return 2
    print(format_table(r))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(r, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
