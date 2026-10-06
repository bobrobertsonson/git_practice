"""Matcher loss (documented weights).

total = W_LTAS * ltas + W_BUZZ * buzz + W_DECAY * decay + W_STFT * stft (matched pairs only) + W_REG * reg + feel

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
* ``tex``    (phase 3.4, stem basis only) high-frequency texture: |flatness 5-10 kHz of output - of reference| (median over
             the active Welch segments; weight 25 per unit) + |level 8-12 kHz re 1-3 kHz difference| (dB, weight 0.12).
             Noise-like fizz has high flatness and a high 8-12 kHz level; real amp+cab guitar is smooth up there.
* ``hf``     (phase 3.4, full-mix fallback basis) the reference then holds cymbals above ~5 kHz: the LTAS bands above
             ``hf_limit_hz`` (4.5 kHz) are ignored by ``ltas`` and replaced by a one-sided ceiling (the render may be darker
             than the reference there, never brighter), plus a one-sided 8-12 kHz level ceiling (weight 0.12 / dB). The
             texture term is not used. The matched-pair STFT term is likewise limited to ``stft_fmax`` (a mix channel).
* ``feel``   (v0.4M) how the tone behaves, not its average spectrum (``matcher/feel.py`` has the exact definitions):
             ``feel = W_TIGHT * tight + W_FIZZ * fizz + W_POLISH * polish`` with initial weights 0.5 / 0.5 / 0.25 (tuned in
             Task D). ``tight``: per-note 60-250 Hz decay time (t12) and sustain after the DI's palm-muted chugs, one-sided
             (floppier than the reference counts fully, tighter half), t12 / 20 ms + sustain / 3 dB. ``fizz``: per-frame
             5-12 kHz re 1-4 kHz level, 5-10 kHz flatness and 5-12 kHz envelope modulation, W1 distance of the
             distributions / (1.5 dB, 0.03, 0.1); off when the reference HF is not usable (full-mix basis, HF limit set, or
             a matched channel that is a full mix; see ``Reference.clean``). ``polish``: W1 of the spectral flux
             (/ 0.5 dB) and of the per-400 ms crest (/ 1.5 dB), plus the inter-note floor re the active level, one-sided
             (/ 6 dB). Matched pair: note by note against the aligned reference; otherwise the reference's own
             features are compared as distributions, every weight x 0.5 and ``floor`` dropped. All gain invariant. Terms
             with too little data (< 3 notes, < 100 ms of gaps, ...) are dropped and recorded in ``feelTerms.dropped``. A reference
             that is a full mix (matched channel not clean / not a stem) switches off fizz, tightness, flux, crest and floor;
             the floor also needs a clean track (see ``reference.feel_*_state``). The Occam margins in ``run.choose`` (0.1 dB,
             0.05 dB) now apply to a total that includes this dimensionless term; to be revisited in D.1.
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace

import numpy as np
from scipy import signal

from ..tonecheck.analysis import (NFFT, HOP, NOMINAL_CENTRES, _exact_centre, buzz_flatness, low_end_decay)
from ..tonecheck.cli import a_weight_db
from . import feel as _feel

RATE = 48000
W_LTAS, W_BUZZ, W_DECAY, W_STFT, W_REG = 1.0, 0.5, 2.0, 0.25, 0.02
W_FLAT, W_HF = 25.0, 0.12
W_TIGHT, W_FIZZ, W_POLISH = _feel.W_TIGHT, _feel.W_FIZZ, _feel.W_POLISH
HF_LIMIT_HZ = 4500.0           # full-mix fallback reference: LTAS above this is a one-sided ceiling, not a target
STFT_FMAX_MIX = 4500.0         # matched-pair STFT upper edge when the matched channel is a full mix (cymbals above)
TEX_FLAT_BAND = (5000.0, 10000.0)
TEX_HF_BAND = (8000.0, 12000.0)
TEX_REF_BAND = (1000.0, 3000.0)
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


BAND_UPPER = band_edges()[:, 1]


def ltas_error(out_db: np.ndarray, ref_db: np.ndarray, hf_limit_hz: float | None = None) -> tuple[float, float]:
    """(A-weighted error dB after offset removal, offset dB = out - ref).

    ``hf_limit_hz``: bands whose upper edge is above it are not fitted; the offset comes from the fitted bands and the
    ignored bands only count when the output is above the reference (one-sided ceiling)."""
    d = np.asarray(out_db) - np.asarray(ref_db)
    w = A_POWER_W
    if hf_limit_hz is None:
        off = float(np.sum(w * d) / np.sum(w))
        d = d - off
        return float(np.sqrt(np.sum(w * d * d) / np.sum(w))), off
    keep = BAND_UPPER <= hf_limit_hz
    if not keep.any():
        keep = np.ones_like(keep)
    off = float(np.sum(w[keep] * d[keep]) / np.sum(w[keep]))
    d = d - off
    d = np.where(keep, d, np.maximum(d, 0.0))
    return float(np.sqrt(np.sum(w * d * d) / np.sum(w))), off


@dataclass
class Features:
    band_db: np.ndarray
    buzz_db: float
    decay: float | None
    n_onsets: int = 0
    flat: float = 0.0          # median spectral flatness 5-10 kHz over the active segments
    hf_db: float = 0.0         # 8-12 kHz level re 1-3 kHz (dB, mean PSD of the active segments)


def hf_texture(X: np.ndarray) -> tuple[float, float]:
    """(median flatness 5-10 kHz over segments, 8-12 kHz level re 1-3 kHz in dB) from segment spectra (S, 4097)."""
    P = np.abs(X) ** 2
    sel = (_FREQS >= TEX_FLAT_BAND[0]) & (_FREQS < TEX_FLAT_BAND[1])
    B = P[:, sel] + 1e-30
    flat = float(np.median(np.exp(np.log(B).mean(axis=1)) / B.mean(axis=1)))

    def lvl(lo, hi):
        return float(np.sum(P[:, (_FREQS >= lo) & (_FREQS < hi)]))
    hf = 10 * np.log10(max(lvl(*TEX_HF_BAND), 1e-30) / max(lvl(*TEX_REF_BAND), 1e-30))
    return flat, float(hf)


def features(x: np.ndarray, starts: np.ndarray, onsets: np.ndarray | None) -> Features:
    X = segment_spectra(x, starts)
    psd = psd_from_spectra(X)
    buzz = 10 * np.log10(max(buzz_flatness(_FREQS, psd), 1e-12))
    decay, n = None, 0
    if onsets is not None and len(onsets) >= MIN_DECAY_ONSETS:
        _, d = low_end_decay(None, x, RATE, onsets=onsets)
        n = d["nMeasured"]
        decay = d["value"] if n >= MIN_DECAY_ONSETS else None
    flat, hf = hf_texture(X)
    return Features(band_db_from_psd(psd), float(buzz), decay, n, flat, hf)


def _logmag(x: np.ndarray, n: int) -> np.ndarray:
    f, t, Z = signal.stft(x, RATE, window="hann", nperseg=n, noverlap=n - n // 4, boundary=None, padded=False)
    return f, 20 * np.log10(np.maximum(np.abs(Z), 1e-7))


def stft_loss(out: np.ndarray, ref: np.ndarray, active: np.ndarray | None = None, fmax: float = 8000.0,
              cache: dict | None = None) -> float:
    """Asymmetric multi-resolution log-magnitude error (dB), see module docstring. ``out``/``ref`` aligned.
    ``cache`` (a dict owned by one Target: the reference, ``active`` and ``fmax`` are fixed for it) keeps the reference
    side per FFT size (selected log-magnitudes, valid mask, column mask); the result is bit-identical to the uncached call."""
    n = min(len(out), len(ref))
    out, ref = out[:n], ref[:n]
    vals = []
    for size in STFT_SIZES:
        if n < size * 2:
            continue
        key = (size, n)
        hit = None if cache is None else cache.get(key)
        f, lo = _logmag(out, size)
        if hit is None:
            _, lr = _logmag(ref, size)
            sel = (f >= 100) & (f <= fmax)
            lr = lr[sel]
            cols = np.ones(lr.shape[1], bool)
            if active is not None:
                hop = size // 4
                centres = np.arange(lr.shape[1]) * hop + size // 2
                cols = active[np.minimum(centres, len(active) - 1)]
                if cols.sum() < 4:
                    cols = np.ones(lr.shape[1], bool)
            lrc = lr[:, cols]
            valid = lrc > (lrc.max() - 80.0)     # ignore bins >80 dB under the reference peak
            hit = (sel, cols, lrc, valid)
            if cache is not None:
                cache[key] = hit
        sel, cols, lrc, valid = hit
        d = lo[sel][:, cols] - lrc
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
    texture: bool = False               # stem basis: HF texture term (flatness 5-10 kHz, 8-12 kHz level) is a target
    hf_limit_hz: float | None = None    # full-mix basis: LTAS above this is a one-sided ceiling (and 8-12 kHz too)
    stft_fmax: float = 8000.0
    feel: "_feel.FeelTarget | None" = None   # v0.4M feel features of the reference (None: no feel term)
    stft_cache: dict = field(default_factory=dict, compare=False, repr=False)   # reference side of stft_loss, per size


def without_feel(tgt: Target) -> Target:
    """The same target without the feel term (LTAS-led objective; shares the STFT cache)."""
    return tgt if tgt.feel is None else replace(tgt, feel=None)


@dataclass
class LossResult:
    total: float
    ltas: float
    buzz: float
    decay: float | None
    stft: float | None
    reg: float
    offset_db: float
    tex: float = 0.0           # texture term (stem basis) or one-sided HF ceiling (full-mix fallback), weighted units
    weights: dict = field(default_factory=lambda: {"ltas": W_LTAS, "buzz": W_BUZZ, "decay": W_DECAY,
                                                    "stft": W_STFT, "reg": W_REG,
                                                    "texFlat": W_FLAT, "texHf": W_HF,
                                                    "feelTight": W_TIGHT, "feelFizz": W_FIZZ, "feelPolish": W_POLISH})
    feel: float = 0.0          # weighted feel term (included in ``total``)
    feel_terms: dict | None = None   # every feel sub-term (raw + normalised), note counts, noteSet, dropped terms

    @property
    def feelTerms(self) -> dict | None:
        return self.feel_terms

    def as_dict(self) -> dict:
        d = {k: (None if v is None else (float(v) if not isinstance(v, dict) else v))
             for k, v in self.__dict__.items()}
        d["feelTerms"] = d.pop("feel_terms")
        if d.get("feel") is not None and not np.isfinite(d["feel"]):
            d["feel"] = None                  # non-finite render: null in result.json (total is inf as before)
        return d


def evaluate(out: np.ndarray, tgt: Target, eq_gains_db: np.ndarray | None = None) -> LossResult:
    f = features(out, tgt.starts, tgt.onsets)
    ltas, off = ltas_error(f.band_db, tgt.ref.band_db, tgt.hf_limit_hz)
    buzz = abs(f.buzz_db - tgt.ref.buzz_db)
    decay = None
    if f.decay is not None and tgt.ref.decay is not None:
        decay = abs(f.decay - tgt.ref.decay)
    stft = stft_loss(out, tgt.matched, tgt.active, tgt.stft_fmax, tgt.stft_cache) if tgt.matched is not None else None
    tex = 0.0
    if tgt.texture:
        tex = W_FLAT * abs(f.flat - tgt.ref.flat) + W_HF * abs(f.hf_db - tgt.ref.hf_db)
    elif tgt.hf_limit_hz is not None:
        tex = W_HF * max(0.0, f.hf_db - tgt.ref.hf_db)
    reg = float(np.sqrt(np.mean(np.square(eq_gains_db)))) if eq_gains_db is not None and len(eq_gains_db) else 0.0
    feel, feel_terms = 0.0, None
    if tgt.feel is not None:
        feel, feel_terms = _feel.evaluate(out, tgt.feel)
    total = W_LTAS * ltas + W_BUZZ * buzz + W_REG * reg + tex + feel
    if decay is not None:
        total += W_DECAY * decay
    if stft is not None:
        total += W_STFT * stft
    if not np.isfinite(total):
        total = np.inf          # non-finite renders are never selected
    return LossResult(float(total), ltas, buzz, decay, stft, reg, off, float(tex), feel=float(feel),
                      feel_terms=feel_terms)
