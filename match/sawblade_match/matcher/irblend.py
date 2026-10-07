"""Two-IR blend (v0.4M B2.1, matcher half): the winner's cab becomes one combined "irMix" IR of two cabs / mics.

``h = (1 - mix) hA + mix s hB[i - offset]`` (core, docs/PRESET_SCHEMA.md): the second IR is aligned to the first by the lag of
the largest |cross-correlation| of the first 5 ms of the two IRs (|lag| <= 256 samples), the sign of that peak decides
``invertB``, and the mix is tried on a grid after the last linear block. Still one combined IR, so the no-cab export is exact
(live-compatible). The pair must beat the single IR by ``MIN_GAIN`` (Occam). The input is a plain list of candidate IRs (the
top 6 of the cab sweep today; the analytic IR screen of B3 supplies it for large pools): ``pair_search`` never looks at the pool.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf

from . import loss as L
from .engine import RATE, Engine, to48
from .pool import Capture
from .refine import MIX_GRID, relinear
from .screen import Scored
from .space import Space

TOP_IRS = 6
MIN_GAIN = 0.05          # a pair must lower the total loss by at least this much (Occam)
WINDOW_MS = 5.0
MAX_LAG = 256
GRID_PAIRS = 3           # pairs (best at mix 0.5) that get the mix grid


def load_ir48(cap: Capture) -> np.ndarray:
    """Left channel of an IR file at 48 kHz (as the core loads it; the L2 normalisation does not change lag or sign)."""
    x, fs = sf.read(cap.path, dtype="float64", always_2d=True)
    return np.asarray(to48(x[:, 0], int(fs)), dtype=np.float64)


def ir_alignment(ha: np.ndarray, hb: np.ndarray, window_ms: float = WINDOW_MS, max_lag: int = MAX_LAG) -> tuple[int, bool, int]:
    """(``offsetSamplesB``, ``invertB``, lag) aligning IR B to IR A. ``lag`` k maximises |c[k]| with
    ``c[k] = sum_{n < N} hA[n] hB[n + k]`` over the first ``window_ms`` of A (B zero-padded on both sides): a B that arrives k
    samples after A has c peaking at +k, and the core delays B for a positive offset, so the offset is ``-k``. ``invertB`` is
    the sign of the peak (negative = opposite polarity)."""
    n = int(round(window_ms * RATE / 1000.0))
    a = np.zeros(n)
    m = min(n, len(ha))
    a[:m] = ha[:m]
    b = np.zeros(n + 2 * max_lag)
    m = min(len(hb), n + max_lag)
    b[max_lag:max_lag + m] = hb[:m]                     # b[max_lag + j] = hB[j]
    lags = np.arange(-max_lag, max_lag + 1)
    c = np.array([float(np.dot(a, b[max_lag + k:max_lag + k + n])) for k in lags])
    i = int(np.argmax(np.abs(c)))
    k = int(lags[i])
    return -k, bool(c[i] < 0), k


def pair_search(eng: Engine, cand: Scored, space: Space, ex, tgt: L.Target, irs: list[Capture], *, seed: int, gens: int,
                pop: int, patience: int | None = None, tol: float = 0.0, log=print) -> dict:
    """Try two-IR cabs for the refined candidate ``cand`` (its stage-2 parameters and NAM cores): every pair (A = the lower key) of
    ``irs`` at mix 0.5, the best ``GRID_PAIRS`` on the mix grid, then one more linear block (``relinear``) on the best. Returns
    the record ``tried, won, pair, offset, invert, mix, gainVsSingle, ...``; when the pair won it also holds ``_won`` =
    (combo, params, LossResult) for the caller to turn into the candidate (and to drop before writing JSON)."""
    v = cand.extra["params"]
    paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
    cores = [eng.core(cand.combo, v, p, ex.x) for p in paths]
    ca, cb = cores[0], (cores[1] if len(cores) > 1 else None)
    eq = space.eq_gains(v)
    rec: dict = {"tried": 0, "won": False, "minGain": MIN_GAIN, "singleLoss": cand.loss, "irs": [c.key for c in irs],
                 "pair": None, "offset": None, "invert": None, "mix": None, "gainVsSingle": 0.0, "pairLoss": None}
    if len(irs) < 2:
        rec["note"] = "fewer than two IRs"
        return rec
    ir = {c.key: load_ir48(c) for c in irs}
    jobs = []
    for a in irs:
        for b in irs:
            if a.key < b.key:        # one orientation per pair (A = the lower key): (A, B, m) and (B, A, 1 - m) are the same
                off, inv, lag = ir_alignment(ir[a.key], ir[b.key])       # blend up to a shift; the offset carries the alignment,
                jobs.append((a, b, off, inv))                            # and the choice cannot flip with the seed

    def ev(job, mix):
        a, b, off, inv = job
        combo = cand.combo.with_pair(a, b, off, inv)
        vv = {**v, "cab.mix": float(mix)}
        y = ex.trim(eng.emulate(combo, vv, ca, cb, cand.align, cand.levels))
        return L.evaluate(y, tgt, eq).total

    base = eng.map(lambda j: ev(j, 0.5), jobs)
    rec["tried"] = len(jobs)
    order = sorted(range(len(jobs)), key=lambda i: base[i])[:GRID_PAIRS]

    def best_mix(i):
        scores = [(ev(jobs[i], m) if m != 0.5 else base[i], m) for m in MIX_GRID]
        return min(scores)

    graded = eng.map(best_mix, order)
    top = min(zip(graded, order), key=lambda t: t[0][0])
    (loss0, mix0), i = top
    a, b, off, inv = jobs[i]
    rec["topPairs"] = [{"irA": jobs[j][0].key, "irB": jobs[j][1].key, "offset": jobs[j][2], "invert": jobs[j][3],
                        "mix": g[1], "loss": g[0]} for g, j in zip(graded, order)]
    combo2 = cand.combo.with_pair(a, b, off, inv)
    v0 = {**v, "cab.mix": mix0}
    sp2 = Space.for_combo(combo2, filters="post.hp" in space.idx)
    v2, r2 = relinear(eng, combo2, sp2, ex, tgt, cand.align, v0, levels=cand.levels, seed=seed, gens=gens, pop=pop,
                      patience=patience, tol=tol, log=log)
    rec.update(pair={"irA": a.key, "irB": b.key, "titleA": a.title, "titleB": b.title}, offset=off, invert=inv,
               mix=v2["cab.mix"], pairLoss=r2.total, gainVsSingle=cand.loss - r2.total)
    if cand.loss - r2.total >= MIN_GAIN:
        rec["won"] = True
        rec["_won"] = (combo2, v2, r2)
    return rec
