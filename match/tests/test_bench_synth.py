"""v0.7 benchmark harness on synthetic cases (spec B.7 tests 1-4, 6): hidden chains from the fixture captures are rendered by the
core into ``tmp_path``; nothing is committed. The two matcher runs are shared by the tests of this module."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

pytest.importorskip("sawblade_match.core")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sawblade_match.core import render as core_render                                 # noqa: E402
from sawblade_match.bench import cli as bench_cli, manifest as MF, runner as RN   # noqa: E402
from sawblade_match.matcher import known_answer as K, refsum as RS               # noqa: E402
from sawblade_match.matcher.engine import RATE, Engine                           # noqa: E402
from sawblade_match.matcher.pathcheck import single_preset                       # noqa: E402
from sawblade_match.matcher.run import gate_envelope_floor_db                    # noqa: E402
from sawblade_match.matcher.space import build_preset, gate_preset               # noqa: E402
from test_matcher import fixture_pool, hidden, mkplan                            # noqa: E402

SEED = 5
EXCERPT_S = 2.0


def small_plan():
    """The plan of test_matcher's single-path known answer, single topologies only."""
    return mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=30, gens_gain=6, gens_final=20, pop_linear=16,
                  pop_gain=8, n_rescore_single=12, n_cab_single=12)


def riff_di() -> np.ndarray:
    """13 s synthetic DI: the fixture riff (4 s) looped three times at 1.0 / 0.9 / 1.1 (so the fit and the held-out sections
    differ in level), then a short ring-out; 48 kHz float32. Nothing is committed."""
    x, fs = sf.read(Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "di_riff.wav", dtype="float32")
    assert fs == RATE
    loops = [x * g for g in (1.0, 0.9, 1.1)]
    return np.concatenate(loops + [np.zeros(RATE, np.float32)]).astype(np.float32)


def blend_plan():
    return mkplan(top_k={"blend": 1, "single": 0, "single2": 0}, gens_linear=40, gens_gain=8, gens_final=25, pop_linear=16, pop_gain=8)


def _case(cid, kind, **kw):
    c = {"id": cid, "tier": 1, "kind": kind, "style": "synthetic", "counts": kw.pop("counts", True), "confirmed": True}
    c.update(kw)
    return c


def _write_manifest(path: Path, cases: list[dict]) -> Path:
    m = MF.validate({"schema": MF.SCHEMA, "version": 1, "rootHint": "", "cases": cases, "gaps": []})
    path.write_text(json.dumps({"schema": MF.SCHEMA, "version": 1, "rootHint": "", "cases": cases, "gaps": []}))
    return path, m


def _hidden_preset(pool, topo, di):
    combo, sp, v = hidden(pool, topo)
    gate = gate_preset(gate_envelope_floor_db(di, RATE))
    eng = Engine(gate)
    try:
        return combo, build_preset(combo, v, gate=gate, align=eng.probe_align(combo, v))
    finally:
        eng.close()


class Spy:
    """Records the Config of every run_match call (test 3)."""

    def __init__(self, real):
        self.real, self.calls = real, []

    def __call__(self, cfg, log=None):
        di, _ = sf.read(str(cfg.di), dtype="float32")
        self.calls.append({"di": di, "ref": np.asarray(cfg.ref.matched_sig), "cfg": cfg})
        return self.real(cfg, log)


def _run(tmp, cases, plan, spy=None, root=None, mdir=None):
    mp = pytest.MonkeyPatch()
    try:
        if spy is not None:
            mp.setattr(RN, "run_match", spy)
        manifest_path, m = _write_manifest((mdir or tmp) / "cases.json", cases)
        return RN.run_bench(m["cases"], root or tmp / "root", tmp / "out", pool=fixture_pool(), plan=plan, seed=SEED, threads=2,
                            excerpt_s=EXCERPT_S, manifest_path=manifest_path, args={"t": 1})
    finally:
        mp.undo()


