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

from sawblade_match.matcher import loss as L
from sawblade_match.matcher.pool import Pool, load_pool
from sawblade_match.matcher.space import (BUTTER4_Q, Combo, POST_HP_RANGE, POST_LP_RANGE, Space, boost_block,
                                          build_preset, gate_preset, manual_align, post_eq, post_filters_from_eq)

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher.cli import build_parser, parse_tone_ids      # noqa: E402
from sawblade_match.matcher.engine import Engine, to48                   # noqa: E402
from sawblade_match.matcher import preeq as PE                       # noqa: E402
from sawblade_match.matcher import studio as ST                       # noqa: E402
from sawblade_match.matcher.irblend import ir_alignment, load_ir48        # noqa: E402
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


def _known(tmp: Path, pool: Pool, combo: Combo, v: dict, gate=None, di=None, bus_comp=None):
    """Hidden preset -> matched reference of the DI (offset 0). Returns (di path, reference)."""
    x, fs = _loadwav(FIX / "di_riff.wav") if di is None else (di, FS)
    dip = _write_di(tmp, x)
    gate = gate or gate_preset(gate_envelope_floor_db(to48(x, fs), FS))
    y, _ = core.render(build_preset(combo, v, gate=gate, align=manual_align(), bus_comp=bus_comp), x, float(fs))
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
        assert v["post.hp"] == POST_HP_RANGE[0] and v["post.lp"] == POST_LP_RANGE[1]
        assert v["post.hp_slope"] < 0.5 and v["post.lp_slope"] < 0.5          # 12 dB/oct
        assert [b["type"] for b in post_eq(v)] == ["peak"] * 3                 # neutral filters are omitted
        assert len(sp.eq_gains(v)) == len(Space(shape, filters=False).eq_gains(v))
        for n in ("post.hp", "post.hp_slope", "post.lp", "post.lp_slope"):
            assert not sp.params[sp.idx[n]].eq_gain
        # post.hp and the slopes are discrete: not CMA-ES dimensions in either group
        assert set(sp.indices("linear")) | set(sp.indices("gain")) == set(range(len(sp))) - set(sp.indices("discrete"))
        assert {sp.names[i] for i in sp.indices("discrete") if not sp.names[i].startswith("pre.")} \
            == {"post.hp", "post.hp_slope", "post.lp_slope"}
    assert "post.hp" not in Space((1, None), filters=False).idx
    assert "post.hp" not in post_eq(Space((1, None), filters=False).default())


def test_post_filter_emission_slopes_and_inverse():
    v = Space((1, None)).default()
    v.update({"post.hp": 100.0, "post.hp_slope": 0.2, "post.lp": 8000.0, "post.lp_slope": 0.9})
    bands = post_eq(v)
    hp = [b for b in bands if b["type"] == "highPass"]
    lp = [b for b in bands if b["type"] == "lowPass"]
    assert len(hp) == 1 and hp[0]["q"] == pytest.approx(0.707) and hp[0]["freq"] == 100.0           # 12 dB/oct: one biquad
    assert [b["q"] for b in lp] == [pytest.approx(0.541, abs=1e-3), pytest.approx(1.307, abs=1e-3)]    # 24 dB/oct: two
    assert [b["q"] for b in lp] == list(BUTTER4_Q) and {b["freq"] for b in lp} == {8000.0}
    back = post_filters_from_eq(json.loads(json.dumps(bands)))
    assert back == {"hp": (100.0, 12), "lowpass": [(8000.0, 24)]}
    v.update({"post.hp_slope": 1.0, "post.lp_slope": 0.0})
    assert post_filters_from_eq(post_eq(v)) == {"hp": (100.0, 24), "lowpass": [(8000.0, 12)]}
    assert "post.lp2" not in Space((1, None)).idx                           # one post low-pass: post.lp with a slope
    # a filter at its range edge is off, whatever its slope; just off the edge it is on
    v2 = Space((1, None)).default()
    v2.update({"post.hp_slope": 1.0, "post.lp_slope": 1.0})
    assert post_filters_from_eq(post_eq(v2)) == {"hp": None, "lowpass": []}
    v2.update({"post.hp": 62.0, "post.lp": 10900.0})
    assert post_filters_from_eq(post_eq(v2)) == {"hp": (62.0, 24), "lowpass": [(10900.0, 24)]}


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
    assert set(ABLATIONS) == {"feel", "boost", "filters", "irsweep", "irblend", "studio", "preeq"}
    with pytest.raises(ValueError, match="unknown suspect"):
        parse_ablate("feel,bogus")
    with pytest.raises(ValueError, match="tone id"):
        parse_tone_ids("12,abc")
    helptext = " ".join(build_parser().format_help().split())
    assert "irblend = no two-IR blend" in helptext and "--trace-tones" in helptext


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
    assert [n for n in sp.names if n.startswith("boost.")] == ["boost.drive", "boost.tone"]      # boost.level is fixed at 8 (redundant with the amp gain)
    assert all(sp.params[sp.idx[n]].group == "gain" for n in sp.names if n.startswith("boost."))
    assert Space((1, None)).default().get("boost.drive") is None                      # only boost combos have the params
    with pytest.raises(ValueError):
        Space((1, 1), boost=True)
    v = sp.default()
    v.update({"boost.drive": 2.0, "boost.level": 9.0, "boost.tone": 4.0, "post.hp": 110.0, "post.hp_slope": 1.0,
              "post.lp": 7500.0, "post.lp_slope": 1.0, "post.g1": 2.0})
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
        for kind, f0 in (("hp", 120.0), ("lp", 7000.0)):
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
    v.update({"boost.drive": 3.0, "boost.level": 10.0, "gain.a.amp": 12.0})    # gain after the gate: its residue shows
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
    gs, floor = _gate_case(tmp_path, lambda f: cell_gate(f, 20.0, 20.0, 2.0, -90.0))      # a reference gated hard and fast
    assert gs["skipped"] is None and gs["mode"] == "paired" and gs["renders"] == len(gs["grid"]) <= 20
    assert [st["axis"] for st in gs["steps"]] == ["thresholdOffsetDb", "holdMs", "releaseMs", "rangeDb"]
    assert gs["changed"] and gs["picked"]["offsetDb"] > 10.0
    assert gs["gate"]["holdMs"] == gs["picked"]["holdMs"] and gs["gate"]["rangeDb"] == gs["picked"]["rangeDb"]
    assert gs["picked"]["thresholdDb"] == gs["gate"]["thresholdDb"]
    assert gs["picked"]["thresholdDb"] > gs["baseline"]["thresholdDb"] and gs["gate"]["thresholdDb"] == gs["picked"]["thresholdDb"]
    assert gs["picked"]["floorTerm"] < gs["baseline"]["floorTerm"]
    assert gs["picked"]["feasible"] and gs["picked"]["ltas"] <= gs["baseline"]["ltas"] + gs["ltasToleranceDb"]
    assert (gs["baseline"]["offsetDb"], gs["baseline"]["holdMs"], gs["baseline"]["releaseMs"],
            gs["baseline"]["rangeDb"]) == (10.0, 40.0, 150.0, -50.0)
    assert gs["baseline"]["thresholdDb"] == pytest.approx(gate_preset(floor)["thresholdDb"])
    # the gate gets cleaner with the threshold: the output's floor falls (more negative) from +10 (default) to +20 dB at 150 ms
    by = {(r["offsetDb"], r["holdMs"], r["releaseMs"], r["rangeDb"]): r for r in gs["grid"]}
    assert by[(10.0, 40.0, 150.0, -50.0)] is gs["baseline"] and gs["picked"]["floorDbOut"] < gs["baseline"]["floorDbOut"] - 1.0
    assert {r["offsetDb"] for r in gs["grid"] if r["holdMs"] == 40.0 and r["releaseMs"] == 150.0 and r["rangeDb"] == -50.0} \
        == set(GATE_OFFSETS_DB)


