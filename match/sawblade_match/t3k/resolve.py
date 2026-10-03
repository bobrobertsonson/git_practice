"""`resolve`: fill capture file/sha256/source from the API (or the cache, offline when possible)."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .cache import Cache
from .client import T3KClient
from .errors import T3KError
from .fetch import choose_model, ensure_capture


def _captures(node: Any, path: str = "") -> list[tuple[str, dict]]:
    """All capture objects (dicts with a tone3000 `source`) in a preset, with a JSON-ish path."""
    out = []
    if isinstance(node, dict):
        src = node.get("source")
        if isinstance(src, dict) and src.get("provider") == "tone3000" and src.get("id") is not None:
            out.append((path or "$", node))
        else:
            for k, v in node.items():
                out.extend(_captures(v, f"{path}.{k}" if path else k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            out.extend(_captures(v, f"{path}[{i}]"))
    return out


def resolve_capture(client: T3KClient, cache: Cache, cap: dict, prefer_size: str = "standard") -> None:
    src = cap["source"]
    tone_id = str(src["id"])
    model_id = str(src["modelId"]) if src.get("modelId") is not None else cache.default_model_id(tone_id)

    entry = cache.get(tone_id, model_id) if model_id else None
    if entry is None:
        tone = client.get_tone(tone_id)
        if model_id:
            model = client.get_model(model_id)
            if model.tone_id and model.tone_id != tone.id:
                raise T3KError(f"model {model_id} belongs to tone {model.tone_id}, not {tone.id}")
        else:
            picked = choose_model(client, tone, prefer_size)
            if picked is None:
                raise T3KError(f"tone {tone_id} has no usable models")
            model = picked[0]
        entry = ensure_capture(client, cache, tone, model, chosen=True)
    t = entry.tone
    cap["file"] = str(entry.path)
    cap["sha256"] = entry.sha256
    src["modelId"] = str(entry.model_id)
    src["url"] = t.get("url", src.get("url"))
    src["title"] = t.get("title", src.get("title"))
    user = t.get("user") or {}
    src["creator"] = user.get("display_name") or user.get("username") or src.get("creator")
    src["license"] = t.get("license", src.get("license"))


def resolve_preset(client: T3KClient, cache: Cache, preset: dict, prefer_size: str = "standard") -> list[str]:
    """Rewrite ``preset`` in place; returns the JSON paths of the captures that were resolved."""
    caps = _captures(preset)
    for _, cap in caps:
        resolve_capture(client, cache, cap, prefer_size)
    return [p for p, _ in caps]


def resolve_file(client: T3KClient, cache: Cache, preset_path: Path, out_path: Path | None = None,
                 prefer_size: str = "standard") -> list[str]:
    preset_path = Path(preset_path)
    preset = json.loads(preset_path.read_text())
    done = resolve_preset(client, cache, preset, prefer_size)
    Path(out_path or preset_path).write_text(json.dumps(preset, indent=2) + "\n")
    return done
