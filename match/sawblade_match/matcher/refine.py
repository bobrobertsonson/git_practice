"""Stage 2: seeded CMA-ES on the continuous parameters of one discrete combo.

The parameters split into two groups with very different cost (see space.py):
* ``linear`` (blend, levels, path EQs, post EQ: 25 params): evaluated with two cheap renders of the same C++ renderer
  on the stored NAM cores (~20 ms) + the loss;
* ``gain`` (NAM input gains, 3-4 params): each evaluation re-renders both NAM cores.
They are optimised by alternating CMA-ES blocks  L -> G -> L  (block coordinate CMA-ES; each block is a full seeded CMA-ES
over its group with the other group fixed at the current best). All randomness comes from ``seed``.
"""
from __future__ import annotations

import time

import numpy as np

from . import cma
from . import loss as L
from .engine import Engine
from .space import Combo, Space


def refine_combo(eng: Engine, combo: Combo, space: Space, ex, tgt: L.Target, align: dict, v0: dict, *,
                 seed: int, levels=None, gens_linear: int = 30, pop_linear: int = 16, gens_gain: int = 8, pop_gain: int = 8,
                 gens_final: int = 20, patience: int | None = None, patience_gain: int | None = None, tol: float = 0.0, on_gen=None,
                 gex=None, gtgt=None, short_linear: bool = False, log=print) -> tuple[dict, L.LossResult, dict]:
    t0 = time.time()
    lin_idx, gain_idx = space.indices("linear"), space.indices("gain")
    u = space.encode(v0)
    info = {"evals": {"linear": 0, "gain": 0}}

    paths = ("a", "b") if combo.topology == "blend" else ("a",)

    # the gain block may run on a shorter excerpt (``gex``/``gtgt``): each of its evaluations re-renders the NAMs
    gx, gt = (gex, gtgt) if (gex is not None and gtgt is not None) else (ex, tgt)
    # staged objective: the first linear block (EQs/levels/blend, the coarse spectral fit) is LTAS-led, the feel term is
    # added from the gain block on. On a combo that is not the exact answer the feel optimum differs from the spectral
    # one; letting it steer the first fit trades spectral accuracy for feel before the spectrum is even in place.
    tgt_l1 = L.without_feel(tgt)
    gt_l1 = L.without_feel(gt)

    def cores_for(vv):          # serial: also called from pool threads (never nest pool maps)
        r = [eng.core(combo, vv, p, gx.x) for p in paths]
        return r[0], (r[1] if len(r) > 1 else None)

    def cores_top(vv, x=None):  # top level only: the paths in parallel
        r = eng.map(lambda p: eng.core(combo, vv, p, ex.x if x is None else x), paths)
        return r[0], (r[1] if len(r) > 1 else None)

    def score(vv, ca, cb, ex_=ex, tgt_=tgt):
        y = ex_.trim(eng.emulate(combo, vv, ca, cb, align, levels))
        return L.evaluate(y, tgt_, space.eq_gains(vv))

    def run_block(u, idx, popsize, gens, sigma, block, seed_off, cores=None, label=None, ctx=(ex, tgt)):
        names = [space.names[i] for i in idx]

        def full_u(x):
            uu = u.copy()
            uu[idx] = x
            return uu

        if block == "linear":
            ca, cb = cores

            def f(x):
                return score(space.decode(full_u(x)), ca, cb, *ctx).total

            def batch(X):
                info["evals"]["linear"] += len(X)
                return eng.map(f, list(X))
        else:
            def f(x):
                vv = space.decode(full_u(x))
                ca, cb = cores_for(vv)
                return score(vv, ca, cb, gx, gt).total

            def batch(X):
                info["evals"]["gain"] += len(X)
                return eng.map(f, list(X))      # each evaluation renders both cores in its own thread
        bx, bf, hist = cma.minimize(None, u[idx], sigma, popsize, gens, seed + seed_off, evaluate_batch=batch,
                                    patience=patience if block == "linear" else patience_gain, tol=tol,
                                    on_gen=None if on_gen is None else (lambda g, n: on_gen(label or block, g, n)))
        out = u.copy()
        out[idx] = bx
        return out, bf, hist

    v = space.decode(u)
    ca, cb = cores_top(v)
    r0 = score(v, ca, cb)
    log(f"  start loss {r0.total:.3f} (ltas {r0.ltas:.2f})")
    if short_linear and gx is not ex:       # first linear block on the short window too (its cores: one render per path)
        ca_s, cb_s = cores_top(v, gx.x)
        u, f1, h1 = run_block(u, lin_idx, pop_linear, gens_linear, 0.2, "linear", 1, (ca_s, cb_s), "L1", (gx, gt_l1))
    else:
        u, f1, h1 = run_block(u, lin_idx, pop_linear, gens_linear, 0.2, "linear", 1, (ca, cb), "L1", (ex, tgt_l1))
    log(f"  block L1: {f1:.3f} ({time.time() - t0:.0f}s)")
    u, f2, h2 = run_block(u, gain_idx, pop_gain, gens_gain, 0.25, "gain", 2, None, "G")
    log(f"  block G : {f2:.3f} ({time.time() - t0:.0f}s)")
    v = space.decode(u)
    ca, cb = cores_top(v)
    u, f3, h3 = run_block(u, lin_idx, pop_linear, gens_final, 0.1, "linear", 3, (ca, cb), "L2")
    v = space.decode(u)
    r = score(v, ca, cb)
    log(f"  block L2: {r.total:.3f} ({time.time() - t0:.0f}s)")
    info.update(startLoss=r0.total, history={"L1": h1, "G": h2, "L2": h3}, seconds=time.time() - t0)
    return v, r, info
