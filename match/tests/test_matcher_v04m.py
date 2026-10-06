"""v0.4M Task B (+ B2.2, B2.4): tight boost, post-cab filters, gate matched to the reference, cab breadth, ablations and
tone tracing of the matcher. Render-based tests need the sawblade_core extension and the fixtures of tests/fixtures."""
from __future__ import annotations

import dataclasses
import json
import tempfile
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.matcher.pool import Pool, load_pool
from sawblade_match.matcher.space import (BUTTER4_Q, Combo, POST_HP_RANGE, POST_LP2_RANGE, Space, boost_block,
                                          build_preset, gate_preset, manual_align, post_eq, post_filters_from_eq)

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher.cli import build_parser, parse_tone_ids      # noqa: E402
from sawblade_match.matcher.engine import Engine, to48                   # noqa: E402
from sawblade_match.matcher.gatesweep import (GATE_OFFSETS_DB, cell_gate, gate_sweep,    # noqa: E402
                                               reference_floor_db)
from sawblade_match.matcher.reference import build_target, load_reference, make_excerpt   # noqa: E402
from sawblade_match.matcher.run import (ABLATIONS, Config, Log, OCCAM_DB, choose, gate_envelope_floor_db,
                                        parse_ablate, run_match)          # noqa: E402
from sawblade_match.matcher.screen import Scored                          # noqa: E402

from test_matcher import FIX, _fake_cache, cap, fixture_pool, hidden, mkplan   # noqa: E402

FS = 48000


def _loadwav(p):
    x, fs = sf.read(str(p), dtype="float32")
    return x if x.ndim == 1 else x[:, 0], fs


def _write_di(tmp: Path, x: np.ndarray, name="di.wav") -> Path:
    p = tmp / name
    sf.write(str(p), x, FS, subtype="FLOAT")
    return p


def _known(tmp: Path, pool: Pool, combo: Combo, v: dict, gate=None, di=None):
    """Hidden preset -> matched reference of the DI (offset 0). Returns (di path, reference)."""
    x, fs = _loadwav(FIX / "di_riff.wav") if di is None else (di, FS)
    dip = _write_di(tmp, x)
    gate = gate or gate_preset(gate_envelope_floor_db(to48(x, fs), FS))
    y, _ = core.render(build_preset(combo, v, gate=gate, align=manual_align()), x, float(fs))
    refwav = tmp / "hidden.wav"
    sf.write(str(refwav), y, fs, subtype="FLOAT")
    return dip, load_reference(refwav, channel="mid", matched="mono", offset_ms=0.0)


def distinct_tone_pool() -> Pool:
    """The fixture pool with one tone id per amp (tone 2 holds all three amps there)."""
    p = fixture_pool()
    amps = [dataclasses.replace(a, tone_id=21 + i) for i, a in enumerate(p.amps)]
    return Pool(list(p.pedals), amps, list(p.cabs))


# ---- post-cab filters (no core) ------------------------------------------------------------------------------------------
def test_post_filters_are_neutral_by_default_and_not_in_the_regulariser():
    for shape in ((1, None), (1, 1), (0, 0)):
        sp = Space(shape)
        v = sp.default()
        assert v["post.hp"] == POST_HP_RANGE[0] and v["post.lp2"] == POST_LP2_RANGE[1]
        assert v["post.hp_slope"] < 0.5 and v["post.lp2_slope"] < 0.5          # 12 dB/oct
        assert [b["type"] for b in post_eq(v)] == ["peak"] * 3                 # neutral filters are omitted
        assert len(sp.eq_gains(v)) == len(Space(shape, filters=False).eq_gains(v))
        for n in ("post.hp", "post.hp_slope", "post.lp2", "post.lp2_slope"):
            assert not sp.params[sp.idx[n]].eq_gain
        # the slopes are discrete: not CMA-ES dimensions in either group
        assert set(sp.indices("linear")) | set(sp.indices("gain")) == set(range(len(sp))) - set(sp.indices("discrete"))
        assert {sp.names[i] for i in sp.indices("discrete")} == {"post.hp_slope", "post.lp2_slope"}
    assert "post.hp" not in Space((1, None), filters=False).idx
    assert "post.hp" not in post_eq(Space((1, None), filters=False).default())


