"""Channel / onset helpers for the calibration methods (kept out of the CLI so they are unit-tested)."""
from __future__ import annotations

import numpy as np

from ..tonecheck import analysis as A


def side_channel(l: np.ndarray, r: np.ndarray) -> np.ndarray:
    """Method 3 signal: ``(L - R) / 2`` (length of the shorter channel). Hard-panned guitars survive; centred
    content (bass, kick, snare, lead vocal) cancels."""
    n = min(len(l), len(r))
    return 0.5 * (np.asarray(l[:n], dtype=float) - np.asarray(r[:n], dtype=float))


def mid_channel(l: np.ndarray, r: np.ndarray) -> np.ndarray:
    n = min(len(l), len(r))
    return 0.5 * (np.asarray(l[:n], dtype=float) + np.asarray(r[:n], dtype=float))


def cover_di_onsets(di_l: np.ndarray, di_r: np.ndarray, offsets_s: dict[str, float],
                    fs: int = A.ANALYSIS_RATE) -> np.ndarray:
    """Note onsets of the cover guitars in *mix time*: onsets detected on each DI, plus that DI's offset inside
    the mix (an onset at DI time t sounds at t + offset), merged and sorted."""
    on = [A.detect_onsets(di_l, fs) + offsets_s["L"], A.detect_onsets(di_r, fs) + offsets_s["R"]]
    return np.sort(np.concatenate(on))


STEM_SIDE_MIN_DB = -3.0     # stem side/mid RMS at or above this: guitars are (nearly) hard-panned -> use the side channel


def stem_guitar_signal(audio: np.ndarray) -> tuple[np.ndarray, str, float, dict]:
    """Mono analysis signal of a separated guitar stem ``(samples, channels)``.

    Hard-panned (double-tracked) guitars: side ``(L-R)/2`` and mid have the same power, so side/mid RMS is near 0 dB (or
    above); centred guitars cancel in the side channel. Side/mid RMS >= ``STEM_SIDE_MIN_DB`` -> side, else mid.
    Returns (signal, "side"|"mid", per-guitar level offset in dB, info). The offset (+3.01 dB for side: two uncorrelated
    hard-panned guitars, P_side = P_guitar / 2; 0 dB for mid) is what the matcher adds to get the per-guitar level."""
    a = np.asarray(audio, dtype=np.float64)
    if a.ndim == 1 or a.shape[1] == 1:
        m = a.reshape(len(a), -1)[:, 0]
        return m, "mid", 0.0, {"sideMidDb": None, "note": "mono stem"}
    side, mid = side_channel(a[:, 0], a[:, 1]), mid_channel(a[:, 0], a[:, 1])
    rms = lambda v: float(np.sqrt(np.mean(v ** 2)))
    ratio = 20 * np.log10(max(rms(side), 1e-12) / max(rms(mid), 1e-12))
    if ratio >= STEM_SIDE_MIN_DB:
        return side, "side", 3.0103, {"sideMidDb": float(ratio)}
    return mid, "mid", 0.0, {"sideMidDb": float(ratio)}
