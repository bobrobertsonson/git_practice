"""Sawblade-owned deterministic NAM training signal (no third-party audio, no licence to carry).

Everything is generated from one integer ``seed`` with ``numpy.random.SeedSequence(seed).spawn`` (one independent
stream per segment, so changing the length of one segment never shifts the others); the same seed and the same
``SIGNAL_VERSION`` give bit-identical float32 output on any machine with the same numpy. ``signal_hash`` (sha256 of
the float32 bytes) is recorded in every export report.

Layout of the **training** part (48 kHz, mono, float32; every block is separated by 0.4 s of digital silence, and the
signal starts with 1.0 s of silence)::

    pink-noise level steps    band-limited 40 Hz-10 kHz, RMS -48 .. -12 dBFS in 6 dB steps, 2.0 s each
    white-noise level steps   full band, RMS -45/-33/-21/-12 dBFS, 1.5 s each
    log sweeps                sine 30 Hz -> 12 kHz (up) and 12 kHz -> 30 Hz (down), peak -24/-12/-3 dBFS, 4 s each
    synthetic plucks          chord-like additive tones (see below), >= 135 s

and a separate **held-out validation** segment (own seed stream, not in the training part): pink-noise steps at
-39/-27/-15 dBFS, one sweep, 4 pluck phrases.  Digital silence in the middle of the validation segment exercises
the chain's noise floor behaviour.

A synthetic pluck event is a *phrase*: one chord (root MIDI 33-57, i.e. A1-A3, so down-tuned playing is covered;
voicings: single note, octave, power chord 0-7-12, 0-7-12-16 minor-less, 0-7-12-15, 0-6-12 tritone, 0-5-12) played as
a strum (0-12 ms between notes), repeated 1-16 times at a rhythm of 60-500 ms (palm-muted chugs: tau 0.05-0.25 s;
open ringing chords: tau 0.6-2.5 s; tremolo bursts: 60-100 ms) with per-hit velocity (peak -30 .. -1 dBFS
log-uniform, accents).  Each note is a sum of up to 24 partials (amplitude ``n^-1.1``, inharmonic stretch
``f_n = n f0 sqrt(1 + 4e-5 n^2)``, partial decay ``tau / (1 + 0.35 n)``, 2 ms attack) plus a short noise burst (pick
noise, 1.5-5 kHz band).  Events overlap (polyphony) because each ring-out is mixed in at its onset.

No gates, reverb, delay or compression are used anywhere (they must not reach NAM training).
"""
from __future__ import annotations

import hashlib
from dataclasses import asdict, dataclass

import numpy as np
from scipy import signal as sps

SIGNAL_VERSION = 1
RATE = 48000

PINK_LEVELS_DB = (-48, -42, -36, -30, -24, -18, -12)
WHITE_LEVELS_DB = (-45, -33, -21, -12)
SWEEP_PEAKS_DB = (-24, -12, -3)
VOICINGS = ((0,), (0, 12), (0, 7, 12), (0, 7, 12, 16), (0, 7, 12, 15), (0, 6, 12), (0, 5, 12))
PEAK_CEILING_DB = -1.0


@dataclass(frozen=True)
class SignalSpec:
    seed: int = 1
    train_plucks_s: float = 135.0     # pluck block seconds (total training part >= 3 min with the noise/sweep blocks)
    valid_plucks: int = 4             # phrases in the validation segment

    def to_json(self) -> dict:
        return {"version": SIGNAL_VERSION, "rate": RATE, **asdict(self)}


def _db(x: float) -> float:
    return 10.0 ** (x / 20.0)