def test_gate_sweep_keeps_the_default_when_the_reference_has_the_default_gate(tmp_path):
    gs, _ = _gate_case(tmp_path, lambda f: cell_gate(f, 10.0))
    assert gs["skipped"] is None and not gs["changed"]
    assert gs["picked"] is gs["baseline"] and gs["baseline"]["floorTerm"] == pytest.approx(0.0, abs=1e-6)
    assert gs["gate"] == gate_preset(gs["diNoiseFloorDb"])             # untouched


def test_reference_floor_is_measured_in_the_references_own_gaps():
    f = reference_floor_db(_gap_di().astype(np.float64))             # soft target: no matched pair, the reference's own gaps
    assert f is not None and -70.0 < f < -20.0
    mix = np.random.default_rng(0).standard_normal(FS * 4) * 0.05    # a dense mix has no real silence to match
    assert reference_floor_db(mix) is None


def test_gate_sweep_descends_threshold_then_hold_then_release_then_range(monkeypatch):
    """The descent on a stand-in chain whose gap floor only depends on the hold (short hold = cleaner) and the range."""
    import types
    from sawblade_match.matcher import gatesweep as G
    monkeypatch.setattr(G, "render_gate", lambda eng, cand, ex, g: np.array([g["holdMs"], g["rangeDb"]]))
    monkeypatch.setattr(G._feel, "floor_db", lambda y, plan: -40.0 + 0.5 * float(y[0]) + 0.1 * float(y[1]))
    monkeypatch.setattr(G.L, "evaluate", lambda y, tgt, eq: types.SimpleNamespace(total=1.0, ltas=1.0, feel_terms={"tight": 0.1}))
    ft = types.SimpleNamespace(mode="paired", plan=types.SimpleNamespace(gap_ok=True), ref=types.SimpleNamespace(floor=-80.0))
    eng = types.SimpleNamespace(map=lambda f, items: [f(i) for i in items])
    sp = types.SimpleNamespace(eq_gains=lambda v: None)
    gs = gate_sweep(eng, types.SimpleNamespace(extra={"params": {}}), sp, None, types.SimpleNamespace(feel=ft), -60.0)
    assert [st["axis"] for st in gs["steps"]] == ["thresholdOffsetDb", "holdMs", "releaseMs", "rangeDb"]
    assert gs["picked"]["holdMs"] == 2.0 and gs["picked"]["rangeDb"] == -90.0 and gs["picked"]["offsetDb"] == 10.0
    assert gs["picked"]["releaseMs"] == 150.0 and gs["changed"] and gs["renders"] == len(gs["grid"]) <= 20
    assert gs["gate"]["holdMs"] == 2.0 and gs["gate"]["rangeDb"] == -90.0 and gs["gate"]["thresholdDb"] == -50.0
    # an acceptance guard: when the LTAS error rises by more than 0.05 dB the cell is not feasible and the default stays
    monkeypatch.setattr(G.L, "evaluate", lambda y, tgt, eq: types.SimpleNamespace(
        total=1.0, ltas=1.0 + (0.0 if (y[0], y[1]) == (40.0, -50.0) else 0.5), feel_terms={"tight": 0.1}))
    gs2 = gate_sweep(eng, types.SimpleNamespace(extra={"params": {}}), sp, None, types.SimpleNamespace(feel=ft), -60.0)
    assert not gs2["changed"] and gs2["gate"] == gate_preset(-60.0)


def test_gate_sweep_honours_the_feel_floor_switch_and_the_clean_reference_rule():
    import types
    plan = types.SimpleNamespace(gap_ok=True)
    eng = types.SimpleNamespace(map=lambda f, items: pytest.fail("nothing may be rendered"))
    sp = types.SimpleNamespace(eq_gains=lambda v: None)
    cand = types.SimpleNamespace(extra={"params": {}})
    off = types.SimpleNamespace(mode="paired", plan=plan, ref=types.SimpleNamespace(floor=-40.0), off=frozenset({"floor"}),
                                dropped={"floor": "matched channel is a full mix"})
    gs = gate_sweep(eng, cand, sp, None, types.SimpleNamespace(feel=off), -60.0)
    assert gs["skipped"] == "matched channel is a full mix" and not gs["changed"] and gs["gate"] == gate_preset(-60.0)
    soft = types.SimpleNamespace(mode="soft", plan=plan, ref=types.SimpleNamespace(floor=None), off=frozenset(), dropped={})
    gs = gate_sweep(eng, cand, sp, None, types.SimpleNamespace(feel=soft), -60.0, -50.0, ref_clean=False)
    assert gs["skipped"] == "reference is not a clean guitar track" and not gs["changed"]


