"""Feel features for the matcher loss (phase v0.4M, Task A): low-end tightness, fizz, polish.

The LTAS loss finds the average spectrum, not how a tone *behaves*: how fast the low end stops after a palm-muted
chug, whether the top is noise-like fizz, whether the signal is smooth and controlled between the notes. These
features compare a render against the reference on the same excerpt and the same active mask (48 kHz, pure
numpy + ``scipy.signal.butter/sosfilt``; the DI-derived inputs - onsets, gap regions, active mask - are passed in).

Every feature is level-invariant (dB differences, ratios, flatness, coefficient of variation, crest, level re the
active power), so output gain stays a free parameter as in the rest of the loss.

A.1 tightness (60-250 Hz, per note).  4th-order Butterworth band-pass (``butter(4, [60, 250])``), power over 5 ms
    frames with a 2.5 ms hop, in dB. Notes = DI onsets; note window = onset to the next onset, capped at 400 ms.
    A note *counts* if its window is >= 80 ms and its low-band peak (first 30 ms) is within 30 dB of the loudest
    note's peak. Per note: ``t12`` = ms from the peak until the envelope first falls 12 dB (censored at the window
    end), ``sustainDb`` = mean power in [peak + 40, peak + 120 ms] (clipped to the window) re the peak power.
    *Chugs* = counting notes whose DI low-band t12 <= 150 ms and not censored (palm-muted; a note cut by the next
    onset while still ringing is not a chug); used if >= 3, else all counting notes (``noteSet``); < 3 notes: dropped.
    Selection is made on the DI so that every candidate is scored on the same notes.
    ``tight = median(asym(t12_out - t12_ref)) / 20 ms + median(asym(sustain_out - sustain_ref)) / 3 dB`` with
    ``asym(d) = d`` for d > 0 (floppier than the reference) and ``0.5 |d|`` otherwise.
A.2 fizz (per 2048-pt Hann frame, hop 1024, frames >= 80 % active, compared as distributions).
    ``hfRatioDb`` = 10 log10(E[5-12 kHz] / E[1-4 kHz]); ``hfFlat`` = spectral flatness 5-10 kHz; ``hfMod`` =
    coefficient of variation of the 5-12 kHz Hilbert envelope (low-passed at 1 kHz) inside the frame. Distance per
    feature = 1-D Wasserstein W1 estimated as the mean |quantile difference| at q = 0.05 ... 0.95;
    ``fizz = W1(hfRatioDb) / 1.5 dB + W1(hfFlat) / 0.03 + W1(hfMod) / 0.1``. Off when the reference HF is not usable
    (the caller says so: full-mix reference, HF limit set).
A.3 polish.  ``flux`` = per-frame mean |delta dB| between consecutive 1024-pt log-magnitude frames (hop 512), bins
    300 Hz-8 kHz, active frames; W1 / 0.5 dB. ``crest`` = peak/RMS (dB) per 400 ms active window (hop 200 ms); W1 /
    1.5 dB. ``floor`` = output power in the DI gap regions re its active power (dB), same for the reference, one-sided
    ``max(0, d) / 6 + 0.25 max(0, -d) / 6`` (d = out - ref); dropped with < 100 ms of gaps.
    ``polish = flux + crest + floor``.

``feel = W_TIGHT * tight + W_FIZZ * fizz + W_POLISH * polish`` (0.5 / 0.5 / 0.25; tuned in Task D). Without a matched
pair (``mode == "soft"``) the reference's own features (its onsets, its chugs, its frames) are compared to the
render's as W1 of the per-note / per-frame distributions, ``floor`` is dropped (a mix has no clean gaps) and every
weight is scaled by 0.5.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import signal

RATE = 48000
W_TIGHT, W_FIZZ, W_POLISH = 0.5, 0.5, 0.25
SOFT_SCALE = 0.5

# ---- A.1 ---------------------------------------------------------------------------------------------------------
TIGHT_BAND = (60.0, 250.0)
ENV_SIZE = 240                  # 5 ms
ENV_HOP = 120                   # 2.5 ms
HOP_MS = 1000.0 * ENV_HOP / RATE
NOTE_MAX_S = 0.4
NOTE_MIN_MS = 80.0
NOTE_PEAK_S = 0.030
NOTE_RANGE_DB = 30.0
CHUG_T12_MS = 150.0
MIN_NOTES = 3
T12_DROP_DB = 12.0
SUS_FROM_FRAMES = 16            # +40 ms
SUS_TO_FRAMES = 48              # +120 ms
SUS_CLIP_DB = (-60.0, 6.0)
T12_NORM_MS, SUS_NORM_DB = 20.0, 3.0
ASYM_UNDER = 0.5

# ---- A.2 ---------------------------------------------------------------------------------------------------------
FIZZ_N, FIZZ_HOP = 2048, 1024
HF_BAND = (5000.0, 12000.0)
MID_BAND = (1000.0, 4000.0)
FLAT_BAND = (5000.0, 10000.0)
ENV_LP_HZ = 1000.0
ACTIVE_FRACTION = 0.8
Q_LEVELS = np.arange(1, 20) / 20.0
RATIO_NORM_DB, FLAT_NORM, MOD_NORM = 1.5, 0.03, 0.1

# ---- A.3 ---------------------------------------------------------------------------------------------------------
FLUX_N, FLUX_HOP = 1024, 512
FLUX_BAND = (300.0, 8000.0)
FLUX_NORM_DB = 0.5
CREST_WIN, CREST_HOP = int(0.4 * RATE), int(0.2 * RATE)
CREST_NORM_DB = 1.5
FLOOR_NORM_DB = 6.0
FLOOR_UNDER = 0.25
FLOOR_MIN_S = 0.100
LEVEL_FLOOR_DB = -90.0
MIN_CREST_WINDOWS = 3
MIN_FRAMES = 3

_BATCH = 256
_SOS_LOW = signal.butter(4, TIGHT_BAND, btype="bandpass", fs=RATE, output="sos")
_SOS_HF = signal.butter(4, HF_BAND, btype="bandpass", fs=RATE, output="sos")
_SOS_ENV = signal.butter(2, ENV_LP_HZ, btype="lowpass", fs=RATE, output="sos")


def _hann(n: int) -> np.ndarray:
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)       # periodic Hann (scipy fftbins=True)


_WIN_FIZZ = _hann(FIZZ_N)
_WIN_FLUX = _hann(FLUX_N)
_F_FIZZ = np.fft.rfftfreq(FIZZ_N, 1.0 / RATE)
_SEL_HF = (_F_FIZZ >= HF_BAND[0]) & (_F_FIZZ < HF_BAND[1])
_SEL_MID = (_F_FIZZ >= MID_BAND[0]) & (_F_FIZZ < MID_BAND[1])
_SEL_FLAT = (_F_FIZZ >= FLAT_BAND[0]) & (_F_FIZZ < FLAT_BAND[1])
_F_FLUX = np.fft.rfftfreq(FLUX_N, 1.0 / RATE)
_SEL_FLUX = (_F_FLUX >= FLUX_BAND[0]) & (_F_FLUX <= FLUX_BAND[1])


def w1(a: np.ndarray, b: np.ndarray) -> float | None:
    """1-D Wasserstein distance estimated as the mean |quantile difference| at q = 0.05 ... 0.95."""
    if len(a) == 0 or len(b) == 0:
        return None
    return float(np.mean(np.abs(np.quantile(a, Q_LEVELS) - np.quantile(b, Q_LEVELS))))


SOFT_DELTA = 1.0


def huber(x: float) -> float:
    """Huber-style transform of a normalised (>= 0) sub-term: quadratic below ``SOFT_DELTA`` (a mismatch under about one
    normaliser is within the noise of these statistics and must not pull the search against the spectral fit: its slope
    vanishes at 0), linear above (a real mismatch keeps a constant pull). Continuous, monotone, huber(0) = 0."""
    x = abs(float(x))
    return x * x / (2.0 * SOFT_DELTA) if x < SOFT_DELTA else x - 0.5 * SOFT_DELTA


def asym(d: np.ndarray) -> np.ndarray:
    """One-sided: positive d (output floppier / more sustained than the reference) counts fully, negative 0.5x."""
    return np.where(d > 0, d, ASYM_UNDER * np.abs(d))


def floor_term(out_db: float, ref_db: float) -> float:
    d = out_db - ref_db
    return max(0.0, d) / FLOOR_NORM_DB + FLOOR_UNDER * max(0.0, -d) / FLOOR_NORM_DB


# ---- analysis plan: everything that depends only on the DI (mask, onsets, gaps), computed once per target -----------
class _Plan:
    def __init__(self, n: int, mask: np.ndarray | None, onsets_s: np.ndarray | None,
                 gaps: list[tuple[int, int]] | None = None):
        self.n = int(n)
        m = np.ones(self.n, bool) if mask is None else np.asarray(mask[:self.n], bool)
        if len(m) < self.n:
            m = np.concatenate([m, np.zeros(self.n - len(m), bool)])
        self.mask = m
        cs = np.concatenate([[0], np.cumsum(m.astype(np.int64))])

        def active_starts(size: int, hop: int) -> np.ndarray:
            st = np.arange(0, self.n - size + 1, hop)
            if len(st) == 0:
                return st
            return st[(cs[st + size] - cs[st]) >= ACTIVE_FRACTION * size]
        self.fizz_starts = active_starts(FIZZ_N, FIZZ_HOP)
        fl = active_starts(FLUX_N, FLUX_HOP)
        self.flux_starts = fl
        self.flux_pair = np.nonzero(np.diff(fl) == FLUX_HOP)[0] + 1       # index i: frames fl[i-1], fl[i] are adjacent
        self.crest_starts = active_starts(CREST_WIN, CREST_HOP)
        # note windows in envelope-frame units
        self.nf = max(0, (self.n - ENV_SIZE) // ENV_HOP + 1)
        on = np.zeros(0) if onsets_s is None else np.asarray(onsets_s, float)
        o = np.round(on * RATE).astype(np.int64)
        nxt = np.concatenate([o[1:], [self.n]]) if len(o) else o
        e = np.minimum(np.minimum(nxt, o + int(NOTE_MAX_S * RATE)), self.n)
        centres = np.arange(self.nf) * ENV_HOP + ENV_SIZE // 2
        self.onsets = on
        self.win_ms = 1000.0 * (e - o) / RATE if len(o) else np.zeros(0)
        self.j0 = np.searchsorted(centres, o, "left")
        self.jp = np.searchsorted(centres, o + int(NOTE_PEAK_S * RATE), "left")
        self.j1 = np.searchsorted(centres, e, "left")
        # gap samples (floor)
        g = [np.arange(max(0, a), min(self.n, b)) for a, b in (gaps or []) if min(self.n, b) > max(0, a)]
        self.gap_idx = np.concatenate(g) if g else np.zeros(0, np.int64)
        self.gap_ok = len(self.gap_idx) >= FLOOR_MIN_S * RATE
        self.active_idx = np.nonzero(m)[0]


# ---- note statistics ---------------------------------------------------------------------------------------------
@dataclass
class NoteStats:
    t12: np.ndarray             # ms, NaN if not measurable
    sus: np.ndarray            # dB re peak
    cens: np.ndarray           # t12 censored at the window end
    peak_db: np.ndarray
    win_ms: np.ndarray

    @property
    def valid(self) -> np.ndarray:
        return ~(np.isnan(self.t12) | np.isnan(self.sus))


def lowband_power(x: np.ndarray, nf: int) -> np.ndarray:
    """Power (mean square) of the 60-250 Hz band over 5 ms frames, hop 2.5 ms."""
    y = signal.sosfilt(_SOS_LOW, x)
    cs = np.concatenate([[0.0], np.cumsum(y * y)])
    j = np.arange(nf) * ENV_HOP
    return (cs[j + ENV_SIZE] - cs[j]) / ENV_SIZE


def note_stats(p: np.ndarray, plan: _Plan) -> NoteStats:
    pw = p + 1e-20
    env = 10.0 * np.log10(pw)
    n = len(plan.j0)
    t12 = np.full(n, np.nan)
    sus = np.full(n, np.nan)
    pk_db = np.full(n, np.nan)
    cens = np.zeros(n, bool)
    for k in range(n):
        a, b, e = int(plan.j0[k]), int(plan.jp[k]), int(plan.j1[k])
        if b <= a or e - a < 2:
            continue
        i = a + int(np.argmax(env[a:b]))
        pk = env[i]
        seg = env[i:e]
        thr = pk - T12_DROP_DB
        below = np.flatnonzero(seg <= thr)
        if below.size:
            kk = int(below[0])          # >= 1: seg[0] is the peak
            y0, y1 = seg[kk - 1], seg[kk]
            t = kk - 1 + (y0 - thr) / max(y0 - y1, 1e-12)
        else:
            t = float(len(seg) - 1)
            cens[k] = True
        s0, s1 = i + SUS_FROM_FRAMES, min(i + SUS_TO_FRAMES + 1, e)
        t12[k] = t * HOP_MS
        pk_db[k] = pk
        if s1 > s0:
            sus[k] = float(np.clip(10.0 * np.log10(np.mean(pw[s0:s1]) / pw[i]), *SUS_CLIP_DB))
    return NoteStats(t12, sus, cens, pk_db, plan.win_ms)


def select_notes(st: NoteStats) -> tuple[np.ndarray | None, str | None, dict]:
    """(indices, noteSet, counts) from the stats of one signal (the DI, or the reference in soft mode)."""
    v = st.valid & (st.win_ms >= NOTE_MIN_MS)
    counts = {"onsets": int(len(st.t12)), "counting": 0, "chugs": 0, "used": 0}
    if not v.any():
        return None, None, counts
    counting = v & (st.peak_db >= np.nanmax(st.peak_db[v]) - NOTE_RANGE_DB)
    chug = counting & ~st.cens & (st.t12 <= CHUG_T12_MS)
    counts["counting"], counts["chugs"] = int(counting.sum()), int(chug.sum())
    if chug.sum() >= MIN_NOTES:
        idx, ns = np.nonzero(chug)[0], "chugs"
    elif counting.sum() >= MIN_NOTES:
        idx, ns = np.nonzero(counting)[0], "all"
    else:
        return None, None, counts
    counts["used"] = int(len(idx))
    return idx, ns, counts


# ---- fizz / polish per-frame features ----------------------------------------------------------------------------
def _fast_len(n: int) -> int:
    """Smallest m >= n whose only prime factors are 2, 3, 5 (fast FFT length)."""
    m = max(int(n), 2)
    while True:
        r = m
        for f in (2, 3, 5):
            while r % f == 0:
                r //= f
        if r == 1:
            return m
        m += 1


def _analytic_abs(y: np.ndarray) -> np.ndarray:
    """|analytic signal| (Hilbert envelope) by FFT, in chunks so that a long reference stays small in memory."""
    chunk, pad = 1 << 20, 4096
    out = np.empty(len(y))
    for s in range(0, len(y), chunk):
        a, b = max(0, s - pad), min(len(y), s + chunk + pad)
        seg = y[a:b]
        m = _fast_len(len(seg))
        Z = np.zeros(m, complex)
        Z[:m // 2 + 1] = np.fft.rfft(seg, m)
        Z[1:(m + 1) // 2] *= 2.0
        env = np.abs(np.fft.ifft(Z))[:len(seg)]
        out[s:min(len(y), s + chunk)] = env[s - a:s - a + min(chunk, len(y) - s)]
    return out


def fizz_features(x: np.ndarray, starts: np.ndarray) -> dict[str, np.ndarray]:
    """Per-frame hfRatioDb, hfFlat, hfMod for the frames at ``starts`` (2048 samples each)."""
    ar = np.arange(FIZZ_N)
    ratio, flat = [], []
    for i in range(0, len(starts), _BATCH):
        idx = starts[i:i + _BATCH, None] + ar[None, :]
        P = np.abs(np.fft.rfft(x[idx] * _WIN_FIZZ[None, :], axis=1)) ** 2
        hf = P[:, _SEL_HF].sum(axis=1)
        mid = P[:, _SEL_MID].sum(axis=1)
        ratio.append(10.0 * np.log10((hf + 1e-30) / (mid + 1e-30)))
        B = P[:, _SEL_FLAT] + 1e-30
        flat.append(np.exp(np.log(B).mean(axis=1)) / B.mean(axis=1))
    env = signal.sosfilt(_SOS_ENV, _analytic_abs(signal.sosfilt(_SOS_HF, x)))
    mod = []
    for i in range(0, len(starts), _BATCH):
        e = env[starts[i:i + _BATCH, None] + ar[None, :]]
        mod.append(e.std(axis=1) / (e.mean(axis=1) + 1e-20))
    return {"hfRatioDb": np.concatenate(ratio), "hfFlat": np.concatenate(flat), "hfMod": np.concatenate(mod)}


def flux_values(x: np.ndarray, plan: _Plan) -> np.ndarray:
    """Mean |delta dB| (300 Hz-8 kHz) between adjacent active 1024-pt frames (hop 512)."""
    st = plan.flux_starts
    if len(st) < 2 or len(plan.flux_pair) == 0:
        return np.zeros(0)
    ar = np.arange(FLUX_N)
    db = np.empty((len(st), int(_SEL_FLUX.sum())))
    for i in range(0, len(st), _BATCH):
        idx = st[i:i + _BATCH, None] + ar[None, :]
        mag = np.abs(np.fft.rfft(x[idx] * _WIN_FLUX[None, :], axis=1))[:, _SEL_FLUX]
        db[i:i + _BATCH] = 20.0 * np.log10(np.maximum(mag, 1e-7))
    p = plan.flux_pair
    return np.mean(np.abs(db[p] - db[p - 1]), axis=1)


def crest_values(x: np.ndarray, plan: _Plan) -> np.ndarray:
    """Peak / RMS (dB) per 400 ms active window (hop 200 ms)."""
    st = plan.crest_starts
    out = np.empty(len(st))
    for i, s in enumerate(st):
        w = x[s:s + CREST_WIN]
        out[i] = 20.0 * np.log10(np.max(np.abs(w)) + 1e-12) - 10.0 * np.log10(np.mean(w * w) + 1e-24)
    return out


def floor_db(x: np.ndarray, plan: _Plan) -> float | None:
    """Power in the gap samples re the power over the active samples (dB, floored at -90)."""
    if not plan.gap_ok or len(plan.active_idx) == 0:
        return None
    g = float(np.mean(x[plan.gap_idx] ** 2))
    a = float(np.mean(x[plan.active_idx] ** 2))
    return float(max(10.0 * np.log10(max(g, 1e-30)) - 10.0 * np.log10(max(a, 1e-30)), LEVEL_FLOOR_DB))


@dataclass
class Measured:
    notes: NoteStats | None = None
    fizz: dict | None = None
    flux: np.ndarray | None = None
    crest: np.ndarray | None = None
    floor: float | None = None


def measure(x: np.ndarray, plan: _Plan, *, notes: bool = True, fizz: bool = True, floor: bool = True,
            flux: bool = True, crest: bool = True) -> Measured:
    x = np.asarray(x, dtype=np.float64)
    if len(x) < plan.n:
        x = np.concatenate([x, np.zeros(plan.n - len(x))])
    x = x[:plan.n]
    m = Measured()
    if notes and len(plan.j0):
        m.notes = note_stats(lowband_power(x, plan.nf), plan)
    if fizz and len(plan.fizz_starts) >= MIN_FRAMES:
        m.fizz = fizz_features(x, plan.fizz_starts)
    if flux:
        m.flux = flux_values(x, plan)
    if crest:
        m.crest = crest_values(x, plan)
    if floor:
        m.floor = floor_db(x, plan)
    return m


# ---- target + evaluation -----------------------------------------------------------------------------------------
@dataclass
class FeelTarget:
    mode: str                           # "paired" | "soft"
    scale: float
    plan: _Plan                         # the DI's (also the output's) frames, notes, gaps
    sel: np.ndarray | None              # DI-selected note indices
    note_set: str | None
    counts: dict
    fizz_on: bool
    fizz_reason: str | None
    ref: Measured
    ref_plan: _Plan | None = None       # soft mode: the reference's own plan
    ref_sel: np.ndarray | None = None
    ref_note_set: str | None = None
    ref_counts: dict | None = None
    dropped: dict = field(default_factory=dict)
    off: frozenset = frozenset()        # terms switched off by the caller (floor / flux / crest), reasons in ``dropped``

    def summary(self) -> dict:
        """What the reference looks like through these features (for result.json)."""
        def med(a):
            return None if a is None or len(a) == 0 else float(np.median(a))
        r = self.ref
        sel = self.ref_sel if self.mode == "soft" else self.sel
        t12 = sus = None
        if r.notes is not None and sel is not None:
            t12, sus = med(r.notes.t12[sel]), med(r.notes.sus[sel])
        return {"mode": self.mode, "scale": self.scale, "fizzOn": self.fizz_on, "fizzOffReason": self.fizz_reason,
                "noteSet": self.ref_note_set if self.mode == "soft" else self.note_set,
                "notes": self.ref_counts if self.mode == "soft" else self.counts,
                "diNoteSet": self.note_set, "diNotes": self.counts,
                "t12MsMedian": t12, "sustainDbMedian": sus,
                "hfRatioDbMedian": med(None if r.fizz is None else r.fizz["hfRatioDb"]),
                "hfFlatMedian": med(None if r.fizz is None else r.fizz["hfFlat"]),
                "hfModMedian": med(None if r.fizz is None else r.fizz["hfMod"]),
                "fluxDbMedian": med(r.flux), "crestDbMedian": med(r.crest), "floorDb": r.floor,
                "dropped": dict(self.dropped)}


def make_target(di: np.ndarray, onsets_s: np.ndarray | None, mask: np.ndarray | None,
                gaps: list[tuple[int, int]] | None, ref_sig: np.ndarray | None = None, *,
                fizz_on: bool = True, fizz_reason: str | None = None,
                tight_on: bool = True, tight_reason: str | None = None, off: dict | None = None,
                soft_ref: tuple | None = None, cache: dict | None = None) -> FeelTarget:
    """Feel target. Matched pair: ``ref_sig`` is the reference aligned to ``di`` (same length), compared note by note
    and frame by frame. Soft: ``soft_ref = (signal, active_mask, onsets_s)`` is the reference's guitar-dominant
    signal in its own timeline, compared as distributions; ``cache`` (a dict kept on the Reference) memoises its
    features across the excerpts. ``fizz_on`` / ``tight_on`` switch those terms off (with the reason, recorded as
    dropped) when the reference cannot be trusted there (a full mix: cymbals above 5 kHz, bass and kick in the low
    band); ``off`` does the same for "floor", "flux" and "crest" ({name: reason}). ``di``, ``onsets_s``, ``mask`` and ``gaps`` (sample index pairs) are the DI's."""
    di = np.asarray(di, dtype=np.float64)
    plan = _Plan(len(di), mask, onsets_s, gaps)
    sel = note_set = None
    counts = {"onsets": int(len(plan.onsets)), "counting": 0, "chugs": 0, "used": 0}
    dropped: dict = {}
    off = dict(off or {})
    if tight_on and len(plan.j0):
        sel, note_set, counts = select_notes(note_stats(lowband_power(di, plan.nf), plan))
    if not tight_on:
        dropped["tight"] = tight_reason or "reference low end not usable"
    elif sel is None:
        dropped["tight"] = f"fewer than {MIN_NOTES} usable notes in the DI excerpt ({counts['onsets']} onsets, " \
                           f"{counts['counting']} counting)"
    if not fizz_on:
        dropped["fizz"] = fizz_reason or "reference HF not usable"
    if soft_ref is None:
        assert ref_sig is not None
        rs = np.asarray(ref_sig, dtype=np.float64)[:len(di)]
        ref = measure(rs, plan, notes=sel is not None, fizz=fizz_on, floor="floor" not in off,
                      flux="flux" not in off, crest="crest" not in off)
        ft = FeelTarget("paired", 1.0, plan, sel, note_set, counts, fizz_on, fizz_reason, ref)
        if "floor" not in off and not plan.gap_ok:
            dropped["floor"] = f"less than {int(FLOOR_MIN_S * 1000)} ms of DI gaps in the excerpt"
    else:
        sig, rmask, ron = soft_ref
        key = ("soft", fizz_on, tight_on, "flux" not in off, "crest" not in off)
        hit = None if cache is None else cache.get(key)
        if hit is None:
            rplan = _Plan(len(sig), rmask, ron, None)
            rm = measure(sig, rplan, notes=tight_on, fizz=fizz_on, floor=False, flux="flux" not in off,
                         crest="crest" not in off)
            rsel, rns, rcounts = (None, None, {"onsets": int(len(rplan.onsets)), "counting": 0, "chugs": 0, "used": 0})
            if rm.notes is not None:
                rsel, rns, rcounts = select_notes(rm.notes)
            hit = (rplan, rm, rsel, rns, rcounts)
            if cache is not None:
                cache[key] = hit
        rplan, rm, rsel, rns, rcounts = hit
        ft = FeelTarget("soft", SOFT_SCALE, plan, sel, note_set, counts, fizz_on, fizz_reason, rm, rplan, rsel, rns,
                        rcounts)
        if rsel is None and tight_on:
            dropped["tight"] = f"fewer than {MIN_NOTES} usable notes in the reference " \
                               f"({rcounts['onsets']} onsets, {rcounts['counting']} counting)"
        dropped["floor"] = "no matched pair: the reference has no clean DI-aligned gaps"
    dropped.update({k: v for k, v in off.items()})
    ft.dropped = dropped
    ft.off = frozenset(off)
    return ft


