"""Excerpt selection: the guitar-dominant, densely played window of a DI."""
from __future__ import annotations

import numpy as np

FRAME_S = 0.050


def frame_db(x: np.ndarray, fs: int) -> np.ndarray:
    n = int(round(FRAME_S * fs))
    nf = len(x) // n
    fr = x[: nf * n].reshape(nf, n).astype(np.float64)
    return 10 * np.log10(np.maximum(np.mean(fr * fr, axis=1), 1e-20))


def select_excerpt(di: np.ndarray, fs: int, length_s: float = 6.0, stride_s: float = 0.5,
                   dense_db: float = 12.0, avoid_clip: bool = True) -> tuple[int, int, dict]:
    """Return (start, end, info) in samples. Score = fraction of 50 ms frames within ``dense_db`` of the DI's
    95th-percentile frame level (dense, loud playing: no gaps, no ringing-out tails), tie-broken by mean frame level;
    windows containing a (near) full-scale sample are skipped when ``avoid_clip`` and another window exists.
    Deterministic; the first best window wins."""
    n = len(di)
    win = int(round(length_s * fs))
    if n <= win:
        return 0, n, {"score": None, "note": "DI shorter than the excerpt length; using all of it"}
    db = frame_db(di, fs)
    p95 = np.percentile(db, 95)
    fl = int(round(FRAME_S * fs))
    wf = int(round(length_s / FRAME_S))
    dense = (db >= p95 - dense_db).astype(float)
    cs = np.concatenate([[0.0], np.cumsum(dense)])
    cl = np.concatenate([[0.0], np.cumsum(np.maximum(db - p95, -40.0))])
    best, best_key = None, None
    clip_frames = np.nonzero(np.abs(di[: len(db) * fl]).reshape(len(db), fl).max(axis=1) >= 0.999)[0]
    step = max(1, int(round(stride_s / FRAME_S)))
    for s in range(0, len(db) - wf + 1, step):
        frac = (cs[s + wf] - cs[s]) / wf
        mean_lvl = (cl[s + wf] - cl[s]) / wf
        clipped = avoid_clip and np.any((clip_frames >= s) & (clip_frames < s + wf))
        key = (not clipped, round(frac, 6), mean_lvl)
        if best_key is None or key > best_key:
            best, best_key = s, key
    a = best * fl
    return a, a + win, {"score": float(best_key[1]), "meanLevelRelP95Db": float(best_key[2]),
                        "startS": a / fs, "endS": (a + win) / fs, "avoidedClip": bool(best_key[0])}
