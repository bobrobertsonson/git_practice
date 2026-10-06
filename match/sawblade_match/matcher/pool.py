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
from .classify import AMP_CLASSES, PEDAL_CLASSES, classify


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
    # tone id -> what the manifest says about it (title, slot, status, models with a downloaded flag); for --trace-tones
    catalog: dict = field(default_factory=dict)

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
        gear = t.get("slot") or t.get("gear")
        mds = [md for md in t.get("models", []) if "id" in md]        # a model entry without an id is skipped
        cat = {"title": t.get("title"), "slot": gear, "status": t.get("status", "included"), "license": t.get("license"),
               "models": [{"modelId": int(md["id"]), "name": md.get("name", ""), "downloaded": False}
                          for md in mds], "reason": None}
        try:
            pool.catalog[int(t["tone_id"])] = cat
        except (KeyError, TypeError, ValueError):
            pass
        if t.get("status", "included") != "included":
            cat["reason"] = f"status {t.get('status')}"
            continue
        if gear not in ("pedal", "amp", "cab"):
            cat["reason"] = f"gear {gear!r} is not a pool slot"
            continue
        try:
            check_license(t.get("license"), f"tone {t['tone_id']}")
        except Exception:
            cat["reason"] = f"license {t.get('license')!r} not allowed"
            continue
        for md, cm in zip(mds, cat["models"]):
            entry = cache.get(t["tone_id"], md["id"])
            if entry is None:      # not downloaded (or sha mismatch): not a candidate
                continue
            cm["downloaded"] = True
            kind = md.get("classOverride") or t.get("classOverride") or classify(gear, t["title"], md.get("name", ""))
            allowed = {"pedal": PEDAL_CLASSES, "amp": AMP_CLASSES, "cab": ("cab",)}[gear]
            if kind not in allowed:
                raise ValueError(f"tone {t['tone_id']} model {md['id']}: classOverride {kind!r} not valid for gear "
                                 f"{gear!r} (one of {', '.join(allowed)})")
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
    """Generic starter baseline (class-agnostic, nothing style- or gear-specific): the first amp and the first cab of the
    pool, no pedals, flat EQ. Needs only one amp and one cab; every pedal class may be missing."""
    if not pool.amps or not pool.cabs:
        raise ValueError("the pool needs at least one amp and one cab")
    return {"amp": pool.amps[0], "cab": pool.cabs[0]}
