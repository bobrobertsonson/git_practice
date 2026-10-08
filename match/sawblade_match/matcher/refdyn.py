"""Reporting only (no matcher behaviour): the dynamics of the reference next to the result's, and the input gain each NAM block got.

``window_dynamics`` measures a 48 kHz mono signal with ONE definition, used for the reference window and for the renders alike
(the tonecheck definitions of best_L's crest / LRA): ``crestFactorDb`` (peak / RMS over the active samples), ``medianCrest400Db``
(the median peak / RMS of the 400 ms windows, hop 200 ms, that are mostly active; the feel term's window), ``lraLu`` and the gated
p10 / p95 of the 3 s short-term loudness with their spread (= the EBU Tech 3342 LRA). ``block_gains`` reads ``inputGainDb`` etc.
from the emitted preset. Lets the lead tell whether a render's compression comes from the recorded amp or from over-driven captures.
"""
from __future__ import annotations

import numpy as np

from ..tonecheck.analysis import activity_mask, crest_factor_db, loudness_range_detail
from . import feel as _feel


def window_dynamics(x: np.ndarray, fs: int = 48000) -> dict:
    """Dynamics of ``x`` (mono, 48 kHz): see the module doc. Fields that cannot be measured (signal shorter than 3 s / 400 ms) are None."""
    x = np.asarray(x, dtype=np.float64)
    out = {"seconds": float(len(x) / fs), "crestFactorDb": None, "medianCrest400Db": None, "lraLu": None,
           "shortTermP10Lufs": None, "shortTermP95Lufs": None, "shortTermSpreadLu": None}
    if len(x) == 0 or not np.any(x):
        return out
    mask, _, _ = activity_mask(x, fs)
    mask = np.asarray(mask, bool)
    out["crestFactorDb"] = float(crest_factor_db(x, mask))
    n = min(len(mask), len(x))
    cs = np.concatenate([[0], np.cumsum(mask[:n].astype(np.int64))])
    win, hop = _feel.CREST_WIN, _feel.CREST_HOP
    vals = []
    for s in range(0, n - win + 1, hop):
        if cs[s + win] - cs[s] >= _feel.ACTIVE_FRACTION * win:
            w = x[s:s + win]
            vals.append(20.0 * np.log10(np.max(np.abs(w)) + 1e-12) - 10.0 * np.log10(np.mean(w * w) + 1e-24))
    if vals:
        out["medianCrest400Db"] = float(np.median(vals))
    d = loudness_range_detail(x, fs)
    if d is not None:
        out["lraLu"], out["shortTermP10Lufs"], out["shortTermP95Lufs"] = d["lraLu"], d["p10Lufs"], d["p95Lufs"]
        if d["p10Lufs"] is not None:
            out["shortTermSpreadLu"] = float(d["p95Lufs"] - d["p10Lufs"])
    return out


def reference_dynamics(ref, renders48: dict, offset: int) -> dict:
    """``renders48``: name -> 48 kHz mono full-length render. The reference window is the DI-aligned stretch of the matched channel
    (``offset`` = reference index of DI sample 0) as long as the longest render; without a matched channel the whole guitar
    isolation. The renders are measured whole, exactly as best_L's tonecheck metrics are."""
    n = max((len(v) for v in renders48.values()), default=0)
    if ref.matched_sig is not None:
        seg = np.asarray(ref.matched_sig[max(offset, 0):max(offset, 0) + n], dtype=np.float64)
        window = f"matched channel, reference samples {max(offset, 0)}..{max(offset, 0) + len(seg)} (DI-aligned)"
    else:
        seg = np.asarray(ref.ltas_sig, dtype=np.float64)
        window = f"whole guitar isolation ({ref.basis}); not time-aligned"
    return {"window": window, "reference": window_dynamics(seg), **{k: window_dynamics(v) for k, v in renders48.items()}}


def block_gains(preset: dict) -> dict:
    """Per path, per block of the emitted preset: the input gain / drive each block received. ``inputGainDb`` is dB relative to
    unity (a NAM block without the field gets 0.0), ``normalizeLoudness`` as emitted, and the params of the modelled pedals
    (``drive`` / ``level`` / ``tone`` of the boost) where they are params."""
    out: dict = {}
    for pname, path in (preset.get("paths") or {}).items():
        if path.get("enabled") is False and not path.get("blocks"):
            continue
        blocks = []
        for b in path.get("blocks") or []:
            src = ((b.get("model") or {}).get("source") or {})
            rec = {"id": b.get("id"), "type": b.get("type"), "slot": b.get("slot"),
                   "capture": None if not src else f"{src.get('id')}/{src.get('modelId')} {src.get('title')}",
                   "inputGainDb": float(b.get("inputGainDb", 0.0)) if b.get("type") == "nam" else None,
                   "normalizeLoudness": bool(b.get("normalizeLoudness", False)) if b.get("type") == "nam" else None}
            if b.get("params"):
                rec["params"] = {k: float(v) for k, v in b["params"].items()}
            blocks.append(rec)
        out[pname] = {"role": path.get("role"), "levelDb": path.get("levelDb"), "blocks": blocks}
    return out
