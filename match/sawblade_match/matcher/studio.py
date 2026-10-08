"""Studio processing in the reference (v0.4M B2.3): detect dynamics / EQ the capture + IR chain cannot reach, then reproduce it.

Detection (after stage 2 / the IR blend, on the best candidate without a bus comp; always written to ``result.json -> studio``):

* ``compressed``: the reference's median 400 ms crest factor is >= 1.5 dB below the candidate's, or its short-term loudness
  range (EBU 3342 style, 3 s windows, 95th - 10th percentile) is >= 2 LU narrower;
* ``eqd``: an EQ the chain cannot reach. What is left of the LTAS residual after the candidate's own post EQ took what it could
  within its +-6 dB range (``absorbable``: a search that is merely not converged leaves a residual the post EQ can still
  absorb) is >= 1.0 dB RMS and either a 3rd-order polynomial in log-frequency explains >= 60 % of it, or a post-EQ gain
  already sits at >= 90 % of its range.

Only a clean guitar reference can be judged (a mix has other instruments' dynamics and spectrum): otherwise the detector reports
``skipped``. When it fires, ``studio_stage`` refines the winner with the bus compressor (threshold -30..-6 dB re the pre-headroom
level, ratio 1.5-4, knee 6 dB, attack 1-30 ms, release 30-150 ms, so it stays NAM-trainable; make-up 0, the output gain is set
after the search) and / or the post-EQ gains widened to +-9 dB, each kept only if the total loss falls by >= ``MIN_GAIN``.
"""
from __future__ import annotations

import numpy as np

from ..tonecheck.analysis import loudness_range_lu
from . import cma
from . import feel as _feel
from . import loss as L
from .engine import Engine
from .refine import relinear
from .screen import Scored
from .space import POST_GAIN, Space

MIN_GAIN = 0.05
CREST_DROP_DB = 1.5
LRA_NARROWER_LU = 2.0
EQ_RANGE_FRACTION = 0.9
POLY_DEGREE = 3
POLY_EXPLAINED = 0.6
RESIDUAL_RMS_DB = 1.0
WIDE_POST_GAIN = 9.0
COMP_RANGES = {"thresholdDb": (-30.0, -6.0), "ratio": (1.5, 4.0), "attackMs": (1.0, 30.0), "releaseMs": (30.0, 150.0)}
KNEE_DB = 6.0


def _poly_explained(d: np.ndarray) -> float:
    """Fraction of the variance of ``d`` (per-band residual in dB over the 1/3-octave band centres) explained by the best
    polynomial of degree 3 in log-frequency (least squares, A-weighted bands as in the loss)."""
    x = np.log10(np.array(L.BAND_CENTRES, float))
    x = (x - x.mean()) / (x.std() + 1e-12)
    w = np.sqrt(L.A_POWER_W / L.A_POWER_W.sum())
    A = np.vander(x, POLY_DEGREE + 1)
    coef, *_ = np.linalg.lstsq(A * w[:, None], d * w, rcond=None)
    res = d - A @ coef
    tot = float(np.sum(w * w * (d - np.sum(w * w * d)) ** 2))
    return float(1.0 - np.sum(w * w * (res - np.sum(w * w * res)) ** 2) / tot) if tot > 1e-12 else 0.0


