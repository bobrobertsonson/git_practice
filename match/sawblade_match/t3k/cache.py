"""On-disk capture cache: <root>/<tone_id>/<model_id>.<nam|wav> + <root>/<tone_id>/meta.json.

A cache hit (file present, sha256 matches meta) never touches the network.
"""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .types import Model, Tone

FORMAT_EXT = {"nam": "nam", "ir": "wav"}


def default_cache_root() -> Path:
    env = os.environ.get("SAWBLADE_CACHE_DIR")
    return Path(env) if env else Path.home() / ".cache" / "sawblade" / "captures"


def ext_for(tone: Tone) -> str:
    try:
        return FORMAT_EXT[tone.format]
    except KeyError:
        raise ValueError(f"unsupported tone format {tone.format!r} (Sawblade loads nam and ir)") from None


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass(frozen=True)
class CacheEntry:
    tone_id: int
    model_id: int
    path: Path
    sha256: str
    tone: dict[str, Any]
    model: dict[str, Any]
    fetched_at: str


class Cache:
    def __init__(self, root: Path | None = None):
        self.root = Path(root) if root else default_cache_root()

    def tone_dir(self, tone_id: int | str) -> Path:
        return self.root / str(tone_id)

    def path_for(self, tone_id: int | str, model_id: int | str, ext: str) -> Path:
        return self.tone_dir(tone_id) / f"{model_id}.{ext}"

    def _meta_path(self, tone_id: int | str) -> Path:
        return self.tone_dir(tone_id) / "meta.json"

    def read_meta(self, tone_id: int | str) -> dict[str, Any]:
        try:
            return json.loads(self._meta_path(tone_id).read_text())
        except (FileNotFoundError, ValueError):
            return {}

    def _write_meta(self, tone_id: int | str, meta: dict[str, Any]) -> None:
        d = self.tone_dir(tone_id)
        d.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=d, prefix=".meta.")
        with os.fdopen(fd, "w") as f:
            json.dump(meta, f, indent=2, sort_keys=True)
        os.replace(tmp, self._meta_path(tone_id))

    def get(self, tone_id: int | str, model_id: int | str) -> CacheEntry | None:
        meta = self.read_meta(tone_id)
        m = (meta.get("models") or {}).get(str(model_id))
        if not m:
            return None
        path = self.tone_dir(tone_id) / m["file"]
        try:
            if sha256_file(path) != m["sha256"]:
                return None  # corrupted/modified: treat as a miss and re-download
        except FileNotFoundError:
            return None
        return CacheEntry(int(tone_id), int(model_id), path, m["sha256"], meta.get("tone", {}),
                          m.get("model", {}), m.get("fetched_at", ""))

    def default_model_id(self, tone_id: int | str) -> str | None:
        """Model last chosen for this tone (lets `resolve` run offline without a modelId)."""
        return self.read_meta(tone_id).get("chosen_model_id")

    def put_meta(self, tone: Tone, model: Model, path: Path, sha256: str, *, chosen: bool = True) -> None:
        meta = self.read_meta(tone.id)
        meta["tone"] = tone.raw
        meta.setdefault("models", {})[str(model.id)] = {
            "model": model.raw,
            "sha256": sha256,
            "file": path.name,
            "fetched_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        }
        if chosen:
            meta["chosen_model_id"] = str(model.id)
        self._write_meta(tone.id, meta)
