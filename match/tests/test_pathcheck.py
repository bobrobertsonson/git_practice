"""v0.4M Task F.2: per-path check of a matched blend. Synthetic DI generated here; fixture NAMs / IRs of tests/fixtures."""
from __future__ import annotations

import json

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher import refsum as RS
from sawblade_match.matcher.space import build_preset, gate_preset, manual_align

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher import pathcheck as PC                    # noqa: E402
from sawblade_match.matcher.engine import Engine                      # noqa: E402
from sawblade_match.matcher.reference import load_reference           # noqa: E402
from sawblade_match.matcher.run import Config, Log, run_match         # noqa: E402
from test_matcher import _setup_known, fixture_pool, hidden, mkplan   # noqa: E402

FS = 48000


def synth_di(seconds=8.0, seed=3):
    """Karplus-Strong palm-mute-ish plucks (low strings), 0.3 s apart, plus a little noise floor. Seeded."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    x = rng.normal(size=n) * 1e-4
    t = 0
    for f in (82.4, 82.4, 110.0, 82.4, 98.0, 82.4, 123.5, 82.4) * 4:
        if t + FS // 3 >= n:
            break
        p = int(FS / f)
        buf = rng.uniform(-1, 1, p)
        m = int(0.28 * FS)
        out = np.empty(m)
        for i in range(m):
            out[i] = buf[i % p]
            buf[i % p] = 0.5 * (buf[i % p] + buf[(i + 1) % p]) * 0.996
        x[t:t + m] += out * 0.3
        t += int(0.3 * FS)
    return x.astype(np.float32)


def make_run(tmp_path, topology="blend", mutate=None):
    """A fake run folder: result.json + best.preset.resolved.json (a fixture-pool preset), a synthetic DI file."""
    pool = fixture_pool()
    combo, sp, v = hidden(pool, topology)
    preset = build_preset(combo, v, gate=gate_preset(-60.0), align=manual_align(0, False))
    if mutate:
        mutate(preset)
    run = tmp_path / "run"
    run.mkdir()
    di = tmp_path / "di.wav"
    sf.write(str(di), synth_di(), FS, subtype="FLOAT")
    (run / "best.preset.resolved.json").write_text(json.dumps(preset))
    res = {"schema": "sawblade.match_result", "di": str(di), "best": {"preset": "best.preset.resolved.json"},
           "offsetRefinement": {"final": {"L": {"offsetSamples": 0, "rate": FS}}}}
    (run / "result.json").write_text(json.dumps(res))
    return run / "result.json", di, preset


def render_wav(path, preset, x):
    y, _ = core.render(preset, x, float(FS))
    sf.write(str(path), y, FS, subtype="FLOAT")
    return y


def test_single_preset_folds_trims_and_two_singles_sum_to_the_full_render(tmp_path):
    def trims(p):
        p["levelMatch"] = {"mode": "manual", "trimADb": 0.0, "trimBDb": 4.0}
    _, di, preset = make_run(tmp_path, mutate=trims)
    x, _ = sf.read(str(di), dtype="float32")
    eng = Engine(None, 1)
    try:
        full, rep = eng.render(preset, x, FS)
        assert rep["levelMatch"]["trimBDb"] == pytest.approx(4.0)
        t = {"a": rep["levelMatch"]["trimADb"], "b": rep["levelMatch"]["trimBDb"]}
        ya, _ = eng.render(PC.single_preset(preset, "a", t), x, FS)
        yb, _ = eng.render(PC.single_preset(preset, "b", t), x, FS)
        bare, _ = eng.render(PC.single_preset(preset, "b", {}), x, FS)       # without the fold B is 4 dB quieter
    finally:
        eng.close()
    assert np.max(np.abs(full - (ya + yb))) < 1e-4 * np.max(np.abs(full))
    r = np.sqrt(np.mean(yb.astype(np.float64) ** 2) / np.mean(bare.astype(np.float64) ** 2))
    assert 20 * np.log10(r) == pytest.approx(4.0, abs=0.01)


def test_blend_with_refs_rendered_per_path(tmp_path):
    result, di, preset = make_run(tmp_path)
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b, ref_k = tmp_path / "a.wav", tmp_path / "b.wav", tmp_path / "blend.wav"
    render_wav(ref_a, PC.single_preset(preset, "a", {}), x)
    render_wav(ref_b, PC.single_preset(preset, "b", {}), x)
    a, _ = RS.read_mono(ref_a)
    b, _ = RS.read_mono(ref_b)
    sf.write(str(ref_k), (a + b).astype(np.float32), FS, subtype="FLOAT")
    out_json = tmp_path / "pc.json"
    rc = PC.main(["--result", str(result), "--di", str(di), "--ref-a", str(ref_a), "--ref-b", str(ref_b),
                  "--ref-blend", str(ref_k), "--json", str(out_json)])
    assert rc == 0
    r = json.loads(out_json.read_text())
    assert r["singlePath"] is False and r["offset"]["samples48"] == 0 and r["offset"]["source"] == "run final offset"
    assert r["a"]["aWeightedErrorDb"] < 0.1 and r["b"]["aWeightedErrorDb"] < 0.1 and r["full"]["aWeightedErrorDb"] < 0.1
    assert r["a"]["ltasLossDb"] < 0.1 and r["b"]["ltasLossDb"] < 0.1
    # a role swap is visible: the swapped pairing is clearly larger than the right one
    assert r["swapped"]["aVsRefB"] > r["a"]["aWeightedErrorDb"] + 0.3
    assert r["swapped"]["bVsRefA"] > r["b"]["aWeightedErrorDb"] + 0.3
    # blend ratio of the renders == ratio of the references
    q = r["ratio"]
    assert abs(q["chosenDb"] - q["refDb"]) < 0.1 and abs(q["diffDb"]) < 0.1
    assert q["chosenDb"] == pytest.approx(q["chosenDb"]) and abs(q["chosenDb"]) > 0.0
    for k in ("a", "b", "full"):
        fe = r[k].get("feel")
        assert fe is not None and fe["mode"] == "paired" and "tight" in fe and "fizz" in fe and "polish" in fe
    # human block names every pairing
    txt = PC.format_report(r)
    assert "A alone" in txt and "B alone" in txt and "swapped pairing" in txt and "blend ratio" in txt


def test_blend_db_moves_the_reference_ratio_only(tmp_path):
    result, di, preset = make_run(tmp_path)
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b = tmp_path / "a.wav", tmp_path / "b.wav"
    render_wav(ref_a, PC.single_preset(preset, "a", {}), x)
    render_wav(ref_b, PC.single_preset(preset, "b", {}), x)
    r0 = PC.pathcheck(result, di, ref_a, ref_b)
    r1 = PC.pathcheck(result, di, ref_a, ref_b, blend_db=(-3.0, 2.0))
    assert "full" not in r0
    assert r1["ratio"]["refDb"] - r0["ratio"]["refDb"] == pytest.approx(-5.0, abs=0.01)
    assert r1["ratio"]["chosenDb"] == pytest.approx(r0["ratio"]["chosenDb"])


def test_single_path_winner(tmp_path):
    result, di, preset = make_run(tmp_path, "single")
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b, ref_k = tmp_path / "a.wav", tmp_path / "b.wav", tmp_path / "blend.wav"
    y = render_wav(ref_k, preset, x)
    sf.write(str(ref_a), y, FS, subtype="FLOAT")
    sf.write(str(ref_b), y * 0.5, FS, subtype="FLOAT")
    r = PC.pathcheck(result, di, ref_a, ref_b, ref_k)
    assert r["singlePath"] is True and r["path"] == "a" and "a" not in r and "swapped" not in r and "ratio" not in r
    assert r["full"]["ref"] == "blend" and r["full"]["aWeightedErrorDb"] < 0.1
    assert "single-path" in PC.format_report(r)


def test_level_off_path_counts_as_single(tmp_path):
    def off(p):
        p["paths"]["b"]["levelDb"] = -60.0
    result, di, _ = make_run(tmp_path, mutate=off)
    st = PC.path_state(json.loads((result.parent / "best.preset.resolved.json").read_text()))
    assert st == {"a": True, "b": False}


def test_unreadable_inputs_exit_2(tmp_path):
    assert PC.main(["--result", str(tmp_path / "nope.json"), "--di", "x", "--ref-a", "y", "--ref-b", "z"]) == 2
    assert PC.main(["--result", "r", "--di", "x", "--ref-a", "y", "--ref-b", "z", "--blend-db", "q"]) == 2


def test_metrics_equal_the_runs_own_after_numbers(tmp_path):
    """A real (tiny) matcher run on fixture captures; pathcheck of its winning preset against the same reference must
    reproduce the run's ``after[0].aWeightedErrorDb`` (to its 3-decimal rounding)."""
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2, plan=mkplan(),
                 write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    r = PC.pathcheck(tmp_path / "out" / "result.json", di, ref.path, ref.path, ref.path)
    want = res["after"][0]["aWeightedErrorDb"]
    assert want is not None
    assert r["full"]["aWeightedErrorDb"] == pytest.approx(want, abs=6e-4)
    assert r["runAfterDb"] == want and r["offset"]["source"] == "run final offset"
    assert r["full"]["feel"] is not None and r["full"]["feel"]["mode"] == "paired"


