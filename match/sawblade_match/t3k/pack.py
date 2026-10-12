"""`pack`: download every model of an IR tone and describe the set in a manifest (for the plugin's mic page)."""
from __future__ import annotations

from typing import Callable

from .cache import Cache
from .client import T3KClient
from .errors import T3KError
from .fetch import ensure_capture, list_candidates
from .ids import require_id
from .licenses import check_license


def build_pack(client: T3KClient, cache: Cache, tone_id: int | str,
               progress: Callable[[int, int, str], None] | None = None) -> dict:
    """Manifest ``{toneId, title, creator, license, url, models:[{modelId, name, file, sha256}]}``.

    ``file`` is the absolute cache path. ``progress(done, total, name)`` runs after each model is cached.
    """
    tid = require_id(tone_id, "tone id")
    tone = client.get_tone(tid)
    if tone.format != "ir":
        raise T3KError(f"tone {tid} ({tone.title!r}) is a {tone.format or 'unknown'!r} tone, not an IR pack")
    check_license(tone.license, f"tone {tid}")
    found = list_candidates(client, tone)
    if found is None:
        raise T3KError(f"tone {tid} ({tone.title!r}) has no usable models")
    models = found[1]
    out = []
    for i, m in enumerate(models, 1):
        e = ensure_capture(client, cache, tone, m)
        out.append({"modelId": str(m.id), "name": m.name, "file": str(e.path.resolve()), "sha256": e.sha256})
        if progress:
            progress(i, len(models), m.name)
    return {"toneId": str(tone.id), "title": tone.title, "creator": tone.user.creator,
            "license": tone.license, "url": tone.url, "models": out}
