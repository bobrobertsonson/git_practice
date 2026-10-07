"""v0.4M Task G.4: the exporter follows the ACTIVE dynamics set (core resolve_dynamics); the notes say which."""
from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from sawblade_match.export import notes as N
from sawblade_match.export import plan as P

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
if getattr(core, "_core", None) is None or not hasattr(core._core, "resolve_dynamics"):
    pytest.skip("sawblade_core built before Task G (no resolve_dynamics)", allow_module_level=True)

PRESETS = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "presets"


def match_preset() -> dict:
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    p["gate"] = {"enabled": True, "thresholdDb": -55.0, "hysteresisDb": 6.0, "attackMs": 0.5, "holdMs": 10.0,
                 "releaseMs": 20.0, "rangeDb": -50.0}
    # a long-release record comp: refused as a with-cab export in record mode, absent in the live set of a match
    p["busComp"] = {"enabled": True, "thresholdDb": -18.0, "ratio": 4.0, "kneeDb": 3.0, "attackMs": 5.0,
                    "releaseMs": 200.0, "makeupDb": 0.0}
    p["postEq"] = []
    p["output"] = {"gainDb": 0.0}
    p["origin"] = "match"
    p["dynamicsMode"] = "live"
    return p


def notes_of(p, mode="nocab", allow_inexact=False):
    return N.build_export_notes(p, P.make_plan(p, mode, allow_inexact), "x.nam", "x.ir.wav")


def test_live_match_preset_notes_say_live_and_list_the_live_gate_no_comp():
    p = match_preset()
    plan = P.make_plan(p, "nocab")                                 # live: no comp, so no refusal
    assert plan.dynamics == "live" and plan.to_json()["dynamics"] == "live"
    n = N.build_export_notes(p, plan, "x.nam", "x.ir.wav")
    assert n["dynamics"] == "live"
    stages = {s["stage"]: s for s in n["stages"]}
    assert "busComp" not in stages
    g = stages["gate"]["settings"]
    assert g["mode"] == "expander" and g["releaseMs"] == 120.0 and g["holdMs"] == 40.0       # derived live gate, not the record one


def test_record_mode_follows_the_record_set():
    p = match_preset()
    p["dynamicsMode"] = "record"
    with pytest.raises(P.ExportRefused):                           # record comp with a 200 ms release: not trainable (with cab)
        P.make_plan(p, "withcab")
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    n = N.build_export_notes(p, plan, "x.nam", "x.ir.wav")
    assert n["dynamics"] == "record" and "busComp" in {s["stage"] for s in n["stages"]}
    assert {s["stage"]: s for s in n["stages"]}["gate"]["settings"]["releaseMs"] == 20.0
    live = match_preset()
    assert P.make_plan(live, "withcab").dynamics == "live"         # the same rig, live: with-cab export allowed


def test_no_dynamics_key_no_notes_line_and_identical_notes():
    p = match_preset()
    for k in ("origin", "dynamicsMode"):
        p.pop(k)
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    assert plan.dynamics is None and "dynamics" not in plan.to_json()
    n = N.build_export_notes(p, plan, "x.nam", "x.ir.wav")
    assert "dynamics" not in n
    # byte-identical to the notes built without any Task G code path: the stored gate / comp, unresolved
    assert {s["stage"]: s for s in n["stages"]}["gate"]["settings"]["releaseMs"] == 20.0
    assert json.dumps(n, sort_keys=True) == json.dumps(
        N.build_export_notes(copy.deepcopy(p), plan.to_json(), "x.nam", "x.ir.wav"), sort_keys=True)


def test_notes_from_a_raw_preset_resolve_too():
    p = match_preset()                                              # a plan dict without the label: notes resolve it themselves
    n = N.build_export_notes(p, {"mode": "nocab", "bypassed": []}, "x.nam", "x.ir.wav")
    assert n["dynamics"] == "live" and "busComp" not in {s["stage"] for s in n["stages"]}
