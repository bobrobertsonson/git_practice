"""Phase 6b: matcher speed levers (--quick / --thorough, plateau stop, coarse screen, core cache) and --progress-json."""
from __future__ import annotations

import json
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher import cma
from sawblade_match.matcher.cli import build_parser
from sawblade_match.matcher.progress import NullProgress, Progress
from sawblade_match.matcher.screen import select_coarse

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher.engine import Engine           # noqa: E402
from sawblade_match.matcher.run import Config, Log, Plan, run_match    # noqa: E402
from sawblade_match.matcher.space import chain_blocks      # noqa: E402
from sawblade_match.matcher.prescreen import DEFAULT_V      # noqa: E402

from test_matcher import FIX, _setup_known, fixture_pool, mkplan   # noqa: E402


# ---- CMA-ES plateau stop ---------------------------------------------------------------------------------------------
def test_cma_plateau_stop_is_deterministic_and_off_by_default():
    f = lambda x: float(np.sum((x - 0.3) ** 2))
    _, _, h_full = cma.minimize(f, np.full(4, 0.9), 0.3, generations=40, seed=2)
    assert len(h_full) == 41                                         # no patience: every generation runs
    bx, bf, h = cma.minimize(f, np.full(4, 0.9), 0.3, generations=40, seed=2, patience=3, tol=1e-3)
    assert len(h) < len(h_full) and bf < 0.05
    bx2, bf2, h2 = cma.minimize(f, np.full(4, 0.9), 0.3, generations=40, seed=2, patience=3, tol=1e-3)
    assert np.array_equal(bx, bx2) and h == h2
    seen = []
    cma.minimize(f, np.full(4, 0.9), 0.3, generations=5, seed=2, on_gen=lambda g, n: seen.append((g, n)))
    assert seen == [(i, 5) for i in range(1, 6)]


# ---- coarse selection -------------------------------------------------------------------------------------------------
def test_select_coarse_keeps_both_members_of_the_best_blends():
    n = 20
    e1 = np.linspace(3.0, 6.0, n)                    # singles ranked by index
    eb = np.full((n, n), 5.0)
    np.fill_diagonal(eb, np.inf)
    eb[17, 18] = eb[18, 17] = 0.5                    # a great blend of two poor singles
    keep = select_coarse(e1, eb, 6)
    assert 17 in keep and 18 in keep and 0 in keep and len(keep) <= 7
    assert keep == sorted(keep)
    assert select_coarse(e1, eb, 100) == list(range(n))


# ---- core cache ---------------------------------------------------------------------------------------------------------
def test_core_cache_hits_are_bit_identical_and_can_be_disabled():
    pool = fixture_pool()
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    blocks = chain_blocks("a", (pool.pedals[0],), pool.amps[1], {})
    e1, e0 = Engine(None, 1), Engine(None, 1, core_cache_bytes=0)
    try:
        a = e1.core_blocks(blocks, pool.cabs[0], x)
        b = e1.core_blocks(blocks, pool.cabs[0], x)
        c = e0.core_blocks(blocks, pool.cabs[0], x)
        e1.core_blocks(blocks, pool.cabs[0], x * 0.5)               # another signal: a different entry
        assert (e1.core_hits, e1.core_misses) == (1, 2) and e0.core_hits == 0
        assert np.array_equal(a, b) and np.array_equal(a, c)
        assert e1.linear(pool.cabs[0], DEFAULT_V, "a", a).shape == a.shape         # read-only cores render fine
    finally:
        e1.close()
        e0.close()