def test_post_filter_emission_slopes_and_inverse():
    v = Space((1, None)).default()
    v.update({"post.hp": 100.0, "post.hp_slope": 0.2, "post.lp2": 8000.0, "post.lp2_slope": 0.9})
    bands = post_eq(v)
    hp = [b for b in bands if b["type"] == "highPass"]
    lp = [b for b in bands if b["type"] == "lowPass"]
    assert len(hp) == 1 and hp[0]["q"] == pytest.approx(0.707) and hp[0]["freq"] == 100.0           # 12 dB/oct: one biquad
    assert [b["q"] for b in lp] == [pytest.approx(0.541, abs=1e-3), pytest.approx(1.307, abs=1e-3)]    # 24 dB/oct: two
    assert [b["q"] for b in lp] == list(BUTTER4_Q) and {b["freq"] for b in lp} == {8000.0}
    back = post_filters_from_eq(json.loads(json.dumps(bands)))
    assert back == {"hp": (100.0, 12), "lowpass": [(8000.0, 24)]}
    v.update({"post.hp_slope": 1.0, "post.lp2_slope": 0.0})
    assert post_filters_from_eq(post_eq(v)) == {"hp": (100.0, 24), "lowpass": [(8000.0, 12)]}
    # both lows together (post.lp roll-off at 12 dB + a 24 dB lp2) are told apart by the Butterworth pair
    v.update({"post.lp": 7000.0, "post.lp2_slope": 1.0})
    assert post_filters_from_eq(post_eq(v))["lowpass"] == [(7000.0, 12), (8000.0, 24)]
    # at the range edge a 24 dB/oct filter is real (only the 12 dB edge is "off")
    v2 = Space((1, None)).default()
    v2["post.hp_slope"] = 1.0
    assert post_filters_from_eq(post_eq(v2))["hp"] == (60.0, 24)


def test_choose_boost_needs_to_beat_the_plain_candidate_by_the_occam_margin():
    pool = fixture_pool()
    plain = Combo((pool.pedals[0],), pool.amps[0], None, None, pool.cabs[0])
    boosted = dataclasses.replace(plain, boost=True)
    assert boosted.topology == "single" and boosted.key() != plain.key() and boosted.pair_key() != plain.pair_key()
    assert "boost=pedal.ts" in boosted.describe() and boosted.with_cab(pool.cabs[1]).boost
    mk = lambda c, l: Scored(c, l, 0.0, manual_align(), None, "refined", {})
    assert choose([mk(plain, 1.0), mk(boosted, 1.0 - OCCAM_DB * 0.9)]).combo is plain       # within 0.1 dB: no boost
    assert choose([mk(plain, 1.0), mk(boosted, 1.0 - OCCAM_DB * 1.5)]).combo is boosted     # clearly better: boost
    assert choose([mk(boosted, 1.0)]).combo is boosted                                      # nothing to compare with


def test_cli_flags_and_ablate_parsing():
    a = build_parser().parse_args(["--di", "d", "--ref", "r", "--pool", "p", "--ablate", "feel, boost,irblend",
                                   "--trace-tones", "57492,79751,57492"])
    assert parse_ablate(a.ablate) == ("feel", "boost", "irblend") and parse_tone_ids(a.trace_tones) == (57492, 79751)
    assert parse_ablate("") == () and parse_ablate(["studio"]) == ("studio",)
    assert set(ABLATIONS) == {"feel", "boost", "filters", "irsweep", "irblend", "studio"}
    with pytest.raises(ValueError, match="unknown suspect"):
        parse_ablate("feel,bogus")
    with pytest.raises(ValueError, match="tone id"):
        parse_tone_ids("12,abc")
    helptext = " ".join(build_parser().format_help().split())
    assert "no-ops until" in helptext and "--trace-tones" in helptext


def test_pool_catalog_records_what_the_manifest_says(tmp_path):
    p = load_pool(_fake_cache(tmp_path))
    assert set(p.catalog) == {1, 2, 3, 4, 5}
    models = {m["modelId"]: m["downloaded"] for m in p.catalog[1]["models"]}
    assert models == {10: True, 11: True, 1010: False, 1011: False}       # the manifest lists models that were never pulled
    assert p.catalog[1]["title"] == "Amp A" and p.catalog[1]["reason"] is None


