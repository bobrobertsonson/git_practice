"""The validation script's summary printer: matched gate and bus comp in full, per-path check, blend runs first."""
from __future__ import annotations

import json

from sawblade_match.matcher import validation_summary as VS

GATE = {"enabled": True, "mode": "expander", "thresholdDb": -61.5, "hysteresisDb": 6.0, "attackMs": 0.5, "holdMs": 40.0,
        "releaseMs": 150.0, "rangeDb": -50.0, "ratio": 3.0, "keyHighPassHz": 120.0, "releaseCurve": "exponential"}
COMP = {"enabled": True, "thresholdDb": -14.0, "ratio": 2.5, "kneeDb": 6.0, "attackMs": 10.0, "releaseMs": 120.0, "makeupDb": 0.0}


def make_run(out, name, gate=None, comp=None, floor=-65.5):
    d = out / name
    d.mkdir()
    preset = {"gate": gate or {"enabled": False}}
    if comp:
        preset["busComp"] = comp
    (d / "best.preset.resolved.json").write_text(json.dumps(preset))
    (d / "result.json").write_text(json.dumps({"best": {"preset": "best.preset.resolved.json", "topology": "blend"},
                                               "after": [{"aWeightedErrorDb": 1.25}], "wallSeconds": 60,
                                               **({"diNoiseFloorDb": floor} if floor is not None else {})}))
    return d


def test_gate_and_comp_printed_in_full(tmp_path):
    make_run(tmp_path, "L_blend", GATE, COMP)
    txt = VS.summary(tmp_path)
    gl = next(ln for ln in txt.splitlines() if "gate:" in ln)
    for want in ("enabled=True", "mode=expander", "thresholdDb=-61.5", "hysteresisDb=6", "attackMs=0.5", "holdMs=40",
                 "releaseMs=150", "rangeDb=-50", "ratio=3", "keyHighPassHz=120", "releaseCurve=exponential",
                 "DI noise floor -65.5 dBFS"):
        assert want in gl, want
    cl = next(ln for ln in txt.splitlines() if "busComp:" in ln)
    for want in ("enabled=True", "thresholdDb=-14", "ratio=2.5", "kneeDb=6", "attackMs=10", "releaseMs=120"):
        assert want in cl, want


def test_ratio_only_for_expander_and_comp_off_when_absent(tmp_path):
    g = {k: v for k, v in GATE.items() if k not in ("mode",)}
    make_run(tmp_path, "L_ubr", g, None, floor=None)
    txt = VS.summary(tmp_path)
    gl = next(ln for ln in txt.splitlines() if "gate:" in ln)
    assert "ratio=" not in gl and "mode=default" in gl and "noise floor" not in gl
    assert "busComp: off" in txt


def test_blend_runs_first_pathcheck_and_held_out(tmp_path):
    make_run(tmp_path, "L_hm2", GATE)
    d = make_run(tmp_path, "L_blend", GATE)
    pc = {"singlePath": False, "a": {"aWeightedErrorDb": 1.1}, "b": {"aWeightedErrorDb": 2.2},
          "swapped": {"aVsRefB": 5.5, "bVsRefA": 6.6}, "ratio": {"chosenDb": 3.0, "refDb": 2.5, "diffDb": 0.5}}
    (d / "pathcheck.json").write_text(json.dumps(pc))
    (tmp_path / "L_blend_on_R.pathcheck.json").write_text(json.dumps({**pc, "singlePath": True, "path": "a", "full": {"ref": "blend", "aWeightedErrorDb": 3.3}}))
    txt = VS.summary(tmp_path)
    assert txt.index("== L_blend") < txt.index("== L_hm2")
    assert "A vs ref-a 1.10, B vs ref-b 2.20" in txt and "swapped: A vs ref-b 5.50, B vs ref-a 6.60" in txt
    assert "chosen 3.00 dB vs reference 2.50 dB" in txt
    assert "L_blend_on_R.pathcheck.json" in txt and "singlePath true" in txt