# ---- progress file -------------------------------------------------------------------------------------------------------
def test_progress_json_schema_atomic_monotonic_and_heartbeat(tmp_path):
    path = tmp_path / "sub" / "progress.json"
    pr = Progress(path, "quick", interval=0.05).start()
    try:
        snaps = []
        for st in ("prepare", "prescreen", "screen", "refine", "finalize"):
            pr.stage(st, f"msg {st}")
            for k in range(1, 5):
                pr.update(k / 4)
                snaps.append(json.loads(path.read_text()))          # never torn: always valid JSON
        pr.best(2.5)
        pr.best(3.0)                                                # worse values are ignored
        t0 = path.stat().st_mtime_ns
        time.sleep(0.3)
        assert path.stat().st_mtime_ns > t0                          # heartbeat rewrites without any state change
    finally:
        pr.close("done", 1.3)
    last = json.loads(path.read_text())
    assert {"stage", "fraction", "etaSeconds", "bestErrorDb", "message"} <= set(last)
    assert last["fraction"] == 1.0 and last["etaSeconds"] == 0.0 and last["done"] and last["bestErrorDb"] == 1.3
    fr = [s["fraction"] for s in snaps]
    assert fr == sorted(fr) and fr[0] < 0.2 and fr[-1] > 0.95
    assert {s["stage"] for s in snaps} == {"prepare", "prescreen", "screen", "refine", "finalize"}
    assert not [p for p in path.parent.iterdir() if p.name != "progress.json"]          # no temp files left behind
    assert NullProgress().callback()(1, 2) is None


def test_progress_eta_extrapolates():
    t = [100.0]
    pr = Progress("/dev/null", "thorough", clock=lambda: t[0])
    pr.stage("screen")
    pr.update(0.5)
    t[0] += 60.0
    snap = pr.snapshot()
    assert 0.3 < snap["fraction"] < 0.9
    assert snap["etaSeconds"] == pytest.approx(60.0 * (1 - snap["fraction"]) / snap["fraction"], rel=0.02)


# ---- CLI flags -----------------------------------------------------------------------------------------------------------
def test_cli_flags():
    p = build_parser()
    base = ["--di", "d.wav", "--ref", "r.wav", "--pool", "p.json"]
    a = p.parse_args(base)
    assert not a.quick and not a.thorough and not a.listen and a.progress_json is None and a.threads == 4
    a = p.parse_args(base + ["--quick", "--jobs", "3", "--listen", "--progress-json", "x.json"])
    assert a.quick and a.threads == 3 and a.listen and a.progress_json == "x.json"
    with pytest.raises(SystemExit):
        p.parse_args(base + ["--quick", "--thorough"])
    assert p.parse_args(base + ["--thorough"]).thorough


def test_quick_plan_is_cheaper_and_thorough_is_unchanged():
    q, t = Plan.from_budget(1.0, 3, None, True), Plan.from_budget(1.0, 3)
    assert q.mode == "quick" and t.mode == "thorough"
    assert t.coarse_s == 0 and not t.blend_aware and t.patience is None and t.cap_pairs == 800 and t.top_k["blend"] == 3
    assert q.coarse_s > 0 and q.gain_s > 0 and q.capped_prescreen and not q.blend_aware and q.patience and q.cap_pairs < t.cap_pairs
    assert q.gens_gain * q.pop_gain < t.gens_gain * t.pop_gain and q.top_k["blend"] < t.top_k["blend"]


# ---- end to end -------------------------------------------------------------------------------------------------------------
def _quick_plan(**kw):
    d = dict(mode="quick", coarse_s=2.0, coarse_keep=0.5, coarse_min_keep=4, blend_aware=True, capped_prescreen=True, prescreen_n_ped=1,
             patience=2, patience_gain=1, plateau_tol=0.01, blend_extra_ped=1, blend_extra_amp=1, gain_s=2.5, cap_pairs=100, top_k={"blend": 1, "single": 1, "single2": 0},
             gens_linear=6, gens_gain=2, gens_final=3)
    d.update(kw)
    return mkplan(**d)