# ---- the modeled boost and the filters through the renderer ------------------------------------------------------------------
def test_boost_block_is_schema_valid_and_emulation_equals_full_render():
    pool = fixture_pool()
    combo = Combo((pool.pedals[2],), pool.amps[2], None, None, pool.cabs[1], boost=True)
    sp = Space.for_combo(combo)
    assert [n for n in sp.names if n.startswith("boost.")] == ["boost.drive", "boost.level", "boost.tone"]
    assert all(sp.params[sp.idx[n]].group == "gain" for n in sp.names if n.startswith("boost."))
    assert Space((1, None)).default().get("boost.drive") is None                      # only boost combos have the params
    with pytest.raises(ValueError):
        Space((1, 1), boost=True)
    v = sp.default()
    v.update({"boost.drive": 2.0, "boost.level": 9.0, "boost.tone": 4.0, "post.hp": 110.0, "post.hp_slope": 1.0,
              "post.lp2": 7500.0, "post.lp2_slope": 1.0, "post.g1": 2.0})
    gate = gate_preset(-60.0)
    preset = build_preset(combo, v, gate=gate, align=manual_align())
    blocks = preset["paths"]["a"]["blocks"]
    assert [b["type"] for b in blocks] == ["nam", "pedal.ts", "nam"] and blocks[1]["slot"] == "boost"      # before the amp
    assert blocks[1] == boost_block("a2", v) and blocks[1]["params"] == {"drive": 2.0, "level": 9.0, "tone": 4.0}
    assert len({b["id"] for b in blocks}) == 3
    x, fs = _loadwav(FIX / "di_riff.wav")
    x = to48(x, fs)[:FS * 2]
    eng = Engine(gate, 2)
    try:
        ca = eng.core(combo, v, "a", x)                      # the pedal's 50 samples of latency are absorbed by the renderer
        em = eng.emulate(combo, v, ca, None, manual_align())
        full, rep = eng.render(preset, x)
        assert rep["latencySamples"] == 50 and rep["liveCompatible"] is True
        assert np.max(np.abs(full - em)) < 1e-5 * max(1.0, np.max(np.abs(full)))
        # a different gate is a different core (the gate is part of the memo key) and really changes the output
        g2 = cell_gate(-60.0, 20.0, 80.0)
        assert not np.array_equal(eng.core(combo, v, "a", x, gate=g2), ca)
        assert np.array_equal(eng.core(combo, v, "a", x, gate=gate), ca)
    finally:
        eng.close()
    # JSON round trip of the whole preset incl. the pedal block and the 24 dB/oct filters: strict re-parse + same render
    again = json.loads(json.dumps(preset))
    assert again == preset
    y2, _ = core.render(again, x, float(FS))
    assert np.array_equal(y2, full)
    assert post_filters_from_eq(again["postEq"]) == {"hp": (110.0, 24), "lowpass": [(7500.0, 24)]}


def test_post_filter_24db_response_is_a_fourth_order_butterworth():
    impulse = Pool([], [], [cap(FIX / "ir" / "impulse.wav", 4, 9, "cab", "impulse")])
    eng = Engine(None, 1)
    x = np.zeros(8192, np.float32)
    x[0] = 1.0
    f = np.fft.rfftfreq(len(x), 1.0 / FS)
    sel = (f >= 40.0) & (f <= 14000.0)
    try:
        sp = Space((0, None))
        h0 = np.fft.rfft(eng.linear(impulse.cabs[0], sp.default(), "a", x).astype(np.float64))      # filters neutral
        for kind, f0 in (("hp", 120.0), ("lp2", 7000.0)):
            for slope, order in ((0.0, 2), (1.0, 4)):
                v = sp.default()
                v.update({f"post.{kind}": f0, f"post.{kind}_slope": slope})
                h1 = np.fft.rfft(eng.linear(impulse.cabs[0], v, "a", x).astype(np.float64))
                sos = signal.butter(order, f0, btype="highpass" if kind == "hp" else "lowpass", fs=FS, output="sos")
                _, ref = signal.sosfreqz(sos, worN=f, fs=FS)
                d = 20 * np.log10(np.abs(h1[sel] / h0[sel])) - 20 * np.log10(np.abs(ref[sel]))
                assert np.max(np.abs(d)) < 0.1, (kind, slope, float(np.max(np.abs(d))))
                if slope >= 0.5:        # and it really is steeper than the 12 dB/oct filter, one octave out
                    i = int(np.argmin(np.abs(f - (f0 / 2 if kind == "hp" else f0 * 2))))
                    assert 20 * np.log10(np.abs(h1[i] / h0[i])) < -20.0
    finally:
        eng.close()


