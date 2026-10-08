"""v0.8 I4a: the matcher's calibration option, v5 presets and the offline stereo-DI rule."""
from __future__ import annotations

import json

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher import calibration as CAL
from sawblade_match.matcher.space import build_preset, gate_preset, manual_align

FS = 48000


# ---- pure: the option and the stereo rule (no core) ------------------------------------------------------------------
def test_default_is_legacy_and_there_is_one_constant_to_flip():
    from sawblade_match.matcher.cli import build_parser
    assert CAL.DEFAULT_CALIBRATION == "legacy"
    assert CAL.CalibrationOptions().mode == CAL.DEFAULT_CALIBRATION and not CAL.CalibrationOptions().calibrated
    a = build_parser().parse_args([])
    assert a.calibration == CAL.DEFAULT_CALIBRATION and a.device_dbu is None and a.di_channel == "auto"


def test_cli_flags_parse_and_reach_the_config():
    from sawblade_match.matcher.cli import build_parser
    a = build_parser().parse_args(["--calibration", "calibrated", "--device-dbu", "9.5", "--di-channel", "R"])
    assert (a.calibration, a.device_dbu, a.di_channel) == ("calibrated", 9.5, "R")
    with pytest.raises(SystemExit):
        build_parser().parse_args(["--calibration", "maybe"])


def test_options_validate_and_record():
    with pytest.raises(ValueError):
        CAL.CalibrationOptions("maybe")
    with pytest.raises(ValueError):
        CAL.CalibrationOptions("calibrated", 75.0)
    leg = CAL.CalibrationOptions().record()
    assert leg["mode"] == "legacy" and leg["deviceDbu"] is None and leg["deviceAssumed"] is None
    assert CAL.CalibrationOptions().render_kwargs() == {"calibration": "legacy"}
    ass = CAL.CalibrationOptions("calibrated")
    r = ass.record({"di": {"used": "L"}})
    assert r["mode"] == "calibrated" and r["deviceDbu"] == 12.0 and r["deviceAssumed"] is True and "assumed" in r["note"]
    assert r["diChannel"] == {"di": {"used": "L"}} and ass.render_kwargs() == {"calibration": "calibrated"}
    giv = CAL.CalibrationOptions("calibrated", 9.0)
    assert giv.record()["deviceDbu"] == 9.0 and giv.record()["deviceAssumed"] is False
    assert giv.render_kwargs() == {"calibration": "calibrated", "device_dbu": 9.0}
    assert "no effect" in CAL.CalibrationOptions("legacy", 9.0).record()["note"]


def _stereo(l_gain, r_gain, n=4800, seed=1):
    s = np.random.default_rng(seed).standard_normal(n).astype(np.float32) * 0.1
    return np.stack([s * l_gain, s * r_gain], axis=1)


def test_stereo_rule_louder_channel_tie_and_explicit():
    x = _stereo(0.1, 1.0)
    m, info = CAL.pick_di_channel(x)
    assert info["used"] == "R" and info["rule"] == "auto" and info["fileChannels"] == 2
    assert np.array_equal(m, x[:, 1]) and m.dtype == np.float32
    assert info["rmsDbfsR"] - info["rmsDbfsL"] == pytest.approx(20.0, abs=1e-3)
    m, info = CAL.pick_di_channel(_stereo(1.0, 0.1))
    assert info["used"] == "L"
    m, info = CAL.pick_di_channel(_stereo(1.0, 1.0))                  # tie: L (as the core)
    assert info["used"] == "L"
    m, info = CAL.pick_di_channel(np.zeros((100, 2), np.float32))     # two silent channels: L, no dB values
    assert info["used"] == "L" and info["rmsDbfsL"] is None
    m, info = CAL.pick_di_channel(x, "L")
    assert info["used"] == "L" and info["rule"] == "L" and np.array_equal(m, x[:, 0])
    m, info = CAL.pick_di_channel(x, "mix")
    assert info["used"] == "mix" and np.allclose(m, 0.5 * (x[:, 0] + x[:, 1]), atol=1e-7)
    m, info = CAL.pick_di_channel(np.hstack([x, x]))                  # channels beyond the second are dropped
    assert info["used"] == "R" and info["fileChannels"] == 4 and len(m) == len(x)
    with pytest.raises(ValueError):
        CAL.pick_di_channel(x, "left")


def test_mono_is_unchanged_and_block_size_cannot_matter():
    mono = _stereo(1.0, 1.0)[:, 0]
    for x in (mono, mono[:, None]):
        m, info = CAL.pick_di_channel(x)
        assert info["used"] == "mono" and info["rmsDbfsL"] is None and np.array_equal(m, mono)


def test_options_from_run_and_di_rule_from_run():
    assert CAL.options_from_run({}, {}) == CAL.CalibrationOptions("legacy")
    assert CAL.options_from_run({}, {"calibration": {"mode": "calibrated"}}) == CAL.CalibrationOptions("calibrated")
    res = {"calibration": {"mode": "calibrated", "deviceDbuGiven": 9.0, "diChannel": {"di": {"rule": "R"}}}}
    assert CAL.options_from_run(res, {}) == CAL.CalibrationOptions("calibrated", 9.0)
    assert CAL.di_rule_from_run(res) == "R" and CAL.di_rule_from_run({}) == "auto"


