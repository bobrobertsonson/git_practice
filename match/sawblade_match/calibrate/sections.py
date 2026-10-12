"""Method 2: guitar-dominant section selection (frame masks) for full mixes.

All masks are per 50 ms frame (the tonecheck activity-gate frame, 2400 samples at 48 kHz); ``True`` = frame
is kept for the guitar LTAS.

Heuristics (documented per the spec; all deterministic, no random processes)
---------------------------------------------------------------------------
Drum transients (both references)
    STFT 4096 / hop 480 (10 ms) on the mix. Band power in the *kick* band (50-120 Hz) and the *snare* band
    (180-250 Hz) in dB; a hit is a rise ``P[t] - P[t-5 hops]`` that exceeds max(``DRUM_MIN_RISE_DB``,
    median + ``DRUM_MAD_K`` x 1.4826 MAD) of that band's rise distribution. Every 50 ms frame overlapping
    ``[t - DRUM_PRE_S, t + DRUM_POST_S]`` (window smear before, kick decay after) is excluded.
    Caveat: palm-mute chugs also raise 50-120 Hz, so some guitar transients are excluded with the drums
    (their sustain frames are kept); a hit hidden under a guitar onset of equal size is not seen.

Cover (``cover_guitar_frames``)
    A frame is kept when (the Guitar_L DI **or** the Guitar_R DI is active there) **and** the mix's
    2-5 kHz band envelope correlates with that DI's envelope (Pearson, ``CORR_WINDOW_FRAMES`` = 1 s window,
    >= ``CORR_MIN``), after aligning the DI inside the mix (offset refined around the +190 / +175 ms priors by
    envelope cross-correlation), and it contains no drum transient. The DI is the clean guitar, the mix's 2-5 kHz
    band is where the distorted guitar dominates; kick/bass carry no 2-5 kHz envelope that tracks the DI.
    (The spec says "rendered-DI envelope"; the raw DI 300 Hz-4 kHz envelope is used so no render/preset
    is needed.)

Original (``original_guitar_frames``)
    No guitar-only signal is available, so frames are kept when they are active, contain no drum
    transient, and have low *vocal likelihood*: a 300 Hz-3 kHz harmonic-structure estimate (autocorrelation of
    the log-magnitude spectrum -> f0 in 80-400 Hz with salience) that holds a continuous, slowly moving pitch
    (|d f0| < 1.5 semitones per frame over >= 4 frames with a vibrato-like wobble) is flagged as voice.
    **This is not reliable for death-metal vocals** (growls are largely aperiodic noise, and high-gain
    guitar chords are also pitched), so the original's frame choice is a *hypothesis* - the report lists the
    chosen time ranges and the spread between 10 s chunks, and ``--sections a:b,c:d`` replaces the heuristic
    with user-chosen ranges.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import signal

from ..tonecheck.analysis import ANALYSIS_RATE, FRAME_S, activity_mask

FS = ANALYSIS_RATE
FRAME_N = int(round(FRAME_S * FS))          # 2400

DRUM_BANDS = {"kick": (50.0, 120.0), "snare": (180.0, 250.0)}
DRUM_MIN_RISE_DB = 9.0
DRUM_MAD_K = 3.0
DRUM_PRE_S = 0.050
DRUM_POST_S = 0.200
CORR_WINDOW_FRAMES = 21
CORR_MIN = 0.35
DI_ACTIVE_BELOW_P95_DB = 20.0
DI_ACTIVE_ABOVE_FLOOR_DB = 6.0
ENV_HOP_N = 240                              # 5 ms hop for offset refinement
OFFSET_PRIOR_S = {"L": 0.190, "R": 0.175}
OFFSET_SEARCH_S = 0.040


@dataclass
class Selection:
    """Frame selection result. ``mask`` is per 50 ms frame; ``reasons`` are the exclusion masks."""
    mask: np.ndarray
    reasons: dict[str, np.ndarray] = field(default_factory=dict)
    info: dict = field(default_factory=dict)

    @property
    def fraction(self) -> float:
        return float(self.mask.mean()) if self.mask.size else 0.0

    def sample_mask(self, n: int) -> np.ndarray:
        m = np.zeros(n, dtype=bool)
        k = min(len(self.mask), n // FRAME_N)
        m[: k * FRAME_N] = np.repeat(self.mask[:k], FRAME_N)
        return m

    def ranges(self, min_s: float = 0.0) -> list[tuple[float, float]]:
        """Selected time ranges (seconds), merged consecutive frames, optionally dropping short ones."""
        out, start = [], None
        for i, v in enumerate(list(self.mask) + [False]):
            if v and start is None:
                start = i
            elif not v and start is not None:
                if (i - start) * FRAME_S >= min_s:
                    out.append((start * FRAME_S, i * FRAME_S))
                start = None
        return out


# --- shared helpers --------------------------------------------------------------------------------
def band_envelope_db(x: np.ndarray, lo: float, hi: float, hop: int = FRAME_N, fs: int = FS) -> np.ndarray:
    """Band-passed (Butterworth 4) mean-square level in dB per non-overlapping ``hop`` frame."""
    sos = signal.butter(4, (lo, hi), btype="bandpass", fs=fs, output="sos")
    y = signal.sosfilt(sos, x)
    nf = len(y) // hop
    return 10 * np.log10(np.mean(y[: nf * hop].reshape(nf, hop) ** 2, axis=1) + 1e-12)


def _frames_from_times(times: np.ndarray, nf: int, pre: float, post: float) -> np.ndarray:
    out = np.zeros(nf, dtype=bool)
    for t in times:
        a = max(0, int(np.floor((t - pre) / FRAME_S)))
        b = min(nf, int(np.floor((t + post) / FRAME_S)) + 1)
        out[a:b] = True
    return out


def drum_hit_times(x: np.ndarray, fs: int = FS) -> dict[str, np.ndarray]:
    """Times (s) of kick / snare transients from rises of the 50-120 / 180-250 Hz band power (see module doc)."""
    nper, hop = 4096, 480
    f, t, Z = signal.stft(x, fs, window="hann", nperseg=nper, noverlap=nper - hop, boundary=None, padded=False)
    p = np.abs(Z) ** 2
    out = {}
    lag = 5
    for name, (lo, hi) in DRUM_BANDS.items():
        sel = (f >= lo) & (f <= hi)
        pb = 10 * np.log10(p[sel].sum(axis=0) + 1e-20)
        d = np.concatenate([np.zeros(lag), pb[lag:] - pb[:-lag]])
        med = np.median(d)
        thr = max(DRUM_MIN_RISE_DB, med + DRUM_MAD_K * 1.4826 * np.median(np.abs(d - med)))
        pk, _ = signal.find_peaks(d, height=thr, distance=3)
        out[name] = t[pk]
    return out


def drum_frames(x: np.ndarray, nf: int, fs: int = FS) -> tuple[np.ndarray, dict[str, np.ndarray]]:
    hits = drum_hit_times(x, fs)
    m = np.zeros(nf, dtype=bool)
    for ts in hits.values():
        m |= _frames_from_times(ts, nf, DRUM_PRE_S, DRUM_POST_S)
    return m, hits


# --- cover -----------------------------------------------------------------------------------------
def refine_offset(di_env: np.ndarray, mix_env: np.ndarray, prior_s: float) -> float:
    """DI start inside the mix (s): best Pearson correlation of 5 ms-hop envelopes within +-40 ms of ``prior_s``."""
    hop_s = ENV_HOP_N / FS
    best, best_c = prior_s, -2.0
    for k in range(-int(OFFSET_SEARCH_S / hop_s), int(OFFSET_SEARCH_S / hop_s) + 1):
        off = int(round(prior_s / hop_s)) + k
        if off < 0:
            continue
        a = mix_env[off: off + len(di_env)]
        n = min(len(a), len(di_env))
        if n < 100:
            continue
        c = np.corrcoef(a[:n], di_env[:n])[0, 1]
        if c > best_c:
            best_c, best = c, off * hop_s
    return float(best)


def rolling_corr(a: np.ndarray, b: np.ndarray, win: int) -> np.ndarray:
    """Pearson correlation of a and b over a centred sliding window (NaN-free; flat windows give 0)."""
    n = len(a)
    out = np.zeros(n)
    h = win // 2
    for i in range(n):
        lo, hi = max(0, i - h), min(n, i + h + 1)
        x, y = a[lo:hi], b[lo:hi]
        sx, sy = x.std(), y.std()
        out[i] = 0.0 if sx < 1e-9 or sy < 1e-9 else float(np.mean((x - x.mean()) * (y - y.mean())) / (sx * sy))
    return out


def di_active_frames(di: np.ndarray, fs: int = FS) -> np.ndarray:
    """DI frames carrying played notes: within 20 dB of the 95th-percentile frame and >= 6 dB above the
    DI's 5th-percentile (noise-floor) frame."""
    lv = band_envelope_db(di, 20.0, 20000.0, FRAME_N, fs)
    return lv >= max(np.percentile(lv, 95) - DI_ACTIVE_BELOW_P95_DB, np.percentile(lv, 5) + DI_ACTIVE_ABOVE_FLOOR_DB)


