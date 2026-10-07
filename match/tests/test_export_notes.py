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


def test_a_modeled_pedal_block_is_inside_the_model_not_in_the_notes():
    p = full()
    blocks = p["paths"]["a"]["blocks"]
    blocks.insert(len(blocks) - 1, {"id": "a_boost", "type": "pedal.ts", "slot": "boost", "modelVersion": 1,
                                    "params": {"drive": 1.0, "tone": 5.0, "level": 8.0}})
    n = N.build_export_notes(p, P.make_plan(p, "nocab", allow_inexact=True), "x-nocab-standard.nam", "x-nocab.ir.wav")
    assert [s["stage"] for s in n["stages"]] == ["gate", "cab", "postEq", "busComp"]      # same stages as without the boost
    assert "pedal.ts" not in json.dumps(n) and "boost" not in json.dumps(n).lower()


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


def test_nocab_drop_comp_lists_the_comp_from_the_original_preset():
    # The plugin trains a copy with the bus comp switched off ("drop"); the notes must come from the original rig.
    orig = full()
    dropped = copy.deepcopy(orig)
    dropped["busComp"]["enabled"] = False
    plan = P.make_plan(dropped, "nocab")                       # what the export sees: no comp, nothing bypassed
    assert "busComp" not in [s["stage"] for s in N.build_export_notes(dropped, plan, "m.nam", "m.ir.wav")["stages"]]
    n = N.build_export_notes(orig, plan, "m.nam", "m.ir.wav")
    assert [s["stage"] for s in n["stages"]] == ["gate", "cab", "postEq", "busComp"]
    s = n["stages"][-1]
    assert s["position"] == "after NAM" and s["inModel"] is False
    assert (s["settings"]["thresholdDb"], s["settings"]["ratio"], s["settings"]["attackMs"],
            s["settings"]["releaseMs"], s["settings"]["kneeDb"], s["settings"]["makeupDb"]) == (-18, 4, 5, 80, 3, 2.5)
    assert n == N.build_export_notes(orig, P.make_plan(orig, "nocab", allow_inexact=True), "m.nam", "m.ir.wav")


def test_cli_accepts_notes_preset():
    from sawblade_match.export.cli import build_parser
    a = build_parser().parse_args(["p.json", "--notes-preset", "orig.json"])
    assert a.notes_preset == "orig.json" and build_parser().parse_args(["p.json"]).notes_preset is None


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


def test_pre_eq_is_listed_as_in_the_model_not_as_a_stage():
    p = full()
    p["paths"]["a"]["preEq"] = [{"type": "highPass", "freq": 110.0, "q": 0.707},
                                {"type": "peak", "freq": 900.0, "gainDb": 6.0, "q": 0.8}]
    n = N.build_export_notes(p, P.make_plan(p, "nocab", allow_inexact=True), "x-nocab-standard.nam", "x-nocab.ir.wav")
    assert [s["stage"] for s in n["stages"]] == ["gate", "cab", "postEq", "busComp"]       # nothing to add on hardware
    (pre,) = n["inModel"]
    assert pre["stage"] == "preEq" and pre["inModel"] is True and pre["position"] == "inside NAM" and pre["path"] == "a"
    assert [b["type"] for b in pre["settings"]["bands"]] == ["highPass", "peak"] and pre["settings"]["bands"][1]["gainDb"] == 6.0
    assert "trained into the model" in pre["hardware"] and "110" in pre["hardware"]
    txt = N.format_notes_txt(n, "x")
    assert "In the model (nothing to add) - path a" in txt and "high-pass 110 Hz" in txt
    q = base()
    q["paths"]["a"].pop("preEq", None)
    assert "inModel" not in N.build_export_notes(q, P.make_plan(q, "withcab"), "m.nam")


# ---------------------------------------------------------------- Anagram device profile (v0.6 Task C)

def _anagram(p, mode="nocab", **kw):
    plan = P.make_plan(p, mode, allow_inexact=True)
    n = N.build_export_notes(p, plan, "x-nocab-full.a2_full.nam", "x-nocab.ir.wav")
    return N.build_anagram_profile(p, plan, n, "x-nocab-full.a2_full.nam", "x-nocab.ir.wav",
                                   drive_only=P.drive_only(p, plan), **kw), n


