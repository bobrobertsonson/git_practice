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
ONSET_MIN_RISE_DB = 6.0   # level rise required of an onset (see detect_onsets)
ONSET_RISE_BEFORE = 4     # STFT frames (hop 256) looked back / forward when measuring the rise
ONSET_RISE_AFTER = 3

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


def di_noise_floor_db(di: np.ndarray, fs: int) -> float:
    """DI noise floor: 5th-percentile 50 ms frame level (dBFS, mean-square)."""
    rms_db, _ = frame_rms_db(di, fs)
    return float(np.percentile(rms_db, 5))


GAP_FRAME_S = 0.010      # short-term RMS frame for gap detection
GAP_MIN_S = 0.120        # a gap is a DI region below floor + GAP_MARGIN_DB for at least this long
GAP_SKIP_S = 0.050       # the first part of each gap is ignored (ringing tail)
GAP_THRESHOLD_DB = -50.0  # absolute: a gap is DI 10 ms RMS below this (dBFS); the percentile floor is reporting only
GAP_MIN_TOTAL_S = 3.0    # less real silence than this in total -> rule n/a
GAP_MIN_COUNT = 3        # fewer separate gaps than this -> rule n/a (one intro/outro silence is not a gate test)


def _ms_frames_db(x: np.ndarray, n: int) -> np.ndarray:
    nf = len(x) // n
    fr = x[: nf * n].reshape(nf, n)
    return _db(np.mean(fr * fr, axis=1))


def gap_regions(di: np.ndarray, fs: int, threshold_db: float = GAP_THRESHOLD_DB) -> list[tuple[int, int]]:
    """Real-silence regions of the DI as (start, stop) sample indices.

    A region is a run of 10 ms frames whose RMS stays below the absolute ``threshold_db`` (default -50 dBFS) for at
    least 120 ms; the first 50 ms of each region are skipped (ringing tail)."""
    n = int(round(GAP_FRAME_S * fs))
    quiet = _ms_frames_db(di, n) < threshold_db
    min_f, skip_f = int(round(GAP_MIN_S / GAP_FRAME_S)), int(round(GAP_SKIP_S / GAP_FRAME_S))
    regions: list[tuple[int, int]] = []
    i, nf = 0, len(quiet)
    while i < nf:
        if not quiet[i]:
            i += 1
            continue
        j = i
        while j < nf and quiet[j]:
            j += 1
        if j - i >= min_f:
            regions.append(((i + skip_f) * n, j * n))
        i = j
    return regions


def gap_noise_db(out: np.ndarray, di: np.ndarray | None, fs: int) -> dict:
    """Output RMS in the DI's real-silence gaps relative to the output RMS over its playing frames.

    Gaps: DI regions whose 10 ms RMS stays below an absolute -50 dBFS for >= 120 ms, minus the first 50 ms of each
    (see ``gap_regions``); a dense DI with no real silence therefore gives n/a. Value None with a reason when there is no DI, when there are fewer than 3 gaps or they total less than 3 s
    ("no gaps"), or when the floor is within 10 dB of the median active DI frame ("no clear gaps": a steady DI has no real
    gaps). Also reports ``gapCount`` and ``gapTotalS``. Output and DI are compared sample-aligned (the render has the DI's
    length, advanced by the chain latency)."""
    if di is None:
        return {"value": None, "reason": "no DI supplied (gaps are located on the DI)"}
    n = min(len(out), len(di))
    out, di = out[:n], di[:n]
    regions = gap_regions(di, fs)
    floor = di_noise_floor_db(di, fs)          # reporting only
    total = sum(b - a for a, b in regions) / fs
    res = {"diNoiseFloorDb": floor, "gapCount": len(regions), "gapTotalS": float(total)}
    if len(regions) < GAP_MIN_COUNT or total < GAP_MIN_TOTAL_S:
        return {**res, "value": None, "reason": "no gaps"}
    di_db, _ = frame_rms_db(di, fs)
    _, di_active, _ = activity_mask(di, fs)
    median_active = float(np.median(di_db[di_active]))
    res["diMedianActiveDb"] = median_active
    if median_active - floor < 10.0:
        return {**res, "value": None, "reason": "no clear gaps"}
    gap_ms = sum(float(np.sum(out[a:b] ** 2)) for a, b in regions) / sum(b - a for a, b in regions)
    out_db, _ = frame_rms_db(out, fs)
    _, active, _ = activity_mask(out, fs)
    act = _db(np.mean(10 ** (out_db[active] / 10.0)))
    return {**res, "value": float(_db(gap_ms) - act)}


