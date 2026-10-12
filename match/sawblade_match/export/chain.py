"""Thin wrappers around ``sawblade_core`` for the export: rendering at 48 kHz and the cab + post EQ fold.

All DSP is the C++ core's.  The only numpy DSP here is the impulse bookkeeping (trimming the folded IR); the
convolution/EQ used to *check* the fold lives in the tests (as an oracle).
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from ..core import CaptureCache, render
from .plan import folding_preset

RATE = 48000
FOLD_TRIM_DB = -120.0           # trailing samples below this (re. the peak) are cut from the folded IR
FOLD_HEADROOM_S = 1.0           # impulse length = longest IR (2 s, core limit) + this, for the post-EQ tails
CORE_IR_MAX_S = 2.0


def load_preset(path) -> tuple[dict, Path]:
    p = Path(path)
    return json.loads(p.read_text()), p.resolve().parent


def render48(preset: dict, x: np.ndarray, base_dir, cache: CaptureCache | None = None,
             in_rate: int = RATE) -> tuple[np.ndarray, dict]:
    """Render ``x`` (float32 mono at ``in_rate``) through ``preset`` at 48 kHz.  The output is at 48 kHz, latency
    already trimmed by the core (output i corresponds to input i)."""
    y, rep = render(preset, np.ascontiguousarray(x, dtype=np.float32), int(in_rate), render_rate=RATE,
                    out_rate="render", base_dir=str(base_dir), cache=cache)
    return np.asarray(y, dtype=np.float32), rep


def probe_report(preset: dict, base_dir, cache: CaptureCache | None = None) -> dict:
    """Render a short silence just to get the core's report (warnings, latency, resolved align)."""
    _, rep = render48(preset, np.zeros(4800, np.float32), base_dir, cache)
    return rep


def fold_cab_post_eq(preset: dict, base_dir, cache: CaptureCache | None = None) -> tuple[np.ndarray, dict]:
    """Impulse response of ``cab (shared) -> post EQ`` as the core applies them (IR loading, resampling to 48 kHz,
    truncation to 2 s and L2 normalisation included), i.e. IR (*) post-EQ impulse response.  Rendered by the core
    through an empty chain, so the result is exactly what the chain does, and aligned (latency trimmed)."""
    n = int((CORE_IR_MAX_S + FOLD_HEADROOM_S) * RATE)
    imp = np.zeros(n, np.float32)
    imp[0] = 1.0
    h, rep = render48(folding_preset(preset), imp, base_dir, cache)
    h = h.astype(np.float64)
    pk = float(np.max(np.abs(h))) if len(h) else 0.0
    if pk <= 0.0:
        raise ValueError("folded IR is silent")
    keep = np.nonzero(np.abs(h) >= pk * 10 ** (FOLD_TRIM_DB / 20))[0]
    h = h[: int(keep[-1]) + 1]
    info = {"samples": int(len(h)), "seconds": len(h) / RATE, "peak": pk, "peakDb": float(20 * np.log10(pk)),
            "l2Norm": float(np.sqrt(np.sum(h * h))), "trimDb": FOLD_TRIM_DB,
            "postEqBands": len(preset.get("postEq", [])), "cabEnabled": bool(preset["cab"].get("enabled", True))}
    return h.astype(np.float32), info
