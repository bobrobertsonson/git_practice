"""Refine a coarse DI->mix offset to roughly +-1 ms (sample resolution, not guaranteed sample-exact for distorted renders) by cross-correlating a rendered DI against the mix channel."""
from __future__ import annotations

import numpy as np
from scipy import ndimage, signal


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


# ---------------------------------------------------------------------------------------------------------------------
# Whole-song placement of a DI that is shorter than the reference (used when --offset-ms is absent).
# ---------------------------------------------------------------------------------------------------------------------
PLACE_FAIL_MESSAGE = "could not place the DI in the song: enter where it starts"
# Coarse envelope frame (200 Hz): 5 min of audio is 60 000 frames. The reference envelope is evaluated at two half-hop
# phases (see ``whole_song_search``), so the coarse lag grid is 2.5 ms and a DI starting between frames loses little.
COARSE_HOP_S = 0.005
COARSE_SMOOTH = 3             # frames of energy smoothing (15 ms) at the coarse stage
FINE_HOP_S = 0.001            # fine envelope frame (1 kHz), searched only +-FINE_SEARCH_S around the coarse peak
FINE_SEARCH_S = 0.020
DETREND_S = 1.0               # coarse envelope: remove a 1 s running mean (keeps onsets/mutes, drops level and sustain)
EXCLUSION_S = 1.0             # a rival peak must be at least this far from the best one
MIN_CONFIDENCE = 4.0          # see ``whole_song_search``
# Second acceptance route for a short, strongly matching DI (see ``placement_accepted``): near-perfect best correlation,
# a rival far below it, and still a clear multiple of the noise spread. All three must hold.
STRONG_R1 = 0.85              # best coarse NCC at least this (chance peaks sit near 4 sigma, ~0.5 for a 12 s DI)
STRONG_MARGIN = 0.40          # r1 - r2 at least this, absolute (NCC units)
STRONG_CONF_FRACTION = 0.75   # and (r1 - r2) / sigma >= this fraction of min_confidence (3.0 at the default)
WINDOW_SLACK_S = 3.0          # a DI at most this much shorter than the reference is covered by the +-3 s window search
MIN_RIVAL_LAGS = 20           # fewest rival lags (any hop) for which a confidence is meaningful


def placement_accepted(r1: float, r2: float, conf: float, min_confidence: float = MIN_CONFIDENCE) -> bool:
    """Accept a placement when ``conf >= min_confidence`` (the relative test), or when it is strong in absolute terms.

    Why the second route: for N roughly independent chance lags the best rival r2 sits about sqrt(2 ln N) ~ 4 sigma above
    zero, so (r1 - r2) / sigma >= 4 needs r1 >= ~8 sigma. NCC cannot exceed 1, so a short DI (large sigma: a 12 s DI has
    ~40 independent envelope segments, sigma ~ 0.12) can fail the relative test while being a flawless placement
    (r1 = 0.95, r2 = 0.49 gives 3.9). Chance cannot produce that: the gap between the top two chance peaks is
    exponential with mean sigma / sqrt(2 ln N) ~ 0.25 sigma, so P(gap >= 3 sigma) ~ exp(-3 * 3.9) ~ 1e-5, and in addition
    the best peak must be >= STRONG_R1 and the gap >= STRONG_MARGIN in absolute terms, which unrelated signals (r1 ~ 0.5)
    and loops (r1 - r2 ~ 0) are nowhere near."""
    if conf >= min_confidence:
        return True
    return bool(r1 >= STRONG_R1 and (r1 - r2) >= STRONG_MARGIN and conf >= STRONG_CONF_FRACTION * min_confidence)


class NonFiniteCorrelationError(RuntimeError):
    """Internal invariant broken: a non-finite envelope or correlation (should be impossible after the energy clamp).
    A RuntimeError, so the CLI maps it to "error: ..." and exit 3."""


