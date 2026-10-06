"""Studio processing in the reference (v0.4M B2.3): detect dynamics / EQ the capture + IR chain cannot reach, then reproduce it.

Detection (after stage 2 / the IR blend, on the best candidate without a bus comp; always written to ``result.json -> studio``):

* ``compressed``: the reference's median 400 ms crest factor is >= 1.5 dB below the candidate's, or its short-term loudness
  range (EBU 3342 style, 3 s windows, 95th - 10th percentile) is >= 2 LU narrower;
* ``eqd``: a candidate post-EQ gain sits at >= 90 % of its +-6 dB range, or the LTAS residual's best 3rd-order polynomial in
  log-frequency explains >= 60 % of the residual's variance and the residual RMS is >= 1.0 dB.

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
    out["eqd"] = bool(ev["postGainAtRange"] or (ev["residualPolyExplained"] >= POLY_EXPLAINED
                                                and ev["residualRmsDb"] >= RESIDUAL_RMS_DB))
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
        wide = Space(cand.combo.shape(), boost=cand.combo.boost, filters="post.hp" in space.idx,
                     irmix=cand.combo.cab_b is not None, post_gain=WIDE_POST_GAIN)
        v2, r2 = relinear(eng, cand.combo, wide, ex, tgt, cand.align, v, levels=cand.levels, seed=seed, gens=gens, pop=pop,
                          patience=patience, tol=tol, log=log)
        rec["stage"].append({"step": "post EQ +-9 dB", "loss": r2.total, "kept": bool(best_r.total - r2.total >= MIN_GAIN)})
        if best_r.total - r2.total >= MIN_GAIN:
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