def test_blend_reference_polarity_choice_is_printed(tmp_path):
    (tmp_path / "refs").mkdir()
    (tmp_path / "refs" / "L_blend.json").write_text(json.dumps({
        "polarity": {"mode": "auto", "chosen": "invert-b", "lowBandDbAsis": -41.25, "lowBandDbInvert": -30.5},
        "lagMs": 1.95, "corr": 0.269, "refRatioDb": 3.1, "gainsDb": [0.0, 0.0]}))
    (tmp_path / "refs" / "R_blend.json").write_text(json.dumps({"gainsDb": [0.0, 0.0], "polarity": 1}))   # an old reference
    txt = VS.summary(tmp_path)
    assert "polarity auto -> invert-b" in txt
    assert ("as recorded -41.2 dB" in txt) or ("as recorded -41.3 dB" in txt)      # -41.25 rounds either way
    assert "b inverted -30.5 dB" in txt and "lag 1.95 ms (not shifted)" in txt
    assert "refs/R_blend: polarity: not recorded" in txt


def test_topology_margin_is_printed(tmp_path):
    d = make_run(tmp_path, "L_blend_quick", GATE, None)
    r = json.loads((d / "result.json").read_text())
    r["topology"] = {"bestSingle": 1.05, "bestBlend": 1.0, "deltaPct": 5.0, "determined": False, "mode": "auto"}
    (d / "result.json").write_text(json.dumps(r))
    d2 = make_run(tmp_path, "L_blend_quick_forced", GATE, None)
    r2 = json.loads((d2 / "result.json").read_text())
    r2["topology"] = {"bestSingle": None, "bestBlend": 1.0, "deltaPct": None, "determined": False, "mode": "blend",
                      "note": "only one topology was evaluated (--topology forced it)"}
    (d2 / "result.json").write_text(json.dumps(r2))
    txt = VS.summary(tmp_path)
    assert "best single 1.050 vs best blend 1.000, delta +5.0 %" in txt and "topology not determined" in txt
    assert "topology: blend -- only one topology was evaluated" in txt


def test_dynamics_and_input_gain_lines(tmp_path):
    d = make_run(tmp_path, "L_hm2_quick")
    r = json.loads((d / "result.json").read_text())
    dyn = lambda c4, c, lra, p10, p95: {"medianCrest400Db": c4, "crestFactorDb": c, "lraLu": lra, "shortTermP10Lufs": p10,
                                        "shortTermP95Lufs": p95, "shortTermSpreadLu": p95 - p10}
    r["referenceDynamics"] = {"window": "matched channel, reference samples 0..96000 (DI-aligned)",
                              "reference": dyn(14.2, 17.0, 3.4, -24.1, -20.7), "best_L": dyn(9.8, 15.7, 0.9, -22.0, -21.1),
                              "starter_L": dyn(12.0, 16.9, 5.8, -30.0, -24.2)}
    r["inputGains"] = {"a": {"role": "body", "levelDb": 0.0, "blocks": [
        {"id": "a1", "type": "pedal.ts", "slot": "boost", "capture": None, "inputGainDb": None, "normalizeLoudness": None,
         "params": {"drive": 1.35, "level": 8.0, "tone": 4.8}},
        {"id": "a2", "type": "nam", "slot": "amp", "capture": "57492/1 HM-2 style", "inputGainDb": 2.5, "normalizeLoudness": True}]}}
    (d / "result.json").write_text(json.dumps(r))
    lines = VS.summary(tmp_path).splitlines()
    ref_l = next(ln for ln in lines if ln.strip().startswith("reference "))
    for want in ("crest400 14.2 dB", "LRA 3.4 LU", "-24.1..-20.7 LUFS", "spread 3.4 LU"):
        assert want in ref_l, want
    best_l = next(ln for ln in lines if ln.strip().startswith("best_L "))
    assert "crest400 9.8 dB" in best_l and "LRA 0.9 LU" in best_l
    assert any("DI-aligned" in ln for ln in lines)
    boost = next(ln for ln in lines if "pedal.ts/boost" in ln)
    assert "drive=1.35" in boost and "level=8" in boost and "inputGainDb n/a" in boost
    nam = next(ln for ln in lines if "nam/amp" in ln)
    assert "path A" in nam and "inputGainDb +2.50" in nam and "normalizeLoudness=on" in nam and "57492/1 HM-2 style" in nam


def test_old_result_without_the_new_fields_still_prints(tmp_path):
    make_run(tmp_path, "L_blend")
    txt = VS.summary(tmp_path)
    assert "dynamics" not in txt and "input gain per block" not in txt