# ---- gate matched to the reference --------------------------------------------------------------------------------------------
def _gap_di(seconds=8, seed=3) -> np.ndarray:
    """Palm-muted chugs and rests with hum + hiss (-60 dBFS) and one ring-out: real silence gaps of a DI."""
    rng = np.random.default_rng(seed)
    n = seconds * FS
    t = np.arange(n) / FS
    x = np.zeros(n)
    i, bar = 0, 0
    while i < n - FS:
        if bar % 6 < 3:
            for k in range(4):
                s, m = i + k * int(0.15 * FS), int(0.14 * FS)
                tt = np.arange(m) / FS
                note = sum(np.sin(2 * np.pi * 82.4 * h * tt + rng.uniform(0, 6)) / h for h in range(1, 12)) * np.exp(-tt / 0.04)
                x[s:s + m] += 0.15 * note
            i += int(0.8 * FS)
        else:
            if bar % 6 == 3:
                m = int(1.5 * FS)
                x[i:i + m] += 0.03 * np.sin(2 * np.pi * 82.4 * np.arange(m) / FS) * np.exp(-np.arange(m) / FS / 0.35)
            i += int(1.5 * FS)
        bar += 1
    x += 10 ** (-62.0 / 20) * np.sqrt(2) * (np.sin(2 * np.pi * 60 * t) + 0.5 * np.sin(2 * np.pi * 120 * t))
    x += rng.standard_normal(n) * 10 ** (-68.0 / 20)
    return x.astype(np.float32)


def _gate_case(tmp: Path, ref_gate_fn):
    """Single boost chain on a gappy DI; the reference is the same chain with ``ref_gate_fn(floor)`` as its gate."""
    pool = fixture_pool()
    combo = Combo((), pool.amps[2], None, None, pool.cabs[0], boost=True)
    sp = Space.for_combo(combo)
    v = sp.default()
    di = _gap_di()
    floor = gate_envelope_floor_db(di, FS)
    dip, ref = _known(tmp, pool, combo, v, gate=ref_gate_fn(floor), di=di)
    ex = make_excerpt(di, 8.0, window=(0, len(di)))
    tgt = build_target(ref, ex)
    assert tgt.feel is not None and tgt.feel.plan.gap_ok
    eng = Engine(gate_preset(floor), 2)
    cand = Scored(combo, 0.0, 0.0, manual_align(), None, "refined", {"params": v})
    try:
        return gate_sweep(eng, cand, sp, ex, tgt, floor), floor
    finally:
        eng.close()


def test_gate_sweep_picks_a_higher_threshold_when_the_reference_gaps_are_cleaner(tmp_path):
    gs, floor = _gate_case(tmp_path, lambda f: cell_gate(f, 20.0, 80.0))          # a reference gated hard and fast
    assert gs["skipped"] is None and gs["mode"] == "paired" and len(gs["grid"]) == 15
    assert gs["changed"] and gs["picked"]["offsetDb"] > 4.0
    assert gs["picked"]["thresholdDb"] > gs["baseline"]["thresholdDb"] and gs["gate"]["thresholdDb"] == gs["picked"]["thresholdDb"]
    assert gs["picked"]["floorTerm"] < gs["baseline"]["floorTerm"]
    assert gs["picked"]["feasible"] and gs["picked"]["ltas"] <= gs["baseline"]["ltas"] + gs["ltasToleranceDb"]
    assert (gs["baseline"]["offsetDb"], gs["baseline"]["releaseMs"]) == (4.0, 150.0)
    assert gs["baseline"]["thresholdDb"] == pytest.approx(gate_preset(floor)["thresholdDb"])
    # the gate gets cleaner with the threshold: the output's floor falls (more negative) from +4 to +20 dB at 150 ms
    by = {(r["offsetDb"], r["releaseMs"]): r for r in gs["grid"]}
    assert by[(20.0, 150.0)]["floorDbOut"] < by[(4.0, 150.0)]["floorDbOut"] - 3.0
    assert sorted({r["offsetDb"] for r in gs["grid"]}) == list(GATE_OFFSETS_DB)