def test_run_number_only_for_the_runs_own_di_and_bus_comp_caveat(tmp_path):
    def comp(p):
        p["busComp"] = {"enabled": True, "thresholdDb": -40.0, "ratio": 4.0, "kneeDb": 6.0, "attackMs": 10.0,
                        "releaseMs": 100.0, "makeupDb": 0.0}
    result, di, preset = make_run(tmp_path, mutate=comp)
    x, _ = sf.read(str(di), dtype="float32")
    ref_a, ref_b = tmp_path / "a.wav", tmp_path / "b.wav"
    render_wav(ref_a, PC.single_preset(preset, "a", {}), x)
    render_wav(ref_b, PC.single_preset(preset, "b", {}), x)
    res = json.loads(result.read_text())
    res["after"] = [{"aWeightedErrorDb": 1.5}]
    result.write_text(json.dumps(res))
    same = PC.pathcheck(result, di, ref_a, ref_b)
    assert same["diIsRunDi"] is True and same["runAfterDb"] == 1.5
    assert same["busCompEnabled"] is True and same["ratio"]["busCompEnabled"] is True
    assert "caveat" in PC.format_report(same) and "bus comp" in PC.format_report(same)
    other = tmp_path / "other_di.wav"
    sf.write(str(other), x, FS, subtype="FLOAT")
    r2 = PC.pathcheck(result, other, ref_a, ref_b, offset_ms=0.0)
    assert r2["diIsRunDi"] is False and "runAfterDb" not in r2 and r2["offset"]["source"] == "--offset-ms"
    assert "no run number" in PC.format_report(r2)
