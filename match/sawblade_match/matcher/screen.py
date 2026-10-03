"""Stage 1: screen discrete combos on one short excerpt with default continuous parameters.

1. Path pairs: A = (HM-2 model, saw amp model), B = (boost model or none, body amp model). Because everything after
   the NAMs is linear, the output of a combo is (1-b)*A + b*B, so each path pair is rendered ONCE (NAM core, then the
   cab through the renderer) and every A x B combination is scored without another render: band powers of the blend are
   (1-b)^2 P_A + b^2 P_B + 2 b (1-b) C_AB with C_AB the cross-spectrum summed per 1/3-octave band (one matrix product
   per band). Scored on the A-weighted LTAS error (offset removed) for blends 0.15..0.85, lag 0 (screen only).
2. If the pair product is larger than the render budget, a seeded random subset of A pairs / B pairs is rendered
   (inclusion weighted 2:1 toward low/medium/unknown-gain amps for the saw amp; documented prior, not a hard filter).
3. Top ``rescore`` pairs get a one-time auto-align probe, then the full loss (LTAS+buzz+decay[+STFT]) on a blend grid.
4. Top ``cab_pairs`` pairs are re-scored with every cab IR (cheap: cab is applied by the renderer to the stored cores).
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np

from . import loss as L
from .engine import Engine
from .pool import Pool, Capture, gain_class
from .space import Combo, Space

BLENDS = np.array([0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85])
FINE_BLENDS = np.array([0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8])


@dataclass
class Scored:
    combo: Combo
    loss: float
    blend: float
    align: dict
    result: L.LossResult | None = None
    stage: str = "screen"
    extra: dict = field(default_factory=dict)


def _sample(rng, items: list, weights: list[float], cap: int) -> list:
    if cap >= len(items):
        return list(items)
    w = np.asarray(weights, float)
    idx = rng.choice(len(items), size=cap, replace=False, p=w / w.sum())
    return [items[i] for i in sorted(idx)]


def pair_lists(pool: Pool, cap_a: int, cap_b: int, rng):
    prior = lambda c: 1.0 if gain_class(c.title, c.name) == "high" else 2.0
    A = [(h, a) for h in pool.hm2 for a in pool.amps]
    B = [(b, a) for b in (list(pool.boost) + [None]) for a in pool.amps]
    A = _sample(rng, A, [prior(a) for _, a in A], cap_a)
    B = _sample(rng, B, [1.0] * len(B), cap_b)
    return A, B


def _band_arrays(sigs: list[np.ndarray], starts: np.ndarray):
    """Per band: (N, 2*S*nbins) real matrix of sqrt(w)-scaled [Re|Im] spectra, plus (N, nb) autopower."""
    N = len(sigs)
    X = np.stack([L.segment_spectra(s.astype(np.float64), starts).astype(np.complex64) for s in sigs])  # (N,S,F)
    mats, pw = [], np.zeros((N, len(L.BAND_CENTRES)))
    for b in range(len(L.BAND_CENTRES)):
        nz = np.nonzero(L.BAND_W[b] > 0)[0]
        sw = np.sqrt(L.BAND_W[b, nz]).astype(np.float32)
        Xb = X[:, :, nz] * sw[None, None, :]
        M = np.concatenate([Xb.real.reshape(N, -1), Xb.imag.reshape(N, -1)], axis=1).astype(np.float32)
        mats.append(M)
        pw[:, b] = np.sum(M.astype(np.float64) ** 2, axis=1)
    return mats, pw


def pair_errors(sigs_a, sigs_b, starts, ref_db) -> tuple[np.ndarray, np.ndarray]:
    """LTAS error (N_A, N_B) and best blend (N_A, N_B) over BLENDS."""
    ma, pa = _band_arrays(sigs_a, starts)
    mb, pb = _band_arrays(sigs_b, starts)
    nb = len(L.BAND_CENTRES)
    C = np.zeros((len(sigs_a), len(sigs_b), nb))
    for b in range(nb):
        C[:, :, b] = ma[b] @ mb[b].T
    best = np.full((len(sigs_a), len(sigs_b)), np.inf)
    best_b = np.zeros_like(best)
    w = L.A_POWER_W
    for beta in BLENDS:
        P = (1 - beta) ** 2 * pa[:, None, :] + beta ** 2 * pb[None, :, :] + 2 * beta * (1 - beta) * C
        d = 10 * np.log10(np.maximum(P, 1e-30)) - ref_db[None, None, :]
        d = d - (d * w).sum(-1, keepdims=True) / w.sum()
        e = np.sqrt((w * d * d).sum(-1) / w.sum())
        better = e < best
        best = np.where(better, e, best)
        best_b = np.where(better, beta, best_b)
    return best, best_b


class Screener:
    def __init__(self, eng: Engine, pool: Pool, space_boost: Space, space_noboost: Space, ex, target: L.Target, cab0: Capture,
                 rng, log=print):
        self.eng, self.pool, self.ex, self.tgt, self.cab0, self.rng, self.log = eng, pool, ex, target, cab0, rng, log
        self.sp = {True: space_boost, False: space_noboost}   # keyed by 'has a boost'
        self.stats: dict = {}

    def _v(self, boost) -> dict:
        return self.sp[boost is not None].default()

    def run(self, cap_a: int, cap_b: int, n_rescore: int, n_cab: int, top_k: int) -> list[Scored]:
        eng, ex, tgt = self.eng, self.ex, self.tgt
        t0 = time.time()
        A, B = pair_lists(self.pool, cap_a, cap_b, self.rng)
        full = (len(self.pool.hm2) * len(self.pool.amps)) * ((len(self.pool.boost) + 1) * len(self.pool.amps))
        self.stats.update(pairsA=len(A), pairsB=len(B), comboProductNoCab=full, combosScored=len(A) * len(B),
                          cabs=len(self.pool.cabs))
        self.log(f"stage1: rendering {len(A)} A-pairs + {len(B)} B-pairs "
                 f"(full product {full:,} combos x {len(self.pool.cabs)} cabs; screening {len(A) * len(B):,})")

        def core_a(pair):
            c = Combo(pair[0], pair[1], None, self.pool.amps[0], self.cab0)
            return eng.core(c, self._v(None), "a", ex.x)

        def core_b(pair):
            c = Combo(self.pool.hm2[0], self.pool.amps[0], pair[0], pair[1], self.cab0)
            return eng.core(c, self._v(pair[0]), "b", ex.x)

        cores_a = eng.map(core_a, A)
        self.log(f"stage1: A cores done ({time.time() - t0:.0f}s)")
        cores_b = eng.map(core_b, B)
        self.log(f"stage1: B cores done ({time.time() - t0:.0f}s)")
        v0 = self.sp[False].default()
        lin_a = [ex.trim(s) for s in eng.map(lambda c: eng.linear(self.cab0, v0, "a", c), cores_a)]
        lin_b = [ex.trim(s) for s in eng.map(lambda c: eng.linear(self.cab0, v0, "b", c), cores_b)]
        err, beta = pair_errors(lin_a, lin_b, tgt.starts, tgt.ref.band_db)
        self.log(f"stage1: {err.size:,} pair combos scored on LTAS ({time.time() - t0:.0f}s); best {err.min():.2f} dB")
        order = np.dstack(np.unravel_index(np.argsort(err, axis=None, kind="stable"), err.shape))[0]
        top = [(int(i), int(j)) for i, j in order[:n_rescore]]
        self.stats["screenTop"] = [{"A": [A[i][0].key, A[i][1].key], "B": [B[j][0].key if B[j][0] else None, B[j][1].key],
                                    "ltasErr": float(err[i, j]), "blend": float(beta[i, j])} for i, j in top[:10]]
        # --- rescore with the full loss and the resolved alignment ------------------------------------------------
        def rescore(ij):
            i, j = ij
            combo = Combo(A[i][0], A[i][1], B[j][0], B[j][1], self.cab0)
            v = self.sp[B[j][0] is not None].default()
            align = eng.probe_align(combo, v)
            return self._best_blend(combo, v, cores_a[i], cores_b[j], align, "screen")

        res = eng.map(rescore, top)
        res.sort(key=lambda s: s.loss)
        self.log(f"stage1: {len(res)} pairs rescored with the full loss ({time.time() - t0:.0f}s); best {res[0].loss:.3f}")
        # --- cab sweep on the best pairs ----------------------------------------------------------------------------
        lookup = {}
        for ij in top:
            i, j = ij
            lookup[(A[i][0].key, A[i][1].key, B[j][0].key if B[j][0] else None, B[j][1].key)] = ij
        jobs = []
        for s in res[:n_cab]:
            ij = lookup[(s.combo.hm2.key, s.combo.saw_amp.key, s.combo.boost.key if s.combo.boost else None,
                         s.combo.body_amp.key)]
            for cab in self.pool.cabs:
                if cab.key != self.cab0.key:
                    jobs.append((s, ij, cab))

        def cab_job(job):
            s, (i, j), cab = job
            combo = s.combo.with_cab(cab)
            return self._best_blend(combo, self._v(combo.boost), cores_a[i], cores_b[j], s.align, "screen+cab",
                                    blends=np.array([s.blend - 0.1, s.blend, s.blend + 0.1]).clip(0.05, 0.95))

        cab_res = eng.map(cab_job, jobs)
        allres = res + cab_res
        allres.sort(key=lambda s: s.loss)
        self.log(f"stage1: cab sweep ({len(jobs)} combos) done ({time.time() - t0:.0f}s); best {allres[0].loss:.3f}")
        # top_k distinct path pairs (best cab each), then the remaining best distinct pairs as alternatives
        seen, out = set(), []
        for s in allres:
            k = s.combo.key()[:4]
            if k not in seen:
                seen.add(k)
                out.append(s)
        self.stats["stage1Seconds"] = time.time() - t0
        return out

    def _best_blend(self, combo: Combo, v: dict, core_a, core_b, align, stage, blends=None) -> Scored:
        eng, ex = self.eng, self.ex
        la = eng.linear(combo.cab, v, "a", core_a)
        lb = eng.linear(combo.cab, v, "b", core_b)
        best = None
        for beta in (FINE_BLENDS if blends is None else blends):
            y = ex.trim(eng.mix(la, lb, float(beta), align))
            r = L.evaluate(y, self.tgt, self.sp[combo.boost is not None].eq_gains(v))
            if best is None or r.total < best[0].total:
                best = (r, float(beta))
        return Scored(combo, best[0].total, best[1], align, best[0], stage)