class PlacementError(ValueError):
    """The DI could not be placed in the song with enough confidence (message: PLACE_FAIL_MESSAGE).
    ``details`` holds the search numbers (r1, r2, sigma, confidence, minConfidence) for calibration logging, or
    {"nonFinite": True} when the audio contained NaN / inf."""

    def __init__(self, message: str = PLACE_FAIL_MESSAGE, details: dict | None = None):
        super().__init__(message)
        self.details = details or {}


def _log_env(x: np.ndarray, hop: int, detrend_frames: int, smooth_frames: int = 1) -> np.ndarray:
    """Log-compressed, running-mean-removed RMS envelope at ``hop`` samples per frame (frames start at sample 0).
    Log compression makes a clean DI and its distorted/compressed counterpart comparable; subtracting the running mean
    removes overall level and slow sustain so what remains is onsets, mutes and phrasing."""
    n = len(x) // hop
    if n < 2:
        return np.zeros(max(n, 0))
    f = np.ascontiguousarray(x[:n * hop], dtype=np.float32).reshape(n, hop)
    e = np.einsum("ij,ij->i", f, f).astype(np.float64) / hop
    if smooth_frames > 1:
        e = ndimage.uniform_filter1d(e, smooth_frames, mode="nearest")
    # energy is >= 0 by construction, but the running-sum filter can return tiny negatives (cancellation after a loud
    # frame): clamp, otherwise sqrt gives NaN, which poisons the FFT correlation (argmax of NaN is index 0)
    e = np.sqrt(np.maximum(e, 0.0))
    floor = 1e-3 * float(np.sqrt(np.mean(e * e))) + 1e-12      # -60 dB re the signal's own RMS: silence is flat
    le = np.log(e + floor)
    return le - ndimage.uniform_filter1d(le, max(3, detrend_frames), mode="nearest")