def test_gate_sweep_keeps_the_default_when_the_reference_has_the_default_gate(tmp_path):
    gs, _ = _gate_case(tmp_path, lambda f: cell_gate(f, 4.0, 150.0))
    assert gs["skipped"] is None and not gs["changed"]
    assert gs["picked"] is gs["baseline"] and gs["baseline"]["floorTerm"] == pytest.approx(0.0, abs=1e-6)
    assert gs["gate"] == gate_preset(gs["diNoiseFloorDb"])             # untouched


def test_reference_floor_is_measured_in_the_references_own_gaps():
    f = reference_floor_db(_gap_di().astype(np.float64))             # soft target: no matched pair, the reference's own gaps
    assert f is not None and -70.0 < f < -20.0
    mix = np.random.default_rng(0).standard_normal(FS * 4) * 0.05    # a dense mix has no real silence to match
    assert reference_floor_db(mix) is None


def test_gate_sweep_is_skipped_without_gaps(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")                              # di_riff.wav: its gaps are too short in a 1 s window
    x, fs = _loadwav(FIX / "di_riff.wav")
    dip, ref = _known(tmp_path, pool, combo, v)
    ex = make_excerpt(to48(x, fs), 1.0, window=(0, FS // 2))
    tgt = build_target(ref, ex)
    eng = Engine(gate_preset(-60.0), 1)
    try:
        gs = gate_sweep(eng, Scored(combo, 0.0, 0.0, manual_align(), None, "refined", {"params": v}), sp, ex, tgt, -60.0)
    finally:
        eng.close()
    assert gs["skipped"] and not gs["changed"] and gs["gate"] == gate_preset(-60.0, {"thresholdDb": -56.0, "releaseMs": 150.0})


# ---- the search: boost, filters, cab sweep, ablation, trace ------------------------------------------------------------------
def test_tight_boost_variant_can_win_on_a_hidden_boosted_chain(tmp_path):
    full = fixture_pool()
    pool = Pool([], list(full.amps), list(full.cabs))                  # no pedal capture can make the clipping: only the boost
    hidden_combo = Combo((), full.amps[2], None, None, full.cabs[1], boost=True)
    v = Space.for_combo(hidden_combo).default()
    v.update({"boost.drive": 2.5, "boost.level": 9.0, "boost.tone": 4.0, "post.g1": 1.0})
    di, ref = _known(tmp_path, pool, hidden_combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=16, gens_gain=4, gens_final=10,
                  pop_linear=12, pop_gain=6, n_rescore_single=12, n_cab_single=12)
    res = run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=5, excerpt_s=2.0, threads=2, plan=plan,
                           write_audio=False, refine_offsets=False), Log())
    tb = res["tightBoost"]
    assert tb["tried"] == 3 and tb["won"] is True and tb["ablated"] is False and tb["refined"] >= 1
    assert set(tb["params"]) == {"drive", "level", "tone"} and 0.0 <= tb["params"]["drive"] <= 3.0
    assert 6.0 <= tb["params"]["level"] <= 10.0 and 3.0 <= tb["params"]["tone"] <= 8.0
    assert tb["bestBoostLoss"] + OCCAM_DB < tb["bestPlainSingleLoss"]
    assert res["best"]["tightBoost"] is True and res["best"]["topology"] == "single"
    assert any(c["tightBoost"] for c in res["stage1"]["top"]["single"])           # the variants compete in stage 1 ...
    assert res["after"][0]["aWeightedErrorDb"] < 1.0 < res["before"][0]["aWeightedErrorDb"]      # ... and come close to it
    best = json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())
    assert [b["type"] for b in best["paths"]["a"]["blocks"]] == ["pedal.ts", "nam"]
    assert best["paths"]["a"]["blocks"][0]["slot"] == "boost" and best["paths"]["b"]["enabled"] is False
    core.render(best, np.zeros(2048, np.float32), 48000.0)                         # strict parse


