"""Export notes (Task E): pure, no sawblade_core / torch needed."""
from __future__ import annotations

import copy
import json
from pathlib import Path

from sawblade_match.export import notes as N
from sawblade_match.export import plan as P

REPO = Path(__file__).resolve().parents[2]
PRESETS = REPO / "tests" / "fixtures" / "presets"


def base() -> dict:
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    p["gate"] = {"enabled": False}
    p["busComp"] = {"enabled": False}
    p["postEq"] = []
    p["output"] = {"gainDb": 0.0}
    return p


def full() -> dict:
    p = base()
    p["gate"] = {"enabled": True, "thresholdDb": -55.0, "hysteresisDb": 6.0, "attackMs": 0.5, "holdMs": 20.0,
                 "releaseMs": 60.0, "rangeDb": -90.0}
    p["busComp"] = {"enabled": True, "thresholdDb": -18.0, "ratio": 4.0, "kneeDb": 3.0, "attackMs": 5.0,
                    "releaseMs": 80.0, "makeupDb": 2.5}
    p["postEq"] = [{"type": "highPass", "freq": 80.0, "q": 0.707},
                   {"type": "peak", "freq": 1500.0, "gainDb": 2.0, "q": 1.2},
                   {"type": "lowPass", "freq": 9000.0, "q": 0.707},
                   {"type": "peak", "freq": 500.0, "gainDb": 9.0, "q": 1.0, "enabled": False}]
    p["output"] = {"gainDb": -3.0}
    return p


def test_nocab_gate_comp_posteq_order_and_numbers():
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    n = N.build_export_notes(p, plan, "x-nocab-standard.nam", "x-nocab.ir.wav")
    assert [s["stage"] for s in n["stages"]] == ["gate", "cab", "postEq", "busComp"]
    assert [s["position"] for s in n["stages"]] == ["before NAM", "after NAM", "after NAM", "after NAM"]
    assert all(s["inModel"] is False for s in n["stages"])
    g, cab, eq, comp = n["stages"]
    assert g["settings"]["thresholdDb"] == -55 and g["settings"]["closeThresholdDb"] == -61
    assert (g["settings"]["attackMs"], g["settings"]["holdMs"], g["settings"]["releaseMs"]) == (0.5, 20, 60)
    assert g["settings"]["rangeDb"] == -90 and "DI" in g["settings"]["keyedOn"]
    assert "FIRST" in g["hardware"] and "open at -55 dB" in g["hardware"]
    assert cab["settings"]["exportedIr"] == "x-nocab.ir.wav"
    bands = eq["settings"]["bands"]
    assert [b["type"] for b in bands] == ["highPass", "peak", "lowPass"]          # disabled band dropped
    assert bands[0]["slopeDbPerOct"] == 12 and bands[1]["gainDb"] == 2.0 and bands[1]["q"] == 1.2
    s = comp["settings"]
    assert (s["thresholdDb"], s["ratio"], s["attackMs"], s["releaseMs"], s["kneeDb"], s["makeupDb"]) == \
        (-18, 4, 5, 80, 3, 2.5)
    assert s["thresholdDbFsOut"] == -21.0                                          # output gain -3 is in the model
    assert "-18 dB re the chain's pre-headroom level = -21 dB re 0 dBFS" in comp["hardware"]
    assert "ratio 4:1" in comp["hardware"] and "make-up +2.5 dB" in comp["hardware"]
    assert n["loaderOrder"].startswith("Loader order: gate -> NAM") and n["loaderOrder"].endswith("bus comp")
    assert "message" not in n


def test_withcab_nothing_dropped_lists_only_gate():
    p = full()
    p["busComp"]["releaseMs"] = 80.0
    n = N.build_export_notes(p, P.make_plan(p, "withcab"), "m.nam")
    assert [s["stage"] for s in n["stages"]] == ["gate"]
    assert n["loaderOrder"] == "Loader order: gate -> NAM (m.nam)"


def test_nothing_to_add():
    p = base()
    plan = P.make_plan(p, "withcab")
    n = N.build_export_notes(p, plan, "m.nam")
    assert n["stages"] == [] and n["message"].startswith("Nothing to add")
    assert "Nothing to add" in N.format_notes_txt(n, "x")
    assert n["loaderOrder"] == "Loader order: NAM (m.nam)"


def test_nocab_cab_and_post_eq_without_comp_or_gate():
    p = base()
    p["postEq"] = [{"type": "lowShelf", "freq": 120.0, "gainDb": -2.0, "q": 0.707}]
    n = N.build_export_notes(p, P.make_plan(p, "nocab"), "m.nam", "m.ir.wav")
    assert [s["stage"] for s in n["stages"]] == ["cab", "postEq"]
    assert "low shelf 120 Hz, -2.0 dB" in n["stages"][1]["hardware"]


