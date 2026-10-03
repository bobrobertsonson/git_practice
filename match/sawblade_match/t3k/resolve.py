"""`resolve`: fill capture file/sha256/source from the API (or the cache, offline when possible)."""
from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any

from .cache import Cache
from .client import T3KClient
from .errors import T3KError
from .ids import require_id
from .licenses import check_license
from .fetch import ensure_capture, list_candidates


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


def resolve_capture(client: T3KClient, cache: Cache, cap: dict, first_model: bool = False) -> None:
    src = cap["source"]
    tone_id = require_id(src["id"], "source.id")
    model_id = require_id(src["modelId"], "source.modelId") if src.get("modelId") is not None else None

    entry = cache.get(tone_id, model_id) if model_id else None
    if entry is not None:
        check_license(entry.tone.get("license"), f"cached tone {tone_id}")
    else:
        tone = client.get_tone(tone_id)
        check_license(tone.license, f"tone {tone_id}")
        if model_id:
            model = client.get_model(model_id)
            if model.tone_id and model.tone_id != tone.id:
                raise T3KError(f"model {model_id} belongs to tone {model.tone_id}, not {tone.id}")
        else:
            found = list_candidates(client, tone)
            if found is None:
                raise T3KError(f"tone {tone_id} has no usable models")
            models = found[1]
            if len(models) > 1 and not first_model:
                listing = "; ".join(f"{m.id} ({m.name})" for m in models[:20])
                more = f" (+{len(models) - 20} more)" if len(models) > 20 else ""
                raise T3KError(
                    f"tone {tone_id} ({tone.title!r}) has {len(models)} models and the preset names none. "
                    f"Set source.modelId to one of: {listing}{more}. (Or pass --first-model.)")
            model = models[0]
        entry = ensure_capture(client, cache, tone, model)
    t = entry.tone
    cap["file"] = str(entry.path)
    cap["sha256"] = entry.sha256
    src["modelId"] = str(entry.model_id)
    src["url"] = t.get("url", src.get("url"))
    src["title"] = t.get("title", src.get("title"))
    user = t.get("user") or {}
    src["creator"] = user.get("display_name") or user.get("username") or src.get("creator")
    src["license"] = t.get("license", src.get("license"))


def resolve_preset(client: T3KClient, cache: Cache, preset: dict, first_model: bool = False) -> list[str]:
    """Rewrite ``preset`` in place; returns the JSON paths of the captures that were resolved."""
    caps = _captures(preset)
    for _, cap in caps:
        resolve_capture(client, cache, cap, first_model)
    return [p for p, _ in caps]


def default_output(preset_path: Path) -> Path:
    """`<name>.resolved.json` next to the input (resolved presets hold machine-specific paths)."""
    p = Path(preset_path)
    return p.with_name(p.stem + ".resolved.json")


def resolve_file(client: T3KClient, cache: Cache, preset_path: Path, out_path: Path | None = None,
                 first_model: bool = False) -> list[str]:
    preset_path = Path(preset_path)
    dest = Path(out_path or default_output(preset_path))
    if dest.resolve() == preset_path.resolve():
        raise T3KError("output path equals the input preset; refusing to overwrite it")
    preset = json.loads(preset_path.read_text())
    done = resolve_preset(client, cache, preset, first_model)
    fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=f".{dest.name}.")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(json.dumps(preset, indent=2) + "\n")
        os.replace(tmp, dest)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise
    return done
