"""Pinned hashes, sha256 helpers, the `.sha256` sidecar, and model status."""
from __future__ import annotations

import hashlib
import os
import re
from dataclasses import dataclass
from pathlib import Path

from .paths import MODEL_IDS, checkpoint_path, onnx_path, sidecar_path

# Official checkpoints (dl.fbaipublicfiles.com), sha256 as verified by spikes/separator/scripts/fetch_weights.sh.
CHECKPOINT_BASE_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer"
CHECKPOINTS = {
    "htdemucs": ("955717e8-8726e21a.th", "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"),
    "htdemucs_6s": ("5c90dfd2-34c22ccb.th", "34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd"),
}
# Reproducible export (spikes/separator/RESULTS.md, Phase 5.1a): torch 2.5.1 legacy exporter, onnx 1.23.1, opset 17.
# Other platforms may differ bit-wise; a mismatch is a warning as long as the verification passes.
PINNED_ONNX_SHA256 = {
    "htdemucs": "79189af3c584b1a2145ae5e4182a50c0204f88b76e2829bd27e4d4a88ede427d",
    "htdemucs_6s": "d23996ba2e9396d393e2bd53c29f1411bd33b8cf3451854ad32d746ad3d06132",
}

_HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(chunk):
            h.update(block)
    return h.hexdigest()


def write_sidecar(model_id: str, directory: Path, digest: str) -> Path:
    """Write `<digest>\\n` (64 lowercase hex) atomically."""
    if not _HEX64.match(digest):
        raise ValueError("digest must be 64 lowercase hex characters")
    p = sidecar_path(model_id, directory)
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(digest + "\n")
    os.replace(tmp, p)
    return p


def read_sidecar(model_id: str, directory: Path) -> str | None:
    p = sidecar_path(model_id, directory)
    try:
        s = p.read_text().strip()
    except OSError:
        return None
    return s if _HEX64.match(s) else None


@dataclass(frozen=True)
class ModelStatus:
    model_id: str
    path: Path
    present: bool            # ONNX file exists
    sidecar: str | None      # digest recorded in the sidecar (None if missing/malformed)
    actual: str | None       # sha256 of the file on disk
    sha_ok: bool             # sidecar present and equal to the actual hash
    pinned_match: bool       # actual hash equals the pinned export hash


def model_status(model_id: str, directory: Path) -> ModelStatus:
    p = onnx_path(model_id, directory)
    present = p.is_file()
    actual = sha256_file(p) if present else None
    sidecar = read_sidecar(model_id, directory)
    return ModelStatus(model_id, p, present, sidecar, actual,
                       sha_ok=present and sidecar is not None and sidecar == actual,
                       pinned_match=actual is not None and actual == PINNED_ONNX_SHA256[model_id])


def format_status(directory: Path) -> str:
    lines = [f"models dir: {directory}"]
    for mid in MODEL_IDS:
        st = model_status(mid, directory)
        ck_name, ck_sha = CHECKPOINTS[mid]
        ck = checkpoint_path(ck_name, directory)
        ck_state = "checkpoint cached" if ck.is_file() else "checkpoint not cached"
        if not st.present:
            lines.append(f"{mid}: missing  ({ck_state})  fetch: match/.venv/bin/sawblade-models fetch --model {mid}")
            continue
        lines.append(f"{mid}: present  sha {'ok' if st.sha_ok else 'MISMATCH/NO SIDECAR'}  "
                     f"pinned {'match' if st.pinned_match else 'differs'}  ({ck_state})  sha256 {st.actual}")
    return "\n".join(lines)