def gap_noise_legacy_db(out: np.ndarray, di: np.ndarray, fs: int) -> dict:
    """The pre-3.6 definition (quietest-5 % + 6 dB DI frames, 50 ms), kept only to compare with ``gap_noise_db``."""
    n = min(len(out), len(di))
    out, di = out[:n], di[:n]
    di_db, _ = frame_rms_db(di, fs)
    out_db, _ = frame_rms_db(out, fs)
    gaps = di_db <= float(np.percentile(di_db, 5)) + 6.0
    _, active, _ = activity_mask(out, fs)
    gap = _db(np.mean(10 ** (out_db[gaps] / 10.0)))
    act = _db(np.mean(10 ** (out_db[active] / 10.0)))
    return {"value": float(gap - act), "gapCount": None, "gapTotalS": float(gaps.sum() * FRAME_S)}


# --- EBU R128 loudness range ---------------------------------------------------------------------
_K_SHELF = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
_K_HP = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])


def loudness_range_detail(x: np.ndarray, fs: int = ANALYSIS_RATE) -> dict | None:
    """The numbers behind ``loudness_range_lu``: LRA and the gated 10th / 95th percentiles of the 3 s short-term loudness (LUFS).
    None for signals shorter than 3 s."""
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
        return {"lraLu": 0.0, "p10Lufs": None, "p95Lufs": None}
    rel = -0.691 + 10 * np.log10(np.mean(10 ** ((lufs[keep] + 0.691) / 10))) - 20.0
    keep = lufs > max(rel, -70.0)
    if not keep.any():
        return {"lraLu": 0.0, "p10Lufs": None, "p95Lufs": None}
    p10, p95 = float(np.percentile(lufs[keep], 10)), float(np.percentile(lufs[keep], 95))
    return {"lraLu": float(np.percentile(lufs[keep], 95) - np.percentile(lufs[keep], 10)), "p10Lufs": p10, "p95Lufs": p95}


def loudness_range_lu(x: np.ndarray, fs: int = ANALYSIS_RATE) -> float | None:
    """EBU Tech 3342 LRA: K-weighted 3 s short-term loudness every 0.1 s, -70 LUFS absolute gate,
    -20 LU relative gate, 95th - 10th percentile. None for signals shorter than 3 s."""
    d = loudness_range_detail(x, fs)
    return None if d is None else d["lraLu"]