def _f(v) -> float | None:
    return None if v is None else float(v)


def evaluate(out: np.ndarray, ft: FeelTarget) -> tuple[float, dict]:
    """(weighted feel loss, terms). ``out`` is the render on the excerpt (sample-aligned with the DI)."""
    soft = ft.mode == "soft"
    k = ft.scale
    w = {"tight": W_TIGHT * k, "fizz": W_FIZZ * k, "polish": W_POLISH * k}
    terms: dict = {"mode": ft.mode, "scale": k, "weights": w, "noteSet": ft.note_set if not soft else ft.ref_note_set,
                   "notes": dict(ft.counts if not soft else ft.ref_counts or {}), "diNotes": dict(ft.counts),
                   "dropped": dict(ft.dropped)}
    out = np.asarray(out)
    if not np.all(np.isfinite(out)):
        terms["nonFinite"] = True
        return float("inf"), terms
    fz_on = ft.fizz_on and ft.ref.fizz is not None
    tight_on = ft.sel is not None and ft.ref.notes is not None and (not soft or ft.ref_sel is not None)
    m = measure(out, ft.plan, notes=tight_on, fizz=fz_on,
                floor=not soft and ft.plan.gap_ok and "floor" not in ft.off,
                flux="flux" not in ft.off, crest="crest" not in ft.off)
    total = 0.0
    # --- tightness
    tight = None
    if tight_on and m.notes is not None:
        if soft:
            to, so = m.notes.t12[ft.sel], m.notes.sus[ft.sel]
            tr, sr = ft.ref.notes.t12[ft.ref_sel], ft.ref.notes.sus[ft.ref_sel]
            ok_o, ok_r = ~(np.isnan(to) | np.isnan(so)), ~(np.isnan(tr) | np.isnan(sr))
            a, b = w1(to[ok_o], tr[ok_r]), w1(so[ok_o], sr[ok_r])
            terms["tightRaw"] = {"t12Ms": a, "sustainDb": b}
            used = int(ok_o.sum())
        else:
            ro, rr = m.notes, ft.ref.notes
            idx = ft.sel[ro.valid[ft.sel] & rr.valid[ft.sel]]
            used = int(len(idx))
            if used >= MIN_NOTES:
                a = float(np.median(asym(ro.t12[idx] - rr.t12[idx])))
                b = float(np.median(asym(ro.sus[idx] - rr.sus[idx])))
                terms["tightRaw"] = {"t12Ms": a, "sustainDb": b,
                                     "t12MsOut": float(np.median(ro.t12[idx])), "t12MsRef": float(np.median(rr.t12[idx])),
                                     "sustainDbOut": float(np.median(ro.sus[idx])),
                                     "sustainDbRef": float(np.median(rr.sus[idx]))}
            else:
                a = b = None
        if a is not None and b is not None and used >= MIN_NOTES:
            tight = huber(a / T12_NORM_MS) + huber(b / SUS_NORM_DB)
            terms["tightT12"], terms["tightSustain"] = a / T12_NORM_MS, b / SUS_NORM_DB      # normalised, before huber()
            terms["notes"]["used"] = used
        else:
            terms["dropped"]["tight"] = "too few comparable notes in this render"
    terms["tight"] = tight
    if tight is not None:
        total += w["tight"] * tight
    # --- fizz
    fizz = None
    if fz_on and m.fizz is not None:
        a = w1(m.fizz["hfRatioDb"], ft.ref.fizz["hfRatioDb"])
        b = w1(m.fizz["hfFlat"], ft.ref.fizz["hfFlat"])
        c = w1(m.fizz["hfMod"], ft.ref.fizz["hfMod"])
        if None not in (a, b, c):
            fizz = huber(a / RATIO_NORM_DB) + huber(b / FLAT_NORM) + huber(c / MOD_NORM)
            terms["fizzRaw"] = {"hfRatioDb": a, "hfFlat": b, "hfMod": c}
            terms["fizzHfRatio"], terms["fizzHfFlat"], terms["fizzHfMod"] = a / RATIO_NORM_DB, b / FLAT_NORM, c / MOD_NORM
    elif ft.fizz_on:
        terms["dropped"]["fizz"] = f"fewer than {MIN_FRAMES} active 2048-pt frames"
    terms["fizz"] = fizz
    if fizz is not None:
        total += w["fizz"] * fizz
    # --- polish
    parts = {}
    raw = {}
    if m.flux is not None and ft.ref.flux is not None:
        d = w1(m.flux, ft.ref.flux)
        if d is not None and len(m.flux) >= MIN_FRAMES and len(ft.ref.flux) >= MIN_FRAMES:
            parts["flux"], raw["fluxDb"] = huber(d / FLUX_NORM_DB), d
    if m.crest is not None and ft.ref.crest is not None and len(m.crest) >= MIN_CREST_WINDOWS \
            and len(ft.ref.crest) >= MIN_CREST_WINDOWS:
        d = w1(m.crest, ft.ref.crest)
        parts["crest"], raw["crestDb"] = huber(d / CREST_NORM_DB), d
    if not soft and m.floor is not None and ft.ref.floor is not None:
        parts["floor"] = huber(floor_term(m.floor, ft.ref.floor))
        raw["floorDiffDb"] = m.floor - ft.ref.floor
        raw["floorDbOut"], raw["floorDbRef"] = m.floor, ft.ref.floor
    for name in ("flux", "crest", "floor"):
        if name not in parts and name not in terms["dropped"]:
            terms["dropped"][name] = "too few active frames/windows"
    polish = sum(parts.values()) if parts else None
    terms.update({"flux": parts.get("flux"), "crest": parts.get("crest"), "floor": parts.get("floor"),
                  "polish": polish, "polishRaw": raw})
    if polish is not None:
        total += w["polish"] * polish
    terms["frames"] = {"fizz": int(len(ft.plan.fizz_starts)), "flux": int(len(ft.plan.flux_pair)),
                       "crest": int(len(ft.plan.crest_starts))}
    terms["total"] = float(total)
    return float(total), terms