def test_a_matched_mono_reference_is_clean_so_the_gate_sweep_runs_paired(tmp_path):
    gs, _ = _gate_case(tmp_path, lambda f: cell_gate(f, 10.0))
    assert gs["mode"] == "paired" and gs["skipped"] is None and gs["floorRefSource"].startswith("matched")


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
    assert gs["skipped"] and not gs["changed"] and gs["gate"] == gate_preset(-60.0, {"thresholdDb": -50.0, "releaseMs": 150.0})


# ---- the search: boost, filters, cab sweep, ablation, trace ------------------------------------------------------------------
def test_tight_boost_variant_can_win_on_a_hidden_boosted_chain(tmp_path):
    full = fixture_pool()
    pool = Pool([], list(full.amps), list(full.cabs))                  # no pedal capture can make the clipping: only the boost
    hidden_combo = Combo((), full.amps[2], None, None, full.cabs[1], boost=True)
    v = Space.for_combo(hidden_combo).default()
    v.update({"boost.drive": 2.5, "boost.level": 8.0, "boost.tone": 4.0, "post.g1": 1.0})      # level is not searched: fixed at 8
    di, ref = _known(tmp_path, pool, hidden_combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=16, gens_gain=4, gens_final=10,
                  pop_linear=12, pop_gain=6, n_rescore_single=12, n_cab_single=12)
    res = run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=5, excerpt_s=2.0, threads=2, plan=plan,
                           write_audio=False, refine_offsets=False), Log())
    tb = res["tightBoost"]
    assert tb["tried"] == 3 and tb["won"] is True and tb["ablated"] is False and tb["refined"] >= 1
    assert set(tb["params"]) == {"drive", "tone"} and 0.0 <= tb["params"]["drive"] <= 3.0      # boost.level stays at its default 8
    assert 3.0 <= tb["params"]["tone"] <= 8.0
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
        sw = next((c for c in cs["candidates"] if c["pairKey"] == cand["pairKey"]), None)        # THIS candidate's own sweep
        if sw and cand["loss"] is not None:
            slack = 0.05 if res["gateSweep"].get("changed") else 0.0          # the gate sweep (after the cab sweep) may cost <= its LTAS tolerance
            assert cand["loss"] <= sw["best"]["loss"] + 1e-6 + slack or sw["changed"]
    assert res["ablate"] == [] and res["plan"]["boost"] is True and res["plan"]["filters"] is True
    assert "post.hp" in res["best"]["params"] and res["postFilters"]["searched"] is True
    # the swept threshold stays on the grid {6 ... 28} dB re the peak floor; the default cell (10) is a member, 6 the lowest
    assert res["gateFinal"]["thresholdDb"] >= res["gateDefault"]["thresholdDb"] - 4.0 - 1e-9 and "gateSweep" in res
    assert res["gateFloor"]["peakDb"] is not None and "rmsDb" in res["gateFloor"]
    assert res["gateDefault"]["thresholdDb"] == pytest.approx(res["gateFloor"]["peakDb"] + 10.0, abs=0.01)


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
    assert res["ablate"] == list(names)
    assert res["irBlend"] == {"ablated": True, "tried": 0, "won": False}
    assert (res["plan"]["boost"], res["plan"]["filters"], res["plan"]["cab_sweep"]) == (False, False, False)
    tb = res["tightBoost"]
    assert tb["ablated"] is True and tb["tried"] == 0 and tb["won"] is False and tb["refined"] == 0
    assert not any(c["tightBoost"] for c in res["stage1"]["top"]["single"]) and not any(c["tightBoost"] for c in res["candidatesStage2"])
    assert res["cabSweep"]["ablated"] is True and "candidates" not in res["cabSweep"]
    assert res["postFilters"]["searched"] is False and "post.hp" not in res["best"]["params"] and "post.lp_slope" not in res["best"]["params"]
    assert [b["type"] for b in json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())["postEq"]] \
        .count("highPass") == 0
    bd = res["best"]["breakdown"]
    assert bd["feel"] == 0.0 and bd["feelTerms"] is None                    # no feel term in the search's loss ...
    assert res["referenceTarget"]["feel"] is not None                       # ... but it is still measured for the report
    assert "gateSweep" in res                                               # the gate sweep is not an ablation switch
    with pytest.raises(ValueError, match="unknown suspect"):
        run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out2", ablate=("nope",)), Log())


# ---- two-IR blend (B2.1) ------------------------------------------------------------------------------------------------------
def test_ir_alignment_sign_conventions():
    rng = np.random.default_rng(4)
    ha = rng.standard_normal(400) * np.exp(-np.arange(400) / 80.0)
    late = np.zeros(400)
    late[7:] = -ha[:-7]                                    # B arrives 7 samples after A, opposite polarity
    assert ir_alignment(ha, late) == (-7, True, 7)         # the core delays B for positive offsets: it needs -7
    early = np.zeros(400)
    early[:-5] = ha[5:]                                    # B arrives 5 samples before A
    assert ir_alignment(ha, early) == (5, False, -5)
    assert ir_alignment(ha, ha)[:2] == (0, False)


def _irmix_combo(pool, offset, invert):
    return Combo((), pool.amps[2], None, None, pool.cabs[0]).with_pair(pool.cabs[0], pool.cabs[1], offset, invert)


def test_irmix_combo_emulation_equals_full_render_and_round_trips():
    pool = fixture_pool()
    ha, hb = load_ir48(pool.cabs[0]), load_ir48(pool.cabs[1])
    off, inv, _ = ir_alignment(ha, hb)
    combo = _irmix_combo(pool, off, inv)
    assert combo.cab_b is not None and "irMix" in combo.describe() and combo.key()[-1].startswith("4/7+4/8@")
    sp = Space.for_combo(combo)
    assert sp.params[sp.idx["cab.mix"]].group == "discrete" and (sp.params[sp.idx["cab.mix"]].lo, sp.params[sp.idx["cab.mix"]].hi) == (0.2, 0.8)
    v = sp.default()
    v["cab.mix"] = 0.35
    gate = gate_preset(-60.0)
    preset = build_preset(combo, v, gate=gate, align=manual_align())
    assert preset["cab"]["mode"] == "irMix" and preset["cab"]["mix"] == 0.35
    assert preset["cab"].get("offsetSamplesB", 0) == off and preset["cab"].get("invertB", False) == inv
    x, fs = _loadwav(FIX / "di_riff.wav")
    x = to48(x, fs)[:FS * 2]
    eng = Engine(gate, 2)
    try:
        ca = eng.core(combo, v, "a", x)
        em = eng.emulate(combo, v, ca, None, manual_align())
        full, rep = eng.render(preset, x)
        assert rep["cabMode"] == "irMix" and rep["liveCompatible"] is True
        assert np.max(np.abs(full - em)) < 1e-5 * max(1.0, np.max(np.abs(full)))
        single = eng.emulate(combo.with_cab(pool.cabs[0]), v, ca, None, manual_align())
        assert not np.allclose(single, em)
    finally:
        eng.close()
    assert json.loads(json.dumps(preset)) == preset


