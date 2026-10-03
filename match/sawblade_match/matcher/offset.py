"""Refine a coarse DI->mix offset to roughly +-1 ms (sample resolution, not guaranteed sample-exact for distorted renders) by cross-correlating a rendered DI against the mix channel."""
from __future__ import annotations

import numpy as np
from scipy import signal


def _bandpass(x, fs, lo, hi):
    sos = signal.butter(4, [lo, hi], btype="bandpass", fs=fs, output="sos")
    return signal.sosfiltfilt(sos, x)


def _env(x, fs, ms=2.0):
    k = max(1, int(fs * ms / 1000))
    e = np.sqrt(np.convolve(x * x, np.ones(k) / k, mode="same"))
    return e - e.mean()


def _xcorr_valid(ref_seg: np.ndarray, x: np.ndarray) -> np.ndarray:
    """c[k] = sum_n ref_seg[n + k] * x[n], k = 0 .. len(ref_seg) - len(x)."""
    return signal.correlate(ref_seg, x, mode="valid", method="fft")


def refine_offset(render: np.ndarray, ref: np.ndarray, fs: int, coarse: int, start: int = 0,
                  search: int | None = None, band=(300.0, 4000.0), fine_window_ms: float = 3.0) -> dict:
    """Find ``off`` such that ``ref[start + off + n] ~ render[n]`` (``render`` is the output for DI samples
    ``start..start+len``; ``coarse`` is the expected ``off`` in samples).

    Step 1, envelope cross-correlation (2 ms power envelope of the 300 Hz-4 kHz band, +-``search`` samples, default
    +-250 ms): robust, ~1 ms resolution. Step 2, waveform cross-correlation of the band-passed signals within
    +-``fine_window_ms`` of step 1, whitened (PHAT-like, per-bin magnitude normalised) so the peak is sharp; accepted
    when its peak is >= 2x the median |correlation| of the window, else the envelope result is kept.
    Returns {offset, coarseEnvelope, fineAccepted, peakRatio, envPeakRatio, method}."""
    search = search if search is not None else int(0.25 * fs)
    L = len(render)
    lo_i = start + coarse - search
    hi_i = start + coarse + search + L
    pad_l = max(0, -lo_i)
    seg = ref[max(lo_i, 0):min(hi_i, len(ref))]
    seg = np.concatenate([np.zeros(pad_l), seg, np.zeros(max(0, hi_i - len(ref)))]) if pad_l or hi_i > len(ref) else seg
    r_bp, s_bp = _bandpass(render, fs, *band), _bandpass(seg, fs, *band)
    c = _xcorr_valid(_env(s_bp, fs), _env(r_bp, fs))
    k = int(np.argmax(c))
    ratio_env = float(c[k] / (np.median(np.abs(c)) + 1e-12))
    off_env = coarse - search + k
    out = {"coarseEnvelope": int(off_env), "envPeakRatio": ratio_env, "fineAccepted": False}
    w = int(fine_window_ms * fs / 1000)
    a = start + off_env - w
    seg2 = ref[a:a + L + 2 * w] if a >= 0 and a + L + 2 * w <= len(ref) else None
    if seg2 is not None and len(seg2) == L + 2 * w:
        s2 = _bandpass(seg2, fs, *band)
        n = len(r_bp) + len(s2)
        nfft = int(2 ** np.ceil(np.log2(n)))
        X, Y = np.fft.rfft(s2, nfft), np.fft.rfft(r_bp, nfft)
        G = X * np.conj(Y)
        fr = np.fft.rfftfreq(nfft, 1.0 / fs)
        inband = (fr >= band[0]) & (fr <= band[1])
        G = np.where(inband, G / np.maximum(np.abs(G), 1e-12 * np.max(np.abs(G)) + 1e-30), 0.0)
        cc = np.fft.irfft(G, nfft)[: 2 * w + 1]       # lags 0..2w of ref_seg relative to render start
        j = int(np.argmax(cc))
        pr = float(cc[j] / (np.median(np.abs(cc)) + 1e-12))
        out["peakRatio"] = pr
        if pr >= 2.0:
            out.update(offset=int(off_env - w + j), fineAccepted=True, method="envelope+waveform(PHAT)")
            return out
    out.update(offset=int(off_env), method="envelope", peakRatio=out.get("peakRatio", ratio_env))
    return out
