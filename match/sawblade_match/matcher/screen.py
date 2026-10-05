"""Stage 1: screen discrete combos (three topologies) on one short excerpt with default continuous parameters.

Path pairs (pedal or none, amp) are rendered ONCE through the C++ core (gate -> NAMs) and the shared default cab is
applied by the renderer. Because everything after the NAMs is linear:

* ``single``:  score = LTAS error of the pair's output;
* ``blend``:   output = (1-b) A + b B, so every pair x pair combination is scored from band cross-spectra without another
  render: band powers (1-b)^2 P_A + b^2 P_B + 2 b (1-b) C_AB, C_AB from one matrix product per 1/3-octave band (blend grid
  0.15..0.85, lag 0 for the screen only). Pairs are shared by both paths (no per-path fixed pre-EQ), so one set of pair
  renders serves path A and path B;
* ``single2``: pedal1 -> pedal2 -> amp chains from the pre-screened top pedals/amps (pre-screen is always applied to this
  topology: the ordered pedal pair space is quadratic).

If the pair product exceeds ``cap_pairs`` the per-capture pre-screen (prescreen.py) trims pedals and amps first; if it
still exceeds the cap a seeded random subset of pairs is rendered. The top candidates of each topology are re-scored with
the full loss (LTAS + buzz + decay [+ STFT]); blend candidates first get a one-time auto-align probe. The best get a cab sweep
(cheap: the renderer applies each IR to the stored cores). Output: per topology, the best distinct pairs (best cab each).
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np

from . import loss as L
from .engine import Engine
from .pool import Capture, Pool
from .prescreen import DEFAULT_V, auto_n, auto_n_amps, prescreen
from .progress import NullProgress
from .space import Combo, Space, chain_blocks, manual_align

BLENDS = np.array([0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85])
FINE_BLENDS = np.array([0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8])
TOPOLOGIES = ("single", "single2", "blend")


@dataclass
class Scored:
    combo: Combo
    loss: float
    blend: float
    align: dict
    result: L.LossResult | None = None
    stage: str = "screen"
    extra: dict = field(default_factory=dict)

    @property
    def topology(self) -> str:
        return self.combo.topology


def _band_arrays(sigs: list[np.ndarray], starts: np.ndarray):
    """Per band: (N, 2*S*nbins) real matrix of sqrt(w)-scaled [Re|Im] spectra, plus (N, nb) autopower."""
    N = len(sigs)
    nb = len(L.BAND_CENTRES)
    cols = [np.nonzero(L.BAND_W[b] > 0)[0] for b in range(nb)]
    sw = [np.sqrt(L.BAND_W[b, c]).astype(np.float32) for b, c in enumerate(cols)]
    lo, hi = min(c[0] for c in cols), max(c[-1] for c in cols) + 1
    mats = [np.empty((N, 2 * len(starts) * len(c)), np.float32) for c in cols]
    for n, s in enumerate(sigs):
        X = L.segment_spectra(s.astype(np.float64), starts)[:, lo:hi]
        for b, c in enumerate(cols):
            Xb = X[:, c - lo] * sw[b][None, :]
            m = mats[b][n]
            k = Xb.size
            m[:k] = Xb.real.ravel()
            m[k:] = Xb.imag.ravel()
    pw = np.stack([np.sum(m.astype(np.float64) ** 2, axis=1) for m in mats], axis=1)
    return mats, pw


def single_errors(pw: np.ndarray, ref_db: np.ndarray, hf_limit_hz: float | None = None) -> np.ndarray:
    return np.array([L.ltas_error(10 * np.log10(np.maximum(p, 1e-30)), ref_db, hf_limit_hz)[0] for p in pw])


def blend_errors(mats, pw, ref_db, hf_limit_hz: float | None = None) -> tuple[np.ndarray, np.ndarray]:
    """LTAS error (N, N) of (1-b) sig_i + b sig_j and the best blend; diagonal = inf."""
    N, nb = pw.shape
    C = np.zeros((N, N, nb))
    for b in range(nb):
        C[:, :, b] = mats[b] @ mats[b].T
    best = np.full((N, N), np.inf)
    best_b = np.zeros_like(best)
    w = L.A_POWER_W
    for beta in BLENDS:
        P = (1 - beta) ** 2 * pw[:, None, :] + beta ** 2 * pw[None, :, :] + 2 * beta * (1 - beta) * C
        d = 10 * np.log10(np.maximum(P, 1e-30)) - ref_db[None, None, :]
        if hf_limit_hz is None:
            d -= (d * w).sum(-1, keepdims=True) / w.sum()
        else:       # fitted bands set the offset; ignored (HF) bands only count when above the reference
            keep = L.BAND_UPPER <= hf_limit_hz
            d -= (d[..., keep] * w[keep]).sum(-1, keepdims=True) / w[keep].sum()
            d = np.where(keep, d, np.maximum(d, 0.0))
        e = np.sqrt((w * d * d).sum(-1) / w.sum())
        better = e < best
        best = np.where(better, e, best)
        best_b = np.where(better, beta, best_b)
    np.fill_diagonal(best, np.inf)
    return best, best_b


def _argsort2d(a: np.ndarray, n: int) -> list[tuple[int, int]]:
    flat = np.argsort(a, axis=None, kind="stable")[:n]
    return [(int(i), int(j)) for i, j in zip(*np.unravel_index(flat, a.shape))]


def select_coarse(e1: np.ndarray, eb: np.ndarray, n_keep: int) -> list[int]:
    """Indices of the pairs that go on to the full-length pass: walk the coarse single ranking and the coarse blend
    ranking in turn (a blend adds BOTH its members, so partners survive together) until ``n_keep`` pairs are kept."""
    n = len(e1)
    n_keep = min(max(n_keep, 2), n)
    singles = np.argsort(e1, kind="stable")
    blends = _argsort2d(eb, min(eb.size, 8 * n_keep))
    keep: dict[int, None] = {}
    si = bi = 0
    while len(keep) < n_keep and (si < len(singles) or bi < len(blends)):
        if si < len(singles):
            keep[int(singles[si])] = None
            si += 1
        if bi < len(blends) and len(keep) < n_keep:
            i, j = blends[bi]
            keep[i] = None
            keep[j] = None
            bi += 1
    return sorted(keep)


class Screener:
    """``cex``/``ctgt``: optional coarse (short) excerpt and its target. With them the screen is two-pass (plan.coarse_s):
    every candidate pair is rendered on the coarse excerpt, and only the best ``plan.coarse_keep`` fraction (singles and
    both members of the best blends) are rendered on the full excerpt and re-scored."""

    def __init__(self, eng: Engine, pool: Pool, ex, target: L.Target, cab0: Capture, rng, plan, log=print,
                 cex=None, ctgt=None, prog=None):
        self.eng, self.pool, self.ex, self.tgt, self.cab0, self.rng, self.plan, self.log = \
            eng, pool, ex, target, cab0, rng, plan, log
        self.cex, self.ctgt = (cex, ctgt) if (cex is not None and ctgt is not None) else (None, None)
        self.prog = prog or NullProgress()
        self.stats: dict = {}
        self.v0 = DEFAULT_V

    # ---- helpers ------------------------------------------------------------------------------------------------
    def _cores(self, chains: list[tuple], ex=None, progress=None) -> list[np.ndarray]:
        """chains: tuples (pedals..., amp); renders each NAM core on the excerpt (parallel)."""
        ex = ex or self.ex
        return self.eng.map(lambda ch: self.eng.core_blocks(chain_blocks("a", ch[:-1], ch[-1], {}), self.cab0, ex.x),
                            chains, progress)

    def _lin(self, cores, path="a", ex=None, progress=None) -> list[np.ndarray]:
        ex = ex or self.ex
        return [ex.trim(s) for s in self.eng.map(lambda c: self.eng.linear(self.cab0, self.v0, path, c), cores, progress)]

    def _score(self, combo: Combo, v: dict, cores: tuple, align: dict, blends=None, stage="screen") -> Scored:
        eng, ex = self.eng, self.ex
        sp = Space.for_combo(combo)
        la = eng.linear(combo.cab, v, "a", cores[0])
        if combo.topology != "blend":
            r = L.evaluate(ex.trim(la), self.tgt, sp.eq_gains(v))
            return Scored(combo, r.total, 0.0, align, r, stage, {"_cores": cores})
        lb = eng.linear(combo.cab, v, "b", cores[1])
        best = None
        for beta in (FINE_BLENDS if blends is None else blends):
            y = ex.trim(eng.mix(la, lb, float(beta), align))
            r = L.evaluate(y, self.tgt, sp.eq_gains(v))
            if best is None or r.total < best[0].total:
                best = (r, float(beta))
        return Scored(combo, best[0].total, best[1], align, best[0], stage, {"_cores": cores})

    # ---- main ---------------------------------------------------------------------------------------------------
    def run(self) -> dict[str, list[Scored]]:
        eng, plan, pool = self.eng, self.plan, self.pool
        t0 = time.time()
        pedals, amps = list(pool.pedals), list(pool.amps)
        full_pairs = (len(pedals) + 1) * len(amps)
        classes_p = len({p.kind for p in pedals})
        classes_a = len({a.kind for a in amps})
        coarse = self.cex is not None and plan.coarse_s > 0
        sx, st = (self.cex, self.ctgt) if coarse else (self.ex, self.tgt)     # excerpt/target of the first (screening) pass
        tm: dict[str, float] = {}
        n_ps = plan.prescreen_n
        n_pp = None
        if plan.capped_prescreen:               # quick: the cap stays on; pedal and amp quotas sized separately (amps matter most)
            n_pp = plan.prescreen_n_ped
            n_ps = n_ps if n_ps is not None else auto_n_amps(classes_p, classes_a, n_pp + plan.blend_extra_ped,
                                                             plan.cap_pairs, plan.blend_extra_amp)
        elif n_ps is None and full_pairs > plan.cap_pairs:
            n_ps = auto_n(classes_p, classes_a, plan.cap_pairs)
        self.stats.update(poolPedals=len(pedals), poolAmps=len(amps), poolCabs=len(pool.cabs), fullPairs=full_pairs,
                          capPairs=plan.cap_pairs, coarsePass=coarse)
        pre_p, pre_a = pedals, amps
        t1 = time.time()
        self.prog.stage("prescreen", "pre-screening captures")
        if n_ps is not None or plan.top_k.get("single2", 0):
            n_use = n_ps if n_ps is not None else plan.prescreen_n2
            pp, aa, info = prescreen(eng, pool, sx, st, self.cab0, n_use, self.log, n_pedals_per_class=n_pp,
                                     blend_aware=plan.blend_aware, progress=lambda f: self.prog.update(f),
                                     n_blend_pedals=plan.blend_extra_ped, n_blend_amps=plan.blend_extra_amp)
            self.stats["prescreen"] = {**info, "appliedToBlendSingle": n_ps is not None}
            if n_ps is not None:
                pre_p, pre_a = pp, aa
            top_ped, top_amp = pp, aa
        else:
            top_ped, top_amp = pedals, amps
        tm["prescreen"] = time.time() - t1
        # --- pairs shared by path A / path B / single ---------------------------------------------------------------
        self.prog.stage("screen", "rendering candidate pairs")
        pairs = [((p,) if p is not None else (), a) for p in [None] + pre_p for a in pre_a]
        if len(pairs) > plan.cap_pairs:
            idx = sorted(self.rng.choice(len(pairs), size=plan.cap_pairs, replace=False))
            pairs = [pairs[i] for i in idx]
        n_pairs0 = len(pairs)
        self.log(f"stage1: {len(pairs)} pair renders (pedals {len(pre_p)}+none x amps {len(pre_a)}; "
                 f"full {full_pairs}); blend combos screened {len(pairs) * (len(pairs) - 1):,}"
                 + (f"; coarse pass on {plan.coarse_s:g} s" if coarse else ""))
        if coarse:
            t1 = time.time()
            cc = self._cores([(*p, a) for p, a in pairs], sx, self.prog.callback(0.0, 0.7))
            lc = self._lin(cc, "a", sx)
            del cc
            mats, pw = _band_arrays(lc, st.starts)
            e1c = single_errors(pw, st.ref.band_db, st.hf_limit_hz)
            ebc, _ = blend_errors(mats, pw, st.ref.band_db, st.hf_limit_hz)
            del mats, lc
            n_keep = max(plan.coarse_min_keep, int(np.ceil(plan.coarse_keep * len(pairs))))
            keep = select_coarse(e1c, ebc, n_keep)
            self.prog.best(min(float(e1c.min()), float(ebc.min())))
            self.stats["coarse"] = {"pairs": len(pairs), "kept": len(keep), "bestSingleDb": float(e1c.min()),
                                    "bestBlendDb": float(ebc.min())}
            self.log(f"stage1: coarse pass kept {len(keep)}/{len(pairs)} pairs "
                     f"(best coarse single {e1c.min():.2f} dB, blend {ebc.min():.2f} dB; {time.time() - t1:.0f}s)")
            pairs = [pairs[i] for i in keep]
            tm["pairsCoarse"] = time.time() - t1
        self.stats.update(sampledPairs=[[[c.key for c in p], a.key] for p, a in pairs], pairsRendered=len(pairs),
                          pairsCoarse=n_pairs0, pedalsInSearch=len(pre_p), ampsInSearch=len(pre_a))
        t1 = time.time()
        cores = self._cores([(*p, a) for p, a in pairs], None, self.prog.callback(0.7 if coarse else 0.0, 0.95))
        self.log(f"stage1: pair cores done ({time.time() - t0:.0f}s)")
        tm["pairsFull"] = time.time() - t1
        t1 = time.time()
        lin = self._lin(cores)
        mats, pw = _band_arrays(lin, self.tgt.starts)
        e1 = single_errors(pw, self.tgt.ref.band_db, self.tgt.hf_limit_hz)
        eb, bb = blend_errors(mats, pw, self.tgt.ref.band_db, self.tgt.hf_limit_hz)
        del mats
        self.prog.best(min(float(e1.min()), float(eb.min())))
        tm["pairScoring"] = time.time() - t1
        self.log(f"stage1: scored; best single {e1.min():.2f} dB, best blend {eb.min():.2f} dB ({time.time() - t0:.0f}s)")
        out: dict[str, list[Scored]] = {}
        cab0 = self.cab0
        self.prog.update(0.95, "re-scoring the best candidates")
        # --- single --------------------------------------------------------------------------------------------------
        t1 = time.time()
        if plan.top_k.get("single", 0):
            top = np.argsort(e1, kind="stable")[: plan.n_rescore_single]
            cands = [(Combo(pairs[i][0], pairs[i][1], None, None, cab0), (cores[i],)) for i in top]
            out["single"] = self._finish(cands, plan.n_cab_single, plan.top_k["single"], t0, "single")
        tm["single"] = time.time() - t1
        # --- blend ---------------------------------------------------------------------------------------------------
        t1 = time.time()
        if plan.top_k.get("blend", 0):
            ij = _argsort2d(eb, plan.n_rescore)
            self.stats["blendScreenTop"] = [{"A": [[c.key for c in pairs[i][0]], pairs[i][1].key],
                                              "B": [[c.key for c in pairs[j][0]], pairs[j][1].key],
                                              "ltasErr": float(eb[i, j]), "blend": float(bb[i, j])} for i, j in ij[:10]]
            cands = [(Combo(pairs[i][0], pairs[i][1], pairs[j][0], pairs[j][1], cab0), (cores[i], cores[j]))
                     for i, j in ij]
            out["blend"] = self._finish(cands, plan.n_cab, plan.top_k["blend"], t0, "blend")
        tm["blend"] = time.time() - t1
        # --- single2 -------------------------------------------------------------------------------------------------
        t1 = time.time()
        if plan.top_k.get("single2", 0) and len(top_ped) >= 2:
            n_p = min(len(top_ped), plan.n2_pedals)
            tp = top_ped[:n_p]
            ta = top_amp[:plan.n2_amps]
            chains = [(p1, p2, a) for p1 in tp for p2 in tp if p1.key != p2.key for a in ta]
            self.log(f"stage1: single2 {len(chains)} chains ({len(tp)} pedals x {len(ta)} amps)"
                     + ("; coarse pass" if coarse else ""))
            if coarse:      # score every chain on the short excerpt, render only the best on the full one
                l2 = self._lin(self._cores(chains, sx, self.prog.callback(0.95, 0.98)), "a", sx)
                _, pw2 = _band_arrays(l2, st.starts)
                e2 = single_errors(pw2, st.ref.band_db, st.hf_limit_hz)
                top = np.argsort(e2, kind="stable")[: plan.n_rescore_single]
                c2 = dict(zip((int(i) for i in top), self._cores([chains[i] for i in top])))
            else:
                c2 = dict(enumerate(self._cores(chains)))
                l2 = self._lin([c2[i] for i in range(len(chains))])
                _, pw2 = _band_arrays(l2, self.tgt.starts)
                e2 = single_errors(pw2, self.tgt.ref.band_db, self.tgt.hf_limit_hz)
                top = np.argsort(e2, kind="stable")[: plan.n_rescore_single]
            cands = [(Combo(chains[i][:2], chains[i][2], None, None, cab0), (c2[int(i)],)) for i in top]
            out["single2"] = self._finish(cands, plan.n_cab_single, plan.top_k["single2"], t0, "single2")
        tm["single2"] = time.time() - t1
        self.stats["stage1Seconds"] = time.time() - t0
        self.stats["timings"] = {k: round(v, 2) for k, v in tm.items()}
        for lst in out.values():
            for s in lst:
                s.extra.pop("_cores", None)
        return out

    def _finish(self, cands, n_cab: int, top_k: int, t0: float, name: str) -> list[Scored]:
        eng = self.eng

        def rescore(item):
            combo, cores = item
            align = eng.probe_align(combo, self.v0) if combo.topology == "blend" else manual_align(0, False)
            v = Space.for_combo(combo).default()
            return self._score(combo, v, cores, align)

        res = eng.map(rescore, cands)
        res.sort(key=lambda s: s.loss)
        self.log(f"stage1[{name}]: {len(res)} rescored with the full loss ({time.time() - t0:.0f}s); best {res[0].loss:.3f}")

        def cab_job(job):
            s, cab = job
            combo = s.combo.with_cab(cab)
            blends = None if s.combo.topology != "blend" else \
                np.array([s.blend - 0.1, s.blend, s.blend + 0.1]).clip(0.05, 0.95)
            return self._score(combo, Space.for_combo(combo).default(), s.extra["_cores"], s.align, blends, "screen+cab")

        jobs = [(s, cab) for s in res[:n_cab] for cab in self.pool.cabs if cab.key != self.cab0.key]
        allres = res + eng.map(cab_job, jobs)
        allres.sort(key=lambda s: s.loss)
        self.log(f"stage1[{name}]: cab sweep ({len(jobs)} combos) done ({time.time() - t0:.0f}s); best {allres[0].loss:.3f}")
        seen, out = set(), []
        for s in allres:
            k = s.combo.pair_key()
            if k not in seen:
                seen.add(k)
                out.append(s)
        return out
