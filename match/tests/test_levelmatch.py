"""Phase 10.1 matcher side: trims before the blend fit, linear -> constant-loudness blend, emitted preset keys, export lines.

The C++ ``level_match`` binding is faked throughout; tests that import the engine need the built core and are skipped
without it.
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest

from sawblade_match.matcher import levelmatch as LM
from sawblade_match.matcher.levelmatch import Levels, blend_to_constant_loudness

REPO = Path(__file__).resolve().parents[2]
FIX = REPO / "tests" / "fixtures"


# ---- conversion ------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("b_lin,b_cl", [(0.0, 0.0), (0.25, (2 / math.pi) * math.atan(1 / 3)), (0.5, 0.5),
                                        (0.75, (2 / math.pi) * math.atan(3)), (1.0, 1.0)])
def test_blend_to_constant_loudness_values(b_lin, b_cl):
    assert blend_to_constant_loudness(b_lin) == pytest.approx(b_cl, abs=1e-12)


def test_blend_conversion_preserves_ab_ratio_and_is_monotone():
    bs = np.linspace(0.01, 0.99, 50)
    cl = np.array([blend_to_constant_loudness(b) for b in bs])
    assert np.all(np.diff(cl) > 0) and np.all((cl > 0) & (cl < 1))
    for b, c in zip(bs, cl):
        assert math.tan(math.pi * c / 2) == pytest.approx(b / (1 - b), rel=1e-9)
        # equal-power weights are in the ratio of the linear weights
        assert math.sin(math.pi * c / 2) / math.cos(math.pi * c / 2) == pytest.approx(b / (1 - b), rel=1e-9)
    assert blend_to_constant_loudness(-0.1) == 0.0 and blend_to_constant_loudness(1.2) == 1.0


def test_makeup_interpolation_and_emit_gain_correction():
    mk = (0.0, 1.0, 2.0, 3.0, 4.0)
    assert LM.makeup_at(mk, 0.0) == 0.0 and LM.makeup_at(mk, 1.0) == 4.0
    assert LM.makeup_at(mk, 0.125) == pytest.approx(0.5) and LM.makeup_at(mk, 0.5) == pytest.approx(2.0)
    flat = Levels(0.0, 0.0, (0.0,) * 5)
    # b_lin = 0.5: equal-power sum is +3.01 dB over the linear 50/50; pure paths are unchanged
    assert LM.emit_gain_correction_db(0.5, flat) == pytest.approx(10 * math.log10(2), abs=1e-9)
    assert LM.emit_gain_correction_db(0.0, flat) == pytest.approx(0.0, abs=1e-12)
    assert LM.emit_gain_correction_db(1.0, flat) == pytest.approx(0.0, abs=1e-12)
    # the correction equals the actual level ratio of cos/sin vs (1-b)/b mixing of unit signals
    b = 0.3
    c = blend_to_constant_loudness(b)
    ratio = math.cos(math.pi * c / 2) / (1 - b)
    assert 20 * math.log10(ratio) == pytest.approx(-10 * math.log10(b * b + (1 - b) ** 2), abs=1e-9)


def test_levels_from_core_gains_and_preset_block():
    lv = Levels.from_core({"trimADb": 0.0, "trimBDb": 6.0, "lufsA": -20.0, "lufsB": -26.0, "sumLufs": -19.0,
                           "makeupDb": [0, 0.5, 1, 0.5, 0], "delaySamplesB": 3, "invertB": False})
    assert lv.gains[0] == 1.0 and lv.gains[1] == pytest.approx(10 ** (6 / 20))
    assert lv.preset_block() == {"mode": "manual", "trimADb": 0.0, "trimBDb": 6.0}
    assert lv.makeup_db == (0.0, 0.5, 1.0, 0.5, 0.0)


# ---- export report lines ------------------------------------------------------------------------------------------------
def test_export_level_match_lines_present_and_absent():
    from sawblade_match.export import plan as P
    rep = {"levelMatch": {"mode": "auto", "trimADb": 0.0, "trimBDb": 4.3, "lufsA": -20, "lufsB": -24, "sumLufs": -18},
           "blend": {"value": 0.5, "law": "constantLoudness", "makeupDb": [0.0, 0.4, 0.8, 0.4, 0.0]}}
    lines = P.level_match_lines(rep)
    assert lines[0] == "level match (auto): A +0.0 dB, B +4.3 dB; blend law constantLoudness"
    assert "+0.4" in lines[1] and "+0.8" in lines[1]
    info = P.level_match_info(rep)
    assert info["trimBDb"] == 4.3 and info["makeupDb"] == [0.0, 0.4, 0.8, 0.4, 0.0]
    # older reports: nothing, no error
    assert P.level_match_lines({"warnings": []}) == [] and P.level_match_info({"warnings": []}) is None
    # partial: levelMatch without blend
    only = P.level_match_lines({"levelMatch": {"mode": "off", "trimADb": 0.0, "trimBDb": 0.0}})
    assert only == ["level match (off): A +0.0 dB, B +0.0 dB"]


# ---- need the C++ core (module import) ------------------------------------------------------------------------------------
core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built", exc_type=ImportError)
from sawblade_match.matcher import engine as ENG                 # noqa: E402
from sawblade_match.matcher.engine import Engine                  # noqa: E402
from sawblade_match.matcher.pool import Capture                   # noqa: E402
from sawblade_match.matcher.space import Combo, Space, build_preset, manual_align   # noqa: E402


def _cap(path: Path, tone: int, model: int, kind: str) -> Capture:
    gear = {"amp_high": "amp", "amp_low": "amp", "cab": "cab", "distortion": "pedal"}[kind]
    from sawblade_match.t3k.cache import sha256_file
    return Capture(tone, model, "T", "m", gear, str(path), sha256_file(path), path.stat().st_size, "cc-by", "me",
                   f"https://example/{tone}", kind)


def _blend_combo() -> Combo:
    n = lambda f: FIX / "nam" / f
    a1 = _cap(n("lstm.nam"), 2, 3, "amp_high")
    a2 = _cap(n("wavenet.nam"), 2, 4, "amp_high")
    cab = _cap(FIX / "ir" / "ir_a.wav", 4, 7, "cab")
    return Combo((), a1, (), a2, cab)


def test_probe_levels_calls_core_at_blend_half_with_resolved_align(monkeypatch):
    seen = {}

    def fake(preset, sample_rate, base_dir=None, cache=None, calibration=None):
        seen.update(preset=preset, rate=sample_rate, calibration=calibration)
        return {"trimADb": 0.0, "trimBDb": 6.0, "lufsA": -20.0, "lufsB": -26.0, "sumLufs": -18.0,
                "makeupDb": [0.0, 0.2, 0.4, 0.2, 0.0], "delaySamplesB": 3, "invertB": True}

    monkeypatch.setattr(ENG, "level_match", fake)
    combo = _blend_combo()
    eng = Engine.__new__(Engine)
    eng.cache = object()
    v = Space.for_combo(combo).default()
    v["blend"] = 0.8
    lv = eng.probe_levels(combo, v, manual_align(3, True))
    assert lv == Levels(0.0, 6.0, (0.0, 0.2, 0.4, 0.2, 0.0))
    assert seen["preset"]["blend"] == 0.5 and seen["preset"]["align"] == manual_align(3, True) and seen["rate"] == 48000
    assert seen["calibration"] is eng.calibration and not seen["calibration"].calibrated      # legacy is passed explicitly
    assert "levelMatch" not in seen["preset"]                      # the probe preset is the plain linear candidate
    single = Combo((), combo.a_amp, None, None, combo.cab)
    assert eng.probe_levels(single, Space.for_combo(single).default(), manual_align()) is None


def test_trims_are_applied_before_the_blend_fit():
    rng = np.random.default_rng(7)
    a = rng.standard_normal(20000).astype(np.float32)
    b_full = rng.standard_normal(20000).astype(np.float32)
    b = (b_full * 10 ** (-6 / 20)).astype(np.float32)               # path B renders 6 dB quieter
    target = 0.5 * a + 0.5 * b_full                                  # the tone is a level-matched 50/50
    levels = Levels(0.0, 6.0, (0.0,) * 5)                            # faked level_match: +6 dB trim on B

    def fit(lv):
        grid = np.linspace(0.05, 0.95, 91)
        err = [float(np.mean((Engine.mix(a, b, float(g), manual_align(), lv) - target) ** 2)) for g in grid]
        return float(grid[int(np.argmin(err))])

    assert fit(levels) == pytest.approx(0.5, abs=0.011)
    b_untrimmed = fit(None)
    assert b_untrimmed > 0.57                                       # without trims the fit has to compensate in b
    # ratio-preserving conversion of the trimmed fit
    cl = blend_to_constant_loudness(fit(levels))
    assert cl == pytest.approx(0.5, abs=0.01)
    # trims go on the paths, not on the output: delay / inversion semantics unchanged
    y = Engine.mix(a, b, 0.5, manual_align(2, True), levels)
    ref = 0.5 * a + 0.5 * np.concatenate([np.zeros(2, np.float32), (-b_full)[:-2]])
    assert np.max(np.abs(y[2:] - ref[2:])) < 1e-3


def test_emitted_blend_preset_keys_and_single_path_unchanged():
    combo = _blend_combo()
    v = Space.for_combo(combo).default()
    v["blend"] = 0.75
    levels = Levels(1.5, 0.0, (0.0,) * 5)
    p = build_preset(combo, v, gate=None, align=manual_align(1, False), levels=levels)
    assert p["levelMatch"] == {"mode": "manual", "trimADb": 1.5, "trimBDb": 0.0}
    assert p["blendLaw"] == "constantLoudness"
    assert p["blend"] == pytest.approx((2 / math.pi) * math.atan(3))
    # the trims are not folded into levelDb (that stays the fitted taste offset)
    assert p["paths"]["a"]["levelDb"] == v["levelA"]
    # no levels (starter, known-answer, legacy callers): preset is unchanged
    legacy = build_preset(combo, v, gate=None, align=manual_align())
    assert "levelMatch" not in legacy and "blendLaw" not in legacy and legacy["blend"] == 0.75
    # single path: never gets the keys, even if levels were passed
    single = Combo((), combo.a_amp, None, None, combo.cab)
    ps = build_preset(single, Space.for_combo(single).default(), gate=None, align=manual_align(), levels=levels)
    assert "levelMatch" not in ps and "blendLaw" not in ps and ps["blend"] == 0.0


def test_level_match_real_binding_on_demo_preset():
    """End to end against the built sawblade_core (no fakes); the module-level importorskip skips it without the core."""
    import json
    preset_path = REPO / "presets" / "modeled" / "saw_body_blend_demo.json"
    preset = json.loads(preset_path.read_text())
    r = core.level_match(preset, 48000, base_dir=str(preset_path.parent))
    assert r["trimADb"] == pytest.approx(2.02, abs=0.05)
    assert r["trimBDb"] == 0.0
    mk = r["makeupDb"]
    assert len(mk) == 5
    assert abs(mk[0]) < 0.01 and abs(mk[-1]) < 0.01
    assert mk[2] == pytest.approx(-2.25, abs=0.05)
    assert r["lufsA"] < r["lufsB"]
    assert isinstance(r["invertB"], bool)
    assert isinstance(r["delaySamplesB"], int)
    assert isinstance(r["warnings"], list)
