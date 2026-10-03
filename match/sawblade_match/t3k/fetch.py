"""Model choice and cache-backed capture fetching."""
from __future__ import annotations

import logging
from pathlib import Path

from .cache import Cache, CacheEntry, ext_for
from .client import ARCH_A1, ARCH_A2, ARCH_CUSTOM, T3KClient
from .types import Model, Tone

log = logging.getLogger("sawblade.t3k")


def architecture_order(tone: Tone) -> list[str]:
    """Architectures to query, most preferred first. NAM: A2 then A1 (only those the counts allow).

    IR tones have no NAM architecture; the docs say the parameter is ignored for non-NAM
    formats, so we still pass one explicitly ('1', then 'custom' as a fallback).
    """
    if tone.format == "nam":
        order = []
        if tone.a2_models_count > 0:
            order.append(ARCH_A2)
        if tone.a1_models_count > 0:
            order.append(ARCH_A1)
        return order
    return [ARCH_A1, ARCH_CUSTOM]


def pick_model(models: list[Model], prefer_size: str = "standard") -> Model | None:
    """Preferred size if present, else the first model (API order = owner-set position)."""
    for m in models:
        if m.size == prefer_size:
            return m
    return models[0] if models else None


def choose_model(client: T3KClient, tone: Tone, prefer_size: str = "standard") -> tuple[Model, str] | None:
    for arch in architecture_order(tone):
        m = pick_model(client.list_models(tone.id, arch), prefer_size)
        if m is not None:
            return m, arch
    return None


def ensure_capture(client: T3KClient, cache: Cache, tone: Tone, model: Model, *,
                   chosen: bool = True) -> CacheEntry:
    """Return the cached capture, downloading it first on a miss."""
    hit = cache.get(tone.id, model.id)
    if hit:
        return hit
    ext = ext_for(tone)
    path = cache.path_for(tone.id, model.id, ext)
    digest = client.download_model(model.model_url, path)
    cache.put_meta(tone, model, path, digest, chosen=chosen)
    entry = cache.get(tone.id, model.id)
    assert entry is not None
    log.info("cached %s (tone %s model %s)", Path(entry.path).name, tone.id, model.id)
    return entry