# --- onsets + low-end tightness --------------------------------------------------------------------
def detect_onsets(di: np.ndarray, fs: int = ANALYSIS_RATE) -> np.ndarray:
    """Spectral-flux onset detection on the DI. Returns onset times in seconds.

    STFT 1024/256 Hann, 50 Hz..6 kHz, log-magnitude (dB, floored 70 dB under the peak), half-wave
    rectified frame-to-frame difference summed over bins. Peaks must exceed (0.5 s running median +
    4 x robust scale), be >= 80 ms apart, and occur in frames within 30 dB of the DI's 95th-percentile
    level (so hum/hiss flux between notes is ignored). Finally each peak must be a real level rise:
    max level over the next 3 frames minus min over the previous 4 frames >= 6 dB.

    Known recall limit of that check: a note that re-picks a still-ringing string, so that its level is less
    than ~6 dB above the lowest level of the preceding ~20 ms (a rise of ~4 dB is suppressed, ~9 dB is kept; pinned
    in the tests), is not detected. Dense legato/ringing passages can therefore be under-counted; the
    price is the removal of the spurious onsets inside noisy decays."""
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
    # Minimum-rise check: a real onset raises the 50 Hz-6 kHz level by >= ONSET_MIN_RISE_DB over the frames
    # just before it. Fluctuations of the log-spectrum inside a decaying noisy tail produce flux peaks
    # without any level rise (the old detector reported ~2 spurious onsets per 30-50 ms-decay pluck).
    rise_ok = []
    for p in peaks:
        before = lvl[max(0, p - ONSET_RISE_BEFORE):p]
        after = lvl[p:p + ONSET_RISE_AFTER]
        rise_ok.append(before.size > 0 and after.max() - before.min() >= ONSET_MIN_RISE_DB)
    peaks = peaks[np.array(rise_ok, dtype=bool)] if len(peaks) else peaks
    # Heuristic time correction (+5.3 ms): a frame centred at t already "sees" a new note once the window
    # end passes it, i.e. up to half a window (512 samples) before the onset, and the flux peaks about a
    # quarter window (256 samples = the hop = 5.3 ms at 48 kHz) early. Adding that quarter window brings the
    # estimate to within about +-5 ms of the true onset on synthetic plucks (tested to 10 ms), negligible
    # against the 10 ms envelope and the [-10, +60] ms peak search that follows.
    return t[peaks] + (nper / 4) / fs


def _stats(vals, key):
    if not vals:
        return {key: None, "p25": None, "p75": None}
    return {key: float(np.median(vals)), "p25": float(np.percentile(vals, 25)),
            "p75": float(np.percentile(vals, 75))}


def low_end_decay(di: np.ndarray, out: np.ndarray, fs: int = ANALYSIS_RATE,
                  band: tuple[float, float] = (80.0, 160.0), drop_db: float = 20.0,
                  max_decay_s: float = 2.0, onsets: np.ndarray | None = None) -> tuple[dict, dict]:
    """Returns (lowTightnessMs, lowDecayDbPerMs) metrics, both from DI onsets and the output's 80-160 Hz band.

    Output is band-passed (Butterworth 4, causal), squared and smoothed with a 10 ms moving average
    (energy envelope, dB). For each DI onset the reference is the envelope peak in [t-10 ms, t+60 ms]
    (covers filter delay and the render's alignment delay).
    * lowTightnessMs: median time from that peak to the first -``drop_db`` crossing. Onsets whose decay is
      cut off by the next onset (or ``max_decay_s``) are censored (excluded, counted).
    * lowDecayDbPerMs: linear-regression slope (dB/ms, negative = decaying) of the envelope over
      [peak+5 ms, min(peak+35 ms, next onset)]; windows shorter than 15 ms are censored.
    ``onsets`` (seconds, sorted) overrides the detection on ``di`` (used by the calibration tool)."""
    onsets = detect_onsets(di, fs) if onsets is None else np.asarray(onsets, dtype=float)
    sos = signal.butter(4, band, btype="bandpass", fs=fs, output="sos")
    y = signal.sosfilt(sos, out)
    k = int(0.010 * fs)
    env = np.convolve(y * y, np.ones(k) / k, mode="same")
    env_db = _db(env)
    times, slopes = [], []
    for i, t0 in enumerate(onsets):
        a = int(max(0, (t0 - 0.010) * fs))
        b = int((t0 + 0.060) * fs)
        if b >= len(env_db):
            continue
        pk = a + int(np.argmax(env_db[a:b]))
        nxt = len(env_db) if i + 1 >= len(onsets) else int(onsets[i + 1] * fs)
        end = min(nxt - int(0.020 * fs), pk + int(max_decay_s * fs)) if i + 1 < len(onsets) else \
            min(len(env_db), pk + int(max_decay_s * fs))
        seg = env_db[pk:end]
        below = np.nonzero(seg <= env_db[pk] - drop_db)[0]
        if below.size:
            j = below[0]
            if j > 0:
                y0, y1 = seg[j - 1], seg[j]
                frac = (y0 - (env_db[pk] - drop_db)) / max(y0 - y1, 1e-12)
                j = j - 1 + frac
            times.append(1000.0 * j / fs)
        s0, s1 = pk + int(0.005 * fs), min(pk + int(0.035 * fs), nxt, len(env_db))
        if (s1 - s0) >= int(0.015 * fs):
            tt = 1000.0 * np.arange(s1 - s0) / fs
            slopes.append(float(np.polyfit(tt, env_db[s0:s1], 1)[0]))
    n = len(onsets)
    t = _stats(times, "valueMs")
    tight = {"valueMs": t["valueMs"], "nOnsets": int(n), "nMeasured": len(times),
             "nCensored": int(n - len(times)), "p25Ms": t["p25"], "p75Ms": t["p75"]}
    d = _stats(slopes, "value")
    decay = {"value": d["value"], "p25": d["p25"], "p75": d["p75"], "nOnsets": int(n),
             "nMeasured": len(slopes), "nCensored": int(n - len(slopes))}
    return tight, decay