def test_two_ir_blend_wins_on_a_hidden_irmix_chain_and_ablates(tmp_path):
    pool = fixture_pool()
    ha, hb = load_ir48(pool.cabs[0]), load_ir48(pool.cabs[1])
    off, inv, _ = ir_alignment(ha, hb)
    hidden_combo = Combo((pool.pedals[0],), pool.amps[2], None, None, pool.cabs[0]).with_pair(pool.cabs[0], pool.cabs[1], off, inv)
    v = Space.for_combo(hidden_combo).default()
    v["cab.mix"] = 0.4
    di, ref = _known(tmp_path, pool, hidden_combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0}, gens_linear=10, gens_gain=3, gens_final=8,
                  pop_linear=10, pop_gain=4, n_rescore_single=6, n_cab_single=3)
    kw = dict(di=di, ref=ref, pool=pool, seed=4, excerpt_s=2.0, threads=2, plan=plan, write_audio=False, refine_offsets=False)
    res = run_match(Config(out=tmp_path / "on", **kw), Log())
    ib = res["irBlend"]
    assert ib["ablated"] is False and ib["tried"] == 1 and ib["won"] is True and ib["gainVsSingle"] >= 0.05
    assert {ib["pair"]["irA"], ib["pair"]["irB"]} == {"4/7", "4/8"} and 0.2 <= ib["mix"] <= 0.8
    assert "_won" not in ib and res["best"]["irMix"] is not None
    best = json.loads((tmp_path / "on" / "best.preset.resolved.json").read_text())
    assert best["cab"]["mode"] == "irMix" and best["cab"]["mix"] == pytest.approx(ib["mix"])
    assert best["cab"].get("offsetSamplesB", 0) == ib["offset"] and best["cab"].get("invertB", False) == ib["invert"]
    assert core.render(best, np.zeros(2048, np.float32), 48000.0)[1]["liveCompatible"] is True
    off_res = run_match(Config(out=tmp_path / "off", ablate=("irblend",), **kw), Log())
    assert off_res["irBlend"]["ablated"] is True and off_res["best"]["irMix"] is None
    assert off_res["best"]["loss"] > res["best"]["loss"]


# ---- studio processing (B2.3) ---------------------------------------------------------------------------------------------------
def test_polynomial_shape_detector_separates_a_smooth_curve_from_ripple():
    from sawblade_match.matcher import loss as Lm
    x = np.log10(np.array(Lm.BAND_CENTRES, float))
    smooth = 2.5 * (x - x.mean()) ** 2 - 1.0 * (x - x.mean())
    ripple = 2.0 * np.where(np.arange(len(x)) % 2 == 0, 1.0, -1.0)
    assert ST._poly_explained(smooth - smooth.mean()) > 0.95 and ST._poly_explained(ripple) < ST.POLY_EXPLAINED


def _studio_run(tmp_path, comp, ablate=(), post=None):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")
    v = {**v, **(post or {})}
    di, ref = _known(tmp_path, pool, combo, v, bus_comp=comp)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0}, gens_linear=10, gens_gain=3, gens_final=8,
                  pop_linear=10, pop_gain=4, n_rescore_single=6, n_cab_single=3)
    return run_match(Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=2, excerpt_s=3.5, threads=2, plan=plan,
                            write_audio=False, refine_offsets=False, ablate=ablate), Log())


def test_a_fast_bus_comp_in_the_reference_fires_the_studio_detector_and_is_reproduced(tmp_path):
    comp = {"thresholdDb": -30.0, "ratio": 4.0, "kneeDb": 6.0, "attackMs": 1.0, "releaseMs": 60.0, "makeupDb": 0.0}
    res = _studio_run(tmp_path, comp)
    st = res["studio"]
    assert st["compressed"] is True and st["ablated"] is False
    assert st["evidence"]["crestDropDb"] >= ST.CREST_DROP_DB and "lraCandidateLu" in st["evidence"]
    assert st["busCompUsed"] is True and st["gainVsPlain"] >= ST.MIN_GAIN and "_won" not in st
    c = st["busComp"]
    assert c["releaseMs"] <= 150.0 and 1.5 <= c["ratio"] <= 4.0 and -30.0 <= c["thresholdDb"] <= -6.0 and c["kneeDb"] == 6.0
    best = json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())
    assert best["busComp"]["enabled"] is True and best["busComp"]["releaseMs"] == c["releaseMs"]
    assert "bus comp added by the matcher (studio processing); dropped from no-cab exports" in best["notes"]
    core.render(best, np.zeros(2048, np.float32), 48000.0)                      # strict parse, trainable release
    # --ablate studio: still detected and reported, but nothing is added
    (tmp_path / "abl").mkdir()
    res2 = _studio_run(tmp_path / "abl", comp, ablate=("studio",))
    assert res2["studio"]["compressed"] is True and res2["studio"]["ablated"] is True and res2["studio"]["busCompUsed"] is False
    assert res2["best"]["loss"] > res["best"]["loss"]


def test_absorbable_residual_is_what_the_post_eq_can_still_take_away():
    from sawblade_match.matcher import loss as Lm
    sp = Space((1, None))
    v = sp.default()
    fc = np.array(Lm.BAND_CENTRES, float)
    w = Lm.A_POWER_W / Lm.A_POWER_W.sum()
    d_in = ST._peak_db(fc, v["post.f1"], 4.0)                    # a smooth bump the post EQ's own mid band can take away
    d_in = d_in - np.sum(w * d_in)
    left_in = ST.absorbable(d_in, v)
    assert np.sqrt(np.sum(w * left_in ** 2)) < 0.3 < np.sqrt(np.sum(w * d_in ** 2))
    d_out = ST._peak_db(fc, v["post.f1"], 20.0)                  # far beyond +-6 dB: most of it stays
    d_out = d_out - np.sum(w * d_out)
    left_out = ST.absorbable(d_out, v)
    assert 2.0 < np.sqrt(np.sum(w * left_out ** 2)) < np.sqrt(np.sum(w * d_out ** 2))     # the +-6 dB cap leaves most of it
    assert np.array_equal(ST.absorbable(d_in, {}), d_in)         # no post EQ in the params: nothing is absorbed


