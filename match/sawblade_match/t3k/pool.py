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
IR_GEAR = SLOT_GEAR["cab"]         # the tones/search `gears` value of IR tones; used by --search (cab slot) and --ir-search


def collect(
    client: T3KClient,
    slots: Iterable[str] = ALL_SLOTS,
    *,
    trending: bool = True,
    latest: bool = True,
    searches: Iterable[str] = (),
    add_tones: Iterable[int] = (),
    ir_searches: Iterable[str] = (),
) -> tuple[dict[int, Tone], dict[int, list[str]]]:
    """Favorited (always) + free-tier trending/latest; ``searches`` (each a tones/search query) and
    ``add_tones`` (specific ids, source ``lead-pick``) are opt-in. Duplicates merge, sources accumulate."""
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
    gears = "_".join(SLOT_GEAR[s] for s in slots)
    for q in searches:
        # Opt-in; check the TONE3000 API terms before sharing anything that uses search.
        add("search", client.search(q, gears=gears))
    for q in ir_searches:      # IR-only searches (cab families); same opt-in terms as --search
        add("ir-search", client.search(q, gears=IR_GEAR))
    for tid in add_tones:
        add("lead-pick", [client.get_tone(tid)])
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
    searches: Iterable[str] = (),
    add_tones: Iterable[int] = (),
    force_tones: Iterable[int] = (),
    download: bool = True,
    max_models_per_tone: int = 3,
    max_ir_models_per_tone: int | None = None,
    ir_searches: Iterable[str] = (),
    now: datetime | None = None,
) -> dict[str, Any]:
    """``max_models_per_tone`` caps downloads of pedal/amp tones; ``max_ir_models_per_tone`` (None = all) those of IR tones
    (cab slot: an IR pack's models are the individual IRs, the screen of the matcher ranks them cheaply).
    ``ir_searches`` add IR tones from tones/search (gear ir), under the same filter and licence rules."""
    now = now or datetime.now(timezone.utc)
    slots = list(slots)
    ir_searches = list(dict.fromkeys(ir_searches))
    eval_slots = slots + ["cab"] if ir_searches and "cab" not in slots else slots    # slots decides what is *collected*
    searches = list(dict.fromkeys(searches))
    force_tones = list(dict.fromkeys(int(t) for t in force_tones))
    # a forced tone is also a lead pick, so it is fetched even without --add-tone
    add_tones = list(dict.fromkeys([*(int(t) for t in add_tones), *force_tones]))
    tones, sources = collect(client, slots, trending=trending, latest=latest, searches=searches,
                             add_tones=add_tones, ir_searches=ir_searches)
    decisions, thresholds = evaluate(tones, sources, cfg, now, gears=eval_slots, forced=force_tones)

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
        if download:  # pedals/amps: cap per tone; IR tones: all models (each is one IR of the pack) unless capped
            cap_n = max_ir_models_per_tone if d.slot == "cab" else max_models_per_tone
            for m in models[:cap_n]:
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
        "max_ir_models_per_tone": max_ir_models_per_tone,
        "slots": slots,
        "sources": {"favorited": True, "trending": trending, "latest": latest,
                    "search": searches, "ir_search": ir_searches, "lead_picks": add_tones, "forced": force_tones},
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
