"""Persistent extra pool sources (user state, never committed).

``~/.config/sawblade/pool_sources.json`` (override: ``SAWBLADE_POOL_SOURCES``)::

    {"searches": ["big muff", "hm-2"], "tones": [12345, 67890]}

It is merged into every ``pull`` so the pool is reproducible without repeating flags.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path

from .errors import T3KError
from .ids import require_id


def default_path() -> Path:
    env = os.environ.get("SAWBLADE_POOL_SOURCES")
    return Path(env) if env else Path.home() / ".config" / "sawblade" / "pool_sources.json"


@dataclass
class PoolSources:
    searches: list[str] = field(default_factory=list)
    tones: list[int] = field(default_factory=list)


def load_pool_sources(path: Path | None = None) -> PoolSources:
    """Missing file = empty. A malformed file is an error (silently ignoring it would change the pool)."""
    path = Path(path) if path else default_path()
    if not path.exists():
        return PoolSources()
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        raise T3KError(f"cannot read {path}: {e}") from None
    if not isinstance(data, dict):
        raise T3KError(f"{path}: expected a JSON object {{\"searches\": [...], \"tones\": [...]}}")
    searches, tones = data.get("searches", []), data.get("tones", [])
    if not isinstance(searches, list) or not all(isinstance(s, str) for s in searches):
        raise T3KError(f"{path}: \"searches\" must be a list of strings")
    if not isinstance(tones, list):
        raise T3KError(f"{path}: \"tones\" must be a list of tone ids")
    return PoolSources([s for s in searches if s.strip()], [int(require_id(t, "tone id")) for t in tones])


def merge_unique(*lists):
    """Concatenate, dropping duplicates, keeping first-seen order."""
    out, seen = [], set()
    for lst in lists:
        for x in lst:
            if x not in seen:
                seen.add(x)
                out.append(x)
    return out