class _PostOnlySpace(Space):
    """Stage-2 space with the path EQs frozen: only the post EQ (and the other linear parameters) are free, so a reference whose
    post EQ is beyond +-6 dB cannot be compensated by the +-9 dB pre-cab path EQ bands."""

    def indices(self, group):
        return [i for i in super().indices(group) if not self.names[i].startswith(("a.", "b."))]


def test_a_strongly_post_eqd_reference_fires_eqd_and_the_wider_post_eq_is_kept(tmp_path):
    """Reference = the chain with post EQ gains of -11 / +11 dB (far beyond the +-6 dB range). The candidate is the same chain
    with a flat post EQ (an unfitted stage-2 result): the unreachable residual fires eqd, and the +-9 dB stage keeps its wider
    EQ because it beats an equal-budget re-fit at the normal range, not just the plain result. The path EQs are frozen in the
    stage's space (``_PostOnlySpace``): with them free, the +-9 dB pre-cab bands can stand in for the missing post-EQ range and the
    wider post EQ is rightly not needed."""
    def hid(sp):
        v = sp.default()
        v.update({"post.g0": -11.0, "post.g2": 11.0})
        return v
    eng, cand, sp, ex, tgt = _stage1_candidate(tmp_path, hid)
    try:
        v = dict(sp.default())
        core_a = eng.core(cand.combo, v, "a", ex.x)
        y = ex.trim(eng.emulate(cand.combo, v, core_a, None, manual_align()))
        det = ST.detect(True, tgt, tgt, y, v, None)
        ev = det["evidence"]
        assert det["eqd"] is True and det["compressed"] is False
        assert ev["residualAfterPostEqRmsDb"] >= ST.RESIDUAL_RMS_DB and ev["residualAfterPostEqPolyExplained"] >= ST.POLY_EXPLAINED
        assert ev["residualAfterPostEqRmsDb"] < ev["residualRmsDb"]                  # the post EQ takes part of it away
        assert ST.detect(False, tgt, tgt, y, v, None)["evidence"]["skipped"]          # not judged on a mix reference
        cand.extra.update(params=v, info={})
        post_only = _PostOnlySpace(cand.combo.shape())
        rec = ST.studio_stage(eng, cand, post_only, ex, tgt, {"compressed": False, "eqd": True}, seed=3, gens=12, pop=12)
    finally:
        eng.close()
    st = rec["stage"][0]
    assert rec["widenedPostEq"] is True and st["kept"] is True and rec["busCompUsed"] is False
    assert st["normalRefitLoss"] - st["loss"] >= ST.MIN_GAIN and rec["gainVsPlain"] >= ST.MIN_GAIN
    v2 = rec["_won"][0]
    assert max(abs(v2[f"post.g{i}"]) for i in range(3)) > 6.0                         # a gain outside the old range


def test_the_plain_chain_does_not_fire_the_studio_detector(tmp_path):
    res = _studio_run(tmp_path, None)
    st = res["studio"]
    assert st["compressed"] is False and st["eqd"] is False and st["busCompUsed"] is False and "stage" not in st
    best = json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())
    assert not best.get("busComp", {}).get("enabled")


# ---- pre-EQ (B4) ----------------------------------------------------------------------------------------------------------------
def _stage1_candidate(tmp_path, v_hidden):
    pool = fixture_pool()
    combo, sp, _ = hidden(pool, "single")
    sp = Space.for_combo(combo)
    di, ref = _known(tmp_path, pool, combo, v_hidden(sp))
    x, fs = _loadwav(FIX / "di_riff.wav")
    x48 = to48(x, fs)
    ex = make_excerpt(x48, 2.0)
    tgt = build_target(ref, ex)
    eng = Engine(gate_preset(gate_envelope_floor_db(x48, FS)), 2)
    v0 = sp.default()
    core_ = eng.core(combo, v0, "a", ex.x)
    r = ST.L.evaluate(ex.trim(eng.emulate(combo, v0, core_, None, manual_align())), tgt, sp.eq_gains(v0))
    return eng, Scored(combo, r.total, 0.0, manual_align(), r, "screen", {}), sp, ex, tgt


def test_pre_eq_grid_recovers_a_hidden_hpf_and_mid_peak_deterministically(tmp_path):
    def hid(sp):
        v = sp.default()
        v.update({"pre.a.hpf": 110.0, "pre.a.mid_db": 6.0, "pre.a.mid_hz": 900.0})
        return v
    eng, cand, sp, ex, tgt = _stage1_candidate(tmp_path, hid)
    try:
        nums = PE.di_spectrum_numbers(ex.x[ex.lead:], tgt.starts)
        wide = PE.widening(nums)
        n_wide = len(wide["hpf"]) + len(wide["mid"]) + len(wide["shelf"])
        rec = PE.preeq_candidate(eng, cand, sp, ex, tgt, wide)
        rec2 = PE.preeq_candidate(eng, cand, sp, ex, tgt, wide)
    finally:
        eng.close()
    a = rec["paths"]["a"]
    assert a["values"] == [110.0, 6.0, 900.0, 0.0] and rec["loss"] < 0.01 < rec["offLoss"]            # the exact grid point
    assert rec["params"]["pre.a.hpf"] == 110.0 and rec["params"]["pre.a.mid_db"] == 6.0 and rec["gainVsOff"] > 1.0
    assert rec["renders"] <= 12 + n_wide and a["grid"][0]["setting"] == "off"                       # <= 12 settings + widening
    assert rec2["paths"]["a"]["grid"] == a["grid"] and rec2["params"] == rec["params"]               # deterministic
    # and the preset the grid implies carries the pre-EQ on that path (core renders it)
    combo = cand.combo
    v = {**sp.default(), **rec["params"]}
    preset = build_preset(combo, v, gate=gate_preset(-60.0), align=manual_align())
    assert [b["type"] for b in preset["paths"]["a"]["preEq"]] == ["highPass", "peak"] and preset["paths"]["a"]["preEq"][1]["q"] == 0.8
    core.render(preset, np.zeros(2048, np.float32), 48000.0)