def test_anagram_gate_comp_posteq_nocab_blocks_order_and_numbers():
    p = full()
    p["busComp"].update({"attackMs": 1.0, "releaseMs": 40.0})                       # a fast bus comp
    prof, generic = _anagram(p, model_label="A2 Full", file="x-nocab-full.anagram_notes.txt")
    assert set(prof) == {"device", "message", "stages", "loaderOrder", "file"}
    assert [(s["stage"], s["block"]) for s in prof["stages"]] == [("gate", "Gate"), ("model", "Neural Amp"),
                                                                    ("cab", "IR"), ("busComp", "Comp" "ressor")]
    assert [s["position"] for s in prof["stages"]] == ["1 (first in the chain)", "2", "3", "4 (last in the chain)"]
    assert prof["loaderOrder"] == "Anagram chain: Gate -> Neural Amp -> IR -> Compressor"
    gate, model, ir, comp = prof["stages"]
    assert gate["settings"]["threshold dB"] == -55 and gate["settings"]["close threshold dB"] == -61
    assert (gate["settings"]["attack ms"], gate["settings"]["hold ms"], gate["settings"]["release ms"]) == (0.5, 20, 60)
    assert gate["settings"]["range dB"] == -90 and "guitar input" in gate["settings"]["keyed on"]
    assert "FIRST" in gate["hardware"] and "before every NAM block" in gate["hardware"]
    assert model["settings"] == {"model": "x-nocab-full.a2_full.nam", "bypass": False}
    assert "A2 Full" in model["hardware"] and "Neural Amp block" in model["hardware"]
    # the post EQ is folded into the exported IR: no EQ block, the IR block says so
    assert ir["settings"] == {"file": "x-nocab.ir.wav", "normalise": False, "contains": "cab and post EQ"}
    assert "WITHOUT loudness normalisation" in ir["hardware"] and "post EQ" in ir["hardware"]
    assert comp["settings"] == {"threshold dBFS": -21.0, "ratio": 4, "attack ms": 1.0, "release ms": 40.0,
                                "knee dB": 3, "make-up dB": 2.5, "detector": "peak"}
    assert "LAST, after the IR" in comp["hardware"]
    assert all(set(s) <= {"stage", "block", "position", "settings", "hardware"} for s in prof["stages"])
    blocks = {s["block"] for s in prof["stages"]}
    assert blocks <= {"Neural Amp", "Neural Pedal", "Neural Loader", "IR", "Compressor", "Gate", "EQ"}   # published list only
    assert generic["stages"] and "deviceProfiles" not in generic                      # the generic block is untouched


def test_anagram_model_block_is_neural_pedal_only_for_a_nocab_drive_only_export():
    p = full()
    p["paths"]["a"]["blocks"] = [{"id": "a1", "type": "pedal.ts", "slot": "boost", "modelVersion": 1, "params": {}}]
    p["paths"]["b"]["blocks"] = [{"id": "b1", "type": "nam", "slot": "pedal", "model": {"file": "../nam/wavenet.nam"}}]
    assert P.drive_only(p, P.make_plan(p, "nocab", True)) is True
    prof, _ = _anagram(p)
    assert [s["block"] for s in prof["stages"]][:2] == ["Gate", "Neural Pedal"]
    assert prof["loaderOrder"].startswith("Anagram chain: Gate -> Neural Pedal")
    # a chain with an amp, or any with-cab export, is a Neural Amp
    q = full()
    assert P.drive_only(q, P.make_plan(q, "nocab", True)) is False
    assert P.drive_only(p, P.make_plan(p, "withcab", True)) is False
    prof_w, _ = _anagram(p, mode="withcab")
    assert [s["block"] for s in prof_w["stages"]] == ["Gate", "Neural Amp"]           # nothing else to add around the model


def test_anagram_unfolded_post_eq_becomes_an_eq_block_after_the_ir():
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    n = N.build_export_notes(p, plan, "m.nam", "m.ir.wav")
    for st in n["stages"]:
        if st["stage"] == "postEq":
            st["settings"]["foldedIntoExportedIr"] = False
    prof = N.build_anagram_profile(p, plan, n, "m.nam", "m.ir.wav")
    assert [s["block"] for s in prof["stages"]] == ["Gate", "Neural Amp", "IR", "EQ", "Compressor"]
    eq = prof["stages"][3]
    assert eq["settings"]["band 2"] == "peak 1500 Hz, +2.0 dB, Q 1.2" and eq["settings"]["band 1"].startswith("high-pass 80 Hz")
    assert prof["stages"][2]["settings"]["contains"] == "cab"


def test_anagram_profile_when_the_model_is_the_whole_chain():
    p = base()
    prof, _ = _anagram(p, mode="withcab")
    assert [s["block"] for s in prof["stages"]] == ["Neural Amp"] and prof["stages"][0]["position"] == "1"
    assert prof["loaderOrder"] == "Anagram chain: Neural Amp"


