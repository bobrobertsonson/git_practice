"""Candidate pool: read ``pool_manifest.json`` and the capture cache, keep only downloaded captures.

Slots are typed by gear class (classify.py), never by title filters: pedals (any class, or none), amps (any class) and
cabs (shared IR).
Licenses were filtered by ``sawblade-t3k pull``; they are re-checked here (never non-commercial).
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

import numpy as np
from pathlib import Path

from ..t3k.cache import Cache, default_cache_root
from ..t3k.licenses import check_license
from .classify import classify


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
    kind: str = ""       # gear class (classify.py): drive | distortion | fuzz | preamp | pedal_unknown | amp_low | amp_high | cab
    arch: str = ""       # manifest architecture_version ("1"/"2")
    size_label: str = "" # manifest size, else lite/feather/xstandard/standard parsed from the model name

    @property
    def size_rank(self) -> tuple[int, int]:
        """(category rank, +-10 % byte bucket): smaller is lighter. Category: feather < lite < standard < xstandard
        < custom/unknown-large (label from the manifest ``size`` or the model name; default standard). Bytes are bucketed
        in 10 % steps so near-equal files tie."""
        order = {"feather": 0, "lite": 1, "standard": 2, "xstandard": 3, "custom": 4}
        lab = (self.size_label or "standard").lower()
        return order.get(lab, 2), int(np.log(max(self.size_bytes, 1)) / np.log(1.1))

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
    pedals: list[Capture] = field(default_factory=list)
    amps: list[Capture] = field(default_factory=list)
    cabs: list[Capture] = field(default_factory=list)

    def counts(self) -> dict:
        out = {"pedals": len(self.pedals), "amps": len(self.amps), "cabs": len(self.cabs)}
        for c in (*self.pedals, *self.amps):
            out[c.kind] = out.get(c.kind, 0) + 1
        return out

    def of_class(self, cls: str) -> list[Capture]:
        return [c for c in (*self.pedals, *self.amps, *self.cabs) if c.kind == cls]


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
            kind = classify(gear, t["title"], md.get("name", ""))
            label = (md.get("size") or entry.model.get("size") or "").lower()
            if not label:
                lm = re.search(r"\b(feather|lite|xstandard|custom)\b", md.get("name", ""), re.I)
                label = lm.group(1).lower() if lm else "standard"
            cap = Capture(int(t["tone_id"]), int(md["id"]), t["title"], md.get("name", ""), gear,
                          str(entry.path), entry.sha256, entry.path.stat().st_size, t.get("license", ""),
                          t.get("creator") or t.get("creator_username") or "", t.get("url", ""), kind,
                          str(md.get("architecture_version") or ""), label)
            {"pedal": pool.pedals, "amp": pool.amps, "cab": pool.cabs}[gear].append(cap)
    return pool


def default_cab(cabs: list[Capture]) -> Capture:
    """Deterministic default cab: first 'V30' IR, else the first."""
    for c in cabs:
        if "v30" in c.title.lower() or "v30" in c.name.lower():
            return c
    return cabs[0]


def starter_choice(pool: Pool) -> dict[str, Capture | None]:
    """Captures substituted into presets/chainsaw_body.json for the 'before' measurement (a hand-made starter shape:
    saw pedal / saw amp / boost / body amp / cab). saw pedal: first ``distortion`` class capture whose name suggests
    maxed settings, else the first distortion, else the first pedal; saw amp: lowest gain class (low < medium < unknown
    < high), first in manifest order; boost: first ``drive`` capture with 'ts' in the title, else the first drive;
    body amp: first 'high'-class 5150/6505 amp, else the first 'high' class; cab: default_cab."""
    def first(lst, pred):
        return next((c for c in lst if pred(c)), None)
    dist = [c for c in pool.pedals if c.kind == "distortion"] or pool.pedals
    drive = [c for c in pool.pedals if c.kind == "drive"] or pool.pedals
    hm2 = first(dist, lambda c: re.search(r"full|l-10|max", c.name, re.I)) or dist[0]
    rank = {"low": 0, "medium": 1, "unknown": 2, "high": 3}
    saw = min(pool.amps, key=lambda c: (rank[gain_class(c.title, c.name)], pool.amps.index(c)))
    boost = first(drive, lambda c: "ts" in c.title.lower()) or drive[0]
    body = (first(pool.amps, lambda c: re.search(r"5150|5153|6505", c.title + c.name, re.I)
                  and gain_class(c.title, c.name) == "high")
            or first(pool.amps, lambda c: gain_class(c.title, c.name) == "high") or pool.amps[-1])
    return {"hm2": hm2, "saw_amp": saw, "boost": boost, "body_amp": body, "cab": default_cab(pool.cabs)}