def test_pre_eq_widening_fires_on_a_dark_di_and_not_on_the_fixture_for_dark(tmp_path):
    rng = np.random.default_rng(1)
    from sawblade_match.matcher import loss as Lm
    n = FS * 4
    white = rng.standard_normal(n)
    dark = signal.sosfilt(signal.butter(2, 300.0, btype="lowpass", fs=FS, output="sos"), white)
    bassy = signal.sosfilt(signal.butter(2, 250.0, btype="lowpass", fs=FS, output="sos"), white) + 0.02 * white
    starts = Lm.segment_starts(n, None)
    nd, nb, nw = (PE.di_spectrum_numbers(x, starts) for x in (dark, bassy, white))
    assert nd["diTilt"] < PE.TILT_DARK and (9.0, 800.0) in PE.widening(nd)["mid"]
    assert nb["diLowExcess"] > PE.LOW_EXCESS_BASSY and -6.0 in PE.widening(nb)["shelf"]
    assert nw["diTilt"] > PE.TILT_BRIGHT and 180.0 in PE.widening(nw)["hpf"]
    fx, fs = _loadwav(FIX / "di_riff.wav")
    fnum = PE.di_spectrum_numbers(to48(fx, fs), Lm.segment_starts(len(fx), None))
    assert not PE.widening(fnum)["mid"] and not PE.widening(fnum)["shelf"]       # the fixture DI is not dark / bassy


def test_ablate_preeq_leaves_the_pre_eq_empty_and_the_default_run_reports_it(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")
    di, ref = _known(tmp_path, pool, combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0}, gens_linear=4, gens_gain=2, gens_final=3,
                  n_rescore_single=6, n_cab_single=2)
    kw = dict(di=di, ref=ref, pool=pool, seed=2, excerpt_s=2.0, threads=2, plan=plan, write_audio=False, refine_offsets=False)
    off = run_match(Config(out=tmp_path / "off", ablate=("preeq",), **kw), Log())
    assert off["ablate"] == ["preeq"] and off["preEq"]["ablated"] is True and off["preEq"]["candidates"] == []
    assert off["preEq"]["chosen"] == {"a": "off"} and off["preEq"]["gainVsOff"] == 0.0 and "diTilt" in off["preEq"]
    assert "preEq" not in json.loads((tmp_path / "off" / "best.preset.resolved.json").read_text())["paths"]["a"]
    on = run_match(Config(out=tmp_path / "on", **kw), Log())
    pe = on["preEq"]
    assert pe["ablated"] is False and pe["candidates"] and all(c["renders"] <= 12 + len(pe["widened"]) for c in pe["candidates"])
    assert all({"offLoss", "gridBest", "refitLoss", "kept", "gain"} <= set(c) for c in pe["candidates"])
    assert set(pe["chosen"]) == {"a"} and pe["gainVsOff"] >= 0.0 and set(pe["grid"]) <= {"a"}
    # a chain without a pre-EQ ends with it off, or not worse than with the grid ablated (a pick must survive the re-fit)
    assert on["best"]["loss"] <= off["best"]["loss"] + 1e-9
    best = json.loads((tmp_path / "on" / "best.preset.resolved.json").read_text())
    assert ("preEq" in best["paths"]["a"]) == (pe["chosen"]["a"] != "off")
    for c in pe["candidates"]:
        assert c["kept"] == (c["refitLoss"] is not None and c["offLoss"] - c["refitLoss"] >= pe["keepDb"])
def test_choose_pedal_single_must_beat_the_pedal_less_single_by_the_margin():
    from sawblade_match.matcher.run import PEDAL_OCCAM_DB
    pool = fixture_pool()
    bare = Combo((), pool.amps[0], None, None, pool.cabs[0], boost=True)
    ped = Combo((pool.pedals[0],), pool.amps[0], None, None, pool.cabs[0], boost=True)
    other = Combo((pool.pedals[0],), pool.amps[1], None, None, pool.cabs[0], boost=True)
    mk = lambda c, l: Scored(c, l, 0.0, manual_align(), None, "refined", {})
    w = choose([mk(bare, 1.0), mk(ped, 1.0 - PEDAL_OCCAM_DB * 0.5)])
    assert w.combo is bare and w.extra["pedalOccamDropped"] == [ped.key()]                  # not enough: the pedal is spurious
    w = choose([mk(bare, 1.0), mk(ped, 1.0 - PEDAL_OCCAM_DB * 2)])
    assert w.combo is ped and w.extra["pedalOccamDropped"] == []                           # clearly better: kept
    assert choose([mk(other, 1.0)]).combo is other                                          # no pedal-less single to compare with
    assert choose([mk(bare, 1.0), mk(other, 0.98)]).combo is bare                          # other amp: compared with the best bare


def test_gate_cell_acceptance_has_a_noise_level_tightness_tolerance():
    from sawblade_match.matcher.gatesweep import LTAS_TOL_DB, TIGHT_TOL, cell_feasible
    base = {"ltas": 1.0, "tight": 0.20, "floorTerm": 0.5}
    cell = lambda **k: {"ltas": 1.0, "tight": 0.20, "floorTerm": 0.1, **k}
    assert TIGHT_TOL == 0.05 and LTAS_TOL_DB == 0.05
    assert cell_feasible(cell(tight=0.24), base)                       # worse by 0.04 normalised: accepted
    assert not cell_feasible(cell(tight=0.26), base)                   # worse by 0.06: rejected
    assert cell_feasible(cell(ltas=1.04), base) and not cell_feasible(cell(ltas=1.06), base)       # LTAS rule unchanged
    assert not cell_feasible(cell(floorTerm=None), base) and cell_feasible(cell(tight=None), base)


