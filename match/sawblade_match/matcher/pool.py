"""Candidate pool: read ``pool_manifest.json`` and the capture cache, keep only downloaded captures.

Slots (by gear): pedals whose title contains "HM-2" -> ``hm2`` (path A pedal); other pedals -> ``boost``
(path B, optional); amps -> both ``saw_amp`` and ``body_amp``; cabs -> shared ``cab``.
Licenses were filtered by ``sawblade-t3k pull``; they are re-checked here (never non-commercial).
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from ..t3k.cache import Cache, default_cache_root
from ..t3k.licenses import check_license


@dataclass(frozen=True)
class Capture:
    tone_id: int
    model_id: int
    title: str
    name: str
    gear: str            # pedal | amp | cab
    path: str
    sha256: str
    size_bytes: int
    license: str
    creator: str
    url: str
    kind: str = ""       # hm2 | boost | amp | cab

    @property
    def key(self) -> str:
        return f"{self.tone_id}/{self.model_id}"

    def block_model(self) -> dict:
        """Preset ``Capture`` object with TONE3000 source ids + resolved file path."""
        return {"file": self.path, "sha256": self.sha256,
                "source": {"provider": "tone3000", "id": str(self.tone_id), "modelId": str(self.model_id),
                           "url": self.url, "title": self.title, "creator": self.creator,
                           "license": self.license}}


@dataclass
class Pool:
    hm2: list[Capture] = field(default_factory=list)
    boost: list[Capture] = field(default_factory=list)
    amps: list[Capture] = field(default_factory=list)
    cabs: list[Capture] = field(default_factory=list)

    def counts(self) -> dict:
        return {"hm2": len(self.hm2), "boost": len(self.boost), "amps": len(self.amps), "cabs": len(self.cabs)}


_GAIN_NUM = re.compile(r"gain[\s_\-:]*0?(\d{1,2})\b", re.I)


def gain_class(title: str, name: str) -> str:
    """Heuristic 'low' | 'medium' | 'high' | 'unknown' from titles/model names (soft prior only)."""
    s = f"{title} {name}"
    low = re.search(r"clean|crunch|\blow\b|\blo\b|edge of", s, re.I)
    high = re.search(r"\bhigh\b|\blead\b|\bhi\b|rhythm\s*[34]|brown|\bred\b|insane|ultra|ch\s*4|hbe|gain\s*(?:9|10)\b", s, re.I)
    m = _GAIN_NUM.search(s)
    if m:
        g = int(m.group(1))
        return "low" if g <= 4 else ("medium" if g <= 7 else "high")
    if low and not high:
        return "low"
    if high and not low:
        return "high"
    return "unknown"


def load_pool(manifest: str | Path, cache_root: Path | None = None) -> Pool:
    manifest = Path(manifest)
    m = json.loads(manifest.read_text())
    cache = Cache(cache_root or (manifest.parent if (manifest.parent / "pool_manifest.json").exists()
                                 else default_cache_root()))
    pool = Pool()
    for t in m.get("tones", []):
        if t.get("status", "included") != "included":
            continue
        gear = t.get("slot") or t.get("gear")
        if gear not in ("pedal", "amp", "cab"):
            continue
        try:
            check_license(t.get("license"), f"tone {t['tone_id']}")
        except Exception:
            continue
        for md in t.get("models", []):
            entry = cache.get(t["tone_id"], md["id"])
            if entry is None:      # not downloaded (or sha mismatch): not a candidate
                continue
            if gear == "pedal":
                kind = "hm2" if "hm-2" in t["title"].lower() else "boost"
            else:
                kind = gear
            cap = Capture(int(t["tone_id"]), int(md["id"]), t["title"], md.get("name", ""), gear,
                          str(entry.path), entry.sha256, entry.path.stat().st_size, t.get("license", ""),
                          t.get("creator") or t.get("creator_username") or "", t.get("url", ""), kind)
            {"hm2": pool.hm2, "boost": pool.boost, "amp": pool.amps, "cab": pool.cabs}[kind].append(cap)
    return pool


def default_cab(cabs: list[Capture]) -> Capture:
    """Deterministic default cab: first 'V30' IR, else the first."""
    for c in cabs:
        if "v30" in c.title.lower() or "v30" in c.name.lower():
            return c
    return cabs[0]


def starter_choice(pool: Pool) -> dict[str, Capture | None]:
    """Captures substituted into presets/chainsaw_body.json for the 'before' measurement.

    hm2: first HM-2 model whose name suggests maxed settings (FULL / 10 / MAX) else first; saw amp: lowest
    gain class (low < medium < unknown < high), first in manifest order; boost: first pedal with 'TS' in the
    title else first boost; body amp: first 'high'-class 5150/6505 amp else the first 'high' class; cab: default_cab."""
    def first(lst, pred):
        return next((c for c in lst if pred(c)), None)
    hm2 = first(pool.hm2, lambda c: re.search(r"full|l-10|max", c.name, re.I)) or pool.hm2[0]
    rank = {"low": 0, "medium": 1, "unknown": 2, "high": 3}
    saw = min(pool.amps, key=lambda c: (rank[gain_class(c.title, c.name)], pool.amps.index(c)))
    boost = first(pool.boost, lambda c: "ts" in c.title.lower()) or (pool.boost[0] if pool.boost else None)
    body = (first(pool.amps, lambda c: re.search(r"5150|5153|6505", c.title + c.name, re.I)
                  and gain_class(c.title, c.name) == "high")
            or first(pool.amps, lambda c: gain_class(c.title, c.name) == "high") or pool.amps[-1])
    return {"hm2": hm2, "saw_amp": saw, "boost": boost, "body_amp": body, "cab": default_cab(pool.cabs)}
