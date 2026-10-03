"""Spectral / dynamic analysis. Definitions follow docs/tone_targets.json ("analysis", "metrics").

Method notes
------------
* All audio is analysed at 48 kHz (resampled with a polyphase filter if needed) so the 8192-pt
  Welch segments of the contract mean the same thing for 44.1 and 48 kHz material.
* Activity gate: 50 ms non-overlapping frames; a frame is active when its RMS is within 30 dB of the
  95th-percentile frame RMS. A Welch segment (8192 samples, hop 4096, periodic Hann) is used only if at
  least 80 % of its samples lie in active frames (fallback: all segments, with a warning).
* 1/3-octave band power = integral of the one-sided PSD over the IEC band [fc*2^(-1/6), fc*2^(1/6)],
  with fractional weights for FFT bins straddling an edge. Levels are dB relative to the 1 kHz band.
* Group level = power mean (dB) of the band levels in the group.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from fractions import Fraction

import numpy as np
import soundfile as sf
from scipy import signal

ANALYSIS_RATE = 48000
NFFT = 8192
HOP = NFFT // 2
FRAME_S = 0.050
GATE_WINDOW_DB = 30.0
SEG_ACTIVE_FRACTION = 0.8
DB_FLOOR = -200.0

# IEC 61260 nominal centres 25 Hz .. 12.5 kHz (the contract's bands); exact centres are base-10 thirds.
NOMINAL_CENTRES = [25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                   1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500]


def _exact_centre(nominal: float) -> float:
    idx = round(10 * np.log10(nominal / 1000.0))
    return 1000.0 * 10 ** (idx / 10.0)


def read_mono(path, channel: str = "left") -> tuple[np.ndarray, int]:
    """Read audio as mono float64. channel: left (default, as tonerender), right, or mid (mean)."""
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    if channel == "mid":
        m = x.mean(axis=1)
    elif channel == "right":
        m = x[:, min(1, x.shape[1] - 1)]
    elif channel == "left":
        m = x[:, 0]
    else:
        raise ValueError(f"unknown channel {channel!r}")
    return m, int(fs)


def to_analysis_rate(x: np.ndarray, fs: int) -> np.ndarray:
    if fs == ANALYSIS_RATE:
        return x
    f = Fraction(ANALYSIS_RATE, fs)
    return signal.resample_poly(x, f.numerator, f.denominator)


def _db(p: float | np.ndarray) -> np.ndarray:
    return np.maximum(10.0 * np.log10(np.maximum(p, 1e-30)), DB_FLOOR)


def frame_rms_db(x: np.ndarray, fs: int) -> tuple[np.ndarray, int]:
    n = int(round(FRAME_S * fs))
    nf = len(x) // n
    if nf == 0:
        raise ValueError("signal shorter than one 50 ms frame")
    fr = x[: nf * n].reshape(nf, n)
    return _db(np.mean(fr * fr, axis=1)), n


def activity_mask(x: np.ndarray, fs: int) -> tuple[np.ndarray, np.ndarray, int]:
    """Return (per-sample bool mask, per-frame bool mask, frame length)."""
    rms_db, n = frame_rms_db(x, fs)
    thr = np.percentile(rms_db, 95) - GATE_WINDOW_DB
    frames = rms_db >= thr
    mask = np.zeros(len(x), dtype=bool)
    mask[: len(frames) * n] = np.repeat(frames, n)
    return mask, frames, n


def ltas_psd(x: np.ndarray, fs: int, mask: np.ndarray | None = None,
             warnings: list[str] | None = None) -> tuple[np.ndarray, np.ndarray, int]:
    """Welch (Hann, 8192, 50 % overlap) mean PSD over active segments. Returns (freqs, psd, n_segments)."""
    nfft = NFFT
    w = signal.get_window("hann", nfft, fftbins=True)
    scale = 1.0 / (fs * np.sum(w * w))
    starts = list(range(0, len(x) - nfft + 1, HOP))
    if mask is not None:
        csum = np.concatenate([[0], np.cumsum(mask.astype(np.int64))])
        keep = [s for s in starts if (csum[s + nfft] - csum[s]) >= SEG_ACTIVE_FRACTION * nfft]
        if not keep and starts:
            if warnings is not None:
                warnings.append("no Welch segment passed the activity gate; using all segments")
            keep = starts
        starts = keep
    if not starts:
        raise ValueError("signal shorter than one 8192-sample Welch segment")
    acc = np.zeros(nfft // 2 + 1)
    for s in starts:
        acc += np.abs(np.fft.rfft(x[s:s + nfft] * w)) ** 2
    psd = acc / len(starts) * scale
    psd[1:-1] *= 2.0
    return np.fft.rfftfreq(nfft, 1.0 / fs), psd, len(starts)


def band_power(freqs: np.ndarray, psd: np.ndarray, lo: float, hi: float) -> float:
    """Integral of the PSD over [lo, hi] with fractional weights for edge bins."""
    df = freqs[1] - freqs[0]
    left = np.maximum(freqs - df / 2, lo)
    right = np.minimum(freqs + df / 2, hi)
    wgt = np.clip(right - left, 0, None)
    return float(np.sum(psd * wgt))


def band_levels_db(freqs: np.ndarray, psd: np.ndarray,
                   centres: list[float] = NOMINAL_CENTRES) -> tuple[np.ndarray, np.ndarray]:
    """(absolute band levels dB, levels relative to the 1 kHz band)."""
    p = np.array([band_power(freqs, psd, _exact_centre(c) * 2 ** (-1 / 6), _exact_centre(c) * 2 ** (1 / 6))
                  for c in centres])
    absdb = _db(p)
    ref = absdb[centres.index(1000)]
    if ref <= DB_FLOOR + 1:
        raise ValueError("1 kHz band has no energy; cannot normalise")
    return absdb, absdb - ref


def group_levels(rel_db: np.ndarray, groups: dict[str, list[float]],
                 centres: list[float] = NOMINAL_CENTRES) -> dict[str, float]:
    out = {}
    for name, bands in groups.items():
        lv = np.array([rel_db[centres.index(b)] for b in bands])
        out[name] = float(_db(np.mean(10 ** (lv / 10.0))))
    return out


def spectral_flatness(psd_band: np.ndarray) -> float:
    p = np.maximum(psd_band, 1e-30)
    return float(np.exp(np.mean(np.log(p))) / np.mean(p))


def buzz_flatness(freqs: np.ndarray, psd: np.ndarray, lo: float = 1000.0, hi: float = 3000.0) -> float:
    """Spectral flatness (geo/arith mean of power) of the active-frame mean PSD over lo..hi."""
    sel = (freqs >= lo) & (freqs <= hi)
    return spectral_flatness(psd[sel])


def crest_factor_db(x: np.ndarray, mask: np.ndarray) -> float:
    a = x[mask]
    if a.size == 0:
        a = x
    return float(_db(np.max(np.abs(a)) ** 2) - _db(np.mean(a * a)))


def gap_noise_db(x: np.ndarray, fs: int) -> float:
    """Power-mean RMS of the quietest 5 % of 50 ms frames relative to the active RMS."""
    rms_db, _ = frame_rms_db(x, fs)
    _, frames, _ = activity_mask(x, fs)
    order = np.sort(rms_db)
    k = max(1, int(np.ceil(0.05 * len(order))))
    gap = _db(np.mean(10 ** (order[:k] / 10.0)))
    act = _db(np.mean(10 ** (rms_db[frames] / 10.0)))
    return float(gap - act)


# --- EBU R128 loudness range ---------------------------------------------------------------------
_K_SHELF = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
_K_HP = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])


def loudness_range_lu(x: np.ndarray, fs: int = ANALYSIS_RATE) -> float | None:
    """EBU Tech 3342 LRA: K-weighted 3 s short-term loudness every 0.1 s, -70 LUFS absolute gate,
    -20 LU relative gate, 95th - 10th percentile. None for signals shorter than 3 s."""
    if fs != ANALYSIS_RATE:
        raise ValueError("K-weighting coefficients are for 48 kHz")
    win = 3 * fs
    if len(x) < win:
        return None
    y = signal.lfilter(*_K_SHELF, x)
    y = signal.lfilter(*_K_HP, y)
    sq = np.concatenate([[0.0], np.cumsum(y * y)])
    starts = np.arange(0, len(y) - win + 1, fs // 10)
    ms = (sq[starts + win] - sq[starts]) / win
    lufs = -0.691 + 10 * np.log10(np.maximum(ms, 1e-30))
    keep = lufs > -70.0
    if not keep.any():
        return 0.0
    rel = -0.691 + 10 * np.log10(np.mean(10 ** ((lufs[keep] + 0.691) / 10))) - 20.0
    keep = lufs > max(rel, -70.0)
    if not keep.any():
        return 0.0
    return float(np.percentile(lufs[keep], 95) - np.percentile(lufs[keep], 10))


# --- onsets + low-end tightness --------------------------------------------------------------------
def detect_onsets(di: np.ndarray, fs: int = ANALYSIS_RATE) -> np.ndarray:
    """Spectral-flux onset detection on the DI. Returns onset times in seconds.

    STFT 1024/256 Hann, 50 Hz..6 kHz, log-magnitude (dB, floored 70 dB under the peak), half-wave
    rectified frame-to-frame difference summed over bins. Peaks must exceed (0.5 s running median +
    4 x robust scale), be >= 80 ms apart, and occur in frames within 30 dB of the DI's 95th-percentile
    level (so hum/hiss flux between notes is ignored)."""
    nper, hop = 1024, 256
    f, t, Z = signal.stft(di, fs, window="hann", nperseg=nper, noverlap=nper - hop, boundary=None, padded=False)
    sel = (f >= 50) & (f <= 6000)
    mag = np.abs(Z[sel])
    db = 20 * np.log10(np.maximum(mag, 1e-9))
    db = np.maximum(db, db.max() - 70.0)
    flux = np.sum(np.maximum(0.0, np.diff(db, axis=1)), axis=0)
    flux = np.concatenate([[0.0], flux])
    med = signal.medfilt(flux, kernel_size=2 * int(0.25 * fs / hop) + 1)
    scale = 1.4826 * np.median(np.abs(flux - np.median(flux))) + 1e-9
    thr = med + 4.0 * scale
    peaks, _ = signal.find_peaks(flux, height=thr, distance=max(1, int(0.08 * fs / hop)))
    lvl = _db(np.sum(mag * mag, axis=0))
    ok = lvl[np.minimum(peaks, len(lvl) - 1)] >= np.percentile(lvl, 95) - GATE_WINDOW_DB
    peaks = peaks[ok]
    # frame k of the STFT is centred at t[k]; the flux peak lags the true onset by ~half a window
    return np.maximum(t[peaks] - 0.5 * nper / fs * 0.5, 0.0)


def low_tightness_ms(di: np.ndarray, out: np.ndarray, fs: int = ANALYSIS_RATE,
                     band: tuple[float, float] = (80.0, 160.0), drop_db: float = 20.0,
                     max_decay_s: float = 2.0) -> dict:
    """Median time for the output's 80-160 Hz band energy to fall ``drop_db`` after DI onsets.

    Output is band-passed (Butterworth 4, causal), squared and smoothed with a 10 ms moving average
    (energy envelope, dB). For each DI onset the reference is the envelope peak in [t-10 ms, t+60 ms]
    (covers filter delay and the render's alignment delay); the decay time is the first -``drop_db``
    crossing after it. Onsets whose decay is cut off by the next onset (or ``max_decay_s``) are
    censored and excluded from the median (counted)."""
    onsets = detect_onsets(di, fs)
    sos = signal.butter(4, band, btype="bandpass", fs=fs, output="sos")
    y = signal.sosfilt(sos, out)
    k = int(0.010 * fs)
    env = np.convolve(y * y, np.ones(k) / k, mode="same")
    env_db = _db(env)
    times = []
    for i, t0 in enumerate(onsets):
        a = int(max(0, (t0 - 0.010) * fs))
        b = int((t0 + 0.060) * fs)
        if b >= len(env_db):
            continue
        pk = a + int(np.argmax(env_db[a:b]))
        end = len(env_db) if i + 1 >= len(onsets) else int((onsets[i + 1] - 0.020) * fs)
        end = min(end, pk + int(max_decay_s * fs))
        seg = env_db[pk:end]
        below = np.nonzero(seg <= env_db[pk] - drop_db)[0]
        if below.size == 0:
            continue
        j = below[0]
        # linear interpolation of the crossing
        if j > 0:
            y0, y1 = seg[j - 1], seg[j]
            target = env_db[pk] - drop_db
            frac = (y0 - target) / max(y0 - y1, 1e-12)
            j = j - 1 + frac
        times.append(1000.0 * j / fs)
    n = len(onsets)
    return {"valueMs": float(np.median(times)) if times else None, "nOnsets": int(n),
            "nMeasured": len(times), "nCensored": int(n - len(times)),
            "p25Ms": float(np.percentile(times, 25)) if times else None,
            "p75Ms": float(np.percentile(times, 75)) if times else None}


# --- whole-signal analysis --------------------------------------------------------------------------
@dataclass
class Analysis:
    rate_in: int
    centres: list[float]
    abs_db: np.ndarray
    rel_db: np.ndarray
    groups: dict[str, float]
    metrics: dict
    n_segments: int
    active_fraction: float
    warnings: list[str] = field(default_factory=list)


def analyze(x: np.ndarray, fs: int, targets: dict, di: tuple[np.ndarray, int] | None = None) -> Analysis:
    """Full analysis of one signal. ``di`` (samples, rate) enables lowTightnessMs."""
    warnings: list[str] = []
    xa = to_analysis_rate(x, fs)
    mask, frames, _ = activity_mask(xa, ANALYSIS_RATE)
    freqs, psd, nseg = ltas_psd(xa, ANALYSIS_RATE, mask, warnings)
    absdb, rel = band_levels_db(freqs, psd)
    groups = group_levels(rel, targets["analysis"]["bandGroups"])
    metrics = {
        "buzz": {"value": buzz_flatness(freqs, psd)},
        "crestFactorDb": {"value": crest_factor_db(xa, mask)},
        "loudnessRangeLU": {"value": loudness_range_lu(xa)},
        "gapNoiseDb": {"value": gap_noise_db(xa, ANALYSIS_RATE)},
    }
    if di is not None:
        dia = to_analysis_rate(di[0], di[1])
        metrics["lowTightnessMs"] = low_tightness_ms(dia, xa)
    else:
        metrics["lowTightnessMs"] = {"valueMs": None, "note": "no DI supplied (onsets are detected on the DI)"}
    for name, m in targets.get("metrics", {}).items():
        if name in metrics:
            metrics[name]["def"] = m["def"]
            metrics[name]["target"] = m["target"]
    return Analysis(fs, NOMINAL_CENTRES, absdb, rel, groups, metrics, nseg, float(frames.mean()), warnings)
