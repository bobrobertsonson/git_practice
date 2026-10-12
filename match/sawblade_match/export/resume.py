"""Resumable training bookkeeping: atomic file writes, ``progress.json``, identity checks, ``--resume auto`` search.

Layout of ``<out>/checkpoint/``: ``last.ckpt`` (Lightning checkpoint: model, optimiser, scheduler, epoch, RNG states),
``best.ckpt`` (best-so-far model), ``progress.json`` (written last; it is the commit marker).
"""
from __future__ import annotations

import json
import os
from pathlib import Path

from .plan import ExportRefused

CKPT_DIRNAME = "checkpoint"
LAST = "last.ckpt"
BEST = "best.ckpt"
PROGRESS = "progress.json"
# What must match for a run to be resumed (spec: preset sha, signal sha, size, mode) plus the training settings that
# change the learning trajectory (resuming with another seed / lr schedule would silently not be the same run).
IDENTITY_KEYS = ("presetSha256", "signalSha256", "validSha256", "mode", "size")
CONFIG_KEYS = ("seed", "batchSize", "epochs", "lrGamma")


def atomic_write_text(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def atomic_copy(src: Path, dst: Path) -> None:
    import shutil
    tmp = dst.with_name(dst.name + ".tmp")
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


def ckpt_dir(outdir) -> Path:
    return Path(outdir) / CKPT_DIRNAME


def write_progress(cdir, data: dict) -> None:
    atomic_write_text(Path(cdir) / PROGRESS, json.dumps(data, indent=2, default=float))


def read_progress(cdir) -> dict | None:
    f = Path(cdir) / PROGRESS
    try:
        return json.loads(f.read_text())
    except (OSError, ValueError):
        return None


def mismatches(progress: dict, identity: dict, config: dict | None = None) -> list[str]:
    """Human-readable differences between a stored run and the requested one (empty = compatible)."""
    out = []
    names = {"presetSha256": "preset sha256", "signalSha256": "training-signal sha256", "validSha256": "validation-signal sha256",
             "mode": "mode", "size": "size"}
    for k in IDENTITY_KEYS:
        if k in identity and progress.get(k) != identity[k]:
            out.append(f"{names[k]} differs (checkpoint {progress.get(k)!r}, requested {identity[k]!r})")
    for k, v in (config or {}).items():
        stored = (progress.get("config") or {}).get(k)
        if stored is None:
            continue
        if isinstance(v, float) or isinstance(stored, float):
            same = abs(float(stored) - float(v)) < 1e-12
        else:
            same = stored == v
        if not same:
            out.append(f"{k} differs (checkpoint {stored!r}, requested {v!r})")
    return out


def validate_resume_dir(outdir, identity: dict, config: dict | None = None) -> dict:
    """Return the progress of an existing run directory or raise ``ExportRefused`` with a clear message."""
    cdir = ckpt_dir(outdir)
    prog = read_progress(cdir)
    if prog is None or not (cdir / LAST).is_file():
        raise ExportRefused(f"cannot resume: no checkpoint in {cdir} (expected {PROGRESS} and {LAST})")
    bad = mismatches(prog, identity, config)
    if bad:
        raise ExportRefused(f"cannot resume {outdir}: " + "; ".join(bad))
    return prog


def find_auto(root, identity: dict, config: dict | None = None) -> tuple[Path | None, list[str]]:
    """Newest unfinished run under ``root`` matching preset, mode, size and signal (and training settings).
    Returns (dir or None, notes about candidates that were skipped)."""
    root = Path(root)
    cands = []
    notes = []
    if root.is_dir():
        for d in root.iterdir():
            cdir = ckpt_dir(d)
            prog = read_progress(cdir) if d.is_dir() else None
            if prog is None or prog.get("complete") or not (cdir / LAST).is_file():
                continue
            bad = mismatches(prog, identity, config)
            if bad:
                notes.append(f"skipped {d.name}: " + "; ".join(bad))
                continue
            cands.append(((cdir / PROGRESS).stat().st_mtime, d))
    cands.sort(key=lambda t: t[0])
    return (cands[-1][1] if cands else None), notes
