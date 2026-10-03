"""Per-capture pre-screen (spec 3.3 A): keep the top-N captures per gear class before the pair search.

Each capture is rendered once on the DI excerpt and scored with the matcher's A-weighted LTAS error against the
reference (level offset removed), through the default cab:

* amps: amp alone (no pedal) -> cab;
* pedals: pedal -> proxy amp -> cab, for two proxy amps (the best ``amp_low`` and the best ``amp_high`` amp found above);
  a pedal's score is the better of the two.

The top ``n`` of every class survive (classes: drive, distortion, fuzz, preamp, pedal_unknown; amp_low, amp_high), so
every style keeps candidates even when its class scores poorly overall. Used automatically only when the full pair
product exceeds the pair cap (``Plan.cap_pairs``); ``--prescreen N`` forces it. Recall against the full search is
measured by ``matcher/recall.py`` (see README).
"""
from __future__ import annotations

import numpy as np

from . import loss as L
from .classify import AMP_CLASSES, PEDAL_CLASSES
from .engine import Engine
from .pool import Capture, Pool
from .space import Space, chain_blocks

DEFAULT_V = Space((1, 1)).default()


def _score(eng: Engine, blocks, cab0, ex, tgt) -> float:
    core = eng.core_blocks(blocks, cab0, ex.x)
    y = ex.trim(eng.linear(cab0, DEFAULT_V, "a", core))
    f = L.features(y, tgt.starts, None)
    return L.ltas_error(f.band_db, tgt.ref.band_db)[0]


def auto_n(n_ped_classes: int, n_amp_classes: int, cap_pairs: int) -> int:
    """Largest per-class N such that (pedal slots + none) x amps <= cap_pairs."""
    n = 1
    while ((n_ped_classes * (n + 1) + 1) * (n_amp_classes * (n + 1)) <= cap_pairs):
        n += 1
    return n


def prescreen(eng: Engine, pool: Pool, ex, tgt, cab0: Capture, n_per_class: int, log=print) -> tuple[list, list, dict]:
    amp_scores = dict(zip((a.key for a in pool.amps),
                          eng.map(lambda a: _score(eng, chain_blocks("a", (), a, {}), cab0, ex, tgt), pool.amps)))
    proxies = []
    for cls in AMP_CLASSES:
        c = [a for a in pool.amps if a.kind == cls]
        if c:
            proxies.append(min(c, key=lambda a: (amp_scores[a.key], a.key)))
    jobs = [(p, m) for p in pool.pedals for m in proxies]
    res = eng.map(lambda pm: _score(eng, chain_blocks("a", (pm[0],), pm[1], {}), cab0, ex, tgt), jobs) if jobs else []
    ped_scores: dict[str, float] = {}
    for (p, m), e in zip(jobs, res):
        ped_scores[p.key] = min(e, ped_scores.get(p.key, np.inf))
    keep_amps, keep_pedals = [], []
    for cls in AMP_CLASSES:
        c = sorted((a for a in pool.amps if a.kind == cls), key=lambda a: (amp_scores[a.key], a.key))
        keep_amps += c[:n_per_class]
    for cls in PEDAL_CLASSES:
        c = sorted((p for p in pool.pedals if p.kind == cls), key=lambda p: (ped_scores.get(p.key, np.inf), p.key))
        keep_pedals += c[:n_per_class]
    keep_amps.sort(key=lambda a: (amp_scores[a.key], a.key))
    keep_pedals.sort(key=lambda p: (ped_scores.get(p.key, np.inf), p.key))
    info = {"nPerClass": n_per_class, "proxyAmps": [a.key for a in proxies],
            "keptAmps": [a.key for a in keep_amps], "keptPedals": [p.key for p in keep_pedals],
            "ampScores": {k: round(float(v), 3) for k, v in amp_scores.items()},
            "pedalScores": {k: round(float(v), 3) for k, v in ped_scores.items()}}
    log(f"prescreen: kept {len(keep_pedals)}/{len(pool.pedals)} pedals and {len(keep_amps)}/{len(pool.amps)} amps "
        f"(top {n_per_class} per class)")
    return keep_pedals, keep_amps, info
