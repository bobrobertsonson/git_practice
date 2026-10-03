"""Model choice and cache-backed capture fetching."""
from __future__ import annotations

import logging
from pathlib import Path

from .cache import Cache, CacheEntry, ext_for
from .licenses import check_license
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


def list_candidates(client: T3KClient, tone: Tone) -> tuple[str, list[Model]] | None:
    """ALL models of the tone for the most-preferred architecture that has any (A2, then A1).

    Models within a tone are usually different settings (gain, channel, boost), so choosing
    among them is a tone decision left to the matcher/user, not made here.
    """
    for arch in architecture_order(tone):
        ms = client.list_models(tone.id, arch)
        if ms:
            return arch, ms
    return None


def ensure_capture(client: T3KClient, cache: Cache, tone: Tone, model: Model) -> CacheEntry:
    """Return the cached capture, downloading it first on a miss."""
    check_license(tone.license, f"tone {tone.id}")
    hit = cache.get(tone.id, model.id)
    if hit:
        check_license(hit.tone.get("license"), f"cached tone {tone.id}")
        return hit
    ext = ext_for(tone)
    path = cache.path_for(tone.id, model.id, ext)
    digest = client.download_model(model.model_url, path)
    cache.put_meta(tone, model, path, digest)
    entry = cache.get(tone.id, model.id)
    assert entry is not None
    log.info("cached %s (tone %s model %s)", Path(entry.path).name, tone.id, model.id)
    return entry
