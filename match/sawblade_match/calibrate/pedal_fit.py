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
HARM_FLOOR_DB = -70.0
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


def hm_preset(low: float, high: float, distortion: float, level: float = LEVEL_REF) -> dict:
    return _preset({"id": "a1", "type": "pedal.hm", "slot": "pedal", "modelVersion": 1,
                    "params": {"level": float(level), "low": float(low), "high": float(high),
                               "distortion": float(distortion)}})


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


def features(y: np.ndarray, probe: np.ndarray, layout: ProbeLayout, slots) -> dict:
    y = np.asarray(y, float)
    if len(y) < layout.n_total:
        y = np.pad(y, (0, layout.n_total - len(y)))
    y = y[:layout.n_total]
    di_y, di_x = y[layout.di_sl], np.asarray(probe, float)[layout.di_sl]
    return {"ltas": ltas_bands(di_y), "harm": harmonic_profile(y, slots), **env_features(di_y, di_x),
            "sweep": sweep_response(y[layout.sweep_sl], np.asarray(probe, float)[layout.sweep_sl])}


def level_limits_db() -> tuple[float, float]:
    """Output-gain range of the level knob relative to level 8: (-24, +6) dB."""
    return DB_PER_LEVEL * (0.0 - LEVEL_REF), DB_PER_LEVEL * (10.0 - LEVEL_REF)