def test_write_export_notes_writes_both_text_files_and_the_device_profile(tmp_path):
    p = full()
    plan = P.make_plan(p, "nocab", allow_inexact=True)
    nam = tmp_path / "riff-nocab-full.a2_full.nam"
    nam.write_text("{}")
    notes, txt = N.write_export_notes(p, plan, nam, tmp_path / "riff-nocab.ir.wav", "licence text", stem="riff-nocab-full",
                                      model_label="A2 Full")
    assert txt.name == "riff-nocab-full.export_notes.txt" and notes["file"] == txt.name
    a = tmp_path / "riff-nocab-full.anagram_notes.txt"
    prof = notes["deviceProfiles"]["anagram"]
    assert prof["file"] == a.name and a.is_file() and txt.is_file()
    body = a.read_text()
    assert body.startswith("Sawblade export notes for the Anagram - Golden shared (live-compatible)")
    assert "1. Gate [1 (first in the chain)]" in body and "2. Neural Amp [2]  (model)" in body
    assert "   model: riff-nocab-full.a2_full.nam" in body and "   bypass: no" in body and "   threshold dBFS: -21" in body
    assert "Anagram chain: Gate -> Neural Amp -> IR -> Compressor" in body and body.rstrip().endswith("licence text")
    generic = json.loads(json.dumps({k: v for k, v in notes.items() if k != "deviceProfiles"}))
    assert generic == {**N.build_export_notes(p, plan, nam.name, "riff-nocab.ir.wav"), "file": txt.name}   # v0.4M block unchanged
    assert N.NOTES_VERSION == 1
    # A1 exports (stem defaults to the .nam stem) get the same profile
    nam1 = tmp_path / "x-nocab-standard.nam"
    nam1.write_text("{}")
    n1, t1 = N.write_export_notes(p, plan, nam1, tmp_path / "x-nocab.ir.wav", None)
    assert t1.name == "x-nocab-standard.export_notes.txt" and (tmp_path / "x-nocab-standard.anagram_notes.txt").is_file()
    assert n1["deviceProfiles"]["anagram"]["stages"][1]["settings"]["model"] == "x-nocab-standard.nam"
def test_notes_preset_problems():
    orig = full()
    trained = copy.deepcopy(orig)
    trained["busComp"]["enabled"] = False
    trained["name"], trained["export"] = "other", {"mode": "nocab"}
    assert P.notes_preset_problems(trained, orig) == []
    assert P.notes_only(trained, orig)[0]["what"] == "busComp" and P.notes_only(orig, orig) == []
    trained["blend"] = 0.123                                   # differs beyond the comp
    assert "beyond the bus comp" in P.notes_preset_problems(trained, orig)[0]
    assert any("only for no-cab" in m for m in P.notes_preset_problems(orig, orig, "withcab"))
    kept = copy.deepcopy(orig)
    kept["busComp"]["ratio"] = 9.0                             # trained keeps its comp: settings must match
    assert "different bus comp settings" in P.notes_preset_problems(kept, orig)[0]


def test_notes_preset_ignores_derived_auto_trim():
    orig = full()
    orig["output"] = {"gainDb": -3.0, "autoTrimDb": -4.2, "autoTrimHash": "abc"}
    trained = copy.deepcopy(orig)
    trained["busComp"]["enabled"] = False
    trained["output"] = {"gainDb": -3.0}                       # the plugin strips autoTrimDb / autoTrimHash
    assert P.notes_preset_problems(trained, orig) == []
    trained["output"]["gainDb"] = -4.0                         # a real output change is still refused
    assert "beyond the bus comp" in P.notes_preset_problems(trained, orig)[0]


def test_anagram_profile_from_a_notes_preset_lists_the_dropped_comp(tmp_path):
    """--notes-preset flow: the trained preset has no comp, the notes are built from the original rig: the Anagram profile
    still ends with the Compressor block."""
    orig = full()
    trained = copy.deepcopy(orig)
    trained["busComp"]["enabled"] = False
    plan = P.make_plan(trained, "nocab")
    nam = tmp_path / "r-nocab-full.a2_full.nam"
    nam.write_text("{}")
    notes, _ = N.write_export_notes(orig, plan, nam, tmp_path / "r-nocab.ir.wav", None, stem="r-nocab-full")
    blocks = [s["block"] for s in notes["deviceProfiles"]["anagram"]["stages"]]
    assert blocks == ["Gate", "Neural Amp", "IR", "Compressor"]