def test_boost_is_tried_but_loses_the_occam_rule_on_a_plain_chain(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")                               # [pedal] -> amp -> cab, no boost
    di, ref = _known(tmp_path, pool, combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=8, gens_gain=3, gens_final=6,
                  pop_linear=8, pop_gain=4, n_rescore_single=12, n_cab_single=12)
    res = run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=5, excerpt_s=2.0, threads=2, plan=plan,
                           write_audio=False, refine_offsets=False), Log())
    tb = res["tightBoost"]
    assert tb["tried"] == 12 and tb["refined"] >= 1                     # boost variants of the 12 re-scored combos
    assert tb["won"] is False and tb["params"] is None and res["best"]["tightBoost"] is False
    assert tb["bestPlainSingleLoss"] <= tb["bestBoostLoss"] + OCCAM_DB      # the plain chain was refined next to the boost


_SHARED: dict = {}


def _shared_run():
    """One small run (single + blend, distinct tone ids, tracing) shared by the cab-sweep, trace and flags-default tests."""
    if "res" not in _SHARED:
        tmp = Path(tempfile.mkdtemp())
        pool = distinct_tone_pool()
        combo = Combo((pool.pedals[0],), pool.amps[1], None, None, pool.cabs[1])
        v = Space.for_combo(combo).default()
        v.update({"post.g1": 1.5})
        di, ref = _known(tmp, pool, combo, v)
        plan = mkplan(top_k={"blend": 1, "single": 1, "single2": 0}, gens_linear=6, gens_gain=2, gens_final=4)
        cfg = Config(di=di, ref=ref, pool=pool, out=tmp / "out", seed=3, excerpt_s=2.0, threads=2, plan=plan,
                     write_audio=False, refine_offsets=False, trace_tones=(21, 22, 23, 4, 1, 99))
        _SHARED["res"], _SHARED["pool"], _SHARED["combo"], _SHARED["out"] = run_match(cfg, Log()), pool, combo, tmp / "out"
    return _SHARED["res"], _SHARED["pool"]


def test_cab_sweep_covers_every_pool_cab_and_lists_every_ir_loss():
    res, pool = _shared_run()
    cs = res["cabSweep"]
    assert cs["ablated"] is False and cs["poolCabs"] == 2 and cs["topPerTopology"] == 3
    assert {c["topology"] for c in cs["candidates"]} == {"single", "blend"}
    keys = {c.key for c in pool.cabs}
    for c in cs["candidates"]:
        assert {i["cab"] for i in c["irs"]} == keys and c["nCabs"] == len(keys) == len(c["irs"])     # every IR, once
        assert all(np.isfinite(i["loss"]) for i in c["irs"])
        losses = [i["loss"] for i in c["irs"]]
        assert losses == sorted(losses) and c["best"]["loss"] == losses[0] and c["worst"]["loss"] == losses[-1]
        assert sum(1 for i in c["irs"] if i["current"]) == 1 and c["currentLoss"] is not None
        assert c["changed"] == (c["best"]["cab"] != c["currentCab"] and c["best"]["loss"] < c["currentLoss"] - 1e-9)
        assert {"feel", "tight", "fizz", "polish", "ltas"} <= set(c["irs"][0])
        if c["changed"]:
            assert c["newCab"] == c["best"]["cab"] and c["lossAfterRelinear"] <= c["lossBeforeRelinear"] + 1e-9
    # whatever the winner's cab is, no refined candidate ends with a cab that is worse than another cab of the sweep
    for cand in res["candidatesStage2"]:
        sw = next((c for c in cs["candidates"] if c["topology"] == cand["topology"] and c["boost"] == cand["tightBoost"]), None)
        if sw and cand["loss"] is not None:
            assert cand["loss"] <= sw["best"]["loss"] + 1e-6 or sw["changed"]
    assert res["ablate"] == [] and res["plan"]["boost"] is True and res["plan"]["filters"] is True
    assert "post.hp" in res["best"]["params"] and res["postFilters"]["searched"] is True
    assert res["gateFinal"]["thresholdDb"] >= res["gateDefault"]["thresholdDb"] - 1e-9 and "gateSweep" in res