# ---- with the core ---------------------------------------------------------------------------------------------------
try:
    from sawblade_match import core as _core
    HAS_CAL = "calibration" in (_core.render.__doc__ or "")
except Exception:                                                          # noqa: BLE001
    _core, HAS_CAL = None, False
needs_core = pytest.mark.skipif(_core is None, reason="sawblade_core not built")
needs_cal = pytest.mark.skipif(not HAS_CAL, reason="sawblade_core without calibration options")


def _fixtures():
    from test_matcher import fixture_pool, hidden
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    return combo, v


@needs_core
def test_emitted_preset_is_v5_with_the_mode_and_internal_presets_are_unchanged():
    combo, v = _fixtures()
    gate = gate_preset(-50.0)
    for mode in ("legacy", "calibrated"):
        p = build_preset(combo, v, gate=gate, align=manual_align(0, False), emit=True, calibration_mode=mode)
        assert p["version"] == 5 and p["calibration"] == {"mode": mode}
        _core.render(p, np.zeros(2048, np.float32), 48000.0)            # the core's parser accepts it
    d = build_preset(combo, v, gate=gate, align=manual_align(0, False), emit=True)
    assert d["calibration"] == {"mode": CAL.DEFAULT_CALIBRATION}
    q = build_preset(combo, v, gate=gate, align=manual_align(0, False))
    assert q["version"] == 1 and "calibration" not in q
    with pytest.raises(ValueError):
        build_preset(combo, v, gate=gate, align=manual_align(0, False), emit=True, calibration_mode="x")


@needs_core
def test_engine_passes_the_calibration_to_the_core(monkeypatch):
    from sawblade_match.matcher import engine as ENG
    seen = []

    def fake(preset, x, fs, **kw):
        seen.append(kw)
        return np.asarray(x), {}
    monkeypatch.setattr(ENG._core, "render", fake)
    x = np.zeros(64, np.float32)
    for cal in (None, CAL.CalibrationOptions("calibrated"), CAL.CalibrationOptions("calibrated", 7.0)):
        e = ENG.Engine(None, 1, calibration=cal)
        e.render({}, x)
        e.close()
    assert [{k: v for k, v in s.items() if k != "cache"} for s in seen] == [
        {"calibration": "legacy"}, {"calibration": "calibrated"}, {"calibration": "calibrated", "device_dbu": 7.0}]


@needs_cal
def test_engine_calibrated_render_reports_the_device_level():
    from sawblade_match.matcher.engine import Engine
    combo, v = _fixtures()
    p = build_preset(combo, v, gate=None, align=manual_align(0, False))
    x = (np.random.default_rng(1).standard_normal(4800) * 0.1).astype(np.float32)
    got = {}
    for tag, cal in (("legacy", CAL.CalibrationOptions()), ("assumed", CAL.CalibrationOptions("calibrated")),
                     ("nine", CAL.CalibrationOptions("calibrated", 9.0))):
        e = Engine(None, 1, calibration=cal)
        try:
            got[tag] = e.render(p, x)[1]["calibration"]
        finally:
            e.close()
    assert got["legacy"]["mode"] == "legacy"
    assert got["assumed"]["mode"] == "calibrated" and got["assumed"]["deviceDbu"] == 12.0 and got["assumed"]["deviceAssumed"] is True
    assert got["nine"]["deviceDbu"] == 9.0 and got["nine"]["deviceAssumed"] is False


@needs_core
def test_level_match_wrapper_passes_the_calibration_explicitly(monkeypatch):
    from sawblade_match.matcher import levelmatch as LM
    calls = []

    def binding(preset, sr, base_dir=None, cache=None, calibration="preset", device_dbu=None):
        calls.append((calibration, device_dbu))
        return {"trimADb": 0.0, "trimBDb": 1.0, "makeupDb": [0.0] * 5}
    monkeypatch.setattr(_core, "level_match", binding)
    LM.level_match({}, 48000, calibration=CAL.CalibrationOptions())                 # legacy is explicit, not left to the preset
    LM.level_match({}, 48000, calibration=CAL.CalibrationOptions("calibrated", 9.0))
    LM.level_match({}, 48000)
    assert calls == [("legacy", None), ("calibrated", 9.0), ("preset", None)]


def _run(tmp_path, tag, di_data, **cfg_kw):
    from test_matcher import _setup_known, mkplan
    from sawblade_match.matcher.run import Config, Log, run_match
    d = tmp_path / tag
    d.mkdir()
    pool, combo, di, ref = _setup_known(d, "single")
    if di_data is not None:
        x, fs = sf.read(str(di), dtype="float32")
        sf.write(str(di), di_data(x), fs, subtype="FLOAT")
    cfg = Config(di=di, ref=ref, pool=pool, out=d / "out", seed=1, excerpt_s=2.0, threads=2,
                 plan=mkplan(top_k={"blend": 0, "single": 1, "single2": 0}), write_audio=False, refine_offsets=False, **cfg_kw)
    return run_match(cfg, Log()), d / "out", di


