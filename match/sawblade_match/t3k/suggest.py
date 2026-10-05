"""Task C pool rule: suggest a high-gain "body" amp capture for path B given path A's amp title.

Pure and deterministic. Candidates are the pool's amp models classified ``amp_high``; ordering is
(different amp family from A first, then already cached first, then pool order).
"""
from __future__ import annotations

import re
from typing import Any, Iterable, Sequence

from ..matcher.classify import _HIGH_AMPS, classify
from .cache import Cache

_ALIAS = re.compile(r"(?P<f5150>5150|5153|6505|evh)|(?P<recto>recto|dual|triple)")


def family_key(title: str | None) -> str:
    """Amp family of a title. First the alias families anywhere in the text (leftmost wins):
    5150/5153/6505/evh -> "5150", recto/dual/triple -> "recto". Otherwise the leftmost other
    ``_HIGH_AMPS`` match (lower-cased, non-alphanumerics removed; "peavey" itself is never folded into
    5150), otherwise the first alphabetic word of the title. ``""`` when there is none (unknown)."""
    t = (title or "").lower()
    m = _ALIAS.search(t)
    if m:
        return "5150" if m.group("f5150") else "recto"
    m = _HIGH_AMPS.search(t)
    if m:
        return re.sub(r"[^a-z0-9]", "", m.group(0))
    w = re.search(r"[a-z]+", t)
    return w.group(0) if w else ""


def pool_candidates(manifest: dict[str, Any] | None, cache: Cache) -> list[dict[str, Any]]:
    """One record per model of every included amp tone of a `pool_manifest.json`, in pool order:
    ``{"tone_id", "model_id", "title", "name", "cached"}`` (``cached`` = the capture file is in the cache)."""
    out = []
    for e in (manifest or {}).get("tones") or []:
        if not isinstance(e, dict) or e.get("tone_id") is None:
            continue
        if (e.get("slot") or e.get("gear")) != "amp" or e.get("status", "included") != "included":
            continue
        for m in e.get("models") or []:
            if not isinstance(m, dict) or m.get("id") is None:
                continue
            out.append({"tone_id": int(e["tone_id"]), "model_id": int(m["id"]), "title": e.get("title") or "",
                        "name": m.get("name") or "",
                        "cached": cache.get(int(e["tone_id"]), int(m["id"])) is not None})
    return out


def suggest_body(pool_records: Iterable[dict[str, Any]], a_title: str | None) -> dict[str, Any] | None:
    """Best body-amp candidate or ``None``. ``pool_records`` are `pool_candidates` records.

    Only ``amp_high`` records qualify. "Different family" needs both families known; an unknown /
    empty A title therefore ranks nobody as different and cached-first decides.
    """
    fa = family_key(a_title)
    best: tuple[tuple[int, int, int], dict[str, Any]] | None = None
    for i, r in enumerate(pool_records):
        if classify("amp", r.get("title") or "", r.get("name") or "") != "amp_high":
            continue
        fr = family_key(r.get("title"))
        key = (0 if fa and fr and fr != fa else 1, 0 if r.get("cached") else 1, i)
        if best is None or key < best[0]:
            best = (key, r)
    if best is None:
        return None
    r = best[1]
    return {"tone_id": r["tone_id"], "model_id": r["model_id"], "title": r.get("title") or "",
            "cached": bool(r.get("cached"))}
