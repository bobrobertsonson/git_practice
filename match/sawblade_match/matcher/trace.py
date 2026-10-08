"""``--trace-tones``: why did a given TONE3000 tone (not) win? (v0.4M Task B2.4)

For every requested tone id the run writes ``result.json -> trace[id]``:

* ``inManifest`` / ``downloaded`` / ``models`` (each model: downloaded?, gear class, size) / ``gear`` / ``gearClass``;
* ``prescreen``: per downloaded capture its pre-screen score (A-weighted LTAS error, dB), rank inside its gear class and
  whether it survived (and whether the pre-screen limited the blend/single search at all);
* ``pair``: the best stage-1 pair containing it (rank among the rendered pairs, LTAS error, best blend error);
* ``stage1``: its best stage-1 candidate per topology after the full-loss re-score (rank, loss);
* ``candidate`` (amps): its best candidate loss. If it was refined in stage 2 that is the refined result; otherwise it is
  rendered once on the excerpt as the winner's chain with this amp in place of the winner's (same pedals, boost and cab, the
  winner's EQ and level parameters) followed by one linear CMA-ES block (``refine.relinear``, stage 2's last block);
* ``vsWinner``: the weighted loss terms of that candidate minus the winner's, largest first, and a one-line ``why``.

Cab tones list their IRs from the winner's cab sweep; pedal tones get the pre-screen / pair information only.
"""
from __future__ import annotations

import numpy as np

from . import loss as L
from .refine import SEED_TRACE, relinear
from .screen import Scored
from .space import Combo, Space, manual_align


def weighted_terms(r: L.LossResult) -> dict[str, float]:
    """The loss split into its weighted contributions (they add up to ``r.total``)."""
    w = (r.feel_terms or {}).get("weights", {})
    t = r.feel_terms or {}
    return {"ltas": L.W_LTAS * r.ltas, "buzz": L.W_BUZZ * r.buzz, "decay": L.W_DECAY * (r.decay or 0.0),
            "stft": L.W_STFT * (r.stft or 0.0), "reg": L.W_REG * r.reg, "tex": float(r.tex),
            "feelTight": w.get("tight", 0.0) * (t.get("tight") or 0.0), "feelFizz": w.get("fizz", 0.0) * (t.get("fizz") or 0.0),
            "feelPolish": w.get("polish", 0.0) * (t.get("polish") or 0.0)}


def vs_winner(r: L.LossResult, winner: L.LossResult) -> dict:
    a, b = weighted_terms(r), weighted_terms(winner)
    d = {k: a[k] - b[k] for k in a}
    top = sorted(d.items(), key=lambda kv: -kv[1])
    gap = r.total - winner.total
    worse = [f"{k} {v:+.2f}" for k, v in top[:3] if v > 0.005]
    why = (f"loss {r.total:.3f} vs the winner's {winner.total:.3f} ({gap:+.3f})"
           + (f"; mostly {', '.join(worse)}" if worse and gap > 0 else ""))
    return {"loss": r.total, "winnerLoss": winner.total, "deltaTotal": gap, "weighted": a, "winnerWeighted": b,
            "delta": dict(top), "why": why}


def _amp_keys(c: Combo) -> list[str]:
    return [c.a_amp.key] + ([c.b_amp.key] if c.b_amp is not None else [])


def _rank(values: dict, key, field: str):
    """1-based rank of ``values[key][field]`` among all entries that have the field."""
    mine = values[key][field]
    pool = [d[field] for d in values.values() if field in d]
    return 1 + sum(1 for x in pool if x < mine), len(pool)


def _prescreen(info: dict | None, caps, pool) -> dict:
    if not info:
        return {"applied": False, "note": "the pre-screen did not run (the pair product fits the cap): every capture is in the search"}
    out = {"applied": True, "limitsBlendSingleSearch": bool(info.get("appliedToBlendSingle")),
           "nPerClass": info.get("nPerClass"), "captures": []}
    for c in caps:
        if c.gear == "cab":
            continue
        scores = info["ampScores"] if c.gear == "amp" else info["pedalScores"]
        kept = info["keptAmps"] if c.gear == "amp" else info["keptPedals"]
        same = sorted((x for x in (pool.amps if c.gear == "amp" else pool.pedals) if x.kind == c.kind),
                      key=lambda x: (scores.get(x.key, np.inf), x.key))
        rank = next((i + 1 for i, x in enumerate(same) if x.key == c.key), None)
        out["captures"].append({"key": c.key, "class": c.kind, "scoreDb": scores.get(c.key), "rankInClass": rank,
                                "classSize": len(same), "survived": c.key in kept})
    return out


def _pair(scr, caps) -> dict:
    keys = {c.key for c in caps if c.gear != "cab"}
    log = scr.pair_log
    if not keys:
        return {}
    mine = {k: d for k, d in log.items() if k[1] in keys or keys & set(k[0])}
    if not log or not mine:
        return {"rendered": False, "note": "no pair containing it was rendered (pair cap / pre-screen / coarse pass)"}
    for field in ("full", "coarse"):
        have = {k: d for k, d in mine.items() if field in d}
        if have:
            k = min(have, key=lambda kk: have[kk][field])
            rank, n = _rank({kk: d for kk, d in log.items() if field in d}, k, field)
            return {"rendered": True, "stage": field, "rank": rank, "of": n, "ltasErrDb": have[k][field],
                    "pedals": list(k[0]), "amp": k[1], "bestBlendLtasDb": have[k].get("blendBest"),
                    "reachedFullPass": field == "full"}
    return {"rendered": True, "stage": None}


