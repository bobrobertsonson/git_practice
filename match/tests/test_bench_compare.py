"""v0.7 Task B tests 5-6: ``sawblade-bench compare`` thresholds and the CLI/preflight behaviour (no matcher run needed)."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.bench import cli, compare as CMP, manifest as MF, runner as RN


def held(awt, **deltas):
    base = {"t12Ms": 5.0, "sustainDb": 0.5, "hfRatioDb": 0.3, "hfFlat": 0.005, "fluxDb": 0.1, "floorDb": 1.0}
    base.update(deltas)
    return {"aWeightedErrorDb": awt, "ltasLossDb": 1.0, "guardrails": {"fail": [], "marginal": []}, "feel": {"deltas": base}}


def case(awt, status="ok", tier=1, counts=True, **deltas):
    c = {"status": status, "tier": tier, "kind": "single", "counts": counts, "heldOut": held(awt, **deltas), "pathchecks": None, "transfer": []}
    return c


def scores(**cases):
    return {"schema": RN.SCHEMA, "version": 1, "gitSha": None, "cases": cases}


def write(tmp, name, s):
    p = tmp / name
    p.write_text(json.dumps(s))
    return p


def test_thresholds_flag_031_db_and_11_ms_but_not_029_db():
    a = scores(awt=case(1.0), t12=case(1.0), ok=case(1.0), better=case(1.0), same=case(1.0), gone=case(1.0))
    b = scores(awt=case(1.31), t12=case(1.0, t12Ms=16.0), ok=case(1.29), better=case(0.5), same=case(0.9), gone=case(1.0, status="error"))
    r = CMP.compare(a, b)
    v = {k: c["verdict"] for k, c in r["cases"].items()}
    assert v == {"awt": "worse", "t12": "worse", "ok": "same", "better": "better", "same": "same", "gone": "not compared"}
    assert r["cases"]["awt"]["rows"][0]["regressed"] == ["aWeightedErrorDb"] and r["cases"]["t12"]["rows"][0]["regressed"] == ["t12Ms"]
    assert r["counts"] == {"better": 1, "worse": 2, "same": 2, "notCompared": 1}
    assert CMP.summary_line(r).startswith("compare: better on 1, worse on 2, same on 2, not compared on 1 (thresholds: ")


def test_a_better_a_wt_with_a_feel_regression_is_worse_and_tier2_is_never_flagged():
    a = scores(x=case(1.0), t2=case(1.0, tier=2, counts=False))
    b = scores(x=case(0.4, sustainDb=2.1), t2=case(5.0, tier=2, counts=False))
    r = CMP.compare(a, b)
    assert r["cases"]["x"]["verdict"] == "worse" and r["cases"]["t2"]["verdict"] == "shown"
    assert r["counts"] == {"better": 0, "worse": 1, "same": 0, "notCompared": 0}


def test_every_threshold_is_the_stated_value():
    assert CMP.THRESHOLDS == {"aWeightedErrorDb": 0.30, "t12Ms": 10.0, "sustainDb": 1.5, "hfRatioDb": 1.0, "hfFlat": 0.02, "fluxDb": 0.3, "floorDb": 3.0}
    from sawblade_match.matcher.known_answer import FEEL_TOLERANCES
    assert {k: v for k, v in CMP.THRESHOLDS.items() if k != "aWeightedErrorDb"} == {k: v for k, v in FEEL_TOLERANCES.items() if k != "aWeightedErrorDb"}


def test_inner_path_checks_and_transfer_are_flagged_the_same_way():
    def blendcase(a_awt, tr):
        c = case(1.0)
        c["pathchecks"] = {"a": {**held(a_awt), "ref": "a"}, "b": {**held(1.0), "ref": "b"}, "ratio": {"diffDb": 0.5}}
        c["transfer"] = [{"status": "ok", "heldOut": held(tr)}]
        return c
    r = CMP.compare(scores(x=blendcase(1.0, 1.0)), scores(x=blendcase(1.5, 1.0)))
    assert r["cases"]["x"]["verdict"] == "worse" and [row["label"] for row in r["cases"]["x"]["rows"]] == ["x", "x/path a", "x/path b", "x/transfer 0"]
    r = CMP.compare(scores(x=blendcase(1.0, 1.0)), scores(x=blendcase(1.0, 1.4)))
    assert r["cases"]["x"]["rows"][3]["regressed"] == ["aWeightedErrorDb"]


def test_newly_failing_guardrails_are_listed_not_flagged():
    a, b = scores(x=case(1.0)), scores(x=case(1.0))
    b["cases"]["x"]["heldOut"]["guardrails"] = {"fail": ["thump_controlled"], "marginal": []}
    r = CMP.compare(a, b)
    assert r["cases"]["x"]["verdict"] == "same" and r["cases"]["x"]["newGuardrailFails"] == ["thump_controlled"]
    assert "thump_controlled" in CMP.format_markdown(r)


def test_cli_compare_exit_codes_threshold_override_json_and_schema_mismatch(tmp_path, capsys):
    pa = write(tmp_path, "a.json", scores(x=case(1.0), y=case(1.0)))
    worse = write(tmp_path, "b.json", scores(x=case(1.31), y=case(1.0)))
    ok = write(tmp_path, "c.json", scores(x=case(1.29), y=case(1.0)))
    assert cli.main(["compare", str(pa), str(worse)]) == 1
    assert cli.main(["compare", str(pa), str(ok)]) == 0
    out = capsys.readouterr().out
    assert "compare: better on 0, worse on 0, same on 2, not compared on 0" in out
    assert cli.main(["compare", str(pa), str(worse), "--threshold", "aWeightedErrorDb=0.5", "--json", str(tmp_path / "r.json")]) == 0
    out = capsys.readouterr().out
    assert "threshold overrides: aWeightedErrorDb=0.5" in out and "aWeightedErrorDb 0.5" in out
    assert json.loads((tmp_path / "r.json").read_text())["thresholds"]["aWeightedErrorDb"] == 0.5
    bad = write(tmp_path, "bad.json", {"schema": "something.else", "cases": {}})
    assert cli.main(["compare", str(pa), str(bad)]) == 2
    assert cli.main(["compare", str(pa), str(tmp_path / "nope.json")]) == 2
    assert cli.main(["compare", str(pa), str(worse), "--threshold", "bogus=1"]) == 2


# ---- preflight / run CLI ---------------------------------------------------------------------------------------------
def _manifest(tmp_path, root):
    sr = 48000
    x = (0.1 * np.random.default_rng(0).standard_normal(sr * 2)).astype(np.float32)
    sf.write(str(root / "di.wav"), x, sr, subtype="FLOAT")
    sf.write(str(root / "amp.wav"), x, sr, subtype="FLOAT")
    sf.write(str(root / "amp44.wav"), x[:44100], 44100, subtype="FLOAT")
    f = lambda p: {"path": p, "channel": "left"}
    mk = lambda cid, ref, counts=True: {"id": cid, "tier": 1, "kind": "single", "style": "t", "di": f("di.wav"), "reference": f(ref),
                                        "counts": counts, "confirmed": cid != "unsure"}
    cases = [mk("present", "amp.wav"), mk("absent", "nope/missing.wav"), mk("rates", "amp44.wav"), mk("unsure", "amp.wav", False)]
    p = tmp_path / "cases.json"
    p.write_text(json.dumps({"schema": MF.SCHEMA, "version": 1, "rootHint": "", "cases": cases, "gaps": []}))
    return p


def test_check_only_names_the_missing_file_and_exits_2(tmp_path, capsys):
    root = tmp_path / "root"
    root.mkdir()
    mp = _manifest(tmp_path, root)
    rc = cli.main(["run", "--root", str(root), "--manifest", str(mp), "--tier", "all", "--check-only", "--out", str(tmp_path / "o")])
    out = capsys.readouterr().out
    assert rc == 2 and "nope/missing.wav" in out and "sample rates differ" in out and "inferred" in out
    assert "| present | di.wav (di) | ok | 48000 |" in out
    rc = cli.main(["run", "--root", str(root), "--manifest", str(mp), "--cases", "present", "--check-only", "--out", str(tmp_path / "o")])
    assert rc == 0


def test_a_missing_case_is_skipped_and_the_total_says_n_of_m(tmp_path, capsys):
    root = tmp_path / "root"
    root.mkdir()
    mp = _manifest(tmp_path, root)
    m = MF.load_manifest(mp)
    s = RN.run_bench([c for c in m["cases"] if c["id"] in ("absent", "rates")], root, tmp_path / "out", pool=None, manifest_path=mp)
    assert s["cases"]["absent"]["status"] == "missing" and "nope/missing.wav" in s["cases"]["absent"]["error"]
    assert s["cases"]["rates"]["status"] == "error"
    assert s["total"]["n"] == 0 and s["total"]["m"] == 2
    assert "bench total: 0 of 2 counted cases ran" in (tmp_path / "out" / "scores.md").read_text()
    assert s["manifest"]["sha256"] == MF.sha256_file(mp)


def test_run_cli_smoke_with_the_runner_stubbed(tmp_path, capsys, monkeypatch):
    root = tmp_path / "root"
    root.mkdir()
    mp = _manifest(tmp_path, root)
    seen = {}

    def fake(cases, root_, out, **kw):
        seen.update(ids=[c["id"] for c in cases], kw=kw)
        return RN.run_bench([c for c in cases if c["id"] == "absent"], root_, out, pool=None, manifest_path=kw["manifest_path"], args=kw["args"])
    monkeypatch.setattr(cli, "run_bench", fake)
    from sawblade_match.matcher import pool as P
    monkeypatch.setattr(P, "load_pool", lambda p: "POOL")
    monkeypatch.setattr("sawblade_match.matcher.cli.resolve_ir_dirs", lambda *a, **k: [])
    rc = cli.main(["run", "--root", str(root), "--manifest", str(mp), "--cases", "absent", "--quick", "--seed", "3", "--threads", "2",
                   "--pool", "pool.json", "--no-ir-dirs", "--out", str(tmp_path / "out")])
    out = capsys.readouterr().out
    assert rc == 0 and seen["ids"] == ["absent"] and seen["kw"]["quick"] is True and seen["kw"]["seed"] == 3 and seen["kw"]["pool"] == "POOL"
    assert out.strip().splitlines()[-1] == "bench total: 0 of 1 counted cases ran, mean held-out A-wt n/a dB, median n/a dB, within 0.5 dB: 0, guardrail fails 0"
    assert cli.main(["run", "--root", str(root), "--manifest", str(mp), "--cases", "nosuch", "--out", str(tmp_path / "o")]) == 2
    assert cli.main(["run", "--root", str(root), "--manifest", str(tmp_path / "none.json"), "--out", str(tmp_path / "o")]) == 2
    assert cli.main(["run", "--manifest", str(mp)]) == 2                      # argparse: --root/--out required
    assert cli.main(["run", "--root", str(root), "--manifest", str(mp), "--cases", "absent", "--out", str(tmp_path / "o")]) == 2   # no --pool