@pytest.fixture(scope="module")
def single_world(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("bench_single")
    root = tmp / "root"
    root.mkdir()
    di = riff_di()
    pool = fixture_pool()
    _, preset = _hidden_preset(pool, "single", di)
    amp, _ = core_render(preset, di, float(RATE))
    sf.write(str(root / "di.wav"), di, RATE, subtype="FLOAT")
    sf.write(str(root / "amp.wav"), amp, RATE, subtype="FLOAT")
    case = _case("hidden_single", "single", di={"path": "di.wav", "channel": "left"}, reference={"path": "amp.wav", "channel": "left"},
                 offsetMs=0.0, fit=[0, 6], heldOut=[6, 12], topology="auto")
    spy = Spy(RN.run_match)
    scores = _run(tmp, [case], small_plan(), spy)
    return {"tmp": tmp, "scores": scores, "spy": spy, "case": case, "di": di}


def test_single_path_known_answer_on_the_held_out_section(single_world):
    c = single_world["scores"]["cases"]["hidden_single"]
    assert c["status"] == "ok", c
    assert c["chosen"]["topology"] == "single"
    assert c["sections"]["fit"] == [0.0, 6.0] and c["sections"]["heldOut"] == [6.0, 12.0] and c["sections"]["source"]["fit"] == "manifest"
    assert c["heldOut"]["aWeightedErrorDb"] <= 0.5, c["heldOut"]
    assert c["heldOut"]["ltasLossDb"] is not None and c["heldOut"]["feel"]["deltas"] is not None
    assert single_world["scores"]["total"]["n"] == 1 and single_world["scores"]["total"]["within05"] == 1
    out = single_world["tmp"] / "out"
    assert (out / "scores.json").is_file() and (out / "scores.md").is_file()
    ref, _ = sf.read(str(out / "listen" / "hidden_single" / "ref.wav"))
    ren, _ = sf.read(str(out / "listen" / "hidden_single" / "render.wav"))
    assert len(ref) == len(ren) and 5.5 * RATE < len(ref) <= 6 * RATE + 1
    from sawblade_match.bench.score import lufs
    assert lufs(ren) == pytest.approx(lufs(ref), abs=0.1)


def test_the_matcher_only_sees_the_fit_section(single_world):
    (call,) = single_world["spy"].calls
    di = single_world["di"]
    n_fit = 6 * RATE
    assert len(call["di"]) == n_fit and len(call["ref"]) == n_fit                    # durations = the fit section
    assert np.array_equal(call["di"], di[:n_fit])                                    # exactly the fit samples of the DI
    held = di[6 * RATE:12 * RATE]
    # no run of the held-out window inside what the matcher received
    probe = held[RATE:RATE + 4096]
    hay = call["di"]
    assert not any(np.array_equal(hay[i:i + 4096], probe) for i in range(0, len(hay) - 4096, 97))
    assert single_world["scores"]["cases"]["hidden_single"]["sections"]["heldOut"][0] >= 6.0


def test_two_runs_with_the_same_seed_give_identical_scores_except_timings(single_world, tmp_path):
    """Determinism of the whole harness (sections, matcher, renders, scores): the same case twice, a tiny plan, same seed."""
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0})
    f = {"path": "di.wav", "channel": "left"}
    a = {"path": "amp.wav", "channel": "left"}
    cases = [{**single_world["case"], "transfer": [{"di": f, "reference": a}]},          # the same take as its own transfer pair (offset searched)
             _case("hidden_tier2", "single", tier=2, counts=False, di=f, reference=a, offsetMs=None, fit=None, heldOut=None, topology="auto")]
    runs = []
    for k in range(2):
        d = tmp_path / f"r{k}"
        d.mkdir()
        runs.append(_run(d, cases, plan, root=single_world["tmp"] / "root", mdir=tmp_path))
    strip = lambda s: json.loads(json.dumps({**{k: v for k, v in s.items() if k != "timings"},
                                             "cases": {i: {k: v for k, v in c.items() if k != "runtimeS"} for i, c in s["cases"].items()}},
                                            default=str))
    assert strip(runs[0]) == strip(runs[1])
    c, t2 = runs[0]["cases"]["hidden_single"], runs[0]["cases"]["hidden_tier2"]
    assert c["status"] == "ok" and t2["status"] == "ok"
    (tr,) = c["transfer"]
    assert tr["status"] == "ok" and np.isfinite(tr["heldOut"]["aWeightedErrorDb"]) and abs(tr["offset"]["samples48"]) < 0.05 * RATE
    assert runs[0]["total"]["n"] == 1 and runs[0]["total"]["m"] == 1                       # tier 2 never counts
    # tier 2: sections in reference seconds chosen automatically, only the A-weighted error is scored
    assert t2["sections"]["timeBase"] == "reference" and t2["sections"]["source"] == {"fit": "auto", "heldOut": "auto"}
    f2, h2 = t2["sections"]["fit"], t2["sections"]["heldOut"]
    assert f2[1] <= h2[0] and h2[1] - h2[0] >= 2.0 and np.isfinite(t2["heldOut"]["aWeightedErrorDb"])
    assert t2["heldOut"]["feel"] is None and t2["heldOut"]["feelReason"] == "unmatched reference" and t2["offset"]["source"].startswith("n/a")
    texts = []
    for k in range(2):
        j = json.loads((tmp_path / f"r{k}" / "out" / "scores.json").read_text())
        j.pop("timings")
        for c in j["cases"].values():
            c.pop("runtimeS")
        texts.append(json.dumps(j, sort_keys=True))
    assert texts[0] == texts[1]                                  # the files on disk, not only the returned dicts