def _ncc_valid(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Normalised cross-correlation of the template ``b`` against every window of ``a`` (FFT-based).
    ncc[k] = sum_i (a[k+i] - mean_k) (b[i] - mean_b) / (|a_k - mean_k| |b - mean_b|), k = 0 .. len(a) - len(b);
    values in [-1, 1]."""
    if not (np.all(np.isfinite(a)) and np.all(np.isfinite(b))):
        raise NonFiniteCorrelationError("internal error: non-finite envelope passed to the placement correlation")
    n = len(b)
    a = a - a.mean()                  # mean removal first: the windowed variance below then cancels far less
    b = b - b.mean()
    nb = float(np.sqrt(np.dot(b, b))) + 1e-12
    raw = signal.correlate(a, b, mode="valid", method="fft")
    cs = np.concatenate([[0.0], np.cumsum(a)])
    cs2 = np.concatenate([[0.0], np.cumsum(a * a)])
    s1 = cs[n:] - cs[:-n]
    s2 = cs2[n:] - cs2[:-n]
    var = np.maximum(s2 - s1 * s1 / n, 0.0)
    var = np.maximum(var, 0.1 * float(np.median(var)) + 1e-12)   # near-silent windows must not score high
    out = raw / (nb * np.sqrt(var))
    if not np.all(np.isfinite(out)):
        raise NonFiniteCorrelationError("internal error: non-finite normalised cross-correlation in DI placement")
    return out


def _fine_place(di: np.ndarray, ref: np.ndarray, centre: int, w: int, fh: int) -> tuple[int, bool]:
    """1 ms envelope NCC of ``di`` against ``ref`` for offsets ``centre - w .. centre + w`` (clamped to the song).
    Returns (offset, hit_edge) where hit_edge is True when the best lag sits on a window edge that is not the song's own
    boundary, i.e. the true peak may lie outside the window."""
    last = len(ref) - len(di)
    lo, hi = max(0, centre - w), min(last, centre + w)
    if hi < lo:
        return min(max(centre, 0), last), False
    seg = ref[lo:hi + len(di)]
    det_f = int(round(0.2 / FINE_HOP_S))
    fa, fb = _log_env(seg, fh, det_f, smooth_frames=5), _log_env(di, fh, det_f, smooth_frames=5)
    if len(fb) < 20 or len(fa) <= len(fb):
        return min(max(centre, 0), last), False
    cf = _ncc_valid(fa, fb)
    j = int(np.argmax(cf))
    off = lo + j * fh
    edge = (j == 0 and lo > 0) or (j == len(cf) - 1 and hi < last)
    return off, bool(edge)


def whole_song_search(di: np.ndarray, ref: np.ndarray, fs: int, min_confidence: float = MIN_CONFIDENCE) -> dict:
    """Find where the (shorter) mono ``di`` starts inside the mono ``ref``; both at rate ``fs``.

    Coarse: envelope cross-correlation. Both signals become a log RMS envelope at 200 Hz (15 ms smoothed; the reference at two
    half-hop phases) with a 1 s running mean
    removed (``_log_env``); the DI envelope is slid over every position of the reference envelope with an FFT-based
    normalised cross-correlation (cost O(M log M); 300 s of audio is a 60 000-point transform). Fine: the same at 1 kHz
    within +-20 ms of the coarse peak (re-centred once if the peak lands on the window edge), so the result is good to
    about 1-2 ms on material whose envelopes correspond
    (the downstream ``refine_offset`` then takes it to waveform accuracy on the rendered excerpt).

    **Confidence** = (r1 - r2) / sigma, where r1 is the best coarse NCC value, r2 the best value at least 1 s away from
    that lag (the best *rival* placement) and sigma the standard deviation of the coarse NCC over all lags outside the
    exclusion zone (the spread of chance correlations). It is the winning margin in units of noise. Unrelated signals
    give well under 1 (the gap between the two highest of many noise peaks); a unique placement gives tens. A DI that
    fits two places equally well (a looped riff) gives ~0 and is rejected as ambiguous, which is the correct outcome.
    Threshold MIN_CONFIDENCE = 4.0: for Gaussian-like noise over thousands of lags a noise gap of 4 sigma is
    vanishingly unlikely, while a true placement of a heavily distorted, mixed-in guitar should clear it on anything
    longer than a few seconds (not calibrated on real stems yet; the number is exposed for tuning). A placement that
    misses it is still accepted when it is strong in absolute terms (``placement_accepted``): r1 >= 0.85, r1 - r2 >= 0.40
    and (r1 - r2) / sigma >= 0.75 * min_confidence, which a short but flawless match needs because sigma grows as the DI
    shortens while NCC is capped at 1.
    Returns {offset (samples), offsetMs, coarseMs, confidence, r1, r2, sigma, ok}.
    The coarse lag axis has step ``hop / 2`` (2.5 ms) with two reference phases, or ``hop`` when the hop is odd."""
    if len(di) >= len(ref):
        raise ValueError("whole-song search needs a DI shorter than the reference")
    if not (np.all(np.isfinite(di)) and np.all(np.isfinite(ref))):
        raise PlacementError(PLACE_FAIL_MESSAGE, {"nonFinite": True})   # NaN / inf audio: no honest placement exists
    hop = int(round(COARSE_HOP_S * fs))
    det = int(round(DETREND_S / COARSE_HOP_S))
    b = _log_env(di, hop, det, COARSE_SMOOTH)
    a0 = _log_env(ref, hop, det, COARSE_SMOOTH)
    two_phase = hop % 2 == 0                      # an odd hop has no exact half-hop shift: single phase then
    a1 = _log_env(ref[hop // 2:], hop, det, COARSE_SMOOTH) if two_phase else a0   # frames shifted by half a hop
    if len(b) < 20 or len(a1) <= len(b):
        return {"offset": 0, "offsetMs": 0.0, "coarseMs": 0.0, "confidence": 0.0, "r1": 0.0, "r2": 0.0, "sigma": 0.0,
                "ok": False}
    if two_phase:
        # Two phases interleaved into one lag axis with step hop/2: lag 2k = DI at ref sample k*hop, lag 2k+1 = k*hop+hop/2.
        # A DI starting between frames then matches one phase to within hop/4 (1.25 ms at 5 ms), instead of hop/2.
        c0, c1 = _ncc_valid(a0, b), _ncc_valid(a1, b)
        c = np.empty(len(c0) + len(c1))
        c[0::2], c[1::2] = c0, c1
        step = hop // 2
    else:
        c, step = _ncc_valid(a0, b), hop
    k1 = int(np.argmax(c))
    ex = int(round(EXCLUSION_S * fs / step))
    mask = np.ones(len(c), bool)
    mask[max(0, k1 - ex):k1 + ex + 1] = False
    r1 = float(c[k1])
    if int(mask.sum()) >= MIN_RIVAL_LAGS:
        out = c[mask]
        r2, sigma = float(out.max()), float(out.std()) + 1e-9
        conf = max(0.0, (r1 - r2) / sigma)
    else:
        r2, sigma, conf = 0.0, 0.0, 0.0       # too few alternative positions to judge
    coarse = k1 * step                        # lag axis is k * step: the coarse peak is quantised to step / 2 = 1.25 ms
    # fine stage: 1 ms frames, +-20 ms around the coarse peak (covers the <= 1.25 ms coarse quantisation plus smoothing).
    # Frame convention: frames start at sample 0 of their own signal and both signals use the same hop, smoothing and
    # detrend filters, so lag k * hop is the DI start exactly: no half-hop or filter-delay bias.
    fh = int(round(FINE_HOP_S * fs))
    w = int(round(FINE_SEARCH_S * fs))
    coarse = min(coarse, len(ref) - len(di))
    off, edge = _fine_place(di, ref, coarse, w, fh)
    if edge:                                   # peak on the window edge: re-centre on it once
        off, _ = _fine_place(di, ref, off, w, fh)
    off = min(int(off), len(ref) - len(di))          # never place the DI past the end of the song
    return {"offset": int(off), "offsetMs": 1000.0 * off / fs, "coarseMs": 1000.0 * coarse / fs,
            "confidence": float(conf), "r1": r1, "r2": r2, "sigma": sigma, "ok": placement_accepted(r1, r2, conf, min_confidence)}


def resolve_offset(di: np.ndarray, ref: np.ndarray, fs: int, offset_given: bool, offset_samples: int = 0,
                   min_confidence: float = MIN_CONFIDENCE) -> dict:
    """Decide how the DI's position in the reference is found, and run the whole-song search when it applies.

    * ``offset_given``          -> {"mode": "given"}: trusted (+-20 ms refinement only), no search.
    * DI not shorter than ref (or at most WINDOW_SLACK_S shorter, which the +-3 s window already covers)
                                -> {"mode": "window"}: the +-3 s search around 0, refined later.
    * otherwise                 -> whole-song search; below ``min_confidence`` raises PlacementError.
    The returned dict is the result JSON's ``offset_search``. Keys per mode:
      given       mode, offset_ms, offset_samples (the hint), confidence (null)
      window      mode, offset_ms, offset_samples (0 here), confidence (null)
      whole_song  mode, offset_ms, offset_samples, confidence, coarse_ms, r1, r2, minConfidence, searchSeconds
    Here offset_ms / offset_samples are the search's own answer (1 ms resolution); ``run_match`` overwrites them with the
    value after the starter-render refinement for window and whole_song (``coarse_ms`` keeps the search's answer)."""
    if offset_given:
        return {"mode": "given", "offset_ms": 1000.0 * offset_samples / fs, "confidence": None,
                "offset_samples": int(offset_samples)}
    if len(di) >= len(ref) - int(WINDOW_SLACK_S * fs):
        return {"mode": "window", "offset_ms": 1000.0 * offset_samples / fs, "confidence": None,
                "offset_samples": int(offset_samples)}
    r = whole_song_search(di, ref, fs, min_confidence)
    if not r["ok"]:
        raise PlacementError(PLACE_FAIL_MESSAGE, {"r1": r["r1"], "r2": r["r2"], "sigma": r["sigma"],
                                                  "confidence": r["confidence"], "minConfidence": min_confidence})
    return {"mode": "whole_song", "offset_ms": r["offsetMs"], "confidence": r["confidence"],
            "offset_samples": r["offset"], "coarse_ms": r["coarseMs"], "r1": r["r1"], "r2": r["r2"],
            "minConfidence": min_confidence}
