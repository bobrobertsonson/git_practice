"""Search space: discrete combos + continuous parameter encoding, and preset construction.

Continuous parameters (all in physical units; the optimizer works in the normalised box [0, 1]):

* ``blend`` 0.05..0.95;  ``levelA``/``levelB`` +-6 dB (path trims; redundant with blend by design, per spec).
* per path (``a``/``b``) path EQ after the blocks (RBJ biquads): 3 peaking bands (q = 1) with log-frequency in
  80-400 / 400-2500 / 2000-8000 Hz and gain +-9 dB, a high-pass 40-300 Hz and a low-pass 3.5-12 kHz (12 dB/oct).
* post EQ (after the shared cab): 3 peaking bands, 100-400 / 400-2500 / 2000-8000 Hz, +-6 dB, q = 1.
* NAM input gains +-12 dB: pedal A, amp A, boost B (if present), amp B.

Not searched (fixed): gate (from the DI noise floor), A pre-EQ (high-pass 90 Hz as in presets/chainsaw_body.json),
NAM output gains (0), loudness normalisation (on for amps), alignment (resolved once per combo, written as manual),
bus compressor (off), output gain (set from the level offset after the search).
"""
from __future__ import annotations

import copy
from dataclasses import dataclass

import numpy as np

from .pool import Capture

PEAK_RANGES = ((80.0, 400.0), (400.0, 2500.0), (2000.0, 8000.0))
POST_RANGES = ((100.0, 400.0), (400.0, 2500.0), (2000.0, 8000.0))
HP_RANGE, LP_RANGE = (40.0, 300.0), (3500.0, 12000.0)
PEAK_GAIN, POST_GAIN, NAM_GAIN = 9.0, 6.0, 12.0
Q = 1.0
DEFAULT_HP, DEFAULT_LP = 60.0, 9000.0


@dataclass(frozen=True)
class Combo:
    hm2: Capture
    saw_amp: Capture
    boost: Capture | None
    body_amp: Capture
    cab: Capture

    def key(self) -> tuple:
        return (self.hm2.key, self.saw_amp.key, self.boost.key if self.boost else None, self.body_amp.key, self.cab.key)

    def captures(self) -> dict:
        return {"hm2": self.hm2, "saw_amp": self.saw_amp, "boost": self.boost, "body_amp": self.body_amp,
                "cab": self.cab}

    def with_cab(self, cab: Capture) -> "Combo":
        return Combo(self.hm2, self.saw_amp, self.boost, self.body_amp, cab)

    def model_bytes(self) -> int:
        return sum(c.size_bytes for c in (self.hm2, self.saw_amp, self.boost, self.body_amp) if c)

    def size_rank(self) -> tuple[int, int]:
        """Sum of the capture size ranks (category, 10 % byte bucket): lighter model sets sort first."""
        r = [c.size_rank for c in (self.hm2, self.saw_amp, self.boost, self.body_amp) if c]
        return sum(x[0] for x in r), sum(x[1] for x in r)


@dataclass(frozen=True)
class P:
    name: str
    lo: float
    hi: float
    default: float
    log: bool = False
    group: str = "linear"      # "linear" (cheap, post-NAM) | "gain" (needs NAM re-render)
    eq_gain: bool = False      # counts toward the EQ-gain regulariser


class Space:
    def __init__(self, has_boost: bool):
        ps: list[P] = [P("blend", 0.05, 0.95, 0.55), P("levelA", -6, 6, 0.0), P("levelB", -6, 6, 0.0)]
        for path in "ab":
            for i, (lo, hi) in enumerate(PEAK_RANGES):
                ps.append(P(f"{path}.f{i}", lo, hi, float(np.sqrt(lo * hi)), log=True))
                ps.append(P(f"{path}.g{i}", -PEAK_GAIN, PEAK_GAIN, 0.0, eq_gain=True))
            ps.append(P(f"{path}.hp", *HP_RANGE, DEFAULT_HP, log=True))
            ps.append(P(f"{path}.lp", *LP_RANGE, DEFAULT_LP, log=True))
        for i, (lo, hi) in enumerate(POST_RANGES):
            ps.append(P(f"post.f{i}", lo, hi, float(np.sqrt(lo * hi)), log=True))
            ps.append(P(f"post.g{i}", -POST_GAIN, POST_GAIN, 0.0, eq_gain=True))
        gains = ["a.pedal", "a.amp"] + (["b.boost"] if has_boost else []) + ["b.amp"]
        for g in gains:
            ps.append(P(f"gain.{g}", -NAM_GAIN, NAM_GAIN, 0.0, group="gain"))
        self.params = ps
        self.names = [p.name for p in ps]
        self.idx = {p.name: i for i, p in enumerate(ps)}
        self.has_boost = has_boost

    def __len__(self) -> int:
        return len(self.params)

    def indices(self, group: str) -> list[int]:
        return [i for i, p in enumerate(self.params) if p.group == group]

    def default(self) -> dict[str, float]:
        return {p.name: p.default for p in self.params}

    def decode(self, u: np.ndarray) -> dict[str, float]:
        """Normalised vector in [0,1]^n -> physical values."""
        out = {}
        for p, v in zip(self.params, np.clip(u, 0.0, 1.0)):
            out[p.name] = float(np.exp(np.log(p.lo) + v * (np.log(p.hi) - np.log(p.lo)))) if p.log \
                else float(p.lo + v * (p.hi - p.lo))
        return out

    def encode(self, values: dict[str, float]) -> np.ndarray:
        u = np.zeros(len(self.params))
        for i, p in enumerate(self.params):
            v = values.get(p.name, p.default)
            u[i] = (np.log(v) - np.log(p.lo)) / (np.log(p.hi) - np.log(p.lo)) if p.log else (v - p.lo) / (p.hi - p.lo)
        return np.clip(u, 0.0, 1.0)

    def eq_gains(self, values: dict[str, float]) -> np.ndarray:
        return np.array([values[p.name] for p in self.params if p.eq_gain])


