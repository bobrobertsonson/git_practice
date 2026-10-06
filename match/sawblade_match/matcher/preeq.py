"""Pre-EQ before the drive (v0.4M B4): a small pruned grid on the per-path ``preEq``, searched after stage 1's re-score.

A pre-EQ changes the NAM input, so every setting costs one NAM re-render; the grid is therefore pruned (coordinate descent,
<= 12 settings per path, plus the guitar-difference widening options) and only the winner goes on into stage 2:

1. HPF {off, 80, 110, 150 Hz} (12 dB/oct), the others off;
2. at the best HPF, a mid peak {+3, +6 dB} x {700, 900 Hz} (Q 0.8);
2b. (not in the first spec, added so that the grid recovers an HPF that only shows next to the mid peak) when a mid peak was
   kept, the HPF options once more at that peak;
3. at the best so far, a low shelf of -3 dB at 200 Hz;
4. up to 3 joint neighbours of the best (one step in two dimensions), as far as the 12 settings per path allow.

A setting is kept only if it beats the previous best by >= ``KEEP_DB`` (0.02) in the full loss (feel term included). A blend's
two paths are searched in turn, each on its own NAM core with the other path as it stands.

Guitar-difference widening, from the DI's own LTAS (active segments): ``diTilt`` is the least-squares slope in dB/octave over the
1/3-octave bands 100 Hz-3 kHz and ``diLowExcess`` the mean level 80-200 Hz minus 200-800 Hz. A dark DI (``diTilt`` < -4.5)
adds a +9 dB mid option, a bright one (> -1.5) an HPF at 180 Hz, a bassy one (``diLowExcess`` > +3 dB) a -6 dB shelf option.
The thresholds are provisional (checked on the fixture DI and the user's DIs: the run prints both numbers).
"""
from __future__ import annotations

import numpy as np

from . import loss as L
from .engine import Engine
from .screen import Scored
from .space import PRE_HPF_OPTIONS, PRE_MID_OPTIONS, PRE_SHELF_DB, PRE_SHELF_HZ, Space

KEEP_DB = 0.02
MAX_NEIGHBOURS = 3
MAX_RENDERS = 12           # settings per path besides the off setting and the widening options
TILT_DARK, TILT_BRIGHT, LOW_EXCESS_BASSY = -4.5, -1.5, 3.0
OFF = (0.0, 0.0, 800.0, 0.0)        # (hpf Hz, mid dB, mid Hz, shelf dB)


def di_spectrum_numbers(di: np.ndarray, starts: np.ndarray) -> dict:
    """``diTilt`` (dB/octave, 100 Hz-3 kHz) and ``diLowExcess`` (dB) of the DI excerpt ``di`` over the Welch segments
    ``starts`` of the loss."""
    f = L.features(np.asarray(di, dtype=np.float64), starts, None)
    fc = np.array(L.BAND_CENTRES, float)
    db = np.asarray(f.band_db, float)
    sel = (fc >= 100.0) & (fc <= 3000.0)
    tilt = float(np.polyfit(np.log2(fc[sel]), db[sel], 1)[0])
    low = float(np.mean(db[(fc >= 80.0) & (fc <= 200.0)]) - np.mean(db[(fc > 200.0) & (fc <= 800.0)]))
    return {"diTilt": tilt, "diLowExcess": low}


def widening(nums: dict) -> dict:
    """The extra grid options the DI numbers ask for: ``{"hpf": [...], "mid": [(dB, Hz), ...], "shelf": [dB, ...]}`` plus the
    list ``widened`` of human-readable names."""
    w: dict = {"hpf": [], "mid": [], "shelf": [], "widened": []}
    if nums["diTilt"] < TILT_DARK:
        w["mid"].append((9.0, 800.0))
        w["widened"].append("mid +9 dB (dark DI)")
    if nums["diTilt"] > TILT_BRIGHT:
        w["hpf"].append(180.0)
        w["widened"].append("HPF 180 Hz (bright DI)")
    if nums["diLowExcess"] > LOW_EXCESS_BASSY:
        w["shelf"].append(-6.0)
        w["widened"].append("low shelf -6 dB (bassy DI)")
    return w


def setting_params(path: str, s: tuple) -> dict:
    return {f"pre.{path}.hpf": s[0], f"pre.{path}.mid_db": s[1], f"pre.{path}.mid_hz": s[2], f"pre.{path}.shelf_db": s[3]}


def describe(s: tuple) -> str:
    parts = ([f"HPF {s[0]:g} Hz"] if s[0] > 0 else []) + ([f"mid {s[1]:+g} dB @ {s[2]:g} Hz"] if s[1] else []) + \
        ([f"low shelf {s[3]:g} dB @ {PRE_SHELF_HZ:g} Hz"] if s[3] else [])
    return ", ".join(parts) or "off"


