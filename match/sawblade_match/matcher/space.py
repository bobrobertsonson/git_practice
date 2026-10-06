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
* post-cab filters (v0.4M, always searched unless ``filters=False``): ``post.hp`` 60-140 Hz, and the post low-pass ``post.lp``
  above (5-12 kHz) gets a slope choice. Each has a discrete slope parameter (``post.hp_slope`` / ``post.lp_slope`` in [0, 1]:
  < 0.5 = 12 dB/oct, >= 0.5 = 24 dB/oct, emitted as two cascaded biquads with the 4th-order Butterworth Qs 0.541 / 1.307). The
  ``post.hp`` and the slopes are not CMA-ES dimensions (group ``discrete``): ``refine.pick_slopes`` tries ``post.hp`` on a short
  grid after the last linear block, then 24 dB/oct for each filter. A filter at its range edge (hp 60 Hz, lp 12 kHz) is "off" whatever its slope. They do not
  count toward the EQ-gain regulariser.
* tight boost (v0.4M, ``Combo.boost``; single topology only): a modeled ``pedal.ts`` (slot ``boost``) directly in front of the
  amp, after any pedal: ``boost.drive`` 0-3, ``boost.level`` 6-10, ``boost.tone`` 3-8 (defaults 1 / 8 / 5). The boost renders
  inside the NAM core, so these three belong to the ``gain`` group.
* NAM input gains +-12 dB for every NAM block (``gain.a.0``, ``gain.a.amp``, ``gain.b.0`` ...).

Not searched by the optimiser: gate (starts at the DI noise floor + 4 dB; a threshold x release sweep on the final chain follows stage 2, see gatesweep.py), NAM output gains (0), loudness normalisation (on for amps),
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
POST_HP_RANGE = (60.0, 140.0)
SLOPE_DEFAULT = 0.4                       # discrete slope parameter: < 0.5 -> 12 dB/oct, >= 0.5 -> 24 dB/oct
BUTTER4_Q = (0.541196, 1.306563)          # Qs of the two biquads of a 4th-order Butterworth (24 dB/oct) pass filter
BOOST_PARAMS = (("drive", 0.0, 3.0, 1.0), ("level", 6.0, 10.0, 8.0), ("tone", 3.0, 8.0, 5.0))   # name, lo, hi, default
IRMIX_RANGE = (0.2, 0.8)                  # mix of the second IR (B2.1)
PEDAL_LATENCY = 50                        # samples of every modeled pedal block (docs/PRESET_SCHEMA.md)


TOPOLOGY_RANK = {"single": 0, "single2": 1, "blend": 2}      # simplest first (Occam)


@dataclass(frozen=True)
class Combo:
    a_pedals: tuple[Capture, ...]
    a_amp: Capture
    b_pedals: tuple[Capture, ...] | None     # None: no path B (single / single2)
    b_amp: Capture | None
    cab: Capture
    boost: bool = False                      # modeled pedal.ts directly in front of the amp (single topology only)
    cab_b: Capture | None = None             # v0.4M B2.1: a second IR; the cab is then one combined "irMix" IR (cab = IR A)
    cab_offset: int = 0                      # offsetSamplesB of the irMix cab (positive = IR B delayed)
    cab_invert: bool = False                 # invertB of the irMix cab

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
        cab = self.cab.key if self.cab_b is None else f"{self.cab.key}+{self.cab_b.key}@{self.cab_offset}{'-' if self.cab_invert else ''}"
        return (self.topology + ("+ts" if self.boost else ""), *[c.key for c in self.nams()], cab)

    def pair_key(self) -> tuple:
        """Identity without the cab."""
        return self.key()[:-1]

    def captures(self) -> dict:
        """Ordered slot -> capture (for reports); slot names depend on the topology."""
        if self.topology == "blend":
            return {"a_pedal": (self.a_pedals or (None,))[0], "a_amp": self.a_amp,
                    "b_pedal": (self.b_pedals or (None,))[0], "b_amp": self.b_amp, **self._cabs()}
        if self.topology == "single2":
            return {"pedal1": self.a_pedals[0], "pedal2": self.a_pedals[1], "amp": self.a_amp, **self._cabs()}
        return {"pedal": (self.a_pedals or (None,))[0], "amp": self.a_amp, **self._cabs()}

    def _cabs(self) -> dict:
        return {"cab": self.cab} if self.cab_b is None else {"cab": self.cab, "cab_b": self.cab_b}

    def with_cab(self, cab: Capture) -> "Combo":
        return Combo(self.a_pedals, self.a_amp, self.b_pedals, self.b_amp, cab, self.boost)

    def with_pair(self, cab_a: Capture, cab_b: Capture, offset: int, invert: bool) -> "Combo":
        """The same chain with a two-IR (irMix) cab: IR A, IR B shifted by ``offset`` samples (positive = B delayed)."""
        return Combo(self.a_pedals, self.a_amp, self.b_pedals, self.b_amp, cab_a, self.boost, cab_b, int(offset), bool(invert))

    def model_bytes(self) -> int:
        return sum(c.size_bytes for c in self.nams())

    def size_rank(self) -> tuple[int, int]:
        """Sum of the capture size ranks (category, 10 % byte bucket): lighter model sets sort first."""
        r = [c.size_rank for c in self.nams()]
        return sum(x[0] for x in r), sum(x[1] for x in r)

    def describe(self) -> str:
        parts = [f"{k}={v.title}:{v.name}" for k, v in self.captures().items() if v]
        if self.boost:      # the boost sits right before the amp: show it in signal order
            i = next((n for n, p in enumerate(parts) if p.startswith("amp=")), len(parts))
            parts.insert(i, "boost=pedal.ts")
        return f"{self.topology}{'+boost' if self.boost else ''}{'+irMix' if self.cab_b is not None else ''}: " + " | ".join(parts)


