"""ITU-R BS.1770-4 integrated loudness and 4x-oversampled true peak (numpy/scipy only), plus the listening-section
helpers that use them (``choose_listen_section``, ``match_gain_db``).

Loudness: K-weighting (high shelf + RLB high-pass, coefficients derived for any rate; at 48 kHz they equal the
BS.1770-4 table), 400 ms blocks at 75 % overlap (100 ms hop), absolute gate -70 LUFS, relative gate 10 LU below the
absolute-gated mean. Channels (last axis of a 2-D ``(n, ch)`` array) all have weight 1.0 (no surround)."""
from __future__ import annotations

import numpy as np
from scipy import signal

BLOCK_S = 0.400
HOP_S = 0.100
ABS_GATE_LUFS = -70.0
REL_GATE_LU = -10.0
LISTEN_SECTION_S = 30.0


def k_weighting_sos(fs: float) -> np.ndarray:
    """Two biquads (shelf, RLB high-pass) as an ``(2, 6)`` SOS array for sample rate ``fs``."""
    f0, g, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = np.tan(np.pi * f0 / fs)
    vh = 10 ** (g / 20)
    vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    shelf = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
             1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    f0, q = 38.13547087602444, 0.5003270373238773
    k = np.tan(np.pi * f0 / fs)
    a0 = 1 + k / q + k * k
    hp = [1.0, -2.0, 1.0, 1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    return np.array([shelf, hp], dtype=np.float64)


def _as2d(x) -> np.ndarray:
    x = np.asarray(x, dtype=np.float64)
    return x[:, None] if x.ndim == 1 else x


def integrated_lufs(x, fs: int) -> float:
    """Gated integrated loudness (LUFS) of ``x`` ((n,) or (n, ch)). ``-inf`` when the signal is shorter than one
    block or every block is below the absolute gate."""
    x = _as2d(x)
    blk, hop = int(round(BLOCK_S * fs)), int(round(HOP_S * fs))
    if x.shape[0] < blk:
        return float("-inf")
    y = signal.sosfilt(k_weighting_sos(fs), x, axis=0)
    p = y * y
    cs = np.concatenate([np.zeros((1, p.shape[1])), np.cumsum(p, axis=0)])
    starts = np.arange(0, x.shape[0] - blk + 1, hop)
    z = ((cs[starts + blk] - cs[starts]) / blk).sum(axis=1)          # channel-summed mean square per block
    with np.errstate(divide="ignore"):
        lk = -0.691 + 10 * np.log10(z)
    keep = lk > ABS_GATE_LUFS
    if not np.any(keep):
        return float("-inf")
    rel = -0.691 + 10 * np.log10(np.mean(z[keep])) + REL_GATE_LU
    keep &= lk > rel
    if not np.any(keep):
        return float("-inf")
    return float(-0.691 + 10 * np.log10(np.mean(z[keep])))


def true_peak_db(x, oversample: int = 4) -> float:
    """True peak (dBTP, relative to full scale): max over channels after ``oversample``x polyphase interpolation.
    ``-inf`` for silence."""
    x = _as2d(x)
    if x.shape[0] == 0:
        return float("-inf")
    up = signal.resample_poly(x, oversample, 1, axis=0)
    p = float(np.max(np.abs(up)))
    return float(20 * np.log10(p)) if p > 0 else float("-inf")


def match_gain_db(ref, y, fs: int) -> tuple[float, float, float]:
    """(gain_db, lufs_ref, lufs_y): the gain that brings ``y`` to the integrated loudness of ``ref``. 0 dB when either
    is unmeasurable (silent or shorter than one block)."""
    lr, ly = integrated_lufs(ref, fs), integrated_lufs(y, fs)
    g = lr - ly if np.isfinite(lr) and np.isfinite(ly) else 0.0
    return float(g), lr, ly


def choose_listen_section(di48: np.ndarray, ref_len: int | None, offset: int, length_s: float = LISTEN_SECTION_S,
                          fs: int = 48000) -> tuple[int, int, dict]:
    """The ``length_s`` guitar-dominant section of the DI (``excerpt.select_excerpt``), in DI samples, moved so that
    ``ref[a + offset : b + offset]`` exists when the reference is a matched pair (``ref_len`` given); the whole
    overlap is used when it is shorter than the section. Returns (a, b, info)."""
    from .excerpt import select_excerpt
    a, b, info = select_excerpt(di48, fs, length_s)
    if ref_len is not None:
        lo, hi = max(0, -offset), min(len(di48), ref_len - offset)       # DI samples that have a reference partner
        if hi <= lo:
            raise ValueError("no overlap between the DI and the reference at this offset")
        if a < lo or b > hi:
            n = b - a
            a = lo if hi - lo <= n else min(max(a, lo), hi - n)
            b = min(a + n, hi)
            info = {**info, "movedToReference": True}
    return int(a), int(b), info