def _peak(f, g):
    return {"type": "peak", "freq": float(f), "gainDb": float(g), "q": Q}


def path_eq(v: dict[str, float], path: str) -> list[dict]:
    bands = [{"type": "highPass", "freq": v[f"{path}.hp"], "q": 0.707}]
    bands += [_peak(v[f"{path}.f{i}"], v[f"{path}.g{i}"]) for i in range(3)]
    bands.append({"type": "lowPass", "freq": v[f"{path}.lp"], "q": 0.707})
    return bands


def post_eq(v: dict[str, float]) -> list[dict]:
    return [_peak(v[f"post.f{i}"], v[f"post.g{i}"]) for i in range(3)]


def gate_preset(noise_floor_db: float) -> dict:
    """Fixed gate ("medium"): open at the DI noise floor + 4 dB, hold 40 ms, release 150 ms, range -50 dB."""
    return {"enabled": True, "thresholdDb": round(noise_floor_db + 4.0, 2), "hysteresisDb": 6.0, "attackMs": 0.5,
            "holdMs": 40.0, "releaseMs": 150.0, "rangeDb": -50.0}


def _nam(id_, slot, cap: Capture, gain_db: float, normalize: bool) -> dict:
    b = {"id": id_, "type": "nam", "slot": slot, "model": cap.block_model()}
    if gain_db:
        b["inputGainDb"] = float(gain_db)
    if normalize:
        b["normalizeLoudness"] = True
    return b


A_PREEQ = [{"type": "highPass", "freq": 90.0, "q": 0.707}]


def path_blocks(combo: Combo, v: dict[str, float], path: str) -> list[dict]:
    if path == "a":
        return [_nam("a1", "pedal", combo.hm2, v.get("gain.a.pedal", 0.0), False),
                _nam("a2", "amp", combo.saw_amp, v.get("gain.a.amp", 0.0), True)]
    blocks = []
    if combo.boost is not None:
        blocks.append(_nam("b1", "boost", combo.boost, v.get("gain.b.boost", 0.0), False))
    blocks.append(_nam("b2", "amp", combo.body_amp, v.get("gain.b.amp", 0.0), True))
    return blocks


def build_preset(combo: Combo, v: dict[str, float], *, gate: dict | None, align: dict, output_db: float = 0.0,
                 name: str = "Matched chainsaw + body", notes: str = "") -> dict:
    """Full ``sawblade.preset`` for a combo and physical parameter values (live-compatible shared cab)."""
    return {
        "schema": "sawblade.preset", "version": 1, "name": name, "notes": notes,
        "gate": gate if gate else {"enabled": False},
        "paths": {
            "a": {"role": "saw", "preEq": copy.deepcopy(A_PREEQ), "blocks": path_blocks(combo, v, "a"),
                  "eq": path_eq(v, "a"), "levelDb": float(v["levelA"])},
            "b": {"role": "body", "blocks": path_blocks(combo, v, "b"), "eq": path_eq(v, "b"),
                  "levelDb": float(v["levelB"])},
        },
        "align": align, "blend": float(v["blend"]),
        "cab": {"mode": "shared", "ir": combo.cab.block_model(), "enabled": True},
        "postEq": post_eq(v),
        "output": {"gainDb": float(output_db)},
    }


def manual_align(delay_b: int = 0, invert_b: bool = False) -> dict:
    return {"mode": "manual", "delaySamplesB": int(delay_b), "invertB": bool(invert_b)}