def test_trace_tones_reports_a_fixture_tone():
    res, pool = _shared_run()
    tr = res["trace"]
    assert set(tr) == {"21", "22", "23", "4", "1", "99"}
    win = {c["toneId"] for k, c in res["best"]["captures"].items() if k.endswith("amp") and c}
    others = [k for k in ("21", "22", "23") if int(k) not in win]
    assert others and all(tr[k]["candidate"]["how"] == "this tone is (part of) the winner" for k in ("21", "22", "23")
                          if k not in others)
    amp = tr[others[0]]                                              # an amp that did not win
    tid = int(others[0])
    caps = [c for c in pool.amps if c.tone_id == tid]
    assert amp["downloaded"] is True and amp["gear"] == "amp" and amp["gearClass"] == [caps[0].kind]
    assert [m["modelId"] for m in amp["models"]] == [c.model_id for c in caps] and amp["models"][0]["downloaded"] is True
    assert amp["prescreen"]["applied"] is False                       # 12 pairs fit the cap: no pre-screen ran
    assert amp["pair"]["rendered"] is True and amp["pair"]["stage"] == "full" and 1 <= amp["pair"]["rank"] <= amp["pair"]["of"]
    assert amp["pair"]["of"] == 12 and np.isfinite(amp["pair"]["ltasErrDb"]) and amp["pair"]["amp"] == caps[0].key
    assert isinstance(amp["stage1"], list)
    assert np.isfinite(amp["candidate"]["loss"]) and amp["candidate"]["breakdown"]["total"] == pytest.approx(amp["candidate"]["loss"])
    vw = amp["vsWinner"]
    assert vw["winnerLoss"] == pytest.approx(res["best"]["loss"]) and vw["deltaTotal"] == pytest.approx(amp["candidate"]["loss"] - res["best"]["loss"])
    assert sum(vw["weighted"].values()) == pytest.approx(vw["loss"], abs=1e-6)       # the split adds up to the loss
    assert isinstance(vw["why"], str) and "winner" in vw["why"] and amp["lost"] == (amp["candidate"]["loss"] > res["best"]["loss"])
    cab = tr["4"]                                                      # a cab tone: its IRs from the winner's sweep
    assert cab["gear"] == "cab" and sorted(c["cab"] for c in cab["cabSweep"]) == ["4/7", "4/8"] and cab["winnerCab"] in ("4/7", "4/8")
    assert tr["1"]["gear"] == "pedal" and "pair" in tr["1"] and "candidate" not in tr["1"]
    assert tr["99"]["downloaded"] is False and "cannot be a candidate" in tr["99"]["note"] and tr["99"]["inManifest"] is None


def test_ablate_switches_the_suspects_off_and_echoes_them(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")
    di, ref = _known(tmp_path, pool, combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=4, gens_gain=2, gens_final=3,
                  n_rescore_single=6, n_cab_single=2)
    names = ("feel", "boost", "filters", "irsweep", "irblend", "studio")
    res = run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=2, excerpt_s=2.0, threads=2, plan=plan,
                           write_audio=False, refine_offsets=False, ablate=names), Log())
    assert res["ablate"] == list(names) and "no-ops" in res["ablateNote"]
    assert (res["plan"]["boost"], res["plan"]["filters"], res["plan"]["cab_sweep"]) == (False, False, False)
    tb = res["tightBoost"]
    assert tb["ablated"] is True and tb["tried"] == 0 and tb["won"] is False and tb["refined"] == 0
    assert not any(c["tightBoost"] for c in res["stage1"]["top"]["single"]) and not any(c["tightBoost"] for c in res["candidatesStage2"])
    assert res["cabSweep"]["ablated"] is True and "candidates" not in res["cabSweep"]
    assert res["postFilters"]["searched"] is False and "post.hp" not in res["best"]["params"] and "post.lp2" not in res["best"]["params"]
    assert [b["type"] for b in json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())["postEq"]] \
        .count("highPass") == 0
    bd = res["best"]["breakdown"]
    assert bd["feel"] == 0.0 and bd["feelTerms"] is None                    # no feel term in the search's loss ...
    assert res["referenceTarget"]["feel"] is not None                       # ... but it is still measured for the report
    assert "gateSweep" in res                                               # the gate sweep is not an ablation switch
    with pytest.raises(ValueError, match="unknown suspect"):
        run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out2", ablate=("nope",)), Log())
