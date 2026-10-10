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


def _lin_sig(eng: Engine, blocks, cab0, ex) -> np.ndarray:
    core = eng.core_blocks(blocks, cab0, ex.x)
    return ex.trim(eng.linear(cab0, DEFAULT_V, "a", core))


def _err(y: np.ndarray, tgt) -> float:
    f = L.features(y, tgt.starts, None)
    return L.ltas_error(f.band_db, tgt.ref.band_db, tgt.hf_limit_hz)[0]


def _score(eng: Engine, blocks, cab0, ex, tgt) -> float:
    return _err(_lin_sig(eng, blocks, cab0, ex), tgt)


def auto_n_amps(n_ped_classes: int, n_amp_classes: int, n_ped: int, cap_pairs: int, n_amp_extra: int = 0) -> int:
    """Largest per-class amp quota N such that (pedal classes x n_ped + none) x (amp classes x (N + extra)) <= cap_pairs
    (pedals are cheaper to rule out than amps, so the quick mode spends the pair budget on amps)."""
    n = 1
    while (n_ped_classes * n_ped + 1) * (n_amp_classes * (n + 1 + n_amp_extra)) <= cap_pairs:
        n += 1
    return n


def auto_n(n_ped_classes: int, n_amp_classes: int, cap_pairs: int) -> int:
    """Largest per-class N such that (pedal slots + none) x amps <= cap_pairs."""
    n = 1
    while ((n_ped_classes * (n + 1) + 1) * (n_amp_classes * (n + 1)) <= cap_pairs):
        n += 1
    return n


def prescreen(eng: Engine, pool: Pool, ex, tgt, cab0: Capture, n_per_class: int, log=print, *,
              n_pedals_per_class: int | None = None, blend_aware: bool = False, progress=None,
              n_blend_pedals: int = 0, n_blend_amps: int = 0) -> tuple[list, list, dict]:
    """Keep the top captures per gear class. ``n_per_class`` is the quota of amp classes (and of pedal classes unless
    ``n_pedals_per_class`` is given), ranked by the capture's own error.

    ``blend_aware``: in addition every class keeps ``n_blend_pedals`` / ``n_blend_amps`` more captures ranked by the best
    BLEND they take part in, so a part that is poor alone but complements another path (a thin, fizzy chainsaw next to a
    thick body) survives. The same renders are reused: the signals are the amps alone and each pedal -> proxy amp chain;
    all their pairs are scored from band cross-spectra (screen.blend_errors, no extra render). The two rankings are kept
    separate (a joint ranking fills the quota with the partners of the one best blend)."""
    n_p = n_per_class if n_pedals_per_class is None else n_pedals_per_class
    n_amps, n_jobs = len(pool.amps), len(pool.pedals) * 2
    done = [0]

    def tick(*_):
        done[0] += 1
        if progress is not None:
            progress(done[0] / max(n_amps + n_jobs, 1))

    amp_sigs: dict[str, np.ndarray] = {}

    def amp_job(a):
        y = _lin_sig(eng, chain_blocks("a", (), a, {}), cab0, ex)
        if blend_aware:
            amp_sigs[a.key] = y.astype(np.float32)
        tick()
        return _err(y, tgt)

    amp_scores = dict(zip((a.key for a in pool.amps), eng.map(amp_job, pool.amps)))
    proxies = []
    for cls in AMP_CLASSES:
        c = [a for a in pool.amps if a.kind == cls]
        if c:
            proxies.append(min(c, key=lambda a: (amp_scores[a.key], a.key)))
    jobs = [(p, m) for p in pool.pedals for m in proxies]
    ped_sigs: dict[tuple[str, str], np.ndarray] = {}

    def ped_job(pm):
        y = _lin_sig(eng, chain_blocks("a", (pm[0],), pm[1], {}), cab0, ex)
        if blend_aware:
            ped_sigs[(pm[0].key, pm[1].key)] = y.astype(np.float32)
        tick()
        return _err(y, tgt)

    res = eng.map(ped_job, jobs) if jobs else []
    ped_scores: dict[str, float] = {}
    for (p, m), e in zip(jobs, res):
        ped_scores[p.key] = min(e, ped_scores.get(p.key, np.inf))
    info_blend = {}
    blend_amp, blend_ped = {}, {}
    if blend_aware:
        from .screen import _band_arrays, blend_errors      # lazy: screen imports this module
        keys = [("amp", a.key) for a in pool.amps] + [("ped", p.key, m.key) for p, m in jobs]
        sigs = [amp_sigs[k[1]] if k[0] == "amp" else ped_sigs[(k[1], k[2])] for k in keys]
        mats, pw = _band_arrays(sigs, tgt.starts)
        eb, _ = blend_errors(mats, pw, tgt.ref.band_db, tgt.hf_limit_hz)
        del mats
        best_blend = eb.min(axis=1)
        for k, bb in zip(keys, best_blend):
            if k[0] == "amp":
                blend_amp[k[1]] = float(bb)
            else:
                blend_ped[k[1]] = min(float(bb), blend_ped.get(k[1], np.inf))
        info_blend = {"blendAware": True, "signals": len(sigs), "nBlendPedals": n_blend_pedals, "nBlendAmps": n_blend_amps}

    def pick(items, single, blend, n_single, n_blend):
        """Top ``n_single`` by own error, then the next ``n_blend`` (not yet kept) by best-blend error."""
        out = sorted(items, key=lambda c: (single.get(c.key, np.inf), c.key))[:n_single]
        if blend and n_blend:
            have = {c.key for c in out}
            out += [c for c in sorted(items, key=lambda c: (blend.get(c.key, np.inf), c.key)) if c.key not in have][:n_blend]
        return out

    keep_amps, keep_pedals = [], []
    for cls in AMP_CLASSES:
        keep_amps += pick([a for a in pool.amps if a.kind == cls], amp_scores, blend_amp, n_per_class, n_blend_amps)
    for cls in PEDAL_CLASSES:
        keep_pedals += pick([p for p in pool.pedals if p.kind == cls], ped_scores, blend_ped, n_p, n_blend_pedals)
    keep_amps.sort(key=lambda a: (amp_scores[a.key], a.key))
    keep_pedals.sort(key=lambda p: (ped_scores.get(p.key, np.inf), p.key))
    info = {"nPerClass": n_per_class, "nPedalsPerClass": n_p, "proxyAmps": [a.key for a in proxies],
            "keptAmps": [a.key for a in keep_amps], "keptPedals": [p.key for p in keep_pedals],
            "ampScores": {k: round(float(v), 3) for k, v in amp_scores.items()},
            "pedalScores": {k: round(float(v), 3) for k, v in ped_scores.items()},
            "ampBlendScores": {k: round(v, 3) for k, v in blend_amp.items()},
            "pedalBlendScores": {k: round(v, 3) for k, v in blend_ped.items()}, **info_blend}
    log(f"prescreen{' (blend-aware)' if blend_aware else ''}: kept {len(keep_pedals)}/{len(pool.pedals)} pedals and "
        f"{len(keep_amps)}/{len(pool.amps)} amps (top {n_p}+{n_blend_pedals} pedals / {n_per_class}+{n_blend_amps} amps per class)")
    return keep_pedals, keep_amps, info