def _peak_db(f: np.ndarray, f0: float, gain_db: float, q: float = 1.0, fs: float = 48000.0) -> np.ndarray:
    """Magnitude (dB) of an RBJ peaking biquad at ``f`` (the same filter as the post-EQ bands)."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * f0 / fs
    al = np.sin(w0) / (2.0 * q)
    b = np.array([1.0 + al * A, -2.0 * np.cos(w0), 1.0 - al * A])
    a = np.array([1.0 + al / A, -2.0 * np.cos(w0), 1.0 - al / A])
    z = np.exp(-1j * 2.0 * np.pi * np.asarray(f, float) / fs)
    h = (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    return 20.0 * np.log10(np.abs(h))


def absorbable(d: np.ndarray, v: dict) -> np.ndarray:
    """What the candidate's own post EQ could still absorb of the LTAS residual ``d`` (dB per band, level offset removed):
    the residual left after the three post-EQ gains move, within the normal +-6 dB range, to their A-weighted least-squares
    best (coordinate descent on three variables). An unconverged search leaves a smooth residual that the post EQ can still
    take away; only what remains is evidence of an EQ the chain cannot reach."""
    fc = np.array(L.BAND_CENTRES, float)
    w = L.A_POWER_W / L.A_POWER_W.sum()
    idx = [i for i in range(3) if f"post.f{i}" in v and f"post.g{i}" in v]
    if not idx:
        return d
    basis = np.stack([_peak_db(fc, v[f"post.f{i}"], 1.0) for i in idx], axis=1)         # dB response per +1 dB gain
    basis = basis - (w[:, None] * basis).sum(axis=0, keepdims=True)                      # the overall level is free in the loss
    d = d - float(np.sum(w * d))
    g0 = np.array([v[f"post.g{i}"] for i in idx])
    delta = np.zeros(len(idx))
    for _ in range(60):
        for k in range(len(idx)):
            r = d + basis @ delta - basis[:, k] * delta[k]            # the residual without band k's move
            best = -np.sum(w * basis[:, k] * r) / max(np.sum(w * basis[:, k] ** 2), 1e-12)
            delta[k] = float(np.clip(best, -POST_GAIN - g0[k], POST_GAIN - g0[k]))
    r = d + basis @ delta
    return r - float(np.sum(w * r))


def detect(ref_clean: bool, tgt_full: L.Target, tgt: L.Target, y: np.ndarray, v: dict, ref_sig: np.ndarray | None) -> dict:
    """The detector on the candidate's excerpt output ``y`` (pre-comp). ``tgt_full`` carries the feel target (crest of the
    reference); ``ref_sig``: the reference signal over the excerpt for the loudness range (None: not measurable)."""
    out: dict = {"compressed": False, "eqd": False, "evidence": {}}
    ev = out["evidence"]
    if not ref_clean:
        ev["skipped"] = "reference is not a clean guitar track: its dynamics and spectrum include other instruments"
        return out
    ft = tgt_full.feel
    y64 = np.asarray(y, dtype=np.float64)
    if ft is not None and ft.ref.crest is not None and len(ft.ref.crest) >= _feel.MIN_CREST_WINDOWS:
        cc = _feel.crest_values(y64, ft.plan)
        if len(cc) >= _feel.MIN_CREST_WINDOWS:
            ev["crestCandidateDb"], ev["crestReferenceDb"] = float(np.median(cc)), float(np.median(ft.ref.crest))
            ev["crestDropDb"] = ev["crestCandidateDb"] - ev["crestReferenceDb"]
    if ref_sig is not None:
        lc, lr = loudness_range_lu(y64), loudness_range_lu(np.asarray(ref_sig, dtype=np.float64))
        if lc is not None and lr is not None:
            ev["lraCandidateLu"], ev["lraReferenceLu"], ev["lraNarrowerLu"] = float(lc), float(lr), float(lc - lr)
    out["compressed"] = bool(ev.get("crestDropDb", 0.0) >= CREST_DROP_DB or ev.get("lraNarrowerLu", 0.0) >= LRA_NARROWER_LU)
    gains = [abs(v[k]) for k in v if k.startswith("post.g")]
    ev["postGainMaxDb"] = max(gains) if gains else 0.0
    ev["postGainAtRange"] = bool(gains and max(gains) >= EQ_RANGE_FRACTION * POST_GAIN)
    f = L.features(y64, tgt.starts, None)
    d = f.band_db - tgt.ref.band_db
    w = L.A_POWER_W
    d = d - float(np.sum(w * d) / np.sum(w))
    ev["residualRmsDb"] = float(np.sqrt(np.sum(w * d * d) / np.sum(w)))
    ev["residualPolyExplained"] = _poly_explained(d)
    # An EQ the chain cannot reach: what is left of the residual after the post EQ took what it could within +-6 dB must still be
    # >= 1 dB RMS, and either have the smooth shape of an EQ curve (3rd-order polynomial in log-frequency, >= 60 % explained) or
    # come with post-EQ gains at the range limit. A search that is simply not converged leaves a residual its post EQ can absorb.
    left = absorbable(d, v)
    ev["residualAfterPostEqRmsDb"] = float(np.sqrt(np.sum(w * left * left) / np.sum(w)))
    ev["residualAfterPostEqPolyExplained"] = _poly_explained(left)
    unreachable = ev["residualAfterPostEqRmsDb"] >= RESIDUAL_RMS_DB
    out["eqd"] = bool(unreachable and (ev["postGainAtRange"] or ev["residualAfterPostEqPolyExplained"] >= POLY_EXPLAINED))
    return out


def _comp_from_u(u: np.ndarray) -> dict:
    lo, hi = COMP_RANGES["thresholdDb"]
    thr = lo + float(u[0]) * (hi - lo)
    lo, hi = COMP_RANGES["ratio"]
    ratio = lo + float(u[1]) * (hi - lo)
    lo, hi = COMP_RANGES["attackMs"]
    att = float(np.exp(np.log(lo) + float(u[2]) * (np.log(hi) - np.log(lo))))
    lo, hi = COMP_RANGES["releaseMs"]
    rel = lo + float(u[3]) * (hi - lo)
    return {"thresholdDb": round(thr, 3), "ratio": round(ratio, 3), "kneeDb": KNEE_DB, "attackMs": round(att, 3),
            "releaseMs": round(rel, 3), "makeupDb": 0.0}


def fit_bus_comp(eng: Engine, y: np.ndarray, cab, tgt: L.Target, eq: np.ndarray, *, seed: int, gens: int, pop: int,
                 patience: int | None = None, tol: float = 0.0) -> tuple[dict, L.LossResult]:
    """Seeded CMA-ES over the four compressor parameters (the other fields are fixed) on the chain output ``y``; the
    loss is level invariant, so make-up stays 0."""
    def score(u):
        return L.evaluate(eng.apply_comp(y, _comp_from_u(u), cab), tgt, eq)

    def f(u):
        return score(np.asarray(u)).total

    bx, bf, hist = cma.minimize(None, np.array([0.5, 0.4, 0.4, 0.5]), 0.3, pop, gens, seed,
                                evaluate_batch=lambda X: eng.map(f, list(X)), patience=patience, tol=tol)
    return _comp_from_u(bx), score(bx)


def studio_stage(eng: Engine, cand: Scored, space: Space, ex, tgt: L.Target, det: dict, *, seed: int, gens: int, pop: int,
                 patience: int | None = None, tol: float = 0.0, log=print) -> dict:
    """Refine the winner with the detected studio processing. Returns the record; when something was kept it holds
    ``_won`` = (params, LossResult, comp | None) for the caller to turn into the candidate (dropped before the JSON)."""
    v, base = cand.extra["params"], cand.loss
    rec: dict = {"stage": [], "busCompUsed": False, "widenedPostEq": False, "gainVsPlain": 0.0, "plainLoss": base,
                 "minGain": MIN_GAIN}
    best_v, best_r, comp = v, cand.result, None
    wide = None
    if det["eqd"]:
        wide = type(space)(cand.combo.shape(), boost=cand.combo.boost, filters="post.hp" in space.idx,
                     irmix=cand.combo.cab_b is not None, post_gain=WIDE_POST_GAIN)
        v2, r2 = relinear(eng, cand.combo, wide, ex, tgt, cand.align, v, levels=cand.levels, seed=seed, gens=gens, pop=pop,
                          patience=patience, tol=tol, log=log)
        # the same re-fit at the normal range: the wider range must beat it, not just the (possibly unconverged) plain result
        _, r_norm = relinear(eng, cand.combo, space, ex, tgt, cand.align, v, levels=cand.levels, seed=seed, gens=gens, pop=pop,
                             patience=patience, tol=tol, log=log)
        keep = r_norm.total - r2.total >= MIN_GAIN and best_r.total - r2.total >= MIN_GAIN
        rec["stage"].append({"step": "post EQ +-9 dB", "loss": r2.total, "normalRefitLoss": r_norm.total, "kept": bool(keep)})
        if keep:
            best_v, best_r, rec["widenedPostEq"] = v2, r2, True
    if det["compressed"]:
        paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
        cores = [eng.core(cand.combo, best_v, p, ex.x) for p in paths]
        y = ex.trim(eng.emulate(cand.combo, best_v, cores[0], cores[1] if len(cores) > 1 else None, cand.align, cand.levels))
        eq = (wide if rec["widenedPostEq"] else space).eq_gains(best_v)
        c, rc = fit_bus_comp(eng, y, cand.combo.cab, tgt, eq, seed=seed + 1, gens=gens, pop=pop, patience=patience, tol=tol)
        rec["stage"].append({"step": "bus comp", "comp": c, "loss": rc.total, "kept": bool(best_r.total - rc.total >= MIN_GAIN)})
        if best_r.total - rc.total >= MIN_GAIN:
            best_r, comp = rc, c
            rec["busCompUsed"], rec["busComp"] = True, c
    rec["gainVsPlain"] = base - best_r.total
    if best_r is not cand.result:
        rec["_won"] = (best_v, best_r, comp)
    return rec