@dataclass(frozen=True)
class P:
    name: str
    lo: float
    hi: float
    default: float
    log: bool = False
    group: str = "linear"      # "linear" (cheap, post-NAM) | "gain" (needs NAM re-render) | "discrete" (not in CMA-ES: tried after it)
    eq_gain: bool = False      # counts toward the EQ-gain regulariser


class Space:
    """Parameters for one combo shape ``(n_pedals_a, n_pedals_b | None)``."""

    def __init__(self, shape: tuple, boost: bool = False, filters: bool = True, irmix: bool = False):
        na, nb = shape
        if boost and nb is not None:
            raise ValueError("the tight boost is a single-path variant")
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
        if filters:
            ps.append(P("post.hp", *POST_HP_RANGE, POST_HP_RANGE[0], log=True, group="discrete"))
            ps.append(P("post.hp_slope", 0.0, 1.0, SLOPE_DEFAULT, group="discrete"))
            ps.append(P("post.lp_slope", 0.0, 1.0, SLOPE_DEFAULT, group="discrete"))
        gains = [f"a.{i}" for i in range(na)] + ["a.amp"]
        if nb is not None:
            gains += [f"b.{i}" for i in range(nb)] + ["b.amp"]
        for g in gains:
            ps.append(P(f"gain.{g}", -NAM_GAIN, NAM_GAIN, 0.0, group="gain"))
        if irmix:       # B2.1: the IR mix, tried on a grid after CMA-ES (group discrete)
            ps.append(P("cab.mix", *IRMIX_RANGE, 0.5, group="discrete"))
        if boost:
            for name, lo, hi, d in BOOST_PARAMS:
                ps.append(P(f"boost.{name}", lo, hi, d, group="gain"))
        self.params = ps
        self.names = [p.name for p in ps]
        self.idx = {p.name: i for i, p in enumerate(ps)}
        self.shape = shape

    @staticmethod
    def for_combo(combo: Combo, filters: bool = True) -> "Space":
        return Space(combo.shape(), boost=combo.boost, filters=filters, irmix=combo.cab_b is not None)

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


def pass_bands(kind: str, freq: float, slope: float) -> list[dict]:
    """High-/low-pass band(s) for a slope parameter: < 0.5 = one 12 dB/oct biquad (q 0.707), >= 0.5 = 24 dB/oct as two
    cascaded biquads with the 4th-order Butterworth Qs (0.541 and 1.307)."""
    if slope >= 0.5:
        return [{"type": kind, "freq": float(freq), "q": q} for q in BUTTER4_Q]
    return [{"type": kind, "freq": float(freq), "q": 0.707}]


def post_filters_from_eq(bands: list[dict]) -> dict:
    """Inverse of the post-cab filter emission (round trip of the preset): ``{"hp": (freq, slope) | None, "lowpass":
    [(freq, slope), ...]}`` with slope 12 or 24 dB/oct. The one post low-pass is ``post.lp``."""
    def groups(kind):
        bs = [b for b in bands if b.get("type") == kind and b.get("enabled", True)]
        out, i = [], 0
        while i < len(bs):
            if (i + 1 < len(bs) and abs(bs[i]["q"] - BUTTER4_Q[0]) < 2e-3 and abs(bs[i + 1]["q"] - BUTTER4_Q[1]) < 2e-3
                    and abs(bs[i]["freq"] - bs[i + 1]["freq"]) < 1e-6 * bs[i]["freq"]):
                out.append((float(bs[i]["freq"]), 24))
                i += 2
            else:
                out.append((float(bs[i]["freq"]), 12))
                i += 1
        return out
    hp = groups("highPass")
    return {"hp": hp[0] if hp else None, "lowpass": groups("lowPass")}