def test_pick_slopes_joint_grid_polish_and_appended_steps():
    from sawblade_match.matcher import refine
    from sawblade_match.matcher.refine import DISCRETE_UP, HP_GRID, MIN_FILTER_GAIN, POST_CMA_STEPS, pick_slopes
    sp = Space((1, None))
    g = HP_GRID[3]

    def make_score(target_hp, target_slope, target_lp=None):
        def score(v):
            t = abs(np.log(v["post.hp"] / target_hp)) + (0.0 if (v["post.hp_slope"] >= 0.5) == (target_slope >= 0.5) else 0.5)
            if target_lp is not None:
                t += abs(np.log(v["post.lp"] / target_lp)) + (0.0 if v["post.lp_slope"] >= 0.5 else 0.3)
            return L.LossResult(1.0 + t, 0.0, 0.0, None, None, 0.0, 0.0)
        return score
    v0 = sp.default()
    calls = []

    def polish(v):
        calls.append(dict(v))
        return v, L.LossResult(0.0, 0.0, 0.0, None, None, 0.0, 0.0)           # a better polish: kept
    v, r = pick_slopes(sp, v0, make_score(g, DISCRETE_UP), polish)
    assert v["post.hp"] == g and v["post.hp_slope"] >= 0.5 and len(calls) == 1 and r.total == 0.0   # joint (g, 24) minimum
    # nothing to gain (the start is already the minimum): polish is never called, the start is returned
    calls.clear()
    start = {**v0, "post.hp": g, "post.hp_slope": DISCRETE_UP}
    v, r = pick_slopes(sp, start, make_score(g, DISCRETE_UP), polish)
    assert not calls and v["post.hp"] == g and v["post.hp_slope"] == DISCRETE_UP
    # a gain below MIN_FILTER_GAIN does not count as a change
    flat = lambda vv: L.LossResult(1.0 - (MIN_FILTER_GAIN / 2 if vv["post.hp"] == g else 0.0), 0.0, 0.0, None, None, 0.0, 0.0)
    v, r = pick_slopes(sp, v0, flat, polish)
    assert not calls and v["post.hp"] == v0["post.hp"]
    # a worse polish is discarded
    worse = lambda vv: (vv, L.LossResult(99.0, 0.0, 0.0, None, None, 0.0, 0.0))
    v, r = pick_slopes(sp, v0, make_score(g, DISCRETE_UP), worse)
    assert v["post.hp"] == g and r.total < 2.0
    # the low-pass step: frequency around the CMA value x slope
    lp0 = 8000.0
    v, r = pick_slopes(sp, {**v0, "post.lp": lp0}, make_score(v0["post.hp"], 0.0, lp0 * 1.12), None)
    assert v["post.lp"] == pytest.approx(lp0 * 1.12) and v["post.lp_slope"] >= 0.5
    # an appended step runs after the built-in ones
    seen = []

    def extra(space, best, r, score):
        seen.append(best["post.hp"])
        return {**best, "post.g0": 3.0}, r, False
    POST_CMA_STEPS.append(extra)
    try:
        v, r = pick_slopes(sp, v0, make_score(g, DISCRETE_UP), None)
    finally:
        POST_CMA_STEPS.remove(extra)
    assert seen == [g] and v["post.g0"] == 3.0 and refine.POST_CMA_STEPS[:2] == [refine._hp_step, refine._lp_step]


def test_lp_step_skips_duplicate_and_current_candidates():
    from sawblade_match.matcher.refine import _lp_step
    sp = Space((1, None))
    seen = []

    def score(v):
        seen.append((v["post.lp"], v["post.lp_slope"] >= 0.5))
        return L.LossResult(1.0, 0.0, 0.0, None, None, 0.0, 0.0)
    v = {**sp.default(), "post.lp": 12000.0, "post.lp_slope": 0.0}           # at the range top: every x>1 factor clips to it
    _lp_step(sp, v, score(v), score)
    seen = seen[1:]
    assert len(seen) == len(set(seen)) and (12000.0, False) not in seen        # no duplicates, not the current setting


def test_refit_without_staging_keeps_feel_in_its_first_block(monkeypatch):
    """The pre-EQ confirmation re-fit starts from a feel-fitted optimum: refine_combo(staged=False) must not drop feel in L1
    (the staged first block of stage 2 does, see test_matcher.test_stage2_first_linear_block_is_ltas_only)."""
    from sawblade_match.matcher import loss as Lm
    from sawblade_match.matcher import refine as R
    seen = []

    class Spy:
        def __getattr__(self, name):
            return getattr(Lm, name)

        def evaluate(self, out, tgt, eq=None):
            seen.append(tgt.feel is not None)
            return Lm.evaluate(out, tgt, eq)

    monkeypatch.setattr(R, "L", Spy())
    tmp = Path(tempfile.mkdtemp())
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "single")
    _, ref = _known(tmp, pool, combo, v)
    x, fs = _loadwav(FIX / "di_riff.wav")
    x48 = to48(x, fs)
    ex = make_excerpt(x48, 2.0)
    tgt = build_target(ref, ex)
    eng = Engine(gate_preset(gate_envelope_floor_db(x48, FS)), 2)
    kw = dict(seed=1, gens_linear=2, pop_linear=6, gens_gain=1, pop_gain=4, gens_final=1, log=lambda *_: None)
    n_l1 = 1 + 2 * 6
    try:
        sp2 = Space.for_combo(combo)
        R.refine_combo(eng, combo, sp2, ex, tgt, manual_align(), sp2.default(), **kw)
        staged, seen[:] = list(seen), []
        R.refine_combo(eng, combo, sp2, ex, tgt, manual_align(), sp2.default(), staged=False, **kw)
        unstaged = list(seen)
    finally:
        eng.close()
    assert staged[0] is True and staged[1:1 + n_l1] == [False] * n_l1 and all(staged[1 + n_l1:])
    assert all(unstaged) and len(unstaged) == len(staged)