def cover_guitar_frames(mix_l: np.ndarray, mix_r: np.ndarray, di_l: np.ndarray, di_r: np.ndarray,
                        fs: int = FS, offsets_s: dict[str, float] | None = None) -> Selection:
    """Frame selection for the cover mix (all inputs mono at 48 kHz). See the module doc for the rule."""
    nf = min(len(mix_l), len(mix_r)) // FRAME_N
    mid = 0.5 * (mix_l[: nf * FRAME_N] + mix_r[: nf * FRAME_N])
    drums, hits = drum_frames(mid, nf, fs)
    keep_any = np.zeros(nf, dtype=bool)
    info: dict = {"offsetsS": {}, "perDi": {}}
    for tag, mix, di in (("L", mix_l, di_l), ("R", mix_r, di_r)):
        env_m = band_envelope_db(mix, 2000.0, 5000.0, ENV_HOP_N, fs)
        env_d = band_envelope_db(di, 300.0, 4000.0, ENV_HOP_N, fs)
        off = (offsets_s or {}).get(tag) or refine_offset(env_d, env_m, OFFSET_PRIOR_S[tag])
        info["offsetsS"][tag] = off
        o = int(round(off * fs))
        # DI frame i (50 ms) starts at mix sample o + i*FRAME_N; compute both envelopes on that grid
        n_di = min(len(di) // FRAME_N, (len(mix) - o) // FRAME_N)
        em = band_envelope_db(mix[o: o + n_di * FRAME_N], 2000.0, 5000.0, FRAME_N, fs)
        ed = band_envelope_db(di[: n_di * FRAME_N], 300.0, 4000.0, FRAME_N, fs)
        corr = rolling_corr(em, ed, CORR_WINDOW_FRAMES)
        active = di_active_frames(di[: n_di * FRAME_N], fs)
        ok_di = active & (corr >= CORR_MIN)
        full = np.zeros(nf, dtype=bool)          # back to mix frame grid (mix frame j starts at j*FRAME_N)
        shift = o // FRAME_N                       # frame shift; residual (< 50 ms) is below the frame size
        for i in range(n_di):
            j = i + shift
            if 0 <= j < nf and ok_di[i]:
                full[j] = True
        keep_any |= full
        info["perDi"][tag] = {"activeFraction": float(active.mean()), "corrMedian": float(np.median(corr)),
                              "frames": int(full.sum())}
    mask = keep_any & ~drums
    info["drumHits"] = {k: int(len(v)) for k, v in hits.items()}
    info["drumHitTimesS"] = {k: v.tolist() for k, v in hits.items()}
    return Selection(mask, {"notGuitarDI": ~keep_any, "drum": drums}, info)


# --- original --------------------------------------------------------------------------------------
VOICE_F0_RANGE = (80.0, 400.0)
VOICE_SALIENCE_MIN = 0.28
VOICE_MAX_JUMP_ST = 1.5
VOICE_MIN_RUN = 4
VOICE_MIN_WOBBLE_ST = 0.05


def frame_f0_salience(x: np.ndarray, fs: int = FS, nper: int = 4096) -> tuple[np.ndarray, np.ndarray]:
    """Per 50 ms frame: (f0 Hz, salience 0..1) from the autocorrelation of the log-magnitude spectrum in
    300 Hz-3 kHz. A harmonic series with spacing f0 gives a peak at lag f0 (in Hz); salience is that
    normalised autocorrelation peak, searched in 80-400 Hz."""
    nf = len(x) // FRAME_N
    w = signal.get_window("hann", nper)
    f = np.fft.rfftfreq(nper, 1 / fs)
    df = f[1] - f[0]
    sel = (f >= 300) & (f <= 3000)
    lo_lag, hi_lag = int(VOICE_F0_RANGE[0] / df), int(VOICE_F0_RANGE[1] / df)
    f0 = np.zeros(nf)
    sal = np.zeros(nf)
    for i in range(nf):
        c = i * FRAME_N + FRAME_N // 2
        a = c - nper // 2
        if a < 0 or a + nper > len(x):
            continue
        spec = np.abs(np.fft.rfft(x[a: a + nper] * w))[sel]
        ls = np.log(spec + 1e-9)
        ls = ls - np.convolve(ls, np.ones(25) / 25, mode="same")      # remove the spectral envelope
        ac = np.correlate(ls, ls, mode="full")[len(ls) - 1:]
        ac = ac / (ac[0] + 1e-12)
        seg = ac[lo_lag: hi_lag + 1]
        k = int(np.argmax(seg))
        sal[i] = seg[k]
        f0[i] = (lo_lag + k) * df
    return f0, sal


def vocal_frames(x: np.ndarray, fs: int = FS) -> tuple[np.ndarray, dict]:
    """Frames flagged as voice: salient f0 forming a continuous (>= 4 frames, < 1.5 semitone jumps), slightly
    moving (vibrato-like wobble) track. Constant-pitch runs are *not* flagged (that is a sustained guitar
    chord/note); see the module doc for why this is unreliable on growled vocals."""
    f0, sal = frame_f0_salience(x, fs)
    voiced = sal >= VOICE_SALIENCE_MIN
    voiced[1:-1] |= voiced[:-2] & voiced[2:]          # bridge single-frame dropouts
    st = 12 * np.log2(np.maximum(f0, 1.0) / 100.0)
    nf = len(f0)
    out = np.zeros(nf, dtype=bool)
    i = 0
    while i < nf:
        if not voiced[i]:
            i += 1
            continue
        j = i
        while j + 1 < nf and voiced[j + 1] and abs(st[j + 1] - st[j]) < VOICE_MAX_JUMP_ST:
            j += 1
        if j - i + 1 >= VOICE_MIN_RUN and np.std(st[i: j + 1]) >= VOICE_MIN_WOBBLE_ST:
            out[i: j + 1] = True
        i = j + 1
    return out, {"f0Hz": f0, "salience": sal, "voicedFraction": float(voiced.mean())}


def parse_ranges(text: str) -> list[tuple[float, float]]:
    """``"0:12,95:110"`` -> [(0.0, 12.0), (95.0, 110.0)] (seconds)."""
    out = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        a, _, b = part.partition(":")
        lo, hi = float(a), float(b)
        if not (0 <= lo < hi):
            raise ValueError(f"bad section range {part!r}")
        out.append((lo, hi))
    if not out:
        raise ValueError("no section ranges given")
    return out


def ranges_to_frames(ranges: list[tuple[float, float]], nf: int) -> np.ndarray:
    m = np.zeros(nf, dtype=bool)
    for lo, hi in ranges:
        m[int(np.ceil(lo / FRAME_S - 1e-9)): min(nf, int(np.floor(hi / FRAME_S + 1e-9)))] = True
    return m


def original_guitar_frames(x: np.ndarray, fs: int = FS,
                           ranges: list[tuple[float, float]] | None = None) -> Selection:
    """Frame selection for the original (mid, 48 kHz). With ``ranges`` (user sections) those ranges are used
    as given, intersected only with the activity gate; otherwise the drum / vocal heuristic applies."""
    nf = len(x) // FRAME_N
    _, active, _ = activity_mask(x, fs)
    active = active[:nf]
    if ranges is not None:
        user = ranges_to_frames(ranges, nf)
        return Selection(user & active, {"outsideUserRanges": ~user, "inactive": ~active},
                         {"mode": "user-sections", "ranges": ranges})
    drums, hits = drum_frames(x[: nf * FRAME_N], nf, fs)
    voice, vinfo = vocal_frames(x[: nf * FRAME_N], fs)
    mask = active & ~drums & ~voice
    return Selection(mask, {"inactive": ~active, "drum": drums, "vocalLike": voice},
                     {"mode": "heuristic", "drumHits": {k: int(len(v)) for k, v in hits.items()},
                      "voicedFraction": vinfo["voicedFraction"], "reliable": False})
