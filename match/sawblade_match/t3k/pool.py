"""Candidate pool: collect -> quality filter -> choose models -> (optionally) download -> manifest."""
from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from .cache import Cache
from .client import T3KClient
from .fetch import ensure_capture, list_candidates
from .filter import Decision, FilterConfig, config_dict, evaluate
from .types import Tone

SLOT_GEAR = {"pedal": "pedal", "amp": "amp", "cab": "cab"}
ALL_SLOTS = ("pedal", "amp", "cab")


def collect(
    client: T3KClient,
    slots: Iterable[str] = ALL_SLOTS,
    *,
    trending: bool = True,
    latest: bool = True,
    search_query: str | None = None,
) -> tuple[dict[int, Tone], dict[int, list[str]]]:
    """Favorited (always) + free-tier trending/latest; search only when ``search_query`` is given."""
    tones: dict[int, Tone] = {}
    sources: dict[int, list[str]] = {}

    def add(src: str, ts: Iterable[Tone]) -> None:
        for t in ts:
            tones.setdefault(t.id, t)
            if src not in sources.setdefault(t.id, []):
                sources[t.id].append(src)

    slots = list(slots)
    add("favorited", client.list_favorited())
    if trending:
        for s in slots:
            add("trending", client.list_trending(SLOT_GEAR[s]))
    if latest:
        add("latest", client.list_latest())
    if search_query is not None:
        # Opt-in; needs a commercial agreement before shipping (see T3KClient.search).
        gears = "_".join(SLOT_GEAR[s] for s in slots)
        add("search", client.search(search_query, gears=gears))
    return tones, sources


def decision_to_json(d: Decision) -> dict[str, Any]:
    t = d.tone
    out = {
        "tone_id": t.id, "title": t.title, "creator": t.user.creator, "creator_username": t.user.username,
        "creator_verified": t.user.is_verified, "gear": t.gear, "format": t.format, "license": t.license,
        "url": t.url, "published_at": t.published_at, "updated_at": t.updated_at,
        "favorites_count": t.favorites_count, "downloads_count": t.downloads_count,
        "a1_models_count": t.a1_models_count, "a2_models_count": t.a2_models_count,
        "irs_count": t.irs_count, "sources": d.sources, "slot": d.slot, "status": d.status,
        "reasons": d.reasons, "flags": d.flags, "thresholds": d.thresholds,
        "models": d.models, "downloads": d.downloads,
    }
    if "calibrated" in t.raw:  # only recorded if the JSON exposes it (it is not a documented field)
        out["calibrated"] = t.raw["calibrated"]
    return out


def build_pool(
    client: T3KClient,
    cache: Cache,
    cfg: FilterConfig,
    *,
    slots: Iterable[str] = ALL_SLOTS,
    trending: bool = True,
    latest: bool = True,
    search_query: str | None = None,
    download: bool = True,
    max_models_per_tone: int = 3,
    now: datetime | None = None,
) -> dict[str, Any]:
    now = now or datetime.now(timezone.utc)
    slots = list(slots)
    tones, sources = collect(client, slots, trending=trending, latest=latest, search_query=search_query)
    decisions, thresholds = evaluate(tones, sources, cfg, now, gears=slots)

    for d in decisions:
        if d.status != "included":
            continue
        found = list_candidates(client, d.tone)
        if found is None:
            d.status = "excluded"
            d.reasons.append("no_models_returned")
            continue
        arch, models = found
        d.models = [{"id": m.id, "name": m.name, "architecture_version": m.architecture_version,
                     "size": m.size, "architecture_queried": arch} for m in models]
        if download:  # cap per tone so 168-IR packs are not bulk-downloaded
            for m in models[:max_models_per_tone]:
                e = ensure_capture(client, cache, d.tone, m)
                d.downloads.append({"model_id": m.id, "path": str(e.path), "sha256": e.sha256})

    counts: dict[str, int] = {}
    for d in decisions:
        counts[d.status] = counts.get(d.status, 0) + 1
    return {
        "schema": "sawblade.t3k_pool",
        "version": 1,
        "generated_at": now.isoformat(timespec="seconds"),
        "config": config_dict(cfg),
        "max_models_per_tone": max_models_per_tone,
        "slots": slots,
        "sources": {"favorited": True, "trending": trending, "latest": latest,
                    "search": search_query if search_query is not None else False},
        "popularity_thresholds": thresholds,
        "counts": counts,
        "tones": [decision_to_json(d) for d in decisions if d.status == "included"],
        "references": [decision_to_json(d) for d in decisions if d.status == "reference"],
        "excluded": [decision_to_json(d) for d in decisions if d.status == "excluded"],
    }


def write_manifest(manifest: dict[str, Any], path: Path) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".pool.")
    with os.fdopen(fd, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
