"""Search space: discrete combos (three topologies) + continuous parameter encoding, and preset construction.

Topologies (all live-compatible: one shared cab IR; pedal slots allow "none"; amp slots accept any amp):

* ``single``  [pedal?] -> amp -> cab                      (path A only, path B disabled)
* ``single2`` pedal1 -> pedal2 -> amp -> cab               (path A only)
* ``blend``   A [pedal?] -> amp, B [pedal?] -> amp, shared cab (path roles saw/body are labels only)

Continuous parameters (physical units; the optimizer works in the normalised box [0, 1]):

* blend topology only: ``blend`` 0.05..0.95; ``levelA``/``levelB`` +-6 dB (redundant with blend by design, per spec).
* per path (``a``; ``b`` for blend) path EQ after the blocks (RBJ biquads): 3 peaking bands (q = 1) with log-frequency in
  80-400 / 400-2500 / 2000-8000 Hz and gain +-9 dB, a high-pass 40-300 Hz and a low-pass 3.5-12 kHz (12 dB/oct).
* post EQ (after the shared cab): 3 peaking bands, 100-400 / 400-2500 / 2000-8000 Hz, +-6 dB, q = 1.
* post EQ roll-off (phase 3.4, a real cab + mic rolls off): a high shelf 3-7 kHz, -8..0 dB (q 0.707) and a low-pass
  5-12 kHz (12 dB/oct). Both are neutral at their default (shelf 0 dB, low-pass at 12 kHz = band omitted); neither counts
  toward the EQ-gain regulariser.
* NAM input gains +-12 dB for every NAM block (``gain.a.0``, ``gain.a.amp``, ``gain.b.0`` ...).

Not searched (fixed): gate (from the DI noise floor), NAM output gains (0), loudness normalisation (on for amps),
alignment (resolved once per blend combo, written as manual), bus compressor (off), output gain (set from the level
offset after the search). No fixed pre-EQ (the old HM-2 high-pass on path A was style-specific and is gone).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .pool import Capture

PEAK_RANGES = ((80.0, 400.0), (400.0, 2500.0), (2000.0, 8000.0))
POST_RANGES = ((100.0, 400.0), (400.0, 2500.0), (2000.0, 8000.0))
HP_RANGE, LP_RANGE = (40.0, 300.0), (3500.0, 12000.0)
PEAK_GAIN, POST_GAIN, NAM_GAIN = 9.0, 6.0, 12.0
Q = 1.0
SHELF_RANGE, SHELF_GAIN_RANGE, POST_LP_RANGE = (3000.0, 7000.0), (-8.0, 0.0), (5000.0, 12000.0)
DEFAULT_HP, DEFAULT_LP = 60.0, 9000.0


TOPOLOGY_RANK = {"single": 0, "single2": 1, "blend": 2}      # simplest first (Occam)


@dataclass(frozen=True)
class Combo:
    a_pedals: tuple[Capture, ...]
    a_amp: Capture
    b_pedals: tuple[Capture, ...] | None     # None: no path B (single / single2)
    b_amp: Capture | None
    cab: Capture

    @property
    def topology(self) -> str:
        if self.b_amp is not None:
            return "blend"
        return "single2" if len(self.a_pedals) == 2 else "single"

    def shape(self) -> tuple:
        return (len(self.a_pedals), None if self.b_amp is None else len(self.b_pedals or ()))

    def nams(self) -> list[Capture]:
        out = [*self.a_pedals, self.a_amp]
        if self.b_amp is not None:
            out += [*(self.b_pedals or ()), self.b_amp]
        return out

    def key(self) -> tuple:
        return (self.topology, *[c.key for c in self.nams()], self.cab.key)

    def pair_key(self) -> tuple:
        """Identity without the cab."""
        return self.key()[:-1]

    def captures(self) -> dict:
        """Ordered slot -> capture (for reports); slot names depend on the topology."""
        if self.topology == "blend":
            return {"a_pedal": (self.a_pedals or (None,))[0], "a_amp": self.a_amp,
                    "b_pedal": (self.b_pedals or (None,))[0], "b_amp": self.b_amp, "cab": self.cab}
        if self.topology == "single2":
            return {"pedal1": self.a_pedals[0], "pedal2": self.a_pedals[1], "amp": self.a_amp, "cab": self.cab}
        return {"pedal": (self.a_pedals or (None,))[0], "amp": self.a_amp, "cab": self.cab}

    def with_cab(self, cab: Capture) -> "Combo":
        return Combo(self.a_pedals, self.a_amp, self.b_pedals, self.b_amp, cab)

    def model_bytes(self) -> int:
        return sum(c.size_bytes for c in self.nams())

    def size_rank(self) -> tuple[int, int]:
        """Sum of the capture size ranks (category, 10 % byte bucket): lighter model sets sort first."""
        r = [c.size_rank for c in self.nams()]
        return sum(x[0] for x in r), sum(x[1] for x in r)

    def describe(self) -> str:
        return f"{self.topology}: " + " | ".join(f"{k}={v.title}:{v.name}" for k, v in self.captures().items() if v)


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
    """Parameters for one combo shape ``(n_pedals_a, n_pedals_b | None)``."""

    def __init__(self, shape: tuple):
        na, nb = shape
        ps: list[P] = []
        paths = "a" if nb is None else "ab"
        if nb is not None:
            ps += [P("blend", 0.05, 0.95, 0.55), P("levelA", -6, 6, 0.0), P("levelB", -6, 6, 0.0)]
        for path in paths:
            for i, (lo, hi) in enumerate(PEAK_RANGES):
                ps.append(P(f"{path}.f{i}", lo, hi, float(np.sqrt(lo * hi)), log=True))
                ps.append(P(f"{path}.g{i}", -PEAK_GAIN, PEAK_GAIN, 0.0, eq_gain=True))
            ps.append(P(f"{path}.hp", *HP_RANGE, DEFAULT_HP, log=True))
            ps.append(P(f"{path}.lp", *LP_RANGE, DEFAULT_LP, log=True))
        for i, (lo, hi) in enumerate(POST_RANGES):
            ps.append(P(f"post.f{i}", lo, hi, float(np.sqrt(lo * hi)), log=True))
            ps.append(P(f"post.g{i}", -POST_GAIN, POST_GAIN, 0.0, eq_gain=True))
        ps.append(P("post.shelf_f", *SHELF_RANGE, float(np.sqrt(SHELF_RANGE[0] * SHELF_RANGE[1])), log=True))
        ps.append(P("post.shelf_g", *SHELF_GAIN_RANGE, 0.0))
        ps.append(P("post.lp", *POST_LP_RANGE, POST_LP_RANGE[1], log=True))
        gains = [f"a.{i}" for i in range(na)] + ["a.amp"]
        if nb is not None:
            gains += [f"b.{i}" for i in range(nb)] + ["b.amp"]
        for g in gains:
            ps.append(P(f"gain.{g}", -NAM_GAIN, NAM_GAIN, 0.0, group="gain"))
        self.params = ps
        self.names = [p.name for p in ps]
        self.idx = {p.name: i for i, p in enumerate(ps)}
        self.shape = shape

    @staticmethod
    def for_combo(combo: Combo) -> "Space":
        return Space(combo.shape())

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
    bands = [_peak(v[f"post.f{i}"], v[f"post.g{i}"]) for i in range(3)]
    if "post.shelf_g" in v and v["post.shelf_g"] < -1e-3:
        bands.append({"type": "highShelf", "freq": float(v["post.shelf_f"]), "gainDb": float(v["post.shelf_g"]), "q": 0.707})
    if "post.lp" in v and v["post.lp"] < POST_LP_RANGE[1] * 0.999:
        bands.append({"type": "lowPass", "freq": float(v["post.lp"]), "q": 0.707})
    return bands


def gate_preset(noise_floor_db: float, extra: dict | None = None) -> dict:
    """Fixed gate ("medium"): open at the DI noise floor + 4 dB, hold 40 ms, release 150 ms, range -50 dB."""
    g = {"enabled": True, "thresholdDb": round(noise_floor_db + 4.0, 2), "hysteresisDb": 6.0, "attackMs": 0.5,
         "holdMs": 40.0, "releaseMs": 150.0, "rangeDb": -50.0}
    if extra:  # phase 3.5 fields (mode, ratio, keyHighPassHz, releaseCurve) pass straight through
        g.update(extra)
    return g


def _nam(id_, slot, cap: Capture, gain_db: float, normalize: bool) -> dict:
    b = {"id": id_, "type": "nam", "slot": slot, "model": cap.block_model()}
    if gain_db:
        b["inputGainDb"] = float(gain_db)
    if normalize:
        b["normalizeLoudness"] = True
    return b


def chain_blocks(pfx: str, pedals, amp: Capture, v: dict[str, float]) -> list[dict]:
    """Blocks of one path: pedals then the amp (ids a1.., b1..; amps get loudness normalisation)."""
    blocks = [_nam(f"{pfx}{i + 1}", "pedal", p, v.get(f"gain.{pfx}.{i}", 0.0), False) for i, p in enumerate(pedals)]
    blocks.append(_nam(f"{pfx}{len(pedals) + 1}", "amp", amp, v.get(f"gain.{pfx}.amp", 0.0), True))
    return blocks


def path_blocks(combo: Combo, v: dict[str, float], path: str) -> list[dict]:
    if path == "a":
        return chain_blocks("a", combo.a_pedals, combo.a_amp, v)
    return chain_blocks("b", combo.b_pedals or (), combo.b_amp, v)


def build_preset(combo: Combo, v: dict[str, float], *, gate: dict | None, align: dict, output_db: float = 0.0,
                 name: str = "Matched tone", notes: str = "", levels=None) -> dict:
    """Full ``sawblade.preset`` for a combo and physical parameter values (live-compatible shared cab)."""
    blend = combo.topology == "blend"
    pa = {"role": "saw" if blend else "body", "blocks": path_blocks(combo, v, "a"), "eq": path_eq(v, "a"),
          "levelDb": float(v.get("levelA", 0.0))}
    pb = ({"role": "body", "blocks": path_blocks(combo, v, "b"), "eq": path_eq(v, "b"), "levelDb": float(v["levelB"])}
          if blend else {"role": "body", "enabled": False, "blocks": []})
    p = {
        "schema": "sawblade.preset", "version": 1, "name": name, "notes": notes,
        "gate": gate if gate else {"enabled": False},
        "paths": {"a": pa, "b": pb},
        "align": align, "blend": float(v["blend"]) if blend else 0.0,
        "cab": {"mode": "shared", "ir": combo.cab.block_model(), "enabled": True},
        "postEq": post_eq(v),
        "output": {"gainDb": float(output_db)},
    }
    if blend and levels is not None:
        # phase 10.1: ``v["blend"]`` is the level-matched *linear* blend fitted after the trims; emit the same A:B ratio on
        # the constant-loudness law, with the trims as manual level match.
        from .levelmatch import blend_to_constant_loudness
        p["levelMatch"] = levels.preset_block()
        p["blend"] = blend_to_constant_loudness(float(v["blend"]))
        p["blendLaw"] = "constantLoudness"
    return p


def manual_align(delay_b: int = 0, invert_b: bool = False) -> dict:
    return {"mode": "manual", "delaySamplesB": int(delay_b), "invertB": bool(invert_b)}