def test_gate_sweep_uses_the_full_di_gaps_when_the_excerpt_has_none(tmp_path):
    """Task H.2: an excerpt of chugs only (no gap) used to skip the sweep; the full DI has rests, so the sweep runs on them
    (gapSource fullDi), picks a gate the hard-gated reference prefers, and the gap noise of the full render falls."""
    pool = fixture_pool()
    combo = Combo((), pool.amps[2], None, None, pool.cabs[0], boost=True)
    sp = Space.for_combo(combo)
    v = sp.default()
    v.update({"boost.drive": 3.0, "boost.level": 10.0, "gain.a.amp": 12.0})
    di = _gap_di()
    floor = gate_envelope_floor_db(di, FS)
    dip, ref = _known(tmp_path, pool, combo, v, gate=cell_gate(floor, 20.0, 20.0, 2.0, -90.0), di=di)
    ex = make_excerpt(di, 0.6, window=(0, int(0.6 * FS)))                    # the first chugs: no 120 ms silence in it
    tgt = build_target(ref, ex)
    assert tgt.feel is not None and not tgt.feel.plan.gap_ok
    eng = Engine(gate_preset(floor), 2)
    cand = Scored(combo, 0.0, 0.0, manual_align(), None, "refined", {"params": v})
    full = {"di": di, "ref": ref.matched_sig, "offset": 0}
    try:
        skipped = gate_sweep(eng, cand, sp, ex, tgt, floor)                  # without the full DI: as before, skipped
        gs = gate_sweep(eng, cand, sp, ex, tgt, floor, full=full)
        assert skipped["skipped"] and skipped["gapSource"] == "excerpt"
        assert gs["skipped"] is None and gs["gapSource"] == "fullDi" and gs["gapWindows"] and gs["changed"]
        assert gs["picked"]["floorTerm"] < gs["baseline"]["floorTerm"]
        assert gs["picked"]["feasible"] and gs["picked"]["offsetDb"] > 4.0
        y0, _ = eng.render({**build_preset(combo, v, gate=gate_preset(floor), align=manual_align())}, di)
        y1, _ = eng.render({**build_preset(combo, v, gate=gs["gate"], align=manual_align())}, di)
    finally:
        eng.close()
    from sawblade_match.tonecheck.analysis import gap_regions
    gaps = [(a, b) for a, b in gap_regions(di.astype(np.float64), FS) if b > a]
    assert gaps

    def gap_noise_db(y):             # output power in the full DI's gaps re its power over the rest
        m = np.zeros(len(y), bool)
        for a, b in gaps:
            m[a:b] = True
        y = np.asarray(y, np.float64)
        return 10 * np.log10(np.mean(y[m] ** 2) / np.mean(y[~m] ** 2))
    assert gap_noise_db(y1) < gap_noise_db(y0) - 1.0                         # gap_noise improves vs the default cell


# ---- Task H.3: topology margin, BLEND_OCCAM_DB, --topology ------------------------------------------------------------------
def test_topology_margin_record_and_determination():
    from sawblade_match.matcher.run import BLEND_OCCAM_DB, TOPOLOGY_DETERMINED_PCT, topology_margin
    pool = fixture_pool()
    p, a = pool.pedals, pool.amps
    blend = Combo((p[0],), a[0], (), a[1], pool.cabs[0])
    single = Combo((p[0],), a[0], None, None, pool.cabs[0])
    single2 = Combo((p[0], p[1]), a[0], None, None, pool.cabs[0])
    mk = lambda c, l: Scored(c, l, 0.5, manual_align(), None, "refined", {})
    assert BLEND_OCCAM_DB == 0.25 and TOPOLOGY_DETERMINED_PCT == 10.0
    r = topology_margin([mk(blend, 1.0), mk(single, 1.05), mk(single2, 1.3)])
    assert r["bestSingle"] == 1.05 and r["bestBlend"] == 1.0 and r["bestSingleTopology"] == "single"
    assert r["deltaPct"] == pytest.approx(5.0) and r["determined"] is False             # 5 % of the smaller loss: not determined
    r = topology_margin([mk(blend, 1.0), mk(single2, 1.12)])
    assert r["deltaPct"] == pytest.approx(12.0) and r["determined"] is True and r["bestSingleTopology"] == "single2"
    r = topology_margin([mk(blend, 1.2), mk(single, 1.0)])                              # negative: the single is better
    assert r["deltaPct"] == pytest.approx(-20.0) and r["determined"] is True
    r = topology_margin([mk(blend, 1.0)])                                               # forced: one side only
    assert r["determined"] is False and r["deltaPct"] is None and r["bestSingle"] is None and "forced" in r["note"]


def test_choose_honours_the_topology_restriction():
    pool = fixture_pool()
    p, a = pool.pedals, pool.amps
    blend = Combo((p[0],), a[0], (), a[1], pool.cabs[0])
    single = Combo((p[0],), a[0], None, None, pool.cabs[0])
    mk = lambda c, l: Scored(c, l, 0.5, manual_align(), None, "refined", {})
    field = [mk(blend, 1.0), mk(single, 1.5)]
    assert choose(field, "blend").combo is blend and choose(field, "single").combo is single and choose(field).combo is blend
    with pytest.raises(ValueError):
        choose([mk(blend, 1.0)], "single")
    with pytest.raises(ValueError):
        choose(field, "sideways")


def test_run_topology_forces_the_search_and_records_the_margin(tmp_path):
    from sawblade_match.matcher.run import Config as Cfg
    pool = fixture_pool()
    combo, sp, v = hidden(pool, "blend")
    x, fs = _loadwav(FIX / "di_riff.wav")
    d = tmp_path
    di = d / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    gate = gate_preset(gate_envelope_floor_db(to48(x, fs), FS))
    hid, _ = core.render(build_preset(combo, v, gate=gate, align=Engine(gate).probe_align(combo, v)), x, float(fs))
    sf.write(str(d / "hidden.wav"), hid, fs, subtype="FLOAT")
    out = {}
    for mode in ("blend", "single", "auto"):
        ref = load_reference(d / "hidden.wav", channel="mid", matched="mono", offset_ms=0.0)
        plan = mkplan(top_k={"blend": 1, "single": 1, "single2": 0})
        cfg = Config(di=di, ref=ref, pool=pool, out=d / mode, seed=1, excerpt_s=2.0, threads=2, plan=plan, write_audio=False,
                     refine_offsets=False, topology=mode)
        out[mode] = run_match(cfg, Log())
    assert set(out["blend"]["topologies"]) == {"blend"} and out["blend"]["best"]["topology"] == "blend"
    assert set(out["single"]["topologies"]) == {"single"} and out["single"]["best"]["topology"] == "single"
    assert set(out["auto"]["topologies"]) == {"single", "blend"}
    tm = out["auto"]["topology"]
    assert tm["mode"] == "auto" and tm["blendOccamDb"] == 0.25 and tm["bestSingle"] is not None and tm["bestBlend"] is not None
    assert isinstance(tm["determined"], bool) and tm["deltaPct"] is not None
    assert out["blend"]["topology"]["determined"] is False and out["blend"]["topology"]["mode"] == "blend"
    import argparse
    from sawblade_match.matcher.cli import build_parser
    assert build_parser().parse_args(["--di", "x", "--ref", "y"]).topology == "auto"
    with pytest.raises(ValueError):
        run_match(Config(di=di, ref=ref, pool=pool, out=d / "bad", plan=plan, topology="sideways"), Log())