def compare(ref: dict, mod: dict) -> dict:
    """Cost of a model render's features against the reference's; level solved in closed form."""
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
    harm = float(np.sqrt(np.mean((ref["harm"] - mod["harm"]) ** 2)))
    dcrest = ref["crest_db"] - mod["crest_db"]
    dspread = ref["env_spread_db"] - mod["env_spread_db"]
    dyn = float(np.sqrt(0.5 * (dcrest ** 2 + dspread ** 2)))
    return {"cost": ltas + W_HARM * harm + W_DYN * dyn, "ltas_rms_db": ltas, "harm_rms_db": harm, "dyn_db": dyn,
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

    def features(self, low: float, high: float, dist: float) -> dict:
        key = (round(low, 4), round(high, 4), round(dist, 4))
        if key not in self.cache:
            fd, name = tempfile.mkstemp(suffix=".wav", dir=self.work)
            os.close(fd)
            out = Path(name)
            try:
                y = render(hm_preset(*key), self.probe_wav, out, self.work)
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


def _minimize(fn_batch, x0, seed: int, popsize: int, generations: int):
    from ..matcher.cma import minimize
    return minimize(None, x0, sigma0=0.3, popsize=popsize, generations=generations, seed=seed,
                    evaluate_batch=fn_batch)


def fit_free(ref: dict, ev: Evaluator, seed: int, restarts: int = 3, popsize: int = 8, generations: int = 14) -> dict:
    """CMA-ES over (low, high, distortion) in [0, 10]^3, ``restarts`` seeded restarts; best of all."""
    def one(x):
        low, high, dist = (10.0 * float(v) for v in x)
        return compare(ref, ev.features(low, high, dist))["cost"]

    def batch(X):
        with ThreadPoolExecutor(max_workers=ev.jobs) as ex:
            return list(ex.map(one, X))

    rng = np.random.default_rng(seed)
    best = (np.inf, None)
    runs = []
    for r in range(restarts):
        x0 = np.array([0.5, 0.5, 0.5]) if r == 0 else rng.uniform(0.05, 0.95, 3)
        bx, bf, _ = _minimize(batch, x0, seed + 1000 * r, popsize, generations)
        runs.append({"restart": r, "seed": seed + 1000 * r, "x0": (10 * x0).round(3).tolist(),
                     "best": (10 * bx).round(3).tolist(), "cost": float(bf)})
        if bf < best[0]:
            best = (bf, bx)
    low, high, dist = (10.0 * float(v) for v in best[1])
    return {"low": low, "high": high, "distortion": dist, "restarts": runs}


def evaluate_params(ref: dict, ev: Evaluator, low: float, high: float, dist: float) -> dict:
    mod = ev.features(low, high, dist)
    c = compare(ref, mod)
    return {"params": {"level": c["level"], "low": low, "high": high, "distortion": dist}, **c}, mod


# ---------------------------------------------------------------------------------------------------------
# models to fit
# ---------------------------------------------------------------------------------------------------------
LABEL_RE = re.compile(r"Lv-(\d+)\s+L-(\d+)\s+H-(\d+)\s+D-(\d+)")


def parse_labels(name: str) -> dict | None:
    m = LABEL_RE.search(name)
    if not m:
        return None
    lv, lo, hi, d = (float(v) for v in m.groups())
    return {"level": lv, "low": lo, "high": hi, "distortion": d}


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


def load_targets(cache: Path, tones: dict | None = None) -> tuple[list[dict], list[dict]]:
    """(models found in the cache, models listed but missing). Reads pool_manifest.json for names."""
    man = json.loads((cache / "pool_manifest.json").read_text())
    by_tone = {t["tone_id"]: t for t in man["tones"]}
    found, missing = [], []
    for tid, (unit, group, mids) in (tones or DEFAULT_TONES).items():
        t = by_tone.get(tid)
        names = {m["id"]: m["name"] for m in (t["models"] if t else [])}
        for mid in mids:
            f = cache / str(tid) / f"{mid}.nam"
            rec = {"tone_id": tid, "model_id": mid, "name": names.get(mid, str(mid)), "unit": unit, "group": group,
                   "file": f, "creator": (t or {}).get("creator"), "license": (t or {}).get("license")}
            (found if f.is_file() else missing).append(rec)
    return found, missing


def fit_model(rec: dict, ev: Evaluator, ref_wav_dir: Path, restarts: int, popsize: int, generations: int,
              say: Callable[[str], None] = print) -> tuple[dict, dict]:
    ref = reference_features(rec["file"], ev, ref_wav_dir / f"{rec['model_id']}.wav")
    labels = parse_labels(rec["name"])
    assumed = labels is None
    pin = labels or ASSUMED.get(rec["name"])   # exact model-name match; None = no pinned fit
    seed = SEED + int(rec["model_id"])
    ev.cache.clear()
    free = fit_free(ref, ev, seed, restarts, popsize, generations)
    fr, fmod = evaluate_params(ref, ev, free["low"], free["high"], free["distortion"])
    fr["restarts"] = free["restarts"]
    out = {k: rec[k] for k in ("tone_id", "model_id", "name", "unit", "group", "creator", "license")}
    out.update({"non_commercial": str(rec.get("license") or "").lower().startswith("cc-by-nc"),
                "labels": labels, "pinned_knobs": pin, "pinned_is_assumed": assumed, "seed": seed,
                "ref": {"crest_db": ref["crest_db"], "env_spread_db": ref["env_spread_db"]}, "free": fr})
    feats = {"ref": ref, "free": fmod}
    if pin:
        cr, cmod = evaluate_params(ref, ev, float(pin["low"]), float(pin["high"]), float(pin["distortion"]))
        out["constrained"] = cr
        feats["constrained"] = cmod
    say(f"  {rec['model_id']} {rec['name']}: free ltas {fr['ltas_rms_db']:.2f} dB "
        f"(L {fr['params']['low']:.1f} H {fr['params']['high']:.1f} D {fr['params']['distortion']:.1f} "
        f"Lv {fr['params']['level']:.1f})"
        + (f", constrained ltas {out['constrained']['ltas_rms_db']:.2f}" if pin else "")
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
            a.semilogx(fc, feats[key]["ltas"] + g - np.max(ref), col, label=f"pedal.hm {key}")
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
        a.plot(x + 2, feats["ref"]["harm"][sel].mean(axis=0), "-o", color=f"C{li}", ms=3,
               label=f"capture {lv:g} dBFS")
        a.plot(x + 2, feats["free"]["harm"][sel].mean(axis=0), "--", color=f"C{li}", lw=1)
    a.set_title("H2..H7 re fundamental (dB; solid capture, dashed free fit)")
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
# CLI
# ---------------------------------------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-calibrate pedal-fit",
                                description="Fit pedal.hm {level, low, high, distortion} to TONE3000 captures of "
                                            "real HM-2-family pedals (phase 7.1). Writes fits.json and PNG plots.")
    p.add_argument("--cache", default=str(DEFAULT_CACHE), help="capture cache (default ~/.cache/sawblade/captures)")
    p.add_argument("--di", default=str(DEFAULT_DI), help="DI for the probe's last 30 s")
    p.add_argument("--work", required=True, help="scratch dir for rendered audio (outside the repo)")
    p.add_argument("--out", default=str(REPO / "docs" / "reports" / "phase7_1"), help="report dir (fits.json + PNGs)")
    p.add_argument("--tone", type=int, action="append", help="restrict to these tone ids (repeatable)")
    p.add_argument("--model", type=int, action="append", help="restrict to these model ids (repeatable)")
    p.add_argument("--restarts", type=int, default=3)
    p.add_argument("--popsize", type=int, default=8)
    p.add_argument("--generations", type=int, default=14)
    p.add_argument("--jobs", type=int, default=4, help="parallel tonerender processes per CMA generation")
    p.add_argument("--no-plots", action="store_true")
    p.add_argument("--merge", action="store_true", help="merge into an existing fits.json instead of replacing it")
    return p


def run(a: argparse.Namespace) -> int:
    work = Path(a.work)
    out = Path(a.out)
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    (work / "refs").mkdir(exist_ok=True)
    found, missing = load_targets(Path(a.cache))
    licenses = {r["model_id"]: r["license"] for r in found + missing}
    if a.tone:
        found = [r for r in found if r["tone_id"] in a.tone]
    if a.model:
        found = [r for r in found if r["model_id"] in a.model]
    probe, layout, slots = build_probe(Path(a.di))
    probe_wav = work / "probe.wav"
    sf.write(str(probe_wav), probe, FS, subtype="FLOAT")
    ev = Evaluator(probe_wav, probe, layout, slots, work, jobs=a.jobs)
    fits_path = out / "fits.json"
    prev = json.loads(fits_path.read_text()) if (a.merge and fits_path.exists()) else {"models": []}
    done = {m["model_id"]: m for m in prev["models"]}
    if a.merge:
        found = [r for r in found if r["model_id"] not in done]
    for rec in found:
        r, feats = fit_model(rec, ev, work / "refs", a.restarts, a.popsize, a.generations)
        done[rec["model_id"]] = r
        fits_path.write_text(json.dumps({"models": sorted(done.values(), key=lambda m: m["model_id"])}))  # crash-safe
        if not a.no_plots:
            model_plot(out / f"model_{rec['tone_id']}_{rec['model_id']}.png", r, feats)
    for m in done.values():   # CLAUDE.md: anything derived from a cc-by-nc* capture is marked non-commercial
        m.setdefault("non_commercial", str(licenses.get(m["model_id"]) or "").lower().startswith("cc-by-nc"))
    fits = sorted(done.values(), key=lambda m: (m["tone_id"], m["model_id"]))
    stock = [f for f in fits if f["group"] == "stock"]
    agg: dict = {}
    if stock:
        agg["knob_map"] = knob_map(fits)
        agg["mean_free_ltas_rms_db_stock"] = float(np.mean([f["free"]["ltas_rms_db"] for f in stock]))
        agg["mean_constrained_ltas_rms_db_stock"] = float(np.mean([f["constrained"]["ltas_rms_db"] for f in stock
                                                                   if "constrained" in f]))
        agg["mean_residual_ltas_db_stock_free"] = mean_residual(fits).tolist()
        agg["mean_residual_ltas_db_stock_constrained"] = mean_residual(fits, which="constrained").tolist()
        agg["eq_correction"] = eq_correction_report(fits)
        agg["custom_mode"] = custom_mode_report(fits, work / "refs", ev)
        if not a.no_plots:
            aggregate_plot(out / "aggregate.png", fits, agg["knob_map"], mean_residual(fits))
    doc = {"schema": "sawblade.pedal_fit", "version": 1, "seed": SEED,
           "probe": {"layout": layout.__dict__, "di": str(Path(a.di).name), "di_bpm": 140.0, "di_start_bar": 20,
                     "rate": FS, "step_freqs": STEP_FREQS, "step_levels_dbfs": STEP_LEVELS_DB},
           "cost": {"ltas": "1/3-oct 60 Hz-12 kHz dB RMS, DI segment, level solved in closed form",
                    "harm_weight": W_HARM, "dyn_weight": W_DYN, "harm_floor_db": HARM_FLOOR_DB},
           "bands_hz": BAND_CENTRES.tolist(), "renderer": "tonerender (C++ core) via subprocess",
           "search": {"optimiser": "CMA-ES (matcher.cma)", "restarts": a.restarts, "popsize": a.popsize,
                      "generations": a.generations},
           "missing_models": [{k: m[k] for k in ("tone_id", "model_id", "name")} for m in missing],
           "models": fits, "aggregate": agg}
    fits_path.write_text(json.dumps(doc, indent=1))
    print(f"wrote {fits_path}")
    if "mean_free_ltas_rms_db_stock" in agg:
        print(f"mean free-fit LTAS error over stock HM-2 models: {agg['mean_free_ltas_rms_db_stock']:.2f} dB RMS")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    try:
        return run(a)
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