def _fade(x: np.ndarray, n: int = 2400) -> np.ndarray:
    n = min(n, len(x) // 2)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    x[:n] *= w
    x[-n:] *= w[::-1]
    return x


def _silence(seconds: float) -> np.ndarray:
    return np.zeros(int(round(seconds * RATE)), np.float64)


def _scale_rms(x: np.ndarray, rms_db: float) -> np.ndarray:
    return x * (_db(rms_db) / max(float(np.sqrt(np.mean(x * x))), 1e-12))


def _pink(rng, n: int) -> np.ndarray:
    """Pink-ish noise (1/f power) via FFT shaping, band-limited to 40 Hz-10 kHz."""
    w = rng.standard_normal(n)
    spec = np.fft.rfft(w)
    f = np.fft.rfftfreq(n, 1.0 / RATE)
    shape = 1.0 / np.sqrt(np.maximum(f, 40.0))
    shape[(f < 40.0) | (f > 10000.0)] = 0.0
    return np.fft.irfft(spec * shape, n)


def noise_steps(rng, kind: str, levels, seconds: float) -> np.ndarray:
    parts = []
    for lv in levels:
        n = int(round(seconds * RATE))
        x = _pink(rng, n) if kind == "pink" else rng.standard_normal(n)
        parts.append(_fade(_scale_rms(x, lv)))
    return np.concatenate(parts)


def log_sweep(f0: float, f1: float, seconds: float, peak_db: float) -> np.ndarray:
    n = int(round(seconds * RATE))
    t = np.arange(n) / RATE
    k = np.log(f1 / f0)
    phase = 2 * np.pi * f0 * seconds / k * (np.exp(t / seconds * k) - 1.0)
    return _fade(np.sin(phase) * _db(peak_db), 1200)


def _note(rng, f0: float, tau: float, length_s: float) -> np.ndarray:
    n = int(length_s * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)
    atk = np.minimum(t / 0.002, 1.0)
    nmax = int(min(24, 0.45 * RATE / f0))
    for k in range(1, nmax + 1):
        fk = k * f0 * np.sqrt(1.0 + 4e-5 * k * k)
        if fk > 0.45 * RATE:
            break
        a = k ** -1.1 * (0.7 + 0.6 * rng.random())
        dec = np.exp(-t / (tau / (1.0 + 0.35 * k)))
        out += a * dec * np.sin(2 * np.pi * fk * t + 2 * np.pi * rng.random())
    nb = int(0.012 * RATE)
    burst = rng.standard_normal(nb) * np.exp(-np.arange(nb) / (0.003 * RATE))
    sos = sps.butter(2, [1500, 5000], btype="bandpass", fs=RATE, output="sos")
    out[:nb] += 0.35 * sps.sosfilt(sos, burst)
    return out * atk


def pluck_phrase(rng) -> np.ndarray:
    """One phrase (chord repeated 1-16 times); returned unscaled except that its peak is the random velocity."""
    root = int(rng.integers(33, 58))
    voicing = VOICINGS[int(rng.integers(len(VOICINGS)))]
    style = rng.choice(["mute", "ring", "tremolo", "mixed"], p=[0.45, 0.25, 0.15, 0.15])
    if style == "mute":
        tau, gap, reps = rng.uniform(0.05, 0.25), rng.choice([0.125, 0.16, 0.2, 0.25, 0.3]), int(rng.integers(4, 13))
    elif style == "ring":
        tau, gap, reps = rng.uniform(0.6, 2.5), rng.choice([0.5, 0.8, 1.2]), int(rng.integers(1, 4))
    elif style == "tremolo":
        tau, gap, reps = rng.uniform(0.08, 0.3), rng.uniform(0.06, 0.1), int(rng.integers(8, 17))
    else:
        tau, gap, reps = rng.uniform(0.1, 0.9), rng.uniform(0.1, 0.5), int(rng.integers(2, 9))
    gap = float(gap)
    nlen = min(3.0, max(0.3, 7.0 * tau))
    total = int((gap * reps + nlen + 0.1) * RATE)
    mix = np.zeros(total)
    base_vel = rng.uniform(-30.0, -4.0)
    for r in range(reps):
        vel = base_vel + rng.uniform(-6.0, 3.0) + (4.0 if r % 4 == 0 else 0.0)
        vel = min(vel, -1.0)
        hit = np.zeros(int(nlen * RATE) + int(0.06 * RATE))
        for j, iv in enumerate(voicing):
            f0 = 440.0 * 2.0 ** ((root + iv - 69) / 12.0)
            off = int(j * rng.uniform(0.0, 0.012) * RATE)
            nt = _note(rng, f0, float(tau), nlen)
            hit[off:off + len(nt)] += nt / np.sqrt(len(voicing))
        hit *= _db(vel) / max(float(np.max(np.abs(hit))), 1e-9)
        s = int(r * gap * RATE)
        end = min(total, s + len(hit))
        mix[s:end] += hit[:end - s]
    return mix


def plucks(rng, seconds: float, gap_s: float = 0.15) -> np.ndarray:
    parts, n = [], 0
    while n < seconds * RATE:
        ph = pluck_phrase(rng)
        parts += [ph, _silence(gap_s * (0.5 + rng.random()))]
        n += len(parts[-2]) + len(parts[-1])
    return np.concatenate(parts)


def _finish(x: np.ndarray) -> np.ndarray:
    pk = float(np.max(np.abs(x)))
    ceil = _db(PEAK_CEILING_DB)
    if pk > ceil:
        x = x * (ceil / pk)
    return x.astype(np.float32)


def generate(spec: SignalSpec = SignalSpec()) -> tuple[np.ndarray, np.ndarray, dict]:
    """(train, valid, info): float32 mono 48 kHz arrays and a description (sections with sample ranges, levels)."""
    ss = np.random.SeedSequence(spec.seed)
    r_pink, r_white, r_pl, r_vp, r_vpl = (np.random.default_rng(s) for s in ss.spawn(5))
    gap = _silence(0.4)
    sections: list[tuple[str, np.ndarray]] = [("silence", _silence(1.0))]
    sections.append(("pink_steps", noise_steps(r_pink, "pink", PINK_LEVELS_DB, 2.0)))
    sections.append(("white_steps", noise_steps(r_white, "white", WHITE_LEVELS_DB, 1.5)))
    for pk in SWEEP_PEAKS_DB:
        sections.append((f"sweep_up_{pk}", log_sweep(30.0, 12000.0, 4.0, pk)))
        sections.append((f"sweep_down_{pk}", log_sweep(12000.0, 30.0, 4.0, pk)))
    sections.append(("plucks", plucks(r_pl, spec.train_plucks_s)))
    train, info_sections, pos = [], [], 0
    for name, x in sections:
        info_sections.append({"name": name, "start": pos, "end": pos + len(x)})
        train += [x, gap]
        pos += len(x) + len(gap)
    train_a = _finish(np.concatenate(train))

    v = [("silence", _silence(1.0)), ("pink_steps", noise_steps(r_vp, "pink", (-39, -27, -15), 2.0)),
         ("sweep", log_sweep(40.0, 10000.0, 4.0, -9.0))]
    rng = r_vpl
    ph = []
    for k in range(spec.valid_plucks):
        ph += [pluck_phrase(rng), _silence(0.1 + 0.5 * rng.random() if k != spec.valid_plucks // 2 else 1.2)]
    v.append(("plucks", np.concatenate(ph)))
    valid, vsec, pos = [], [], 0
    for name, x in v:
        vsec.append({"name": name, "start": pos, "end": pos + len(x)})
        valid += [x, gap]
        pos += len(x) + len(gap)
    valid_a = _finish(np.concatenate(valid))

    def lv(x):
        a = x.astype(np.float64)
        return {"peakDbfs": float(20 * np.log10(np.max(np.abs(a)))), "rmsDbfs": float(10 * np.log10(np.mean(a * a)))}

    info = {"spec": spec.to_json(), "trainSeconds": len(train_a) / RATE, "validSeconds": len(valid_a) / RATE,
            "trainSections": info_sections, "validSections": vsec, "train": lv(train_a), "valid": lv(valid_a),
            "trainSha256": signal_hash(train_a), "validSha256": signal_hash(valid_a)}
    return train_a, valid_a, info


def signal_hash(x: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(x, dtype="<f4").tobytes()).hexdigest()
