"""v0.4M Task F.4: dynamics sweep. Fixture preset (fixture NAMs / IRs) + a synthetic DI generated in the test."""
from __future__ import annotations

import json

import numpy as np
import pytest

from sawblade_match.matcher import dynsweep as DS

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from test_pathcheck import make_run                                    # noqa: E402


def sweep(tmp_path, mutate, topology="single"):
    result, di, preset = make_run(tmp_path, topology, mutate=mutate)
    return DS.dynsweep(result, di), result, di


def mk_gate(thr):
    def f(p):
        p["gate"] = {"enabled": True, "thresholdDb": thr, "hysteresisDb": 6.0, "attackMs": 0.5, "holdMs": 40.0,
                     "releaseMs": 150.0, "rangeDb": -50.0}
    return f


def test_bypassed_set_is_smooth_and_matches_when_nothing_to_bypass(tmp_path):
    r, result, di = sweep(tmp_path, mk_gate(-60.0))              # a gate below the DI's floor: nothing to gate
    assert r["inputOffsetsDb"] == [-12.0, -6.0, 0.0, 6.0] and len(r["matched"]["rows"]) == 4
    sb = r["bypassed"]["slopes"]
    assert all(s is not None and 0.0 < s < 2.0 for s in sb)       # monotone: louder in is never quieter out
    assert max(sb) - min(sb) < 1.0                               # smooth
    assert r["maxAbsSlopeDiff"] < 0.1
    assert [row["lufs"] for row in r["bypassed"]["rows"]] == sorted(row["lufs"] for row in r["bypassed"]["rows"])
    txt = DS.format_table(r)
    assert "matched" in txt and "bypassed" in txt and "max |slope difference|" in txt


def test_high_threshold_gate_shows_a_knee_at_the_low_step(tmp_path):
    r, *_ = sweep(tmp_path, mk_gate(-20.0))
    d = r["slopeDiff"]
    assert d[0] is not None and d[0] > 0.3                        # the gate closes more at -12 dB: matched slope is steeper
    assert abs(d[1]) < 0.1 and abs(d[2]) < 0.1                     # the knee is at the low step only
    assert r["gateEnabled"] is True


def test_four_to_one_comp_flattens_the_matched_slope(tmp_path):
    def comp(p):
        mk_gate(-60.0)(p)
        p["busComp"] = {"enabled": True, "thresholdDb": -40.0, "ratio": 4.0, "kneeDb": 6.0, "attackMs": 10.0,
                        "releaseMs": 100.0, "makeupDb": 0.0}
    r, *_ = sweep(tmp_path, comp)
    sm, sb = r["matched"]["slopes"], r["bypassed"]["slopes"]
    assert sum(sm) < sum(sb) - 0.5 and all(a < b for a, b in zip(sm, sb))
    assert r["busCompEnabled"] is True


def test_cli_writes_json_and_unreadable_exits_2(tmp_path):
    result, di, _ = make_run(tmp_path)
    out = tmp_path / "d.json"
    assert DS.main(["--result", str(result), "--di", str(di), "--json", str(out)]) == 0
    assert json.loads(out.read_text())["schema"] == "sawblade.dynsweep"
    assert DS.main(["--result", str(tmp_path / "x.json"), "--di", str(di)]) == 2


def test_live_set_is_swept_third_and_printed_after_matched_vs_bypassed(tmp_path):
    def match_with_comp(p):
        mk_gate(-60.0)(p)
        p["busComp"] = {"enabled": True, "thresholdDb": -40.0, "ratio": 4.0, "kneeDb": 6.0, "attackMs": 10.0,
                        "releaseMs": 100.0, "makeupDb": 0.0}
        p["origin"], p["dynamicsMode"], p["version"] = "match", "live", 4     # core derives the live set: bus comp off
    r, *_ = sweep(tmp_path, match_with_comp)
    assert len(r["live"]["rows"]) == 4 and len(r["live"]["slopes"]) == 3
    assert r["busCompEnabled"] is True and r["liveBusCompEnabled"] is False
    sm, sb, sl = r["matched"]["slopes"], r["bypassed"]["slopes"], r["live"]["slopes"]
    assert sum(sm) < sum(sb) - 0.5                       # the record comp flattens the matched slope ...
    assert sum(sl) > sum(sm) + 0.5                       # ... the live set (comp off) does not
    assert r["liveMaxAbsSlopeDiff"] is not None
    txt = DS.format_table(r)
    assert txt.index("bypassed") < txt.index("live set") and "max |slope difference| live vs bypassed" in txt


def test_table_formatter_has_no_core_dependency():
    import ast
    import inspect
    from sawblade_match.matcher import dynformat
    tree = ast.parse(inspect.getsource(dynformat))
    mods = [n.module or "" for n in ast.walk(tree) if isinstance(n, ast.ImportFrom)] + \
           [a.name for n in ast.walk(tree) if isinstance(n, ast.Import) for a in n.names]
    assert not any("core" in m or "engine" in m or "dynsweep" in m for m in mods)