@needs_cal
def test_run_records_calibrated_mode_device_and_stereo_rule(tmp_path):
    res, out, di = _run(tmp_path, "cal", lambda x: np.stack([x * 0.05, x], axis=1),
                        calibration=CAL.CalibrationOptions("calibrated", 9.0))
    rec = res["calibration"]
    assert rec["mode"] == "calibrated" and rec["deviceDbu"] == 9.0 and rec["deviceAssumed"] is False
    assert rec["diChannel"]["di"]["used"] == "R" and rec["diChannel"]["di"]["rule"] == "auto"
    assert rec["diChannel"]["di"]["rmsDbfsR"] > rec["diChannel"]["di"]["rmsDbfsL"] + 20
    assert rec["corePlan"]["mode"] == "calibrated" and rec["corePlan"]["deviceDbu"] == 9.0
    on_disk = json.loads((out / "result.json").read_text())
    assert on_disk["calibration"]["mode"] == "calibrated"
    files = [out / res["best"]["preset"]] + sorted(out.glob("alt*.preset.resolved.json"))
    for f in files:
        p = json.loads(f.read_text())
        assert p["version"] == 5 and p["calibration"] == {"mode": "calibrated"}
    assert json.loads((out / "best.preset.json").read_text())["calibration"] == {"mode": "calibrated"}


@needs_cal
def test_run_default_is_legacy_and_assumed_level_is_recorded(tmp_path):
    res, out, di = _run(tmp_path, "leg", None)
    rec = res["calibration"]
    assert rec["mode"] == "legacy" and rec["deviceDbu"] is None and rec["default"] == "legacy"
    assert rec["diChannel"]["di"]["used"] == "mono" and rec["corePlan"]["mode"] == "legacy"
    p = json.loads((out / res["best"]["preset"]).read_text())
    assert p["version"] == 5 and p["calibration"] == {"mode": "legacy"}
    res2, out2, _ = _run(tmp_path, "cal_assumed", None, calibration=CAL.CalibrationOptions("calibrated"))
    r2 = res2["calibration"]
    assert r2["deviceDbu"] == 12.0 and r2["deviceAssumed"] is True and "assumed" in r2["note"]
    assert r2["corePlan"]["deviceAssumed"] is True


@needs_cal
def test_explicit_di_channel_beats_the_rms_rule(tmp_path):
    res, _, _ = _run(tmp_path, "lch", lambda x: np.stack([x * 0.05, x], axis=1), di_channel="L")
    d = res["calibration"]["diChannel"]["di"]
    assert d["rule"] == "L" and d["used"] == "L"


@needs_core
def test_pathcheck_uses_the_louder_channel_of_a_stereo_di(tmp_path):
    from sawblade_match.matcher import pathcheck as PC
    from test_pathcheck import make_run, render_wav
    result, di, preset = make_run(tmp_path)
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b = tmp_path / "a.wav", tmp_path / "b.wav"
    render_wav(ref_a, PC.single_preset(preset, "a", {}), x)
    render_wav(ref_b, PC.single_preset(preset, "b", {}), x)
    st = tmp_path / "di_stereo.wav"
    sf.write(str(st), np.stack([x * 0.02, x], axis=1), FS, subtype="FLOAT")     # the guitar is on R
    mono = PC.pathcheck(result, di, ref_a, ref_b)
    auto = PC.pathcheck(result, st, ref_a, ref_b)
    assert auto["diChannel"]["used"] == "R" and auto["diChannel"]["rule"] == "auto"
    assert mono["diChannel"]["used"] == "mono"
    assert auto["a"]["aWeightedErrorDb"] == pytest.approx(mono["a"]["aWeightedErrorDb"], abs=1e-6)
    assert auto["b"]["aWeightedErrorDb"] == pytest.approx(mono["b"]["aWeightedErrorDb"], abs=1e-6)
    left = PC.pathcheck(result, st, ref_a, ref_b, di_channel="L")                # explicit override: the quiet channel
    assert left["diChannel"]["used"] == "L" and left["a"]["aWeightedErrorDb"] > mono["a"]["aWeightedErrorDb"] + 0.1
    assert auto["calibration"]["mode"] == "legacy"                               # a v1 preset / old result = legacy


@needs_cal
def test_pathcheck_rerenders_with_the_calibration_the_run_used(tmp_path):
    from sawblade_match.matcher import pathcheck as PC
    from test_pathcheck import make_run, render_wav
    result, di, preset = make_run(tmp_path)
    res = json.loads(result.read_text())
    res["calibration"] = {"mode": "calibrated", "deviceDbuGiven": 9.0}
    result.write_text(json.dumps(res))
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b = tmp_path / "a.wav", tmp_path / "b.wav"
    render_wav(ref_a, PC.single_preset(preset, "a", {}), x)
    render_wav(ref_b, PC.single_preset(preset, "b", {}), x)
    r = PC.pathcheck(result, di, ref_a, ref_b)
    assert r["calibration"]["mode"] == "calibrated" and r["calibration"]["deviceDbu"] == 9.0
