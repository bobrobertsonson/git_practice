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
                 seed: int, gens_linear: int = 30, pop_linear: int = 16, gens_gain: int = 8, pop_gain: int = 8,
                 gens_final: int = 20, log=print) -> tuple[dict, L.LossResult, dict]:
    t0 = time.time()
    lin_idx, gain_idx = space.indices("linear"), space.indices("gain")
    u = space.encode(v0)
    info = {"evals": {"linear": 0, "gain": 0}}

    def cores_for(vv):          # serial: also called from pool threads (never nest pool maps)
        return eng.core(combo, vv, "a", ex.x), eng.core(combo, vv, "b", ex.x)

    def cores_top(vv):          # top level only: the two paths in parallel
        ca, cb = eng.map(lambda p: eng.core(combo, vv, p, ex.x), ("a", "b"))
        return ca, cb

    def score(vv, ca, cb):
        y = ex.trim(eng.emulate(combo, vv, ca, cb, align))
        return L.evaluate(y, tgt, space.eq_gains(vv))

    def run_block(u, idx, popsize, gens, sigma, block, seed_off, cores=None):
        names = [space.names[i] for i in idx]

        def full_u(x):
            uu = u.copy()
            uu[idx] = x
            return uu

        if block == "linear":
            ca, cb = cores

            def f(x):
                return score(space.decode(full_u(x)), ca, cb).total

            def batch(X):
                info["evals"]["linear"] += len(X)
                return eng.map(f, list(X))
        else:
            def f(x):
                vv = space.decode(full_u(x))
                ca, cb = cores_for(vv)
                return score(vv, ca, cb).total

            def batch(X):
                info["evals"]["gain"] += len(X)
                return eng.map(f, list(X))      # each evaluation renders both cores in its own thread
        bx, bf, hist = cma.minimize(None, u[idx], sigma, popsize, gens, seed + seed_off, evaluate_batch=batch)
        out = u.copy()
        out[idx] = bx
        return out, bf, hist

    v = space.decode(u)
    ca, cb = cores_top(v)
    r0 = score(v, ca, cb)
    log(f"  start loss {r0.total:.3f} (ltas {r0.ltas:.2f})")
    u, f1, h1 = run_block(u, lin_idx, pop_linear, gens_linear, 0.2, "linear", 1, (ca, cb))
    log(f"  block L1: {f1:.3f} ({time.time() - t0:.0f}s)")
    u, f2, h2 = run_block(u, gain_idx, pop_gain, gens_gain, 0.25, "gain", 2, None)
    log(f"  block G : {f2:.3f} ({time.time() - t0:.0f}s)")
    v = space.decode(u)
    ca, cb = cores_top(v)
    u, f3, h3 = run_block(u, lin_idx, pop_linear, gens_final, 0.1, "linear", 3, (ca, cb))
    v = space.decode(u)
    r = score(v, ca, cb)
    log(f"  block L2: {r.total:.3f} ({time.time() - t0:.0f}s)")
    info.update(startLoss=r0.total, history={"L1": h1, "G": h2, "L2": h3}, seconds=time.time() - t0)
    return v, r, info