def test_disabled_cab_not_listed():
    p = base()
    p["cab"]["enabled"] = False
    n = N.build_export_notes(p, P.make_plan(p, "nocab"), "m.nam")
    assert n["stages"] == []


def test_irmix_lists_both_irs_mix_offset_invert():
    p = base()
    ir = lambda f, t, mic: {"file": f"/a/b/{f}", "source": {"title": t, "creator": "Cr", "license": "cc-by-nc",
                                                            "provider": "tone3000", "id": 7, "mic": mic}}
    p["cab"] = {"mode": "irMix", "irA": ir("a.wav", "Cab A", "SM57"), "irB": ir("b.wav", "Cab B", "R121"),
                "mix": 0.25, "enabled": True, "offsetSamplesB": 3, "invertB": True}
    n = N.build_export_notes(p, P.make_plan(p, "nocab"), "m.nam", "m.ir.wav")
    c = n["stages"][0]
    assert c["settings"]["mix"] == 0.25 and c["settings"]["offsetSamplesB"] == 3 and c["settings"]["invertB"] is True
    assert c["settings"]["irA"]["file"] == "a.wav" and c["settings"]["irB"]["mic"] == "R121"
    h = c["hardware"]
    assert "75% A + 25% B" in h and "Cab A" in h and "Cab B" in h and "SM57" in h and "R121" in h
    assert "B offset 3 samples" in h and "polarity inverted" in h and "cc-by-nc" in h


def test_expander_and_key_hpf():
    p = base()
    p["gate"] = {"enabled": True, "mode": "expander", "ratio": 3.0, "keyHighPassHz": 120.0}
    n = N.build_export_notes(p, P.make_plan(p, "withcab"))
    h = n["stages"][0]["hardware"]
    assert h.startswith("Expander FIRST") and "ratio 3:1" in h and "key high-pass 120 Hz" in h


def test_plan_as_dict_equals_plan_object():
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    assert N.build_export_notes(p, plan) == N.build_export_notes(p, plan.to_json())


def test_writer_txt_and_json(tmp_path):
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    nam = tmp_path / "sw-nocab-standard.nam"
    nam.write_text("{}")
    notes, txt = N.write_export_notes(p, plan, nam, tmp_path / "sw-nocab.ir.wav", P.licence_note(p))
    assert txt == tmp_path / "sw-nocab-standard.export_notes.txt" and notes["file"] == txt.name
    t = txt.read_text()
    assert t.index("1. gate") < t.index("2. cab") < t.index("3. postEq") < t.index("4. busComp")
    assert "Loader order:" in t and "personal use only" in t
    json.dumps(notes)                                                              # serialisable for the report
    assert notes["stages"][0]["hardware"] in t


def test_nc_licence_note_kept_in_txt(tmp_path):
    p = base()
    p["paths"]["a"]["blocks"][0]["model"] = {"file": "m.nam", "source": {"title": "T", "license": "cc-by-nc"}}
    nam = tmp_path / "m.nam"
    nam.write_text("{}")
    _, txt = N.write_export_notes(p, P.make_plan(p, "withcab"), nam, None, P.licence_note(p))
    assert "NON-COMMERCIAL" in txt.read_text()


def test_deterministic():
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    snap = copy.deepcopy(p)
    a = N.build_export_notes(p, plan, "a.nam", "a.ir.wav")
    assert a == N.build_export_notes(p, plan, "a.nam", "a.ir.wav") and p == snap


def test_gate_object_without_enabled_defaults_on():
    p = base()
    p["gate"] = {"thresholdDb": -50.0}                     # core: enabled defaults to true when the object is present
    n = N.build_export_notes(p, P.make_plan(p, "withcab"))
    assert [s["stage"] for s in n["stages"]] == ["gate"] and n["stages"][0]["settings"]["thresholdDb"] == -50
    p["gate"] = {}                                         # present but empty: still enabled, core defaults
    n = N.build_export_notes(p, P.make_plan(p, "withcab"))
    assert [s["stage"] for s in n["stages"]] == ["gate"] and n["stages"][0]["settings"]["thresholdDb"] == -55
    for off in ({"enabled": False}, None):
        p["gate"] = off
        assert N.build_export_notes(p, P.make_plan(p, "withcab"))["stages"] == []
    del p["gate"]
    assert N.build_export_notes(p, P.make_plan(p, "withcab"))["stages"] == []