@pytest.fixture(scope="module")
def blend_world(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("bench_blend")
    root = tmp / "root"
    root.mkdir()
    di = riff_di()
    pool = fixture_pool()
    combo, preset = _hidden_preset(pool, "blend", di)
    full, rep = core_render(preset, di, float(RATE))
    trims = {"a": float((rep.get("levelMatch") or {}).get("trimADb") or 0.0), "b": float((rep.get("levelMatch") or {}).get("trimBDb") or 0.0)}
    a, _ = core_render(single_preset(preset, "a", trims), di, float(RATE))
    b, _ = core_render(single_preset(preset, "b", trims), di, float(RATE))
    for n, x in (("di", di), ("a", a), ("b", b)):
        sf.write(str(root / f"{n}.wav"), x, RATE, subtype="FLOAT")
    cases = [_case("hidden_blend", "blend", di={"path": "di.wav", "channel": "left"},
                   reference={"tracks": [{"path": "a.wav", "role": "a", "gainDb": 0.0}, {"path": "b.wav", "role": "b", "gainDb": 0.0}],
                              "polarity": "auto"}, offsetMs=0.0, fit=[0, 6], heldOut=[6, 12], topology="blend"),
             _case("hidden_a", "pathcheck", reference={"parent": "hidden_blend", "path": "a"}, counts=False),
             _case("hidden_b", "pathcheck", reference={"parent": "hidden_blend", "path": "b"}, counts=False)]
    scores = _run(tmp, cases, blend_plan())
    return {"tmp": tmp, "scores": scores, "full": full, "a": a, "b": b, "di": di, "preset": preset, "combo": combo, "rep": rep,
            "cases": cases}


def test_hidden_paths_sum_to_the_full_render_through_refsum(blend_world):
    w = blend_world
    ssum, rep = RS.blend_refs(w["a"].astype(np.float64), w["b"].astype(np.float64), RATE)
    assert rep["polarity"]["chosen"] == "asis"
    err = np.max(np.abs(ssum - w["full"][:len(ssum)]))
    assert err < 1e-3 * max(1e-9, float(np.max(np.abs(w["full"])))) + 1e-5, err       # linear law, no bus comp: solo renders add up


def test_blend_known_answer_through_refsum_and_the_path_checks(blend_world):
    s = blend_world["scores"]
    c = s["cases"]["hidden_blend"]
    assert c["status"] == "ok", c
    assert c["chosen"]["topology"] == "blend" and c["referenceReport"]["polarity"]["chosen"] == "asis"
    assert c["heldOut"]["aWeightedErrorDb"] <= 0.5, c["heldOut"]
    pc = c["pathchecks"]
    for k in ("a", "b"):
        assert np.isfinite(pc[k]["aWeightedErrorDb"]) and pc[k]["case"] == f"hidden_{k}"
    r = pc["ratio"]
    assert all(np.isfinite(r[k]) for k in ("chosenDb", "refDb", "diffDb"))
    hid = {k: (v.model_id if v else None) for k, v in blend_world["combo"].captures().items()}
    got = {k: (v["modelId"] if v else None) for k, v in c["chosen"]["captures"].items()}
    if got == hid and c["chosen"]["irMix"] is None:        # the hidden chain itself was recovered: the per-path answers must be exact too
        assert pc["a"]["aWeightedErrorDb"] <= 0.5 and pc["b"]["aWeightedErrorDb"] <= 0.5 and abs(r["diffDb"]) < 1.0
    print("\nblend known answer (recovered hidden captures: %s):" % (got == hid), "hidden", hid, "found", got, "", {"heldOut": c["heldOut"]["aWeightedErrorDb"], "a": pc["a"]["aWeightedErrorDb"],
                                   "b": pc["b"]["aWeightedErrorDb"], "ratio": r})
    for k in ("a", "b"):                                             # the path-check cases carry the parent's numbers
        ch = s["cases"][f"hidden_{k}"]
        assert ch["status"] == "ok" and ch["heldOut"]["aWeightedErrorDb"] == pc[k]["aWeightedErrorDb"] and ch["counts"] is False
    assert s["total"]["n"] == 1 and s["total"]["m"] == 1


def test_harness_oracle_the_hidden_chain_itself_scores_zero_on_every_path_check(blend_world):
    """Harness check without the matcher: feed the hidden preset in as the winner. Its held-out A-wt error, both path checks
    and the blend ratio must come out ~0 (so a large path-check value in a real run is the matcher's, not the harness')."""
    w = blend_world
    from sawblade_match.bench import score as SC
    case = MF.validate({"schema": MF.SCHEMA, "version": 1, "cases": w["cases"], "gaps": []})["cases"][0]
    root = w["tmp"] / "root"
    ctx = RN.Ctx(root, w["tmp"] / "oracle", None, None, False, 0, 1, None, (), 2.0, lambda m: None, SC.Scorer())
    rd = RN.load_reference_signal(root, case["reference"])
    di, ref = RN.load_signal(root, case["di"]), rd["sig"]
    eng = Engine(None, 1)
    try:
        y, rep = eng.render(w["preset"], di, float(RATE))
        lo, hi = 6 * RATE, 12 * RATE
        held = RN._held_numbers(ctx, y, di, ref, lo, hi, 0, ctx.out / "ref.wav", None)
        pc = RN._pathchecks(ctx, case, [], ctx.out, eng, w["preset"], rep, rd, di, ref, lo, hi, 0, None)
    finally:
        eng.close()
    assert held["aWeightedErrorDb"] < 0.05 and held["feel"]["deltas"]["t12Ms"] < 1.0
    assert pc["a"]["aWeightedErrorDb"] < 0.05 and pc["b"]["aWeightedErrorDb"] < 0.05
    assert abs(pc["ratio"]["diffDb"]) < 0.1, pc["ratio"]
