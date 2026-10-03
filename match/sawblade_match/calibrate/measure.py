"""Measure one (reference, method) signal: LTAS, group levels, rule values, buzz, low-end decay.

Reuses the tonecheck analysis functions unchanged; the only addition is a caller-supplied frame mask
(section selection) instead of the tonecheck activity gate.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from ..tonecheck import analysis as A
from ..tonecheck.rules import evaluate_rules


@dataclass
class Measured:
    label: str
    abs_db: np.ndarray
    rel_db: np.ndarray
    centres: list[float]
    groups: dict[str, float]
    rules: list[dict]
    metrics: dict
    n_segments: int
    selected_fraction: float
    selected_seconds: float
    warnings: list[str] = field(default_factory=list)
    chunk_spread_db: dict[str, float] | None = None
    source: str = ""


def _chunk_spread(x: np.ndarray, mask: np.ndarray, targets: dict, chunk_s: float = 10.0) -> dict[str, float]:
    """Std (dB) across ``chunk_s`` chunks of each group level (chunks with < 3 s selected are skipped):
    a reliability indicator - if the selected material is one tone, the chunks agree."""
    fs = A.ANALYSIS_RATE
    n = int(chunk_s * fs)
    levels: dict[str, list[float]] = {}
    for a in range(0, len(x) - n + 1, n):
        m = mask[a: a + n]
        if m.sum() < 3 * fs:
            continue
        try:
            fr, psd, _ = A.ltas_psd(x[a: a + n], fs, m, [])
            _, rel = A.band_levels_db(fr, psd)
        except ValueError:
            continue
        for k, v in A.group_levels(rel, targets["analysis"]["bandGroups"]).items():
            levels.setdefault(k, []).append(v)
    return {k: float(np.std(v)) for k, v in levels.items() if len(v) >= 3}


def measure(label: str, x48: np.ndarray, targets: dict, mask: np.ndarray | None = None,
            onsets: np.ndarray | None = None, source: str = "", spread: bool = False,
            no_onsets: bool = False) -> Measured:
    """``x48``: mono at 48 kHz. ``mask``: per-sample bool of frames to keep (None: tonecheck activity gate).
    ``onsets``: note onsets (s) for lowTightnessMs / lowDecayDbPerMs; default: detected on ``x48`` itself,
    restricted to the kept frames (``no_onsets``: report n/a). They are measured on the same signal's 80-160 Hz envelope (stems: the
    guitar stem; sections: the mix, which still contains bass/kick under the kept frames - a caveat)."""
    warnings: list[str] = []
    if mask is None:
        mask, frames, _ = A.activity_mask(x48, A.ANALYSIS_RATE)
        frac = float(frames.mean())
    else:
        frac = float(mask.mean())
    fr, psd, nseg = A.ltas_psd(x48, A.ANALYSIS_RATE, mask, warnings)
    absdb, rel = A.band_levels_db(fr, psd)
    groups = A.group_levels(rel, targets["analysis"]["bandGroups"])
    if no_onsets:      # no reliable onset source (e.g. the side signal of a reference without DIs)
        note = "n/a: no onset source"
        none = {"nOnsets": 0, "nMeasured": 0, "nCensored": 0, "note": note}
        metrics = {"buzz": {"value": A.buzz_flatness(fr, psd)}, "lowTightnessMs": {"valueMs": None, **none},
                   "lowDecayDbPerMs": {"value": None, **none}, "crestFactorDb": {"value": A.crest_factor_db(x48, mask)}}
        return Measured(label, absdb, rel, list(A.NOMINAL_CENTRES), groups, evaluate_rules(groups, targets["rules"]),
                        metrics, nseg, frac, float(mask.sum() / A.ANALYSIS_RATE), warnings,
                        _chunk_spread(x48, mask, targets) if spread else None, source)
    if onsets is None:
        onsets = A.detect_onsets(x48, A.ANALYSIS_RATE)
    keep = np.array([bool(mask[min(len(mask) - 1, int(t * A.ANALYSIS_RATE))]) for t in onsets], dtype=bool)
    onsets = np.asarray(onsets)[keep] if len(onsets) else np.asarray(onsets)
    tight, decay = A.low_end_decay(x48, x48, A.ANALYSIS_RATE, onsets=onsets)
    metrics = {"buzz": {"value": A.buzz_flatness(fr, psd)},
               "lowTightnessMs": tight, "lowDecayDbPerMs": decay,
               "crestFactorDb": {"value": A.crest_factor_db(x48, mask)}}
    return Measured(label, absdb, rel, list(A.NOMINAL_CENTRES), groups, evaluate_rules(groups, targets["rules"]),
                    metrics, nseg, frac, float(mask.sum() / A.ANALYSIS_RATE), warnings,
                    _chunk_spread(x48, mask, targets) if spread else None, source)