def grid_search(score_many, wide: dict) -> tuple[tuple, float, list[dict]]:
    """Coordinate descent over one path. ``score_many(settings) -> losses`` renders (in parallel); the ``OFF`` setting is
    answered by the caller from the current state. Returns (best setting, its loss, every tried setting with step and loss)."""
    hpfs = [0.0, *PRE_HPF_OPTIONS, *wide["hpf"]]
    mids = [(0.0, 800.0), *PRE_MID_OPTIONS, *wide["mid"]]
    shelves = [0.0, PRE_SHELF_DB, *wide["shelf"]]
    tried: dict[tuple, float] = {}
    log: list[dict] = []
    n_widen = len(wide["hpf"]) + len(wide["mid"]) + len(wide["shelf"])

    def evm(settings, step):
        new = [s for s in dict.fromkeys(settings) if s not in tried]
        for s, l in zip(new, score_many(new)):
            tried[s] = float(l)
            log.append({"step": step, "setting": describe(s), "values": list(s), "loss": tried[s]})

    evm([OFF], "off")
    best, best_l = OFF, tried[OFF]

    def take(cands, step):
        nonlocal best, best_l
        evm(cands, step)
        c = min(cands, key=lambda s: tried[s])
        if tried[c] < best_l - KEEP_DB:
            best, best_l = c, tried[c]

    take([(h, 0.0, 800.0, 0.0) for h in hpfs[1:]], "hpf")
    take([(best[0], g, hz, 0.0) for g, hz in mids[1:]], "mid")
    if best[1]:      # a second HPF pass at the best mid peak (the HPF alone is masked while the mid is missing)
        take([(h, best[1], best[2], 0.0) for h in hpfs[1:]], "hpf at mid")
    take([(best[0], best[1], best[2], s) for s in shelves[1:]], "shelf")
    # joint neighbours: one step in each of two dimensions, nearest untried first
    def step(lst, cur, d):
        i = lst.index(cur)
        j = i + d
        return lst[j] if 0 <= j < len(lst) else None
    dims = [(hpfs, 0, lambda s: s[0]), (mids, 1, lambda s: (s[1], s[2])), (shelves, 3, lambda s: s[3])]
    neigh = []
    for a in range(3):
        for b in range(a + 1, 3):
            for da in (1, -1):
                for db_ in (1, -1):
                    s = list(best)
                    ok = True
                    for (lst, idx, get), d in ((dims[a], da), (dims[b], db_)):
                        n = step(lst, get(best), d)
                        if n is None:
                            ok = False
                            break
                        if idx == 1:
                            s[1], s[2] = n
                        else:
                            s[idx] = n
                    t = tuple(s)
                    if ok and t not in tried and t not in neigh:
                        neigh.append(t)
    room = max(0, MAX_RENDERS - (len(tried) - 1 - n_widen))      # renders left of the 12 (widening options come on top)
    if neigh and room:
        take(neigh[:min(MAX_NEIGHBOURS, room)], "neighbours")
    return best, best_l, log


def preeq_candidate(eng: Engine, cand: Scored, space: Space, ex, tgt: L.Target, wide: dict) -> dict:
    """Search the pre-EQ of every path of the stage-1 candidate ``cand`` (default parameters). Returns the record; the chosen
    ``pre.<path>.*`` parameters are in ``rec["params"]`` and the candidate's loss with them in ``rec["loss"]``."""
    combo = cand.combo
    paths = ("a", "b") if combo.topology == "blend" else ("a",)
    v = dict(space.default())
    if "blend" in v:
        v["blend"] = cand.blend
    eq = space.eq_gains(v)
    cur = cand.loss
    rec: dict = {"topology": cand.topology, "captures": combo.describe(), "offLoss": cand.loss, "paths": {}, "params": {}}
    for path in paths:
        other = {p: eng.core(combo, v, p, ex.x) for p in paths if p != path}

        def score_one(s, path=path, other=other):
            vv = {**v, **setting_params(path, s)}
            cores = {**other, path: eng.core(combo, vv, path, ex.x)}
            y = ex.trim(eng.emulate(combo, vv, cores["a"], cores.get("b"), cand.align, cand.levels))
            return L.evaluate(y, tgt, eq).total

        def score_wrapped(settings, cur=cur):
            # the "off" setting is the current state (the candidate's own loss, or the previous path's result)
            todo = [s for s in settings if s != OFF]
            got = dict(zip(todo, eng.map(score_one, todo)))
            return [cur if s == OFF else got[s] for s in settings]
        best, best_l, log = grid_search(score_wrapped, wide)
        rec["paths"][path] = {"chosen": describe(best), "values": list(best), "grid": log, "gain": cur - best_l}
        v.update(setting_params(path, best))
        cur = best_l
    rec["params"] = {k: v[k] for k in v if k.startswith("pre.")}
    rec["loss"] = cur
    rec["gainVsOff"] = cand.loss - cur
    rec["renders"] = sum(len(p["grid"]) - 1 for p in rec["paths"].values())
    return rec