def low_tightness_ms(di, out, fs=ANALYSIS_RATE, **kw) -> dict:
    return low_end_decay(di, out, fs, **kw)[0]


def fizz_texture(x: np.ndarray, mask: np.ndarray | None, fs: int = ANALYSIS_RATE) -> dict:
    """High-frequency texture over the active Welch segments (same segments as the LTAS): median spectral flatness
    5-10 kHz (noise-like fizz -> high, smooth amp+cab roll-off -> low) and the 8-12 kHz level re 1-3 kHz (dB)."""
    nfft = NFFT
    w = signal.get_window("hann", nfft, fftbins=True)
    starts = list(range(0, len(x) - nfft + 1, HOP))
    if mask is not None:
        csum = np.concatenate([[0], np.cumsum(mask.astype(np.int64))])
        keep = [s for s in starts if (csum[s + nfft] - csum[s]) >= SEG_ACTIVE_FRACTION * nfft]
        starts = keep or starts
    if not starts:
        return {"flatness5to10k": None, "hf8to12DbRe1to3k": None, "nSegments": 0}
    P = np.array([np.abs(np.fft.rfft(x[s:s + nfft] * w)) ** 2 for s in starts])
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    B = P[:, (f >= 5000) & (f < 10000)] + 1e-30
    flat = float(np.median(np.exp(np.log(B).mean(axis=1)) / B.mean(axis=1)))
    lv = lambda lo, hi: float(np.sum(P[:, (f >= lo) & (f < hi)]))
    hf = 10 * np.log10(max(lv(8000, 12000), 1e-30) / max(lv(1000, 3000), 1e-30))
    return {"flatness5to10k": flat, "hf8to12DbRe1to3k": float(hf), "nSegments": len(starts)}


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
        "fizzTexture": fizz_texture(xa, mask),
    }
    dia = to_analysis_rate(di[0], di[1]) if di is not None else None
    metrics["gapNoiseDb"] = gap_noise_db(xa, dia, ANALYSIS_RATE)
    if dia is not None:
        metrics["diNoiseFloorDb"] = {"value": di_noise_floor_db(dia, ANALYSIS_RATE),
                                     "note": "5th-percentile 50 ms DI frame level (dBFS); use to calibrate gate thresholds"}
        metrics["lowTightnessMs"], metrics["lowDecayDbPerMs"] = low_end_decay(dia, xa)
    else:
        note = "no DI supplied (onsets are detected on the DI)"
        metrics["lowTightnessMs"] = {"valueMs": None, "note": note}
        metrics["lowDecayDbPerMs"] = {"value": None, "note": note}
    for name, m in targets.get("metrics", {}).items():
        if name in metrics:
            metrics[name]["def"] = m["def"]
            metrics[name]["target"] = m["target"]
    return Analysis(fs, NOMINAL_CENTRES, absdb, rel, groups, metrics, nseg, float(frames.mean()), warnings)