def post_eq(v: dict[str, float]) -> list[dict]:
    bands = [_peak(v[f"post.f{i}"], v[f"post.g{i}"]) for i in range(3)]
    if "post.shelf_g" in v and v["post.shelf_g"] < -1e-3:
        bands.append({"type": "highShelf", "freq": float(v["post.shelf_f"]), "gainDb": float(v["post.shelf_g"]), "q": 0.707})
    if "post.lp" in v and v["post.lp"] < POST_LP_RANGE[1] * 0.999:     # one post low-pass; 12 kHz = off, any slope
        bands += pass_bands("lowPass", v["post.lp"], v.get("post.lp_slope", 0.0))
    if "post.hp" in v:        # v0.4M post-cab filters; a filter at its range edge is off (whatever its slope)
        s = v.get("post.hp_slope", SLOPE_DEFAULT)
        if v["post.hp"] > POST_HP_RANGE[0] * 1.001:
            bands += pass_bands("highPass", v["post.hp"], s)
    return bands


def gate_preset(noise_floor_db: float, extra: dict | None = None) -> dict:
    """Fixed gate ("medium"): open at the DI noise floor + 4 dB, hold 40 ms, release 150 ms, range -50 dB."""
    # a DI with digital silence measures a floor of -200 dBFS: keep the threshold inside the schema range (>= -120)
    g = {"enabled": True, "thresholdDb": round(max(noise_floor_db, -90.0) + 4.0, 2), "hysteresisDb": 6.0, "attackMs": 0.5,
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


def cab_block(combo: Combo, v: dict[str, float] | None = None) -> dict:
    """The preset ``cab`` object of a combo: ``shared`` with one IR, or ``irMix`` (one combined IR, live-compatible) with
    the pair's alignment (``offsetSamplesB`` / ``invertB``) and the mix ``cab.mix`` of ``v``."""
    if combo.cab_b is None:
        return {"mode": "shared", "ir": combo.cab.block_model(), "enabled": True}
    c = {"mode": "irMix", "irA": combo.cab.block_model(), "irB": combo.cab_b.block_model(),
         "mix": float((v or {}).get("cab.mix", 0.5)), "enabled": True}
    if combo.cab_offset:
        c["offsetSamplesB"] = int(combo.cab_offset)
    if combo.cab_invert:
        c["invertB"] = True
    return c


def boost_block(id_: str, v: dict[str, float]) -> dict:
    """The tight boost: modeled ``pedal.ts`` (slot ``boost``, model version 1) with the ``boost.*`` parameters."""
    return {"id": id_, "type": "pedal.ts", "slot": "boost", "modelVersion": 1,
            "params": {n: float(v.get(f"boost.{n}", d)) for n, _, _, d in BOOST_PARAMS}}


def chain_blocks(pfx: str, pedals, amp: Capture, v: dict[str, float], boost: bool = False) -> list[dict]:
    """Blocks of one path: pedals, the optional tight boost, then the amp (ids a1.., b1..; amps get loudness
    normalisation)."""
    blocks = [_nam(f"{pfx}{i + 1}", "pedal", p, v.get(f"gain.{pfx}.{i}", 0.0), False) for i, p in enumerate(pedals)]
    if boost:
        blocks.append(boost_block(f"{pfx}{len(blocks) + 1}", v))
    blocks.append(_nam(f"{pfx}{len(blocks) + 1}", "amp", amp, v.get(f"gain.{pfx}.amp", 0.0), True))
    return blocks


def block_latency(blocks: list[dict]) -> int:
    """Processing latency (samples) the modeled pedal blocks of a chain add (NAM captures: 0 in the matcher's pool). The
    figure is the Latency column of the block-type table in docs/PRESET_SCHEMA.md (50 samples at any rate per pedal.*)."""
    return PEDAL_LATENCY * sum(1 for b in blocks if str(b.get("type", "")).startswith("pedal."))


def path_blocks(combo: Combo, v: dict[str, float], path: str) -> list[dict]:
    if path == "a":
        return chain_blocks("a", combo.a_pedals, combo.a_amp, v, combo.boost)
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
        "cab": cab_block(combo, v),
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
