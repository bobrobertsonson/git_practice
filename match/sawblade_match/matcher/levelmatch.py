"""Phase 10.1 level matching on the matcher side.

The C++ core measures per-path trims (``sawblade_core.level_match``). The matcher mixes in numpy, so it applies those
trims to the cached per-path renders before fitting the *linear* blend ``b_lin``, then converts it to the
constant-loudness position with the same A:B ratio (``cos(pi b/2) : sin(pi b/2) == (1-b_lin) : b_lin``).
"""
from __future__ import annotations

import math
from dataclasses import dataclass


def level_match(preset: dict, sample_rate: float, base_dir=None, cache=None, calibration=None) -> dict:
    """The one call into the C++ binding (tests replace this).

    ``calibration`` (``calibration.CalibrationOptions``): when calibrated, the trims must be measured on the calibrated chain
    that the matcher renders. The binding takes ``calibration=`` / ``device_dbu=`` for that (v0.8 I4a follow-up). A binding
    that does not have them still measures on the uncalibrated chain: the result then carries ``calibrationUnsupported``
    (and the matcher records it) rather than silently passing the trims off as calibrated."""
    from .. import core as _core      # lazy: the pure helpers below do not need the C++ module
    if calibration is None or not calibration.calibrated:
        return _core.level_match(preset, float(sample_rate), base_dir=base_dir, cache=cache)
    try:
        return _core.level_match(preset, float(sample_rate), base_dir=base_dir, cache=cache, **calibration.render_kwargs())
    except TypeError:
        d = dict(_core.level_match(preset, float(sample_rate), base_dir=base_dir, cache=cache))
        d["calibrationUnsupported"] = True
        return d


@dataclass(frozen=True)
class Levels:
    trim_a_db: float = 0.0
    trim_b_db: float = 0.0
    makeup_db: tuple = (0.0,) * 5       # at blend 0, .25, .5, .75, 1 (constantLoudness make-up, linear in dB between)

    @staticmethod
    def from_core(d: dict) -> "Levels":
        return Levels(float(d["trimADb"]), float(d["trimBDb"]), tuple(float(x) for x in d["makeupDb"]))

    @property
    def gains(self) -> tuple[float, float]:
        return 10 ** (self.trim_a_db / 20), 10 ** (self.trim_b_db / 20)

    def preset_block(self) -> dict:
        return {"mode": "manual", "trimADb": float(self.trim_a_db), "trimBDb": float(self.trim_b_db)}


def blend_to_constant_loudness(b_lin: float) -> float:
    """Same A:B ratio on the equal-power law: tan(pi b_cl / 2) = b_lin / (1 - b_lin); clamped to [0, 1]."""
    if b_lin >= 1.0:
        return 1.0
    if b_lin <= 0.0:
        return 0.0
    return min(1.0, max(0.0, (2.0 / math.pi) * math.atan(b_lin / (1.0 - b_lin))))


def makeup_at(makeup_db, b: float) -> float:
    """Make-up in dB at blend ``b`` (the core's five points, linear in dB)."""
    x = min(1.0, max(0.0, b)) * 4.0
    i = min(int(x), 3)
    return float(makeup_db[i] + (x - i) * (makeup_db[i + 1] - makeup_db[i]))


def emit_gain_correction_db(b_lin: float, levels: Levels) -> float:
    """Level of the emitted constantLoudness render relative to the matcher's linear emulation, in dB.
    ``cos A + sin B = ((1-b_lin) A + b_lin B) / sqrt(b^2 + (1-b)^2)``, then the core's make-up at ``b_cl``.
    Subtract it from the output gain so the emitted preset renders at the level the matcher fitted."""
    s = b_lin * b_lin + (1.0 - b_lin) ** 2
    return -10.0 * math.log10(s) + makeup_at(levels.makeup_db, blend_to_constant_loudness(b_lin))
