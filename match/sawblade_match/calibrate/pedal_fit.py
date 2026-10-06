"""Phase 7.1: calibrate the modeled ``pedal.hm`` against TONE3000 captures of real HM-2-family pedals.

Method (docs/specs/phase7_1_hm_calibration.md)
  * one fixed 45 s probe (sweep, stepped sines, real DI) is rendered through the capture (``nam`` block) and
    through ``pedal.hm`` with candidate params, both with the repo's ``tonerender`` (no cab, gate off, flat EQ);
  * the cost is  LTAS (1/3 oct, 60 Hz-12 kHz, dB RMS, DI segment)
                + 0.5 * harmonic-profile error (H2..H7 re fundamental, 16 stepped sines, dB RMS)
                + 0.5 * dynamics error (crest factor and 50 ms envelope spread of the DI segment, dB);
  * ``pedal.hm`` ends in a purely linear output gain (``3*level - 24`` dB), so ``level`` is solved in closed form
    (the mean 1/3-octave dB offset, which is exactly the LTAS-optimal gain) and the search is over
    ``low, high, distortion`` with CMA-ES (3 seeded restarts). The harmonic and dynamics terms do not depend on
    level, so this is the same optimum as a 4-parameter search at a fraction of the cost;
  * the *constrained* fit pins ``low/high/distortion`` to the capture's labelled knob positions (only ``level``
    free) to show the error of the raw knob map.

v0.4a (docs/specs/v0_4a-pedal_accuracy.md) changes, 7.1 behaviour stays reachable with ``--pedal hm --model-version 1``:
  * the harmonic term of the cost is ``harm_rms_db``: profiles clamped at ``HARM_FIXED_FLOOR_DB`` (-40 dB re the
    fundamental); the 7.1 term (floor -70 dB) is still reported as ``harm_rms_db_legacy``; even/odd parts are reported;
  * every render is aligned to the probe by cross-correlating the sweep segment (lag recorded), so a reference with
    latency does not move the stepped-sine slots;
  * ``--pedal {hm,hmx,eye,muff,ts}``: the param space of each block comes from ``PEDALS``; ``level`` is solved in
    closed form only where ``PedalSpec.level_pure_gain`` (proved by ``test_level_is_a_pure_output_gain[<pedal>]``),
    otherwise it is searched like the other knobs.

Python here is only analysis (features, plots, curve fits); every sample of ``pedal.hm`` audio comes from the C++
core through ``tonerender``. Deterministic: every random draw uses a recorded seed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Sequence

import numpy as np
import soundfile as sf
from scipy import signal

from .pedal_accuracy import spread_report

FS = 48000
SEED = 20261004
REPO = Path(__file__).resolve().parents[3]
DEFAULT_DI = REPO / "testdata" / "gatecreeper_cover" / "Guitar_L.wav"
DEFAULT_CACHE = Path.home() / ".cache" / "sawblade" / "captures"
IDENTITY_IR = REPO / "tests" / "fixtures" / "ir" / "impulse.wav"

# pedal.hm level: 3*level - 24 dB; level 8 is 0 dB. Level is the last, linear stage of the model.
LEVEL_REF = 8.0
DB_PER_LEVEL = 3.0
PARAM_NAMES = ("level", "low", "high", "distortion")

# 1/3-octave band centres, 60 Hz .. 12 kHz (base-10: 1000 * 10**(k/10)).
BAND_CENTRES = np.array([1000.0 * 10 ** (k / 10) for k in range(-12, 11)])
BAND_CENTRES = BAND_CENTRES[(BAND_CENTRES >= 60.0) & (BAND_CENTRES <= 12000.0)]

STEP_FREQS = (110.0, 220.0, 440.0, 880.0)
STEP_LEVELS_DB = (-30.0, -20.0, -10.0, -3.0)
HARMONICS = (2, 3, 4, 5, 6, 7)
HARM_FLOOR_DB = -70.0          # 7.1 profile floor ("absent"); the legacy term and the stored profiles use it
HARM_FIXED_FLOOR_DB = -40.0    # v0.4a: audibility / estimator-noise floor of the harmonic term used in the cost
EVEN_COLS = tuple(i for i, k in enumerate(HARMONICS) if k % 2 == 0)
ODD_COLS = tuple(i for i, k in enumerate(HARMONICS) if k % 2 == 1)
SCHEMA_VERSION = 2             # fits.json "version" (1 = phase 7.1, hm v1 only)
W_HARM = 0.5
W_DYN = 0.5


# ---------------------------------------------------------------------------------------------------------
# probe
# ---------------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class ProbeLayout:
    sweep_s: float = 10.0
    steps_s: float = 5.0
    di_s: float = 30.0
    sweep_db: float = -20.0

    @property
    def n_sweep(self) -> int:
        return int(round(self.sweep_s * FS))

    @property
    def n_steps(self) -> int:
        return int(round(self.steps_s * FS))

    @property
    def n_di(self) -> int:
        return int(round(self.di_s * FS))

    @property
    def n_total(self) -> int:
        return self.n_sweep + self.n_steps + self.n_di

    @property
    def sweep_sl(self) -> slice:
        return slice(0, self.n_sweep)

    @property
    def steps_sl(self) -> slice:
        return slice(self.n_sweep, self.n_sweep + self.n_steps)

    @property
    def di_sl(self) -> slice:
        return slice(self.n_sweep + self.n_steps, self.n_total)


def _db(x: float) -> float:
    return 10.0 ** (x / 20.0)


def exp_sweep(n: int, f0: float = 20.0, f1: float = 20000.0, level_db: float = -20.0) -> np.ndarray:
    t = np.arange(n) / FS
    T = n / FS
    k = np.log(f1 / f0)
    ph = 2 * np.pi * f0 * T / k * (np.exp(t / T * k) - 1.0)
    x = _db(level_db) * np.sin(ph)
    fade = int(0.01 * FS)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    x[:fade] *= w
    x[-fade:] *= w[::-1]
    return x


def stepped_sines(n: int) -> tuple[np.ndarray, list[tuple[float, float, slice]]]:
    """4 frequencies x 4 levels, equal slots, 20 ms raised-cosine edges. Returns (signal, [(f, level_db, slice)])."""
    n_slots = len(STEP_FREQS) * len(STEP_LEVELS_DB)
    L = n // n_slots
    out = np.zeros(n)
    slots = []
    fade = int(0.02 * FS)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    i = 0
    for f in STEP_FREQS:
        for lv in STEP_LEVELS_DB:
            t = np.arange(L) / FS
            s = _db(lv) * np.sin(2 * np.pi * f * t)
            s[:fade] *= w
            s[-fade:] *= w[::-1]
            out[i * L:(i + 1) * L] = s
            slots.append((f, lv, slice(i * L, (i + 1) * L)))
            i += 1
    return out, slots


def di_excerpt(di_path: Path, n: int, bpm: float = 140.0, start_bar: int = 20) -> np.ndarray:
    """Bar-aligned excerpt (4/4 at ``bpm``: the cover's tempo, from Drums.mid) of the DI, resampled to 48 kHz."""
    x, fs = sf.read(str(di_path), dtype="float64", always_2d=True)
    x = x[:, 0]
    if fs != FS:
        g = np.gcd(fs, FS)
        x = signal.resample_poly(x, FS // g, fs // g)
    bar = 4 * 60.0 / bpm
    i0 = int(round(start_bar * bar * FS))
    seg = x[i0:i0 + n]
    if len(seg) < n:  # short material (tests): wrap from the start, still deterministic
        seg = np.resize(x, n)
    return seg


def build_probe(di_path: Path = DEFAULT_DI, layout: ProbeLayout = ProbeLayout()):
    """(probe samples float32, ProbeLayout, step slots relative to the whole probe)."""
    sw = exp_sweep(layout.n_sweep, level_db=layout.sweep_db)
    st, slots = stepped_sines(layout.n_steps)
    di = di_excerpt(di_path, layout.n_di)
    x = np.concatenate([sw, st, di]).astype(np.float32)
    off = layout.n_sweep
    return x, layout, [(f, lv, slice(s.start + off, s.stop + off)) for f, lv, s in slots]


# ---------------------------------------------------------------------------------------------------------
# rendering through tonerender
# ---------------------------------------------------------------------------------------------------------
def tonerender_path() -> Path:
    env = os.environ.get("SAWBLADE_TONERENDER")
    cands = [Path(env)] if env else []
    cands += [REPO / d / "cli" / "tonerender" for d in ("build", "build-py", "build-lead")]
    for c in cands:
        if c.is_file():
            return c
    raise RuntimeError("tonerender not found (build it, or set SAWBLADE_TONERENDER)")


def _preset(block: dict) -> dict:
    return {"schema": "sawblade.preset", "version": 1, "name": "pedal-fit", "gate": {"enabled": False},
            "paths": {"a": {"role": "saw", "blocks": [block]}, "b": {"enabled": False, "blocks": []}},
            "align": {"mode": "off"}, "blend": 0.0,
            "cab": {"mode": "shared", "enabled": False, "ir": {"file": str(IDENTITY_IR)}}}


def nam_preset(nam_file: Path) -> dict:
    return _preset({"id": "a1", "type": "nam", "slot": "pedal", "model": {"file": str(nam_file)}})


@dataclass(frozen=True)
class PedalSpec:
    """One modeled pedal block as the fit sees it. ``knobs`` are the searched 0..10 params (the block's own preset
    keys); everything else (clip, boost, midVoice, the hmx mid-frequency knobs, tightness, mix, and muff ``crunch``,
    see PEDALS) stays at the block default. ``level_key`` is the output knob; when ``level_pure_gain`` it is solved in closed form (the pedal ends
    in a linear ``pedalLevelDb`` gain, proved per pedal by ``test_level_is_a_pure_output_gain``), otherwise it is
    searched with the knobs."""
    name: str
    block_type: str
    knobs: tuple
    level_key: str
    level_pure_gain: bool
    default_version: int
    versions: tuple

    def block(self, knobs: Sequence[float], level: float = LEVEL_REF, version: int | None = None) -> dict:
        v = self.default_version if version is None else int(version)
        if v not in self.versions:
            raise ValueError(f"pedal.{self.name} has no modelVersion {v} (known: {list(self.versions)})")
        params = {self.level_key: float(level), **{k: float(x) for k, x in zip(self.knobs, knobs)}}
        return {"id": "a1", "type": self.block_type, "slot": "pedal", "modelVersion": v, "params": params}

    def preset(self, knobs: Sequence[float], level: float = LEVEL_REF, version: int | None = None) -> dict:
        return _preset(self.block(knobs, level, version))

    @property
    def n_dims(self) -> int:
        return len(self.knobs) + (0 if self.level_pure_gain else 1)

    def param_dict(self, vec: Sequence[float], level: float | None = None) -> dict:
        """{level_key: ..., knob: ...} from a search vector (knobs, then level when searched)."""
        d = {k: float(x) for k, x in zip(self.knobs, vec)}
        lv = float(vec[len(self.knobs)]) if not self.level_pure_gain else level
        return {self.level_key: lv, **d}


PEDALS = {
    "hm": PedalSpec("hm", "pedal.hm", ("low", "high", "distortion"), "level", True, 3, (1, 2, 3)),
    "hmx": PedalSpec("hmx", "pedal.hmx", ("low", "lowMid", "highMid", "high", "distortion", "presence"), "level",
                     True, 1, (1,)),
    "eye": PedalSpec("eye", "pedal.eye", ("gain",), "level", True, 1, (1,)),
    # muff: ``crunch`` is not searched. Stage A gain is 6 + 3 * sustain dB and the clipper knee scales with crunch;
    # a clipper of gain g and knee k outputs k * f(g / k), so (sustain, crunch, volume) have an exact null manifold
    # (test_muff_sustain_and_crunch_are_redundant): only g / k is observable and the level solve absorbs k. Searching
    # all three only lets the optimiser wander along that ridge; sustain carries the drive, crunch stays at 5.
    "muff": PedalSpec("muff", "pedal.muff", ("sustain", "tone", "scoop", "voice"), "volume", True, 1, (1,)),
    "ts": PedalSpec("ts", "pedal.ts", ("drive", "tone"), "level", True, 1, (1,)),
}


def hm_preset(low: float, high: float, distortion: float, level: float = LEVEL_REF, model_version: int = 3) -> dict:
    """pedal.hm preset (four stock knobs). ``model_version=1`` reproduces the phase 7.1 fits."""
    return PEDALS["hm"].preset((low, high, distortion), level, model_version)


def render(preset: dict, in_wav: Path, out_wav: Path, tmpdir: Path | None = None) -> np.ndarray:
    """Render ``in_wav`` through ``preset`` (nice'd tonerender); returns the mono float64 output."""
    with tempfile.NamedTemporaryFile("w", suffix=".json", dir=tmpdir, delete=False) as f:
        json.dump(preset, f)
        pj = Path(f.name)
    try:
        r = subprocess.run(["nice", "-n", "10", str(tonerender_path()), "--preset", str(pj), "--in", str(in_wav),
                            "--out", str(out_wav)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"tonerender failed ({r.returncode}): {r.stderr.strip()[:300]}")
    finally:
        pj.unlink(missing_ok=True)
    y, fs = sf.read(str(out_wav), dtype="float64")
    if fs != FS:
        raise RuntimeError(f"unexpected render rate {fs}")
    return y


# ---------------------------------------------------------------------------------------------------------
# features
# ---------------------------------------------------------------------------------------------------------
def ltas_bands(x: np.ndarray, nperseg: int = 16384) -> np.ndarray:
    """1/3-octave band powers in dB (re an arbitrary constant) from a Welch PSD."""
    f, p = signal.welch(x, FS, nperseg=nperseg, noverlap=nperseg // 2, window="hann")
    df = f[1] - f[0]
    out = np.empty(len(BAND_CENTRES))
    for i, fc in enumerate(BAND_CENTRES):
        m = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
        out[i] = 10 * np.log10(np.sum(p[m]) * df + 1e-20)
    return out


def harmonic_profile(y: np.ndarray, slots) -> np.ndarray:
    """(16 slots, 6 harmonics) H2..H7 power re the fundamental, dB, floored at -70 dB."""
    out = np.full((len(slots), len(HARMONICS)), HARM_FLOOR_DB)
    for i, (f0, _lv, sl) in enumerate(slots):
        seg = y[sl]
        n = len(seg)
        a, b = int(0.35 * n), int(0.95 * n)           # skip the onset / filter settling
        z = seg[a:b] * np.hanning(b - a)
        nfft = 1 << int(np.ceil(np.log2(len(z) * 4)))
        P = np.abs(np.fft.rfft(z, nfft)) ** 2
        fr = np.fft.rfftfreq(nfft, 1 / FS)

        def band(fc: float) -> float:
            m = np.abs(fr - fc) <= 12.0
            return float(np.sum(P[m])) + 1e-30

        p1 = band(f0)
        for j, k in enumerate(HARMONICS):
            if k * f0 < 0.45 * FS:
                out[i, j] = max(HARM_FLOOR_DB, 10 * np.log10(band(k * f0) / p1))
    return out


def env_features(y_di: np.ndarray, x_di: np.ndarray) -> dict:
    """Crest factor of the DI-segment output and the p90-p10 spread of its 50 ms RMS envelope (frames where
    the *input* is above -55 dBFS, so noise floors do not count)."""
    hop = int(0.05 * FS)
    nf = len(y_di) // hop
    ry = np.sqrt(np.mean(y_di[:nf * hop].reshape(nf, hop) ** 2, axis=1) + 1e-20)
    rx = np.sqrt(np.mean(x_di[:nf * hop].reshape(nf, hop) ** 2, axis=1) + 1e-20)
    live = 20 * np.log10(rx) > -55.0
    e = 20 * np.log10(ry[live])
    crest = 20 * np.log10(np.max(np.abs(y_di)) / np.sqrt(np.mean(y_di ** 2) + 1e-20) + 1e-20)
    return {"crest_db": float(crest), "env_spread_db": float(np.percentile(e, 90) - np.percentile(e, 10))}


def sweep_response(y: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Small-signal-ish magnitude response (dB) in the 1/3-octave bands from the -20 dBFS sweep."""
    return ltas_bands(y, 8192) - ltas_bands(x, 8192)


LAG_MAX = 8192          # samples; search window of the sweep alignment (up to 170 ms at 48 kHz)
LAG_MIN = -256


def estimate_lag(y: np.ndarray, probe: np.ndarray, layout: ProbeLayout) -> int:
    """Delay (samples, >= LAG_MIN) of ``y`` relative to the probe: the peak of |cross-correlation| of the sweep
    segment. The sweep, not the stepped sines, because it is broadband (a sharp peak) and the distortion products of
    an exponential sweep correlate far outside this search window (they lead by ``T ln(k)/ln(f1/f0)`` seconds)."""
    n = layout.n_sweep
    x = np.asarray(probe, float)[:n]
    seg = np.zeros(n + LAG_MAX)
    got = np.asarray(y, float)[:n + LAG_MAX]
    seg[:len(got)] = got
    nfft = 1 << int(np.ceil(np.log2(2 * n + LAG_MAX)))
    c = np.fft.irfft(np.fft.rfft(seg, nfft) * np.conj(np.fft.rfft(x, nfft)), nfft)
    cand = np.concatenate([c[nfft + LAG_MIN:], c[:LAG_MAX + 1]])
    return int(np.argmax(np.abs(cand))) + LAG_MIN


def align(y: np.ndarray, lag: int) -> np.ndarray:
    """``y`` advanced by ``lag`` samples (same length, zero fill)."""
    y = np.asarray(y, float)
    if lag > 0:
        return np.concatenate([y[lag:], np.zeros(lag)])
    if lag < 0:
        return np.concatenate([np.zeros(-lag), y[:lag]])
    return y


def features(y: np.ndarray, probe: np.ndarray, layout: ProbeLayout, slots) -> dict:
    """Analysis features of a render. ``y`` is first aligned to the probe by the sweep cross-correlation (``lag``
    is recorded), so latency in a capture does not move the stepped-sine slots. ``harm`` is the 7.1 profile (floor
    -70 dB); the cost clamps it again at HARM_FIXED_FLOOR_DB (see ``compare``)."""
    y = np.asarray(y, float)
    lag = estimate_lag(y, probe, layout)
    y = align(y, lag)
    if len(y) < layout.n_total:
        y = np.pad(y, (0, layout.n_total - len(y)))
    y = y[:layout.n_total]
    di_y, di_x = y[layout.di_sl], np.asarray(probe, float)[layout.di_sl]
    return {"ltas": ltas_bands(di_y), "harm": harmonic_profile(y, slots), **env_features(di_y, di_x),
            "sweep": sweep_response(y[layout.sweep_sl], np.asarray(probe, float)[layout.sweep_sl]), "lag": lag}


def level_limits_db() -> tuple[float, float]:
    """Output-gain range of the level knob relative to level 8: (-24, +6) dB."""
    return DB_PER_LEVEL * (0.0 - LEVEL_REF), DB_PER_LEVEL * (10.0 - LEVEL_REF)


def _rms(x) -> float:
    x = np.asarray(x, float)
    return float(np.sqrt(np.mean(x ** 2))) if x.size else 0.0


def harm_fixed(h: np.ndarray, floor_db: float = HARM_FIXED_FLOOR_DB) -> np.ndarray:
    """Harmonic profile clamped at ``floor_db`` (profiles from ``harmonic_profile`` are already clamped at -70)."""
    if floor_db < HARM_FLOOR_DB:
        raise ValueError(f"floor {floor_db} dB is below the profile's own floor {HARM_FLOOR_DB} dB")
    return np.maximum(np.asarray(h, float), floor_db)


def harm_terms(ref_h: np.ndarray, mod_h: np.ndarray, floor_db: float = HARM_FIXED_FLOOR_DB) -> dict:
    """Fixed harmonic error (floor ``floor_db``), its even (H2 H4 H6) and odd (H3 H5 H7) parts, and the 7.1 term."""
    d = harm_fixed(ref_h, floor_db) - harm_fixed(mod_h, floor_db)
    return {"harm_rms_db": _rms(d), "harm_even_rms_db": _rms(d[:, EVEN_COLS]), "harm_odd_rms_db": _rms(d[:, ODD_COLS]),
            "harm_rms_db_legacy": _rms(np.asarray(ref_h, float) - np.asarray(mod_h, float))}


def compare(ref: dict, mod: dict, floor_db: float = HARM_FIXED_FLOOR_DB) -> dict:
    """Cost of a model render's features against the reference's; level solved in closed form (the LTAS shape error
    has the offset removed, so it is the same for every level of a pure-gain pedal)."""
    lo, hi = level_limits_db()
    diff = ref["ltas"] - mod["ltas"]
    g_free = float(np.mean(diff))
    g = float(np.clip(g_free, lo, hi))
    # Shape error: the LTAS-optimal gain is removed even when it lies outside the knob's -24..+6 dB range (the
    # capture's absolute level is arbitrary); ``level_clipped`` flags that case and ``ltas_rms_limited_db`` is the
    # error if the level knob could not go beyond its range.
    resid = diff - g_free
    ltas = float(np.sqrt(np.mean(resid ** 2)))
    ltas_limited = float(np.sqrt(np.mean((diff - g) ** 2)))
    ht = harm_terms(ref["harm"], mod["harm"], floor_db)
    dcrest = ref["crest_db"] - mod["crest_db"]
    dspread = ref["env_spread_db"] - mod["env_spread_db"]
    dyn = float(np.sqrt(0.5 * (dcrest ** 2 + dspread ** 2)))
    return {"cost": ltas + W_HARM * ht["harm_rms_db"] + W_DYN * dyn, "ltas_rms_db": ltas, **ht, "dyn_db": dyn,
            "d_crest_db": float(dcrest), "d_env_spread_db": float(dspread),
            "ltas_rms_limited_db": ltas_limited,
            "level_gain_db": g_free, "level": LEVEL_REF + g_free / DB_PER_LEVEL, "level_clipped": bool(abs(g - g_free) > 1e-9),
            "residual_ltas_db": resid.tolist()}


# ---------------------------------------------------------------------------------------------------------
# fitting
# ---------------------------------------------------------------------------------------------------------
@dataclass
class Evaluator:
    probe_wav: Path
    probe: np.ndarray
    layout: ProbeLayout
    slots: list
    work: Path
    jobs: int = 4
    cache: dict = field(default_factory=dict)
    n_renders: int = 0
    spec: PedalSpec = PEDALS["hm"]
    model_version: int | None = None     # None = the block type's current version
    harm_floor_db: float = HARM_FIXED_FLOOR_DB

    def features(self, *vals: float, level: float = LEVEL_REF) -> dict:
        """Features of the pedal at knob values ``vals`` (``spec.knobs`` order). For a pure-gain pedal the level is
        fixed at LEVEL_REF (the closed-form solve covers it); otherwise it is the last search dimension."""
        if len(vals) == len(self.spec.knobs) + 1:
            *vals, level = vals
        key = tuple(round(float(v), 4) for v in vals) + ((round(float(level), 4),) if not self.spec.level_pure_gain else ())
        if key not in self.cache:
            fd, name = tempfile.mkstemp(suffix=".wav", dir=self.work)
            os.close(fd)
            out = Path(name)
            try:
                y = render(self.spec.preset(key[:len(self.spec.knobs)], level if not self.spec.level_pure_gain else LEVEL_REF,
                                            self.model_version), self.probe_wav, out, self.work)
            finally:
                out.unlink(missing_ok=True)
            self.cache[key] = features(y, self.probe, self.layout, self.slots)
            self.n_renders += 1
        return self.cache[key]


def reference_features(nam_file: Path, ev: Evaluator, keep: Path | None = None) -> dict:
    out = keep or (ev.work / "ref.wav")
    if keep is not None and keep.exists():
        y, _ = sf.read(str(keep), dtype="float64")
    else:
        y = render(nam_preset(nam_file), ev.probe_wav, out, ev.work)
    if keep is None:
        out.unlink(missing_ok=True)
    return features(y, ev.probe, ev.layout, ev.slots)


REFINE_SIGMA = 0.04   # step of the refinement stage, in the [0, 1] search box (0.4 knob units)


def _minimize(fn_batch, x0, seed: int, popsize: int, generations: int, sigma0: float = 0.3):
    from ..matcher.cma import minimize
    return minimize(None, x0, sigma0=sigma0, popsize=popsize, generations=generations, seed=seed,
                    evaluate_batch=fn_batch)


def fit_free(ref: dict, ev: Evaluator, seed: int, restarts: int = 3, popsize: int = 8, generations: int = 14,
             refine_generations: int = 0) -> dict:
    """CMA-ES over the pedal's knobs in [0, 10]^n (plus the level knob when it is not a pure gain), ``restarts``
    seeded restarts; best of all. ``refine_generations`` > 0 adds a last small-step CMA-ES stage (sigma
    ``REFINE_SIGMA``) from the best point: the cost is V-shaped at the optimum, so the wide restarts stop short of
    the vertex. 0 = the phase 7.1 search exactly. Returns ``{knob: value, ..., "restarts": [...]}``."""
    n = ev.spec.n_dims

    def one(x):
        return compare(ref, ev.features(*(10.0 * float(v) for v in x)), ev.harm_floor_db)["cost"]

    def batch(X):
        with ThreadPoolExecutor(max_workers=ev.jobs) as ex:
            return list(ex.map(one, X))

    rng = np.random.default_rng(seed)
    best = (np.inf, None)
    runs = []
    for r in range(restarts):
        x0 = np.full(n, 0.5) if r == 0 else rng.uniform(0.05, 0.95, n)
        bx, bf, _ = _minimize(batch, x0, seed + 1000 * r, popsize, generations)
        runs.append({"restart": r, "seed": seed + 1000 * r, "x0": (10 * x0).round(3).tolist(),
                     "best": (10 * bx).round(3).tolist(), "cost": float(bf)})
        if bf < best[0]:
            best = (bf, bx)
    if refine_generations > 0:
        bx, bf, _ = _minimize(batch, best[1], seed + 7777, popsize, refine_generations, REFINE_SIGMA)
        runs.append({"restart": "refine", "seed": seed + 7777, "x0": (10 * best[1]).round(3).tolist(),
                     "best": (10 * bx).round(3).tolist(), "cost": float(bf)})
        if bf < best[0]:
            best = (bf, bx)
    vals = [10.0 * float(v) for v in best[1]]
    out = {k: v for k, v in zip(ev.spec.knobs, vals)}
    if not ev.spec.level_pure_gain:
        out[ev.spec.level_key] = vals[len(ev.spec.knobs)]
    return {**out, "restarts": runs}


def evaluate_params(ref: dict, ev: Evaluator, *vals: float) -> tuple[dict, dict]:
    """Score the pedal at ``vals`` (knobs, then level when it is searched). Returns (result, model features)."""
    mod = ev.features(*vals)
    c = compare(ref, mod, ev.harm_floor_db)
    spec = ev.spec
    level = c["level"] if spec.level_pure_gain else float(vals[len(spec.knobs)])
    params = {spec.level_key: level, **{k: float(v) for k, v in zip(spec.knobs, vals)}}
    return {"params": params, "model_lag": mod["lag"], **c}, mod


# ---------------------------------------------------------------------------------------------------------
# models to fit
# ---------------------------------------------------------------------------------------------------------
LABEL_RE = re.compile(r"Lv-(\d+)\s+L-(\d+)\s+H-(\d+)\s+D-(\d+)")
LABEL_GROUPS = ("level", "low", "high", "distortion")


def parse_labels(name: str, regex: re.Pattern | str = LABEL_RE, groups: Sequence[str] = LABEL_GROUPS) -> dict | None:
    """Knob positions written into a capture's name; the default is the HM-2 family's ``Lv-7 L-9 H-9 D-2``. A
    targets manifest (docs/reports/v0_4/targets.json) gives another ``label_regex`` / ``label_groups`` per pedal."""
    m = re.search(regex, name) if isinstance(regex, str) else regex.search(name)
    if not m:
        return None
    return {g: float(v) for g, v in zip(groups, m.groups())}


# Knob positions assumed for captures that do not carry a full "Lv L H D" label (see the report). Rotary pots run
# 7 o'clock (0) to 5 o'clock (10): 12 o'clock = 5, +1 per hour clockwise.
ASSUMED = {
    "boss hm-2w - maxed out s mode": {"low": 10, "high": 10, "distortion": 10},
    "boss hm-2w - maxed out c": {"low": 10, "high": 10, "distortion": 10},
    "boss hm-2w - 2 o'clock distortion s": {"low": 10, "high": 10, "distortion": 7},
    "boss hm-2w - 2 o'clock distortion c": {"low": 10, "high": 10, "distortion": 7},
    "Boss HM-2w CHAINSAW Custom": {"low": 10, "high": 10, "distortion": 10},
    "Boss HM-2w CHAINSAW Standard": {"low": 10, "high": 10, "distortion": 10},
    "Boss_Waza_HM-2_Custom": {"low": 10, "high": 10, "distortion": 10},
    "Boss_Waza_HM-2_Standard": {"low": 10, "high": 10, "distortion": 10},
    "Throne Torcher Maxed V2": {"low": 10, "high": 10, "distortion": 10},
    "Throne Torcher Maxed 0 Gain": {"low": 10, "high": 10, "distortion": 0},
    "TC Electronic Eyemaster": {"low": 10, "high": 10, "distortion": 10},
    "EyemasterAHe_Gain12oclk_Vol4oclk": {"low": 5, "high": 5, "distortion": 5},
}

# tone id -> (unit, group, [model ids]); an empty list means every listed model that is cached.
DEFAULT_TONES = {
    58569: ("Boss HM-2 1985 MIJ", "stock", [497155, 496943, 496942, 496941, 496940, 496939, 496938, 496937]),
    74487: ("Boss HM-2W", "hm2w", [652448, 652449, 652450, 652451]),
    78122: ("Boss HM-2W (CHAINSAW)", "hm2w", [680197, 680198]),
    88604: ("Boss Waza HM-2", "hm2w", [754123, 747049]),
    72990: ("Throne Torcher (modded HM-2)", "mod", [642541, 615089]),
    60618: ("TC Eyemaster", "eyemaster", [422214]),
    62523: ("TC Eyemaster", "eyemaster", [385385]),
}


def load_targets(cache: Path, tones: dict | None = None, pedal: dict | None = None) -> tuple[list[dict], list[dict]]:
    """(models found in the cache, models listed but missing). Reads pool_manifest.json for names, creators and
    licences. ``tones``: tone id -> (unit, group, [model ids]); an empty model list means every model of that tone
    in the pool manifest that is cached. ``pedal``: a targets-manifest entry (``label_regex``, ``label_groups``,
    ``assumed``) used to label the records."""
    man = json.loads((cache / "pool_manifest.json").read_text())
    by_tone = {t["tone_id"]: t for t in man["tones"]}
    pedal = pedal or {}
    regex = re.compile(pedal["label_regex"]) if pedal.get("label_regex") else LABEL_RE
    groups = tuple(pedal.get("label_groups") or LABEL_GROUPS)
    assumed = pedal.get("assumed", ASSUMED if pedal == {} else {})
    found, missing = [], []
    for tid, (unit, group, mids) in (tones or DEFAULT_TONES).items():
        t = by_tone.get(tid)
        names = {m["id"]: m["name"] for m in (t["models"] if t else [])}
        for mid in (mids or sorted(names)):
            f = cache / str(tid) / f"{mid}.nam"
            name = names.get(mid, str(mid))
            labels = parse_labels(name, regex, groups)
            rec = {"tone_id": tid, "model_id": mid, "name": name, "unit": unit, "group": group,
                   "file": f, "creator": (t or {}).get("creator"), "license": (t or {}).get("license"),
                   "labels": labels, "pin": labels or assumed.get(name)}
            (found if f.is_file() else missing).append(rec)
    return found, missing


def load_manifest_pedal(path: Path, pedal: str) -> tuple[dict, dict]:
    """(tones dict in DEFAULT_TONES shape, the pedal's manifest entry) from a targets manifest."""
    doc = json.loads(Path(path).read_text())
    ent = doc.get("pedals", {}).get(pedal)
    if ent is None:
        raise ValueError(f"{path} has no entry for pedal '{pedal}'")
    if not ent.get("tones"):
        raise ValueError(f"{path}: pedal '{pedal}' lists no tones yet; resolve ids with `sawblade-t3k search` "
                         "(see its to_resolve entry), add them to \"tones\", and re-run")
    tones = {int(t["tone_id"]): (t.get("unit", ent.get("family", pedal)), t.get("group", "all"),
                                 [int(m) for m in t.get("models", [])]) for t in ent.get("tones", [])}
    return tones, ent


def _pin_vector(spec: PedalSpec, pin: dict) -> tuple[list[float], list[str]]:
    """Knob values (``spec.knobs`` order) of a pinned setting; a knob the label does not carry takes the block
    default 5.0 and is listed in the second return value."""
    vals, filled = [], []
    for k in spec.knobs:
        if k in pin:
            vals.append(float(pin[k]))
        else:
            vals.append(5.0)
            filled.append(k)
    return vals, filled


def fit_model(rec: dict, ev: Evaluator, ref_wav_dir: Path, restarts: int, popsize: int, generations: int,
              say: Callable[[str], None] = print, refine_generations: int = 0) -> tuple[dict, dict]:
    spec = ev.spec
    ref = reference_features(rec["file"], ev, ref_wav_dir / f"{rec['model_id']}.wav")
    if "labels" in rec:
        labels, pin = rec["labels"], rec["pin"]
    else:                                            # 7.1 call style: records straight from DEFAULT_TONES
        labels = parse_labels(rec["name"])
        pin = labels or ASSUMED.get(rec["name"])     # exact model-name match; None = no pinned fit
    assumed = labels is None
    seed = SEED + int(rec["model_id"])
    ev.cache.clear()
    free = fit_free(ref, ev, seed, restarts, popsize, generations, refine_generations)
    fvec = [free[k] for k in spec.knobs] + ([free[spec.level_key]] if not spec.level_pure_gain else [])
    fr, fmod = evaluate_params(ref, ev, *fvec)
    fr["restarts"] = free["restarts"]
    out = {k: rec[k] for k in ("tone_id", "model_id", "name", "unit", "group", "creator", "license")}
    out.update({"pedal": spec.name, "model_version": ev.model_version or spec.default_version,
                "non_commercial": str(rec.get("license") or "").lower().startswith("cc-by-nc"),
                "labels": labels, "pinned_knobs": pin, "pinned_is_assumed": assumed, "seed": seed,
                "ref": {"crest_db": ref["crest_db"], "env_spread_db": ref["env_spread_db"], "lag": ref["lag"],
                        "harm_fixed": np.round(harm_fixed(ref["harm"], ev.harm_floor_db), 2).tolist()},
                "free": fr})
    feats = {"ref": ref, "free": fmod}
    if pin:
        cvec, filled = _pin_vector(spec, pin)
        cr, cmod = evaluate_params(ref, ev, *cvec, *([LEVEL_REF] if not spec.level_pure_gain else []))
        if filled:
            cr["pins_filled_with_default"] = filled
        out["constrained"] = cr
        feats["constrained"] = cmod
    fp = fr["params"]
    say(f"  {rec['model_id']} {rec['name']}: free ltas {fr['ltas_rms_db']:.2f} dB harm {fr['harm_rms_db']:.1f} "
        f"({' '.join(f'{k} {fp[k]:.1f}' for k in (*spec.knobs, spec.level_key))})"
        + (f", constrained ltas {out['constrained']['ltas_rms_db']:.2f}" if "constrained" in out else "")
        + f"  [{ev.n_renders} renders]")
    return out, feats


# ---------------------------------------------------------------------------------------------------------
# aggregation: knob map, systematic residual, EQ correction, Custom-mode deltas
# ---------------------------------------------------------------------------------------------------------
def knob_map(fits: list[dict]) -> dict:
    """Per knob: labelled value -> fitted model param (mean over models sharing that label), a monotone
    (isotonic, non-decreasing) curve through them, and a straight-line summary."""
    from scipy.optimize import isotonic_regression
    stock = [f for f in fits if f.get("labels") and f["group"] == "stock"]
    res = {}
    for knob in ("level", "low", "high", "distortion"):
        pts: dict[float, list[float]] = {}
        for f in stock:
            pts.setdefault(f["labels"][knob], []).append(f["free"]["params"][knob])
        xs = sorted(pts)
        ys = np.array([np.mean(pts[x]) for x in xs])
        n = np.array([len(pts[x]) for x in xs], float)
        iso = isotonic_regression(ys, weights=n).x if len(xs) > 1 else ys
        a, b = (np.polyfit(xs, ys, 1) if len(xs) > 1 else (1.0, 0.0))
        res[knob] = {"points": [{"label": x, "fitted_mean": float(y), "fitted_monotone": float(m), "n": int(k),
                                 "fitted_all": [float(v) for v in pts[x]]}
                                for x, y, m, k in zip(xs, ys, iso, n)],
                     "line": {"slope": float(a), "intercept": float(b)},
                     "rmse_vs_identity": float(np.sqrt(np.mean((ys - np.array(xs)) ** 2)))}
    return res


def mean_residual(fits: list[dict], group: str = "stock", which: str = "free") -> np.ndarray:
    r = [np.array(f[which]["residual_ltas_db"]) for f in fits if f["group"] == group and which in f]
    return np.mean(r, axis=0)


def _rbj(kind: str, f0: float, gain_db: float, q: float):
    w0 = 2 * np.pi * f0 / FS
    A = 10 ** (gain_db / 40)
    al = np.sin(w0) / (2 * q)
    c = np.cos(w0)
    if kind == "peak":
        b = [1 + al * A, -2 * c, 1 - al * A]
        a = [1 + al / A, -2 * c, 1 - al / A]
    elif kind == "lowShelf":
        s = 2 * np.sqrt(A) * al
        b = [A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s)]
        a = [(A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s]
    elif kind == "highShelf":
        s = 2 * np.sqrt(A) * al
        b = [A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s)]
        a = [(A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s]
    elif kind == "lowPass":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
        a = [1 + al, -2 * c, 1 - al]
    else:
        raise ValueError(kind)
    return np.array(b) / a[0], np.array(a) / a[0]


def eq_response_db(bands: Sequence[tuple], freqs: np.ndarray = BAND_CENTRES) -> np.ndarray:
    """Magnitude (dB) of an RBJ biquad cascade ``[(kind, f0, gain_db, q), ...]`` at ``freqs`` (analysis only; the
    same cookbook the core uses, so a proposed correction can be read straight into an EqBand)."""
    h = np.zeros(len(freqs))
    for k, f0, g, q in bands:
        b, a = _rbj(k, f0, g, q)
        _, H = signal.freqz(b, a, worN=2 * np.pi * np.asarray(freqs) / FS)
        h += 20 * np.log10(np.abs(H))
    return h


def fit_eq_correction(target_db: np.ndarray, template: Sequence[tuple], free: Sequence[str], seed: int = SEED) -> dict:
    """Least-squares fit of a biquad cascade to ``target_db`` (1/3-octave bands). ``template`` is a list of
    ``(kind, f0, gain_db, q)``; ``free`` lists which of ``"f", "g", "q"`` may move for each band, as one string per
    band (e.g. ``"fgq"``, ``"g"``). Returns the fitted bands and the remaining RMS error."""
    from scipy.optimize import least_squares
    lo_f, hi_f = 40.0, 14000.0
    idx = []
    x0 = []
    for i, (_k, f0, g, q) in enumerate(template):
        for ch in free[i]:
            idx.append((i, ch))
            x0.append({"f": np.log(f0), "g": g, "q": np.log(q)}[ch])

    def build(x):
        bands = [list(b) for b in template]
        for (i, ch), v in zip(idx, x):
            bands[i][{"f": 1, "g": 2, "q": 3}[ch]] = float(np.exp(v) if ch in "fq" else v)
        bands = [tuple(b) for b in bands]
        for j, b in enumerate(bands):
            bands[j] = (b[0], float(np.clip(b[1], lo_f, hi_f)), float(np.clip(b[2], -24, 24)),
                        float(np.clip(b[3], 0.3, 6.0)))
        return bands

    def resid(x):
        return eq_response_db(build(x)) - target_db

    r = least_squares(resid, np.array(x0, float))
    bands = build(r.x)
    return {"bands": [{"type": k, "freq": round(f, 1), "gainDb": round(g, 2), "q": round(q, 2)} for k, f, g, q in bands],
            "rms_before_db": float(np.sqrt(np.mean(target_db ** 2))),
            "rms_after_db": float(np.sqrt(np.mean(resid(r.x) ** 2)))}


HM_BANDS = [("peak", 100.0, 0.0, 0.8), ("peak", 1000.0, 0.0, 1.2), ("peak", 1500.0, 0.0, 1.2),
            ("peak", 4800.0, 0.0, 2.0), ("lowPass", 9000.0, 0.0, 0.707)]
CUSTOM_PAIRS = [(652449, 652448), (652451, 652450), (680197, 680198), (754123, 747049)]   # (custom, standard)


def eq_correction_report(fits: list[dict]) -> dict:
    """Concrete biquad corrections for the systematic stock-HM-2 residual (mean free-fit residual, capture - model).
    ``deltas``: the existing five EqBands (HmColorEq) with only their gain (LPF: cutoff and Q) free;
    ``extra``: a free-form 4-band cascade (low shelf, two peaks, high shelf) for a structure change."""
    target = mean_residual(fits)
    deltas = fit_eq_correction(target, [HM_BANDS[0], HM_BANDS[1], HM_BANDS[2], HM_BANDS[3],
                                        ("lowPass", 9000.0, 0.0, 0.707)], ["g", "g", "g", "gf", "fq"])
    extra = fit_eq_correction(target, [("lowShelf", 120.0, 0.0, 0.7), ("peak", 600.0, 0.0, 1.0),
                                       ("peak", 3000.0, 0.0, 1.0), ("highShelf", 6000.0, 0.0, 0.7)],
                              ["fg", "fgq", "fgq", "fg"])
    return {"target_mean_residual_db": target.tolist(), "gain_deltas_existing_bands": deltas, "free_cascade": extra}


def custom_mode_report(fits: list[dict], ref_dir: Path, ev: Evaluator) -> list[dict]:
    """Standard -> Custom differences of the real captures (LTAS, sweep, harmonics, dynamics), the refit parameter
    deltas on pedal.hm, and the small EQ (low shelf + high shelf) that explains the LTAS difference."""
    by_id = {f["model_id"]: f for f in fits}
    out = []
    for cid, sid in CUSTOM_PAIRS:
        if cid not in by_id or sid not in by_id:
            continue
        fc_, fs_ = (reference_features(Path("."), ev, ref_dir / f"{i}.wav") for i in (cid, sid))
        d_ltas = fc_["ltas"] - fs_["ltas"]
        shape = d_ltas - np.mean(d_ltas)
        corr = fit_eq_correction(shape, [("lowShelf", 150.0, 0.0, 0.7), ("highShelf", 5000.0, 0.0, 0.7)], ["fg", "fg"])
        pc, ps = by_id[cid]["free"]["params"], by_id[sid]["free"]["params"]
        out.append({"tone_id": by_id[cid]["tone_id"], "custom": cid, "standard": sid, "name": by_id[cid]["name"],
                    "ltas_diff_db": d_ltas.tolist(), "ltas_diff_mean_db": float(np.mean(d_ltas)),
                    "sweep_diff_db": (fc_["sweep"] - fs_["sweep"]).tolist(),
                    "harm_diff_db_mean_by_h": (fc_["harm"] - fs_["harm"]).mean(axis=0).tolist(),
                    "d_crest_db": fc_["crest_db"] - fs_["crest_db"],
                    "d_env_spread_db": fc_["env_spread_db"] - fs_["env_spread_db"],
                    "param_delta_free_fit": {k: pc[k] - ps[k] for k in PARAM_NAMES}, "eq_explaining_shape": corr})
    return out


# ---------------------------------------------------------------------------------------------------------
# plots
# ---------------------------------------------------------------------------------------------------------
def model_plot(path: Path, rec: dict, feats: dict) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fc = BAND_CENTRES
    fig, ax = plt.subplots(2, 2, figsize=(10, 6.2), dpi=80)
    a = ax[0, 0]
    ref = feats["ref"]["ltas"]
    a.semilogx(fc, ref - np.max(ref), "k", lw=2, label="capture")
    for key, col in (("free", "C0"), ("constrained", "C3")):
        if key in feats:
            g = rec[key]["level_gain_db"]
            a.semilogx(fc, feats[key]["ltas"] + g - np.max(ref), col, label=f"{rec.get('pedal', 'pedal.hm')} {key}")
    a.set_title("DI-segment LTAS (dB, 1/3 oct)")
    a.legend(fontsize=7)
    a.grid(True, which="both", alpha=0.3)
    a = ax[0, 1]
    for key, col in (("free", "C0"), ("constrained", "C3")):
        if key in rec:
            a.semilogx(fc, rec[key]["residual_ltas_db"], col,
                       label=f"{key}: {rec[key]['ltas_rms_db']:.2f} dB RMS")
    a.axhline(0, color="k", lw=0.5)
    a.set_title("residual LTAS (capture - model, dB)")
    a.legend(fontsize=7)
    a.grid(True, which="both", alpha=0.3)
    a = ax[1, 0]
    x = np.arange(len(HARMONICS))
    # mean over fundamentals, per drive level
    for li, lv in enumerate(STEP_LEVELS_DB):
        sel = [fi * len(STEP_LEVELS_DB) + li for fi in range(len(STEP_FREQS))]
        a.plot(x + 2, harm_fixed(feats["ref"]["harm"])[sel].mean(axis=0), "-o", color=f"C{li}", ms=3,
               label=f"capture {lv:g} dBFS")
        a.plot(x + 2, harm_fixed(feats["free"]["harm"])[sel].mean(axis=0), "--", color=f"C{li}", lw=1)
    a.set_title("H2..H7 re fundamental (dB, floor -40; solid capture, dashed free fit)")
    a.set_xlabel("harmonic")
    a.legend(fontsize=6)
    a.grid(True, alpha=0.3)
    a = ax[1, 1]
    a.semilogx(fc, feats["ref"]["sweep"], "k", lw=2, label="capture")
    for key, col in (("free", "C0"), ("constrained", "C3")):
        if key in feats:
            a.semilogx(fc, feats[key]["sweep"] + rec[key]["level_gain_db"], col, label=key)
    a.set_title("-20 dBFS sweep response (dB)")
    a.legend(fontsize=7)
    a.grid(True, which="both", alpha=0.3)
    fig.suptitle(f"{rec['unit']} / {rec['name']}  (tone {rec['tone_id']}, model {rec['model_id']})", fontsize=9)
    fig.tight_layout()
    fig.savefig(path, dpi=80)
    plt.close(fig)


def aggregate_plot(path: Path, fits: list[dict], kmap: dict, resid_mean: np.ndarray) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(1, 3, figsize=(12, 3.8), dpi=80)
    for a, knob in zip(ax[:2], ("distortion", "low")):
        pts = kmap[knob]["points"]
        a.plot([0, 10], [0, 10], "k:", label="identity")
        a.plot([p["label"] for p in pts], [p["fitted_monotone"] for p in pts], "C0-o", label="monotone map")
        for p in pts:
            a.plot([p["label"]] * len(p["fitted_all"]), p["fitted_all"], "C1.", ms=5)
        a.set_title(f"knob map: {knob}")
        a.set_xlabel("labelled")
        a.set_ylabel("model param")
        a.legend(fontsize=7)
        a.grid(True, alpha=0.3)
    a = ax[2]
    for f in fits:
        if f["group"] == "stock":
            a.semilogx(BAND_CENTRES, f["free"]["residual_ltas_db"], color="0.7", lw=0.8)
    a.semilogx(BAND_CENTRES, resid_mean, "C3", lw=2, label="mean (stock HM-2)")
    a.axhline(0, color="k", lw=0.5)
    a.set_title("free-fit residual LTAS (dB)")
    a.legend(fontsize=7)
    a.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=80)
    plt.close(fig)


# ---------------------------------------------------------------------------------------------------------
# known answers: the pedal rendered at known params is the "capture"
# ---------------------------------------------------------------------------------------------------------
# Non-default knob sets per pedal (searched knobs in PedalSpec.knobs order) and the level of the "capture".
KNOWN_TRUTH = {
    "hm": ((7.0, 3.0, 8.0), 6.5),
    "hmx": ((6.0, 3.0, 7.0, 4.0, 8.0, 6.0), 6.5),
    "eye": ((7.0,), 6.5),
    "muff": ((7.0, 4.0, 6.0, 6.0), 6.5),
    "ts": ((7.0, 3.0), 6.5),
}
KNOWN_LAYOUT = ProbeLayout(sweep_s=1.0, steps_s=3.2, di_s=4.0)   # short probe: tests and --known-answers
KNOWN_DI = REPO / "tests" / "fixtures" / "di_riff.wav"


def known_answer(spec: PedalSpec, ev: Evaluator, seed: int = SEED, restarts: int = 2, popsize: int = 8,
                 generations: int = 10, refine_generations: int = 0) -> dict:
    """Render ``spec`` at KNOWN_TRUTH as the reference, then (a) score the knob-constrained fit at the true params
    and (b) run the seeded free fit. ``knob_errors`` are |fitted - true| in knob units."""
    truth, tlevel = KNOWN_TRUTH[spec.name]
    fd, name = tempfile.mkstemp(suffix=".wav", dir=ev.work)
    os.close(fd)
    out = Path(name)
    try:
        yref = render(spec.preset(truth, tlevel, ev.model_version), ev.probe_wav, out, ev.work)
    finally:
        out.unlink(missing_ok=True)
    ref = features(yref, ev.probe, ev.layout, ev.slots)
    cres, _ = evaluate_params(ref, ev, *truth, *([LEVEL_REF] if not spec.level_pure_gain else []))
    ev.cache.clear()
    free = fit_free(ref, ev, seed, restarts, popsize, generations, refine_generations)
    fvec = [free[k] for k in spec.knobs] + ([free[spec.level_key]] if not spec.level_pure_gain else [])
    fres, _ = evaluate_params(ref, ev, *fvec)
    errs = {k: abs(fres["params"][k] - t) for k, t in zip(spec.knobs, truth)}
    return {"model_version": ev.model_version or spec.default_version, "seed": seed,
            "truth": {**dict(zip(spec.knobs, truth)), spec.level_key: tlevel},
            "constrained": {k: v for k, v in cres.items() if k not in ("residual_ltas_db",)},
            "free": {k: v for k, v in fres.items() if k not in ("residual_ltas_db", "restarts")},
            "knob_errors": errs, "level_error": abs(fres["params"][spec.level_key] - tlevel),
            "n_renders": ev.n_renders}


def run_known_answers(a: argparse.Namespace) -> int:
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    probe, layout, slots = build_probe(Path(a.di) if a.di else KNOWN_DI, KNOWN_LAYOUT)
    probe_wav = work / "probe_known.wav"
    sf.write(str(probe_wav), probe, FS, subtype="FLOAT")
    names = [a.pedal] if a.pedal_given else list(PEDALS)
    res = {}
    for n in names:
        ev = Evaluator(probe_wav, probe, layout, slots, work, jobs=a.jobs, spec=PEDALS[n],
                       model_version=a.model_version, harm_floor_db=a.harm_floor)
        res[n] = known_answer(PEDALS[n], ev, SEED, a.restarts, a.popsize, a.generations, a.refine_generations)
        f = res[n]["free"]
        print(f"{n}: free ltas {f['ltas_rms_db']:.3f} harm {f['harm_rms_db']:.3f} dyn {f['dyn_db']:.3f} "
              f"knob errors {({k: round(v, 2) for k, v in res[n]['knob_errors'].items()})}", flush=True)
    doc = {"schema": "sawblade.pedal_known_answers", "version": 1, "seed": SEED, "harm_floor_db": a.harm_floor,
           "probe": f"{layout.sweep_s:g} s sweep + {layout.steps_s:g} s stepped sines + {layout.di_s:g} s DI "
                    f"({Path(a.di).name if a.di else KNOWN_DI.name})",
           "search": {"restarts": a.restarts, "popsize": a.popsize, "generations": a.generations,
                      "refine_generations": a.refine_generations}, "pedals": res}
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "known_answers.json").write_text(json.dumps(doc, indent=1))
    print(f"wrote {out / 'known_answers.json'}")
    return 0


# ---------------------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------------------
DEFAULT_TARGETS = REPO / "docs" / "reports" / "v0_4" / "targets.json"


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-calibrate pedal-fit",
                                description="Fit a modeled pedal block (pedal.hm / hmx / eye / muff / ts) to TONE3000 "
                                            "captures of its real-pedal family. Writes fits_<pedal>.json and PNG "
                                            "plots. Phase 7.1 behaviour: --pedal hm --model-version 1 "
                                            "[--harm-floor -70] [--out docs/reports/phase7_1].")
    p.add_argument("--pedal", choices=sorted(PEDALS), default="hm", help="block type to fit (default hm)")
    p.add_argument("--model-version", type=int, default=None,
                   help="modelVersion of the block (default: its current version; hm = 3)")
    p.add_argument("--targets", default=None, metavar="PATH",
                   help="targets manifest (docs/reports/v0_4/targets.json). Default: the built-in 7.1 list for hm, "
                        "the manifest for the other pedals")
    p.add_argument("--harm-floor", type=float, default=HARM_FIXED_FLOOR_DB,
                   help=f"floor (dB re fundamental) of the harmonic term in the cost (default {HARM_FIXED_FLOOR_DB:g}; "
                        f"{HARM_FLOOR_DB:g} reproduces the 7.1 cost)")
    p.add_argument("--cache", default=str(DEFAULT_CACHE), help="capture cache (default ~/.cache/sawblade/captures)")
    p.add_argument("--di", default=None, help="DI for the probe's last 30 s (default testdata/gatecreeper_cover/"
                                                "Guitar_L.wav; --known-answers: tests/fixtures/di_riff.wav)")
    p.add_argument("--work", required=True, help="scratch dir for rendered audio (outside the repo)")
    p.add_argument("--out", default=str(REPO / "docs" / "reports" / "v0_4"), help="report dir (fits JSON + PNGs)")
    p.add_argument("--fits-name", default=None, help="fits file name inside --out (default fits_<pedal>.json)")
    p.add_argument("--tone", type=int, action="append", help="restrict to these tone ids (repeatable)")
    p.add_argument("--model", type=int, action="append", help="restrict to these model ids (repeatable)")
    p.add_argument("--restarts", type=int, default=3)
    p.add_argument("--popsize", type=int, default=8)
    p.add_argument("--generations", type=int, default=14)
    p.add_argument("--refine-generations", type=int, default=16,
                   help="small-step CMA-ES stage after the restarts (default 16; 0 = the phase 7.1 search)")
    p.add_argument("--jobs", type=int, default=4, help="parallel tonerender processes per CMA generation")
    p.add_argument("--no-plots", action="store_true")
    p.add_argument("--short-probe", action="store_true",
                   help="use the 8 s test probe instead of the 45 s probe (tests and smoke runs; not comparable "
                        "with full-probe fits)")
    p.add_argument("--merge", action="store_true",
                   help="merge into an existing fits file instead of replacing it (schema 1 files from phase 7.1 are "
                        "refused: their harmonic term and records are not comparable)")
    p.add_argument("--known-answers", action="store_true",
                   help="do not read captures: render the pedal(s) at known params as the capture, fit, and write "
                        "known_answers.json (all five pedals unless --pedal is given)")
    return p


def doc_header(a: argparse.Namespace, spec: PedalSpec, layout: ProbeLayout) -> dict:
    return {"schema": "sawblade.pedal_fit", "version": SCHEMA_VERSION, "seed": SEED, "pedal": spec.name,
            "block_type": spec.block_type, "model_version": a.model_version or spec.default_version,
            "searched_knobs": list(spec.knobs), "level_key": spec.level_key,
            "level_solved": "closed form (pure output gain)" if spec.level_pure_gain else "searched",
            "probe": {"layout": layout.__dict__, "di": str(Path(a.di).name), "di_bpm": 140.0, "di_start_bar": 20,
                      "rate": FS, "step_freqs": STEP_FREQS, "step_levels_dbfs": STEP_LEVELS_DB},
            "cost": {"ltas": "1/3-oct 60 Hz-12 kHz dB RMS, DI segment, offset removed",
                     "harm_weight": W_HARM, "dyn_weight": W_DYN, "harm_floor_db": a.harm_floor,
                     "harm_floor_legacy_db": HARM_FLOOR_DB, "harm_terms": ["harm_rms_db", "harm_even_rms_db",
                                                                           "harm_odd_rms_db", "harm_rms_db_legacy"],
                     "alignment": f"sweep cross-correlation, lag in [{LAG_MIN}, {LAG_MAX}] samples, recorded per fit"},
            "bands_hz": BAND_CENTRES.tolist(), "renderer": "tonerender (C++ core) via subprocess",
            "search": {"optimiser": "CMA-ES (matcher.cma)", "restarts": a.restarts, "popsize": a.popsize,
                       "generations": a.generations, "refine_generations": a.refine_generations,
                       "refine_sigma_knob_units": 10 * REFINE_SIGMA}}


def load_previous(path: Path, a: argparse.Namespace, spec: PedalSpec) -> dict:
    """Existing fits file for ``--merge``. Refuses (clear message) anything that is not comparable."""
    prev = json.loads(path.read_text())
    v = int(prev.get("version", 1))
    if prev.get("schema") != "sawblade.pedal_fit" or v < SCHEMA_VERSION:
        raise ValueError(f"{path} is a schema-{v} fits file (phase 7.1: pedal.hm v1, floor -70 dB harmonic term, no "
                         "capture profiles) and cannot be merged with schema-" f"{SCHEMA_VERSION} records; fit again "
                         "into a new file (--fits-name) instead")
    want = a.model_version or spec.default_version
    if prev.get("pedal") != spec.name or prev.get("model_version") != want:
        raise ValueError(f"{path} holds pedal {prev.get('pedal')} modelVersion {prev.get('model_version')}, not "
                         f"{spec.name} modelVersion {want}")
    if prev.get("cost", {}).get("harm_floor_db") != a.harm_floor:
        raise ValueError(f"{path} was fitted with harmonic floor {prev.get('cost', {}).get('harm_floor_db')} dB, "
                         f"not {a.harm_floor} dB")
    return prev


def run(a: argparse.Namespace) -> int:
    spec = PEDALS[a.pedal]
    if a.harm_floor < HARM_FLOOR_DB:
        raise ValueError(f"--harm-floor must be >= {HARM_FLOOR_DB:g} (the profiles' own floor)")
    if a.model_version is not None and a.model_version not in spec.versions:
        raise ValueError(f"pedal.{spec.name} has no modelVersion {a.model_version} (known: {list(spec.versions)})")
    if a.known_answers:
        return run_known_answers(a)
    work = Path(a.work)
    out = Path(a.out)
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    (work / "refs").mkdir(exist_ok=True)
    targets = Path(a.targets) if a.targets else (None if spec.name == "hm" else DEFAULT_TARGETS)
    if targets is None:
        found, missing = load_targets(Path(a.cache))
    else:
        tones, ent = load_manifest_pedal(targets, spec.name)
        found, missing = load_targets(Path(a.cache), tones, ent)
    licenses = {r["model_id"]: r["license"] for r in found + missing}
    if a.tone:
        found = [r for r in found if r["tone_id"] in a.tone]
    if a.model:
        found = [r for r in found if r["model_id"] in a.model]
    a.di = a.di or str(DEFAULT_DI)
    probe, layout, slots = build_probe(Path(a.di), KNOWN_LAYOUT if a.short_probe else ProbeLayout())
    probe_wav = work / "probe.wav"
    sf.write(str(probe_wav), probe, FS, subtype="FLOAT")
    ev = Evaluator(probe_wav, probe, layout, slots, work, jobs=a.jobs, spec=spec, model_version=a.model_version,
                   harm_floor_db=a.harm_floor)
    fits_path = out / (a.fits_name or f"fits_{spec.name}.json")
    head = doc_header(a, spec, layout)
    prev = load_previous(fits_path, a, spec) if (a.merge and fits_path.exists()) else {"models": []}
    done = {m["model_id"]: m for m in prev["models"]}
    if a.merge:
        found = [r for r in found if r["model_id"] not in done]
    for rec in found:
        r, feats = fit_model(rec, ev, work / "refs", a.restarts, a.popsize, a.generations,
                             refine_generations=a.refine_generations)
        done[rec["model_id"]] = r
        fits_path.write_text(json.dumps({**head, "partial": True,
                                         "models": sorted(done.values(), key=lambda m: m["model_id"])}))  # crash-safe
        if not a.no_plots:
            model_plot(out / f"model_{spec.name}_{rec['tone_id']}_{rec['model_id']}.png", r, feats)
    for m in done.values():   # CLAUDE.md: anything derived from a cc-by-nc* capture is marked non-commercial
        m.setdefault("non_commercial", str(licenses.get(m["model_id"]) or "").lower().startswith("cc-by-nc"))
    fits = sorted(done.values(), key=lambda m: (m["tone_id"], m["model_id"]))
    agg: dict = {}
    stock = [f for f in fits if f["group"] == "stock"]
    if spec.name == "hm" and stock:    # the phase 7.1 aggregates (HM-2 knob map, EQ residual, Custom mode)
        agg["knob_map"] = knob_map(fits)
        agg["mean_free_ltas_rms_db_stock"] = float(np.mean([f["free"]["ltas_rms_db"] for f in stock]))
        agg["mean_constrained_ltas_rms_db_stock"] = float(np.mean([f["constrained"]["ltas_rms_db"] for f in stock
                                                                   if "constrained" in f]))
        agg["mean_residual_ltas_db_stock_free"] = mean_residual(fits).tolist()
        agg["mean_residual_ltas_db_stock_constrained"] = mean_residual(fits, which="constrained").tolist()
        agg["eq_correction"] = eq_correction_report(fits)
        agg["custom_mode"] = custom_mode_report(fits, work / "refs", ev)
        if not a.no_plots:
            aggregate_plot(out / f"aggregate_{spec.name}.png", fits, agg["knob_map"], mean_residual(fits))
    doc = {**head, "missing_models": [{k: m[k] for k in ("tone_id", "model_id", "name")} for m in missing],
           "models": fits, "spread": spread_report(fits), "aggregate": agg}
    fits_path.write_text(json.dumps(doc, indent=1))
    print(f"wrote {fits_path}")
    if "mean_free_ltas_rms_db_stock" in agg:
        print(f"mean free-fit LTAS error over stock HM-2 models: {agg['mean_free_ltas_rms_db_stock']:.2f} dB RMS")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    argv = list(argv) if argv is not None else sys.argv[1:]
    a = build_parser().parse_args(argv)
    a.pedal_given = any(x == "--pedal" or x.startswith("--pedal=") for x in argv)
    try:
        return run(a)
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
