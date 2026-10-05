"""Model cache directory resolution and the on-disk file names (contract with the C++ ModelStore)."""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Mapping

ENV_DIR = "SAWBLADE_MODELS_DIR"
MODEL_IDS = ("htdemucs_6s", "htdemucs")
DEFAULT_MODEL = "htdemucs_6s"
OPSET = 17


def models_dir(env: Mapping[str, str] | None = None, platform: str | None = None, home: Path | None = None) -> Path:
    """SAWBLADE_MODELS_DIR, else the platform default (macOS Application Support, Linux XDG data dir)."""
    env = os.environ if env is None else env
    platform = sys.platform if platform is None else platform
    home = Path.home() if home is None else home
    explicit = env.get(ENV_DIR)
    if explicit:
        return Path(explicit).expanduser()
    if platform == "darwin":
        return home / "Library" / "Application Support" / "Sawblade" / "models"
    xdg = env.get("XDG_DATA_HOME")
    base = Path(xdg) if xdg and Path(xdg).is_absolute() else home / ".local" / "share"
    return base / "sawblade" / "models"


def onnx_path(model_id: str, directory: Path) -> Path:
    return directory / f"{model_id}-core-opset{OPSET}.onnx"


def sidecar_path(model_id: str, directory: Path) -> Path:
    return directory / f"{model_id}-core-opset{OPSET}.onnx.sha256"


def torch_home(directory: Path) -> Path:
    """TORCH_HOME for the checkpoint cache: <dir>/checkpoints (files land in hub/checkpoints/)."""
    return directory / "checkpoints"


def checkpoint_path(file_name: str, directory: Path) -> Path:
    return torch_home(directory) / "hub" / "checkpoints" / file_name