def test_quick_run_with_progress_and_coarse_pass(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    prog = tmp_path / "progress.json"
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=3, excerpt_s=4.0, threads=2, plan=_quick_plan(),
                 write_audio=False, refine_offsets=False, progress_json=prog, quick=True)
    res = run_match(cfg, Log())
    assert res["mode"] == "quick"
    st = res["stage1"]
    assert st["coarsePass"] is True and st["pairsCoarse"] > st["pairsRendered"] >= 2
    assert st["prescreen"]["blendAware"] and st["prescreen"]["nPedalsPerClass"] == 1
    assert res["coarseExcerpt"]["endS"] - res["coarseExcerpt"]["startS"] == pytest.approx(2.0)
    assert res["listening"] == {} and not (tmp_path / "out" / "listen").exists()
    tm = res["timings"]
    for k in ("excerpt", "stage1", "stage2", "fullRenders", "tonecheck", "stage1Detail", "coreCache"):
        assert k in tm, k
    assert tm["coreCache"]["hits"] > 0                  # the pre-screen renders are reused by the coarse pass
    assert res["before"] is None and "starter_L" not in res["tonecheck"]          # quick: no full-length "before" render
    assert res["starter"]["excerptLoss"]["total"] > res["best"]["loss"]
    assert res["after"][0]["aWeightedErrorDb"] < 1.5 and res["timings"]["cpuSeconds"] > 0
    assert res["gainExcerpt"]["endS"] > res["gainExcerpt"]["startS"]
    last = json.loads(prog.read_text())
    assert last["done"] and last["fraction"] == 1.0 and last["stage"] == "finalize"
    assert last["bestErrorDb"] == pytest.approx(res["after"][0]["aWeightedErrorDb"], abs=1e-3)


def test_quick_matches_thorough_on_the_known_answer(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    kw = dict(di=di, ref=ref, pool=pool, seed=7, excerpt_s=4.0, threads=2, write_audio=False, refine_offsets=False)
    q = run_match(Config(out=tmp_path / "q", plan=_quick_plan(gens_linear=30, gens_gain=5, gens_final=12, pop_linear=12,
                                                      top_k={"blend": 1, "single": 0, "single2": 0}), **kw), Log())
    t = run_match(Config(out=tmp_path / "t", plan=mkplan(top_k={"blend": 1, "single": 0, "single2": 0}, gens_linear=40,
                                                         gens_gain=8, gens_final=25, pop_linear=16, pop_gain=8), **kw), Log())
    # quick 0.684 dB vs thorough 0.255 dB A-weighted at seed 7 once 10.1's levels entered the loss (other seeds 0.32-0.69 dB);
    # the accepted criterion is 6b's report: within 1 dB A-weighted of --thorough (docs/specs/phase6b_matcher_speed_REPORT.md).
    assert q["after"][0]["aWeightedErrorDb"] < t["after"][0]["aWeightedErrorDb"] + 1.0
    assert q["after"][0]["aWeightedErrorDb"] < 1.0
    assert q["best"]["captures"] == t["best"]["captures"]


def test_progress_file_reports_errors(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    prog = tmp_path / "progress.json"
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2,
                 plan=mkplan(top_k={"blend": 0, "single": 0, "single2": 0}), write_audio=False, refine_offsets=False,
                 progress_json=prog)
    with pytest.raises(ValueError):
        run_match(cfg, Log())
    assert json.loads(prog.read_text())["message"].startswith("error:")


def test_best_r_is_rendered_without_listen_and_feeds_the_clip_guard(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "single")
    x, fs = sf.read(di, dtype="float32")
    di_r = tmp_path / "di_r.wav"
    sf.write(str(di_r), x * 20.0, fs, subtype="FLOAT")          # R much hotter than L
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2, di_r=di_r,
                 plan=mkplan(top_k={"blend": 0, "single": 1, "single2": 0}), write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    pk = res["best"]["fullLengthPeakDbfs"]
    assert set(pk) == {"best_L", "best_R"} and pk["best_R"] > pk["best_L"]
    after = res["best"]["fullLengthPeakAfterGuardDbfs"]
    assert max(after.values()) <= -1.0 + 1e-3                     # guard acts on max(L, R)
    assert res["best"]["clipGuardDb"] < 0 and res["listening"] == {}


def test_quick_run_is_deterministic_end_to_end(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    outs = []
    for tag in "ab":
        cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / tag, seed=11, excerpt_s=4.0, threads=2, plan=_quick_plan(),
                     write_audio=False, refine_offsets=False, quick=True)
        outs.append(run_match(cfg, Log()))
    assert outs[0]["best"]["loss"] == outs[1]["best"]["loss"]
    assert (tmp_path / "a" / "best.preset.resolved.json").read_text() == (tmp_path / "b" / "best.preset.resolved.json").read_text()