def trace_tones(ids, *, eng, pool, scr, ranked: dict, refined: list[Scored], best: Scored, ex, tgt, plan, cab_sweeps: list,
                seed: int, filters: bool = True, gate=None, on_tone=None, log=print) -> dict:
    out: dict = {}
    pre = scr.stats.get("prescreen")
    for n, tid in enumerate(ids):
        if on_tone is not None:
            on_tone(n, len(ids))
        log(f"trace: tone {tid}")
        out[str(tid)] = _trace_one(int(tid), eng, pool, scr, pre, ranked, refined, best, ex, tgt, plan, cab_sweeps, seed,
                                   filters, gate, log)
    return out


def _trace_one(tid, eng, pool, scr, pre, ranked, refined, best, ex, tgt, plan, cab_sweeps, seed, filters, gate, log) -> dict:
    allcaps = [c for c in (*pool.pedals, *pool.amps, *pool.cabs) if c.tone_id == tid]
    cat = pool.catalog.get(tid) if pool.catalog else None
    rec: dict = {"toneId": tid, "inManifest": (cat is not None) if pool.catalog else None,
                 "downloaded": bool(allcaps)}
    if cat:
        rec.update(title=cat["title"], slot=cat["slot"], status=cat["status"], license=cat["license"],
                   notInPoolBecause=None if allcaps else (cat["reason"] or "no model of it is downloaded"))
        by_id = {c.model_id: c for c in allcaps}
        rec["models"] = [{"modelId": m["modelId"], "name": m["name"], "downloaded": m["downloaded"],
                          "class": by_id[m["modelId"]].kind if m["modelId"] in by_id else None,
                          "sizeBytes": by_id[m["modelId"]].size_bytes if m["modelId"] in by_id else None}
                         for m in cat["models"]]
    else:
        rec["models"] = [{"modelId": c.model_id, "name": c.name, "downloaded": True, "class": c.kind,
                          "sizeBytes": c.size_bytes} for c in allcaps]
        if allcaps:
            rec["title"] = allcaps[0].title
    if not allcaps:
        rec["note"] = "no downloaded model of this tone is in the pool: it cannot be a candidate"
        return rec
    rec["gear"] = allcaps[0].gear
    rec["gearClass"] = sorted({c.kind for c in allcaps})
    rec["prescreen"] = _prescreen(pre, allcaps, pool)
    rec["pair"] = _pair(scr, allcaps)
    keys = {c.key for c in allcaps}
    if rec["gear"] == "cab":
        sw = next((s for s in cab_sweeps if tuple(s.get("pairKey", ())) == best.combo.pair_key()), None)     # the winner's own sweep
        if sw is None:
            rec["note"] = "cab sweep did not run for the winning candidate (--ablate irsweep, or it was not among the swept ones)"
        else:
            rec["cabSweep"] = [{"cab": i["cab"], "loss": i["loss"], "rank": 1 + n, "of": sw["nCabs"]}
                               for n, i in enumerate(sw["irs"]) if i["cab"] in keys]
            rec["winnerCab"] = best.combo.cab.key
        return rec
    # stage-1 candidates (full-loss re-score + cab sweep) per topology
    st1 = []
    for topo, lst in ranked.items():
        for i, s in enumerate(lst):
            if keys & set(_amp_keys(s.combo)) or keys & {c.key for c in (*s.combo.a_pedals, *(s.combo.b_pedals or ()))}:
                st1.append({"topology": topo, "boost": bool(s.combo.boost), "rank": i + 1, "of": len(lst), "loss": s.loss})
                break
    rec["stage1"] = st1
    if rec["gear"] != "amp":
        rec["note"] = "pedal tone: pre-screen and pair information only (candidates are traced for amps)"
        return rec
    if best.combo.a_amp.key in keys or (best.combo.b_amp is not None and best.combo.b_amp.key in keys):
        rec["candidate"] = {"how": "this tone is (part of) the winner", "loss": best.loss}
        rec["lost"] = False
        return rec
    ref_c = [s for s in refined if keys & set(_amp_keys(s.combo))]
    if ref_c:
        c = min(ref_c, key=lambda s: s.loss)
        rec["candidate"] = {"how": "refined in stage 2", "captures": c.combo.describe(), "loss": c.loss,
                            "breakdown": c.result.as_dict()}
        r = c.result
    else:
        trial = []
        for cap in [c for c in allcaps if c.gear == "amp"]:
            w = best.combo
            combo = Combo(w.a_pedals, cap, None, None, w.cab, w.boost)
            sp0 = Space.for_combo(combo, filters)
            v0 = sp0.default()
            for k in sp0.names:         # the winner's EQ / level / boost parameters; NAM input gains stay neutral
                if k in best.extra["params"] and (not k.startswith("gain.")):
                    v0[k] = best.extra["params"][k]
            v, r = relinear(eng, combo, sp0, ex, tgt, manual_align(0, False), v0, seed=seed * 1000 + SEED_TRACE, gens=plan.gens_final,
                            pop=plan.pop_linear, patience=plan.patience, tol=plan.plateau_tol, log=lambda *_: None,
                            **({} if gate is None else {"gate": gate}))
            trial.append((r.total, cap, r))
        trial.sort(key=lambda t: t[0])
        _, cap, r = trial[0]
        rec["candidate"] = {"how": "rendered once on the excerpt as the winner's chain with this amp + one linear block "
                                   "(not refined in stage 2)"
                                   + ("; the winner is a blend, so this is a single-path approximation (its path A pedals, "
                                      "boost and EQ)" if best.combo.b_amp is not None else ""),
                            "model": {"modelId": cap.model_id, "name": cap.name}, "loss": r.total,
                            "breakdown": r.as_dict(), "allModels": [{"modelId": c.model_id, "loss": l} for l, c, _ in trial]}
    rec["vsWinner"] = vs_winner(r, best.result)
    rec["lost"] = bool(r.total > best.loss)
    return rec

