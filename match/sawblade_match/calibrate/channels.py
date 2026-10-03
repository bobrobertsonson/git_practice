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
