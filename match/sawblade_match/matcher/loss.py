"""Matcher loss (documented weights).

total = W_LTAS * ltas + W_BUZZ * buzz + W_DECAY * decay + W_STFT * stft (matched pairs only) + W_REG * reg

* ``ltas``   A-weighted RMS difference (dB) of the 1/3-octave LTAS, bands 80 Hz-8 kHz, after removing the overall
             level offset. The offset is the A-weight-power-weighted mean difference, so the error is the weighted
             standard deviation of the per-band difference (level is a free parameter: output gain is set after).
             Bands/PSD follow sawblade_match.tonecheck.analysis (Welch Hann 8192/50 %, fractional band edges), on
             the segments that are >= 80 % active in the DI excerpt (same segments for every signal).
* ``buzz``   |10log10(flatness 1-3 kHz) of output - of reference| in dB (weight 0.5 / dB).
* ``decay``  |lowDecayDbPerMs(output) - lowDecayDbPerMs(reference)| (weight 2 / (dB/ms)); dropped (and recorded)
             when either side has < 3 measurable onsets.
* ``stft``   matched pair only: multi-resolution (512/2048/8192-pt) log-magnitude error in dB between the time-aligned
             output and the reference channel, bins 100 Hz-8 kHz, active frames only, after removing the mean log
             level offset. Asymmetric because the reference mix also holds bass/drums/vocals: output energy above the
             reference counts 1.0, below it 0.3. Weight 0.25 / dB.
* ``reg``    RMS of the EQ gains (dB), weight 0.02 (keeps EQs from becoming extreme on a short excerpt).
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import signal

from ..tonecheck.analysis import (NFFT, HOP, NOMINAL_CENTRES, _exact_centre, buzz_flatness, low_end_decay)
from ..tonecheck.cli import a_weight_db

RATE = 48000
W_LTAS, W_BUZZ, W_DECAY, W_STFT, W_REG = 1.0, 0.5, 2.0, 0.25, 0.02
STFT_SIZES = (512, 2048, 8192)
STFT_UNDER_WEIGHT = 0.3
MIN_DECAY_ONSETS = 3

BAND_CENTRES = [c for c in NOMINAL_CENTRES if 80 <= c <= 8000]
_WIN = signal.get_window("hann", NFFT, fftbins=True)
_PSD_SCALE = 1.0 / (RATE * np.sum(_WIN * _WIN))
_FREQS = np.fft.rfftfreq(NFFT, 1.0 / RATE)


def band_edges() -> np.ndarray:
    return np.array([[_exact_centre(c) * 2 ** (-1 / 6), _exact_centre(c) * 2 ** (1 / 6)] for c in BAND_CENTRES])


def band_weights(freqs: np.ndarray = _FREQS) -> np.ndarray:
    """(n_bands, n_bins) fractional integration weights (same as tonecheck.band_power)."""
    df = freqs[1] - freqs[0]
    e = band_edges()
    left = np.maximum(freqs[None, :] - df / 2, e[:, :1])
    right = np.minimum(freqs[None, :] + df / 2, e[:, 1:])
    return np.clip(right - left, 0, None)


BAND_W = band_weights()
A_POWER_W = 10 ** (a_weight_db(np.array(BAND_CENTRES, float)) / 10)


def segment_starts(n: int, mask: np.ndarray | None) -> np.ndarray:
    """Welch segment starts (hop 4096) with >= 80 % active samples; all segments if none pass."""
    starts = np.arange(0, n - NFFT + 1, HOP)
    if mask is not None and len(starts):
        cs = np.concatenate([[0], np.cumsum(mask[:n].astype(np.int64))])
        keep = starts[(cs[starts + NFFT] - cs[starts]) >= 0.8 * NFFT]
        if len(keep):
            return keep
    return starts


def segment_spectra(x: np.ndarray, starts: np.ndarray) -> np.ndarray:
    """Complex rfft of Hann-windowed segments, shape (S, 4097)."""
    idx = starts[:, None] + np.arange(NFFT)[None, :]
    return np.fft.rfft(x[idx] * _WIN[None, :], axis=1)


def psd_from_spectra(X: np.ndarray) -> np.ndarray:
    psd = np.mean(np.abs(X) ** 2, axis=0) * _PSD_SCALE
    psd[1:-1] *= 2.0
    return psd


def band_db_from_psd(psd: np.ndarray) -> np.ndarray:
    return 10 * np.log10(np.maximum(BAND_W @ psd, 1e-30))


def ltas_error(out_db: np.ndarray, ref_db: np.ndarray) -> tuple[float, float]:
    """(A-weighted error dB after offset removal, offset dB = out - ref)."""
    d = np.asarray(out_db) - np.asarray(ref_db)
    off = float(np.sum(A_POWER_W * d) / np.sum(A_POWER_W))
    d = d - off
    return float(np.sqrt(np.sum(A_POWER_W * d * d) / np.sum(A_POWER_W))), off


@dataclass
class Features:
    band_db: np.ndarray
    buzz_db: float
    decay: float | None
    n_onsets: int = 0


def features(x: np.ndarray, starts: np.ndarray, onsets: np.ndarray | None) -> Features:
    X = segment_spectra(x, starts)
    psd = psd_from_spectra(X)
    buzz = 10 * np.log10(max(buzz_flatness(_FREQS, psd), 1e-12))
    decay, n = None, 0
    if onsets is not None and len(onsets) >= MIN_DECAY_ONSETS:
        _, d = low_end_decay(None, x, RATE, onsets=onsets)
        n = d["nMeasured"]
        decay = d["value"] if n >= MIN_DECAY_ONSETS else None
    return Features(band_db_from_psd(psd), float(buzz), decay, n)


def _logmag(x: np.ndarray, n: int) -> np.ndarray:
    f, t, Z = signal.stft(x, RATE, window="hann", nperseg=n, noverlap=n - n // 4, boundary=None, padded=False)
    return f, 20 * np.log10(np.maximum(np.abs(Z), 1e-7))


def stft_loss(out: np.ndarray, ref: np.ndarray, active: np.ndarray | None = None) -> float:
    """Asymmetric multi-resolution log-magnitude error (dB), see module docstring. ``out``/``ref`` aligned."""
    n = min(len(out), len(ref))
    out, ref = out[:n], ref[:n]
    vals = []
    for size in STFT_SIZES:
        if n < size * 2:
            continue
        f, lo = _logmag(out, size)
        _, lr = _logmag(ref, size)
        sel = (f >= 100) & (f <= 8000)
        lo, lr = lo[sel], lr[sel]
        cols = np.ones(lo.shape[1], bool)
        if active is not None:
            hop = size // 4
            centres = np.arange(lo.shape[1]) * hop + size // 2
            cols = active[np.minimum(centres, len(active) - 1)]
            if cols.sum() < 4:
                cols = np.ones(lo.shape[1], bool)
        d = lo[:, cols] - lr[:, cols]
        floor = lr[:, cols].max() - 80.0       # ignore bins >80 dB under the reference peak
        valid = lr[:, cols] > floor
        d = d - np.mean(d[valid])
        e = np.where(d > 0, d, -STFT_UNDER_WEIGHT * d)
        vals.append(float(np.mean(e[valid])))
    return float(np.mean(vals)) if vals else 0.0


@dataclass
class Target:
    """Everything the loss needs about the reference (all at 48 kHz)."""
    starts: np.ndarray                  # Welch segment starts, indices into the (48 kHz) excerpt
    ref: Features
    onsets: np.ndarray | None           # DI onsets (s) in the excerpt timeline, for the output's lowDecay
    active: np.ndarray | None = None    # per-sample bool mask of the excerpt (for STFT frames)
    matched: np.ndarray | None = None   # reference channel aligned to the excerpt (matched pair only)


@dataclass
class LossResult:
    total: float
    ltas: float
    buzz: float
    decay: float | None
    stft: float | None
    reg: float
    offset_db: float
    weights: dict = field(default_factory=lambda: {"ltas": W_LTAS, "buzz": W_BUZZ, "decay": W_DECAY,
                                                    "stft": W_STFT, "reg": W_REG})

    def as_dict(self) -> dict:
        return {k: (None if v is None else (float(v) if not isinstance(v, dict) else v))
                for k, v in self.__dict__.items()}


def evaluate(out: np.ndarray, tgt: Target, eq_gains_db: np.ndarray | None = None) -> LossResult:
    f = features(out, tgt.starts, tgt.onsets)
    ltas, off = ltas_error(f.band_db, tgt.ref.band_db)
    buzz = abs(f.buzz_db - tgt.ref.buzz_db)
    decay = None
    if f.decay is not None and tgt.ref.decay is not None:
        decay = abs(f.decay - tgt.ref.decay)
    stft = stft_loss(out, tgt.matched, tgt.active) if tgt.matched is not None else None
    reg = float(np.sqrt(np.mean(np.square(eq_gains_db)))) if eq_gains_db is not None and len(eq_gains_db) else 0.0
    total = W_LTAS * ltas + W_BUZZ * buzz + W_REG * reg
    if decay is not None:
        total += W_DECAY * decay
    if stft is not None:
        total += W_STFT * stft
    return LossResult(float(total), ltas, buzz, decay, stft, reg, off)
