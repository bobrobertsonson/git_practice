"""Tests for the matcher (phase 3.2). Needs the sawblade_core extension for the render-based tests."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.matcher import cma, excerpt, loss as L, offset
from sawblade_match.matcher.pool import Capture, Pool, gain_class, load_pool
from sawblade_match.matcher.space import Combo, Space, build_preset, manual_align, gate_preset

REPO = Path(__file__).resolve().parents[2]
FIX = REPO / "tests" / "fixtures"


def cap(path: Path, tone: int, model: int, kind: str, title="T", name="m") -> Capture:
    gear = {"drive": "pedal", "distortion": "pedal", "fuzz": "pedal", "amp_low": "amp", "amp_high": "amp",
            "cab": "cab"}[kind]
    from sawblade_match.t3k.cache import sha256_file
    return Capture(tone, model, title, name, gear, str(path), sha256_file(path), path.stat().st_size, "cc-by", "me",
                   f"https://example/{tone}", kind)


# ---- loss -------------------------------------------------------------------------------------------------------------
def test_ltas_error_removes_level_offset():
    rng = np.random.default_rng(0)
    ref = rng.normal(-40, 5, len(L.BAND_CENTRES))
    e0, off0 = L.ltas_error(ref + 6.0, ref)
    assert e0 == pytest.approx(0.0, abs=1e-9) and off0 == pytest.approx(6.0)
    d = rng.normal(0, 2, len(L.BAND_CENTRES))
    e1, _ = L.ltas_error(ref + d, ref)
    e2, _ = L.ltas_error(ref + d - 13.0, ref)
    assert e1 == pytest.approx(e2)
    assert e1 > 0


def test_ltas_error_is_a_weighted():
    ref = np.zeros(len(L.BAND_CENTRES))
    lo, hi = ref.copy(), ref.copy()
    lo[0] = 6.0                                  # 80 Hz band (A-weighted down)
    mid = int(np.argmin(np.abs(np.array(L.BAND_CENTRES) - 2500)))
    hi[mid] = 6.0                                # 2.5 kHz band (A-weighted up)
    assert L.ltas_error(hi, ref)[0] > L.ltas_error(lo, ref)[0]


def _tone(fs=48000, n=48000 * 5, seed=0):
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(n)
    return signal.sosfilt(signal.butter(2, [100, 6000], btype="band", fs=fs, output="sos"), x).astype(np.float64) * 0.1


def test_evaluate_identical_is_zero_and_gain_invariant():
    x = _tone()
    starts = L.segment_starts(len(x), None)
    tgt = L.Target(starts, L.features(x, starts, None), None, None, x)
    r = L.evaluate(x, tgt)
    assert r.total == pytest.approx(0.0, abs=1e-6) and r.stft == pytest.approx(0.0, abs=1e-6)
    r2 = L.evaluate(x * 0.25, tgt)
    assert r2.ltas == pytest.approx(0.0, abs=1e-6) and r2.stft == pytest.approx(0.0, abs=1e-6)
    assert r2.offset_db == pytest.approx(-12.04, abs=0.01)
    y = signal.sosfilt(signal.butter(1, 1500, fs=48000, output="sos"), x)
    assert L.evaluate(y, tgt).total > r.total + 0.5


def test_stft_loss_is_asymmetric_and_gain_invariant():
    ref = _tone(seed=1)
    extra = ref + 0.8 * _tone(seed=2)       # output has energy the reference lacks (over-prediction)
    over = L.stft_loss(extra, ref)
    under = L.stft_loss(ref, extra)
    assert over > under > 0
    assert L.stft_loss(ref * 3.0, ref) == pytest.approx(0.0, abs=1e-6)


def test_weights_documented():
    assert (L.W_LTAS, L.W_BUZZ, L.W_DECAY, L.W_STFT, L.W_REG) == (1.0, 0.5, 2.0, 0.25, 0.02)


# ---- search space -------------------------------------------------------------------------------------------------------
def test_space_roundtrip_and_bounds():
    for shape, n_gain in (((1, 1), 4), ((0, 0), 2), ((1, None), 2), ((2, None), 3), ((0, None), 1)):
        sp = Space(shape)
        assert len(sp.indices("gain")) == n_gain
        assert ("blend" in sp.idx) == (shape[1] is not None)
        assert ("b.f0" in sp.idx) == (shape[1] is not None)
        d = sp.default()
        u = sp.encode(d)
        back = sp.decode(u)
        for k in d:
            assert back[k] == pytest.approx(d[k], rel=1e-9, abs=1e-9), k
        lo, hi = sp.decode(np.zeros(len(sp))), sp.decode(np.ones(len(sp)))
        for p in sp.params:
            assert lo[p.name] == pytest.approx(p.lo) and hi[p.name] == pytest.approx(p.hi)
        if shape[1] is not None:
            assert sp.decode(np.full(len(sp), 7.0))["blend"] == pytest.approx(0.95)   # clipped into the box
    sp = Space((1, 1))
    assert 0.0 < sp.encode({"a.f0": 200.0})[sp.idx["a.f0"]] < 1.0
    assert sp.decode(sp.encode({"gain.a.amp": -12.0}))["gain.a.amp"] == pytest.approx(-12.0)
    assert len(sp.eq_gains(sp.default())) == 9
    assert len(Space((1, None)).eq_gains(Space((1, None)).default())) == 6


def test_classifier_is_gear_class_not_title_filter():
    from sawblade_match.matcher.classify import classify
    assert classify("pedal", "Boss HM-2", "x") == "distortion"
    assert classify("pedal", "ProCo RAT 2", "x") == "distortion"
    assert classify("pedal", "Ibanez TS808", "x") == "drive"
    assert classify("pedal", "Electro-Harmonix Big Muff", "x") == "fuzz"
    assert classify("pedal", "Dallas Rangemaster", "x") == "fuzz"
    assert classify("pedal", "BOOST PEDAL PACK", "TECH 21 SANSAMP") == "preamp"      # the model name wins
    assert classify("pedal", "BOOST PEDAL PACK", "FORTIN GRIND") == "drive"          # then the title
    assert classify("pedal", "Mystery box", "thing") == "pedal_unknown"
    assert classify("amp", "EVH 5150iii", "Red") == "amp_high"
    assert classify("amp", "Marshall", "Clean") == "amp_low"
    assert classify("amp", "Orange Rockerverb", "x") == "amp_high"
    assert classify("cab", "anything", "x") == "cab"


def test_gain_class():
    assert gain_class("Marshall", "Gain1-Mast6") == "low"
    assert gain_class("Amp", "CH Clean") == "low"
    assert gain_class("6505", "Gain-10") == "high"
    assert gain_class("Plexi", "CH I High") == "high"
    assert gain_class("X", "Y") == "unknown"


# ---- CMA-ES -------------------------------------------------------------------------------------------------------------
def test_cma_toy_and_determinism():
    target = np.array([0.2, 0.8, 0.5, 0.35, 0.65, 0.5])

    def f(x):
        return float(np.sum((x - target) ** 2))

    bx, bf, hist = cma.minimize(f, np.full(6, 0.9), 0.3, generations=60, seed=5)
    assert bf < 1e-3 and np.allclose(bx, target, atol=0.05)
    assert hist[-1] <= hist[0]
    bx2, bf2, _ = cma.minimize(f, np.full(6, 0.9), 0.3, generations=60, seed=5)
    assert np.array_equal(bx, bx2) and bf == bf2
    bx3, _, _ = cma.minimize(f, np.full(6, 0.9), 0.3, generations=60, seed=6)
    assert not np.array_equal(bx, bx3)


def test_cma_respects_bounds():
    es = cma.CMAES(np.full(4, 0.5), 0.8, seed=1)
    for _ in range(5):
        X = es.ask()
        assert X.min() >= 0 and X.max() <= 1
        es.tell(X, X.sum(axis=1))


# ---- excerpt selection ----------------------------------------------------------------------------------------------------
def test_excerpt_picks_dense_region_and_avoids_clip():
    fs = 44100
    rng = np.random.default_rng(0)
    x = rng.standard_normal(fs * 40) * 0.001                  # noise floor
    x[fs * 10:fs * 18] += rng.standard_normal(fs * 8) * 0.2   # dense playing 10-18 s
    x[fs * 25:fs * 27] += rng.standard_normal(fs * 2) * 0.2   # sparse
    a, b, info = excerpt.select_excerpt(x, fs, 6.0)
    assert 10 * fs <= a and b <= 18 * fs + fs // 2 and b - a == 6 * fs
    x2 = x.copy()
    x2[int(fs * 13)] = 1.0                                      # clip inside the best region
    a2, b2, info2 = excerpt.select_excerpt(x2, fs, 6.0)
    assert not (a2 <= int(fs * 13) < b2)
    assert excerpt.select_excerpt(x, fs, 6.0) == excerpt.select_excerpt(x, fs, 6.0)   # deterministic


def test_excerpt_short_di():
    a, b, _ = excerpt.select_excerpt(np.ones(1000), 48000, 6.0)
    assert (a, b) == (0, 1000)


# ---- offset refinement ------------------------------------------------------------------------------------------------------
def _bursts(fs=48000, seconds=8, seed=1):
    rng = np.random.default_rng(seed)
    x = np.zeros(fs * seconds)
    for t in np.arange(0.2, seconds - 0.7, 0.35):
        i, n = int(t * fs), int(0.25 * fs)
        x[i:i + n] += rng.standard_normal(n) * np.exp(-np.arange(n) / (0.08 * fs))
    return signal.sosfilt(signal.butter(2, [200, 5000], btype="band", fs=fs, output="sos"), x)


@pytest.mark.parametrize("true", [9137, 8700, 9455])
def test_offset_refinement_sample_accurate(true):
    fs = 48000
    x = _bursts()
    rng = np.random.default_rng(3)
    ref = np.zeros(len(x) + 30000)
    ref[true:true + len(x)] = 0.6 * x
    ref += rng.standard_normal(len(ref)) * 0.02            # unrelated noise in the "mix"
    start = fs
    r = offset.refine_offset(x[start:start + 4 * fs], ref, fs, coarse=9000, start=start)
    assert r["offset"] == true
    assert r["fineAccepted"]


def test_offset_search_window_ignores_a_stronger_decoy_outside_it():
    """With a +-20 ms window around the hint, a stronger copy 200 ms away is not picked (the cover run chose 176.9 ms over a
    192.2 ms hint with the default +-250 ms)."""
    fs = 48000
    x = _bursts()
    true, decoy = 9137, 9137 + int(0.2 * fs)
    ref = np.zeros(len(x) + 40000)
    ref[true:true + len(x)] += 0.4 * x
    ref[decoy:decoy + len(x)] += 1.0 * x
    ref += np.random.default_rng(3).standard_normal(len(ref)) * 0.01
    wide = offset.refine_offset(x[fs:5 * fs], ref, fs, coarse=9000, start=fs, search=int(0.25 * fs))
    narrow = offset.refine_offset(x[fs:5 * fs], ref, fs, coarse=9000, start=fs, search=int(0.020 * fs))
    assert abs(wide["offset"] - decoy) < 50
    assert narrow["offset"] == true


def test_run_match_searches_only_around_an_explicit_offset_hint(tmp_path, monkeypatch):
    """--offset-ms: the excerpt pass and the final pass both search +-20 ms around the hint; with no hint the wide
    searches (+-3 s / +-250 ms) are unchanged."""
    from sawblade_match.matcher import run as R
    from sawblade_match.matcher.reference import load_reference
    pool, combo, di, ref = _setup_known(tmp_path, "single")
    calls = []
    real = R.refine_offset

    def spy(render, refsig, fs, coarse, start=0, search=None, **kw):
        calls.append((coarse, search, fs))
        return real(render, refsig, fs, coarse, start=start, search=search, **kw)

    monkeypatch.setattr(R, "refine_offset", spy)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0})
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2, plan=plan,
                 write_audio=False, refine_offsets=True)
    res = run_match(cfg, Log())
    assert len(calls) >= 2 and "error" not in res["offsetRefinement"]["final"]
    assert all(c[0] == 0 and c[1] == int(0.020 * c[2]) for c in calls), calls
    # no hint -> wide windows
    calls.clear()
    ref2 = load_reference(Path(ref.path), channel="mid", matched="mono", offset_ms=None)
    run_match(Config(di=di, ref=ref2, pool=pool, out=tmp_path / "out2", seed=1, excerpt_s=2.0, threads=2, plan=plan,
                     write_audio=False, refine_offsets=True), Log())
    assert calls[0][1] == 3 * calls[0][2] and calls[-1][1] == int(0.25 * calls[-1][2]), calls


def test_offset_phat_corrects_a_skewed_envelope():
    """The reference's envelope is tilted (rising gain ramp inside each burst), which biases the envelope correlation by
    a few ms; the waveform (PHAT) step must land on the true offset and with the right sign (both directions)."""
    fs = 48000
    x = _bursts(seed=5)
    for true in (9100, 9700):
        ref = np.zeros(len(x) + 30000)
        ramp = np.exp(np.arange(len(x)) % int(0.35 * fs) / (0.12 * fs))     # gain grows through each 350 ms period
        ref[true:true + len(x)] = x * ramp
        ref += np.random.default_rng(6).standard_normal(len(ref)) * 0.01
        r = offset.refine_offset(x[fs:5 * fs], ref, fs, coarse=9400, start=fs)
        assert r["fineAccepted"] and r["offset"] == true, (true, r)


def test_offset_refinement_coarse_only_when_waveforms_unrelated():
    fs = 48000
    x = _bursts()
    rng = np.random.default_rng(4)
    env_only = np.zeros(len(x) + 30000)
    true = 9300
    env_only[true:true + len(x)] = np.abs(x) * rng.standard_normal(len(x))   # same envelope, unrelated waveform
    r = offset.refine_offset(x[fs:5 * fs], env_only, fs, coarse=9000, start=fs)
    assert abs(r["offset"] - true) < fs * 0.003


# ---- pool ----------------------------------------------------------------------------------------------------------------------
def _fake_cache(tmp_path: Path):
    from sawblade_match.t3k.cache import sha256_file
    root = tmp_path / "captures"
    tones = []
    spec = [(1, "amp", "Amp A", "cc-by", [10, 11], "nam"), (2, "pedal", "Boss HM-2", "t3k", [20], "nam"),
            (3, "pedal", "Ibanez TS808", "cco", [30], "nam"), (4, "cab", "V30 cab", "cc-by-sa", [40], "wav"),
            (5, "amp", "NC amp", "cc-by-nc", [50], "nam")]
    for tid, slot, title, lic, models, ext in spec:
        d = root / str(tid)
        d.mkdir(parents=True)
        meta = {"models": {}, "tone": {"license": lic}}
        for i, m in enumerate(models):
            f = d / f"{m}.{ext}"
            f.write_bytes(b"x" * (100 + i))
            meta["models"][str(m)] = {"file": f.name, "sha256": sha256_file(f), "model": {}}
        (d / "meta.json").write_text(json.dumps(meta))
        tones.append({"tone_id": tid, "title": title, "slot": slot, "gear": slot, "license": lic, "status": "included",
                      "creator": "c", "url": "u", "models": [{"id": m, "name": f"n{m}"} for m in models + [m + 1000 for m in models]]})
    (root / "pool_manifest.json").write_text(json.dumps({"tones": tones}))
    return root / "pool_manifest.json"


def test_load_pool_slots_downloaded_only_and_licenses(tmp_path):
    p = load_pool(_fake_cache(tmp_path))
    assert sorted(c.key for c in p.pedals) == ["2/20", "3/30"]       # no title filter: any pedal is a pedal candidate
    assert {c.key: c.kind for c in p.pedals} == {"2/20": "distortion", "3/30": "drive"}
    assert sorted(c.key for c in p.amps) == ["1/10", "1/11", "5/50"]  # nc amp usable (personal project); undownloaded models skipped
    assert [c.key for c in p.cabs] == ["4/40"]
    assert p.counts()["distortion"] == 1 and p.counts()["drive"] == 1


# ---- profiles (no C++ needed) -----------------------------------------------------------------------------------------------------
def test_swedish_profile_matches_tone_targets_v2():
    from sawblade_match.matcher.profile import load_profile
    prof = load_profile("swedish_death_hm2")
    v2 = json.loads((REPO / "docs" / "tone_targets.json").read_text())
    assert prof["schema"] == "sawblade.profile" and prof["id"] == "swedish_death_hm2"
    assert prof["rules"] == v2["rules"] and prof["analysis"] == v2["analysis"] and prof["metrics"] == v2["metrics"]


def test_derived_profile_loosens_only_failed_rules():
    from sawblade_match.matcher.profile import derive_profile, load_profile
    from sawblade_match.tonecheck.analysis import analyze
    from sawblade_match.tonecheck.rules import evaluate_rules
    base = load_profile("swedish_death_hm2")
    # a spectrally flat noise "reference": fails several HM-2 rules (e.g. no sub roll-off, fizz rule)
    x = np.random.default_rng(0).standard_normal(48000 * 6) * 0.05
    prof, table = derive_profile(base, x, "side", "noise")
    g = analyze(x, 48000, base).groups
    assert any(r["statusOnReference"] != "pass" for r in table)
    from sawblade_match.tonecheck.rules import parse_expr
    changed = 0
    for r, old, new in zip(table, base["rules"], prof["rules"]):
        if r["statusOnReference"] == "pass":
            assert new["expr"] == old["expr"]                  # passes: untouched (loosen-only)
        eo, en = parse_expr(old["expr"]), parse_expr(new["expr"])
        assert (eo.lhs, eo.op, eo.rhs) == (en.lhs, en.op, en.rhs)
        if new["expr"] != old["expr"]:
            changed += 1
            assert (en.offset >= eo.offset) if eo.op == "<=" else (en.offset <= eo.offset), (old["expr"], new["expr"])
    assert changed >= 1
    assert all(r["status"] in ("pass", "marginal") for r in evaluate_rules(g, prof["rules"]))   # reference passes
    assert prof["schema"] == "sawblade.profile" and prof["provenance"]["kind"] == "reference-derived"
    # the label is explicit and independent of the base profile
    other = derive_profile(load_profile("us_death"), x, "side", "noise")[0]
    for q in (prof, other):
        assert q["calibrated"] == "reference-derived" and q["name"] == "derived:noise"
        assert q["provenance"]["rulesOffsets"].startswith("measured") and "inherited priors" in q["provenance"]["metrics"]
    assert prof["notes"] != base["notes"] and prof["provenance"]["baseProfile"] == "swedish_death_hm2"
    assert other["provenance"]["baseProfile"] == "us_death"
    # the original-style signal keeps the v2 rules (no change when everything already passes)
    prof2, t2 = derive_profile(prof, x, "side", "noise")
    assert [r["expr"] for r in prof2["rules"]] == [r["expr"] for r in prof["rules"]]


# ---- presets / emulation (need the C++ core) ----------------------------------------------------------------------------------------
core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher.engine import Engine, to48          # noqa: E402
from sawblade_match.matcher.run import (Config, Log, Plan, choose, pick_output_gain, portable, run_match)  # noqa: E402
from sawblade_match.matcher.reference import Reference            # noqa: E402
from sawblade_match.matcher.screen import Scored                    # noqa: E402


def fixture_pool() -> Pool:
    n = lambda f: FIX / "nam" / f
    i = lambda f: FIX / "ir" / f
    pedals = [cap(n("wavenet.nam"), 1, 1, "distortion", "Dist a"), cap(n("lstm.nam"), 1, 2, "distortion", "Dist b"),
              cap(n("linear_identity_loud24.nam"), 3, 6, "drive", "Drive")]
    amps = [cap(n("lstm.nam"), 2, 3, "amp_high", "Amp lstm"), cap(n("wavenet.nam"), 2, 4, "amp_high", "Amp wavenet"),
            cap(n("linear_identity.nam"), 2, 5, "amp_low", "Amp lin")]
    cabs = [cap(i("ir_a.wav"), 4, 7, "cab", "cab a"), cap(i("ir_b.wav"), 4, 8, "cab", "cab b")]
    return Pool(pedals, amps, cabs)


def mkplan(**kw) -> Plan:
    d = dict(cap_pairs=100, n_rescore=6, n_rescore_single=6, n_cab=2, n_cab_single=2,
             top_k={"blend": 1, "single": 0, "single2": 0}, gens_linear=3, gens_gain=2, gens_final=2,
             pop_linear=8, pop_gain=4)
    d.update(kw)
    return Plan(**d)


def hidden(pool: Pool, topology: str = "blend"):
    p = pool.pedals
    if topology == "blend":
        combo = Combo((p[1],), pool.amps[1], (p[2],), pool.amps[0], pool.cabs[1])
    elif topology == "single2":
        combo = Combo((p[2], p[0]), pool.amps[0], None, None, pool.cabs[1])
    else:
        combo = Combo((p[0],), pool.amps[1], None, None, pool.cabs[1])
    sp = Space.for_combo(combo)
    v = sp.default()
    if topology == "blend":
        v.update({"a.g1": 4.0, "post.g1": 2.0, "blend": 0.4, "levelB": -2.0, "b.g2": -3.0, "gain.a.0": 3.0})
    else:
        v.update({"post.g1": 1.5})          # screening uses default parameters, so keep the hidden one close to them
    return combo, sp, v


def test_preset_emission_is_schema_valid_via_cpp_parser():
    pool = fixture_pool()
    for topo in ("blend", "single", "single2"):
        combo, sp, v = hidden(pool, topo)
        assert combo.topology == topo
        preset = build_preset(combo, v, gate=gate_preset(-50.0), align=manual_align(2, False))
        y, rep = core.render(preset, np.zeros(2048, np.float32), 48000.0)           # strict parse + render
        assert rep["liveCompatible"] is True and len(y) == 2048
        assert preset["cab"]["mode"] == "shared"
        assert all(b["type"] == "nam" for p in preset["paths"].values() for b in p["blocks"])   # nothing non-trainable
        n_nam = sum(len(p["blocks"]) for p in preset["paths"].values())
        assert n_nam == len(combo.nams())
        if topo != "blend":
            assert preset["paths"]["b"]["enabled"] is False and preset["blend"] == 0.0
    combo, sp, v = hidden(pool)
    preset = build_preset(combo, v, gate=gate_preset(-50.0), align=manual_align(2, False))
    src = preset["paths"]["a"]["blocks"][0]["model"]["source"]
    assert src["provider"] == "tone3000" and src["id"] == "1" and src["modelId"] == "2"
    bad = json.loads(json.dumps(preset))
    bad["paths"]["a"]["blocks"][0]["bogus"] = 1
    with pytest.raises(ValueError):
        core.render(bad, np.zeros(2048, np.float32), 48000.0)
    port = portable(preset)
    assert port["cab"]["ir"]["file"] == "captures/4_8.wav" and "sha256" not in port["cab"]["ir"]
    # pedal slots allow "none" on either path
    combo2 = Combo((), combo.a_amp, (), combo.b_amp, combo.cab)
    p2 = build_preset(combo2, Space.for_combo(combo2).default(), gate=None, align=manual_align())
    core.render(p2, np.zeros(2048, np.float32), 48000.0)
    assert len(p2["paths"]["a"]["blocks"]) == 1


@pytest.mark.parametrize("align", [(0, False), (7, False), (-9, True), (5, True)])
def test_emulation_equals_full_render(align):
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    x = to48(x, fs)[:48000 * 2]
    gate = gate_preset(-60.0)
    eng = Engine(gate, 2)
    try:
        assert eng.probe_align(combo, v)["mode"] == "manual"
        align = manual_align(*align)
        ca, cb = eng.core(combo, v, "a", x), eng.core(combo, v, "b", x)
        em = eng.emulate(combo, v, ca, cb, align)
        full, _ = eng.render(build_preset(combo, v, gate=gate, align=align), x)
        assert np.max(np.abs(full - em)) < 1e-5 * max(1.0, np.max(np.abs(full)))
    finally:
        eng.close()


@pytest.mark.parametrize("topo", ["single", "single2"])
def test_emulation_equals_full_render_single_topologies(topo):
    pool = fixture_pool()
    combo, sp, v = hidden(pool, topo)
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    x = to48(x, fs)[:48000 * 2]
    gate = gate_preset(-60.0)
    eng = Engine(gate, 2)
    try:
        ca = eng.core(combo, v, "a", x)
        em = eng.emulate(combo, v, ca, None, manual_align())
        full, _ = eng.render(build_preset(combo, v, gate=gate, align=manual_align()), x)
        assert np.max(np.abs(full - em)) < 1e-5 * max(1.0, np.max(np.abs(full)))
    finally:
        eng.close()


def test_pick_output_gain_and_choose():
    g, clipped = pick_output_gain(0.1, offset_db=-10.0, level_offset_db=3.0)
    assert g == pytest.approx(13.0) and not clipped
    assert pick_output_gain(0.5, -20.0, 0.0)[1]            # +20 dB on a 0.5 peak clips
    pool = fixture_pool()
    from dataclasses import replace
    p, a = pool.pedals, pool.amps
    big = Combo((p[0],), a[0], (), a[1], pool.cabs[0])
    small = Combo((replace(p[1], size_label="lite"),), replace(a[2], size_label="lite"), (),
                  replace(a[2], size_label="lite"), pool.cabs[0])
    twin = Combo((p[0],), a[0], (), a[1], pool.cabs[1])    # same size category as ``big``
    mk = lambda c, l, clipped=False: Scored(c, l, 0.5, manual_align(), None, "refined", {"clipped": clipped})
    assert choose([mk(big, 1.0), mk(small, 1.04)]).combo is small         # within 0.05 dB -> lighter category
    assert choose([mk(big, 1.0), mk(small, 1.2)]).combo is big            # outside the tolerance -> lower loss
    assert choose([mk(big, 1.03), mk(twin, 1.0)]).combo is twin           # same category -> lower loss
    assert choose([mk(big, 1.0, clipped=True), mk(small, 1.4)]).combo is small   # clipping rejected
    assert choose([mk(big, float("nan")), mk(small, 2.0)]).combo is small          # non-finite loss dropped
    assert choose([mk(small, float("inf")), mk(big, 3.0)]).combo is big


def test_choose_prefers_simplest_topology_within_occam_tolerance():
    pool = fixture_pool()
    p, a = pool.pedals, pool.amps
    blend = Combo((p[0],), a[0], (), a[1], pool.cabs[0])
    single = Combo((p[0],), a[0], None, None, pool.cabs[0])
    single2 = Combo((p[0], p[1]), a[0], None, None, pool.cabs[0])
    mk = lambda c, l: Scored(c, l, 0.5, manual_align(), None, "refined", {})
    assert choose([mk(blend, 1.0), mk(single, 1.09)]).combo is single           # 0.09 dB worse -> simpler wins
    assert choose([mk(blend, 1.0), mk(single, 1.11)]).combo is blend            # beyond 0.1 dB
    assert choose([mk(blend, 1.0), mk(single2, 1.05), mk(single, 1.08)]).combo is single
    assert choose([mk(blend, 1.0), mk(single2, 1.05)]).combo is single2
    assert [c for c in (blend, single, single2)].__len__() == 3
    assert (blend.topology, single.topology, single2.topology) == ("blend", "single", "single2")


def test_gate_floor_uses_peak_envelope():
    from sawblade_match.matcher.run import gate_envelope_floor_db
    fs = 48000
    rng = np.random.default_rng(0)
    x = rng.standard_normal(fs * 4) * 0.01            # noise: RMS -40 dBFS
    x[fs:2 * fs] += rng.standard_normal(fs) * 0.3     # playing
    f = gate_envelope_floor_db(x, fs)
    assert -40 < f < -20                                # peak envelope of noise sits well above its RMS
    assert gate_envelope_floor_db(x, fs) == f


def test_size_rank_orders_categories():
    pool = fixture_pool()
    from dataclasses import replace
    a = pool.amps[0]
    assert replace(a, size_label="feather").size_rank < replace(a, size_label="lite").size_rank \
        < replace(a, size_label="standard").size_rank < replace(a, size_label="xstandard").size_rank


def _setup_known(tmp_path, topo):
    from sawblade_match.matcher.run import gate_envelope_floor_db
    from sawblade_match.matcher.reference import load_reference
    pool = fixture_pool()
    combo, sp, v = hidden(pool, topo)
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    di = tmp_path / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    gate = gate_preset(gate_envelope_floor_db(to48(x, fs), 48000))
    hid, _ = core.render(build_preset(combo, v, gate=gate, align=Engine(gate).probe_align(combo, v)), x, fs)
    refwav = tmp_path / "hidden.wav"
    sf.write(str(refwav), hid, fs, subtype="FLOAT")
    ref = load_reference(refwav, channel="mid", matched="mono", offset_ms=0.0)
    return pool, combo, di, ref


def test_known_answer_recovery_fixtures(tmp_path):
    """Hidden preset from fixture captures -> reference = its render of the DI; the matcher must come within 0.5 dB
    A-weighted of the hidden preset (whose own error against the reference is 0)."""
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    plan = mkplan(top_k={"blend": 1, "single": 0, "single2": 0}, gens_linear=40, gens_gain=8, gens_final=25,
                  pop_linear=16, pop_gain=8)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=7, excerpt_s=2.0, threads=2, plan=plan,
                 write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    after = res["after"][0]["aWeightedErrorDb"]
    before = res["before"][0]["aWeightedErrorDb"]
    assert after < 0.5, (before, after)
    assert after <= before
    out = tmp_path / "out"
    for name in ("best.preset.resolved.json", "best.preset.json", "result.json", "tonecheck/best_L/report.json",
                 "profile.derived.json"):
        assert (out / name).exists(), name
    best = json.loads((out / "best.preset.resolved.json").read_text())
    for pth in best["paths"].values():
        for blk in pth["blocks"]:
            assert blk["model"]["source"]["provider"] == "tone3000" and blk["model"]["source"]["modelId"]
    assert best["cab"]["mode"] == "shared"
    core.render(best, np.zeros(2048, np.float32), 48000.0)
    # size category recorded for every candidate and capture
    caps = res["best"]["captures"]
    assert all(c is None or c["sizeCategory"] == "standard" for c in caps.values())
    assert all("sizeRank" in c and "topology" in c for c in res["candidatesStage2"])
    assert res["best"]["topology"] in ("single", "single2", "blend")
    assert res["profile"]["kind"] == "derived"


def test_single_path_known_answer_is_found_as_single(tmp_path):
    """A tone that is a single path [pedal] -> amp must be found as a single path (Occam), with all topologies reported."""
    pool, combo, di, ref = _setup_known(tmp_path, "single")
    plan = mkplan(top_k={"blend": 1, "single": 2, "single2": 1}, gens_linear=30, gens_gain=6, gens_final=20,
                  pop_linear=16, pop_gain=8, n2_pedals=3, n2_amps=3, n_rescore_single=12, n_cab_single=12)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=5, excerpt_s=2.0, threads=2, plan=plan,
                 write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    assert set(res["topologies"]) == {"single", "single2", "blend"}
    assert res["best"]["topology"] == "single"
    assert res["after"][0]["aWeightedErrorDb"] < 0.5
    best = json.loads((tmp_path / "out" / "best.preset.resolved.json").read_text())
    assert best["paths"]["b"]["enabled"] is False


def test_prescreen_keeps_top_per_class_and_is_deterministic():
    from sawblade_match.matcher.prescreen import auto_n
    assert auto_n(4, 2, 800) == 9 and auto_n(1, 1, 10) >= 1
    n = auto_n(4, 2, 800)
    assert (4 * n + 1) * (2 * n) <= 800 < (4 * (n + 1) + 1) * (2 * (n + 1))


def test_prescreen_runs_and_respects_class_quota(tmp_path):
    from sawblade_match.matcher.prescreen import prescreen
    from sawblade_match.matcher.reference import build_target, load_reference, make_excerpt
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    x, fs = sf.read(di, dtype="float32")
    ex = make_excerpt(to48(x, fs), 2.0)
    tgt = build_target(ref, ex)
    eng = Engine(None, 2)
    try:
        kp, ka, info = prescreen(eng, pool, ex, tgt, pool.cabs[0], 1, lambda *_: None)
        kp2, ka2, _ = prescreen(eng, pool, ex, tgt, pool.cabs[0], 1, lambda *_: None)
    finally:
        eng.close()
    assert [c.key for c in kp] == [c.key for c in kp2] and [c.key for c in ka] == [c.key for c in ka2]
    assert len(kp) == 2 and len(ka) == 2                  # one per class: drive, distortion | amp_low, amp_high
    assert {c.kind for c in kp} == {"drive", "distortion"} and {c.kind for c in ka} == {"amp_low", "amp_high"}
    assert info["nPerClass"] == 1 and len(info["proxyAmps"]) == 2


def test_forced_prescreen_trims_the_pair_search(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2,
                 plan=mkplan(prescreen_n=1), write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    st = res["stage1"]
    assert st["fullPairs"] == 12 and st["pairsRendered"] == 6 and st["prescreen"]["appliedToBlendSingle"]


def test_determinism_with_seed(tmp_path):
    pool = fixture_pool()
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    di = tmp_path / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    from sawblade_match.matcher.reference import load_reference
    refwav = tmp_path / "ref.wav"
    sf.write(str(refwav), x * 0.5, fs, subtype="FLOAT")

    def go(tag, seed, **plan_kw):
        ref = load_reference(refwav, channel="mid")
        cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / tag, seed=seed, excerpt_s=2.0, threads=2,
                     plan=mkplan(**plan_kw), write_audio=False)
        return run_match(cfg, Log())

    # subsampled pair search: pre-screen leaves 6 pairs, cap 4
    a, b, c = go("a", 3, cap_pairs=4), go("b", 3, cap_pairs=4), go("c", 4, cap_pairs=4)
    assert a["best"]["params"] == b["best"]["params"] and a["best"]["loss"] == b["best"]["loss"]
    assert a["best"]["captures"] == b["best"]["captures"]
    assert a["seed"] == 3 and "seed=3" in a["randomness"]
    assert a["best"]["breakdown"]["stft"] is None and a["starter"]["excerptLoss"]["stft"] is None   # unmatched
    assert c["seed"] == 4
    assert a["stage1"]["pairsRendered"] == 4
    assert a["stage1"]["sampledPairs"] == b["stage1"]["sampledPairs"]
    assert a["stage1"]["sampledPairs"] != c["stage1"]["sampledPairs"]           # subset sampling follows the seed
    assert a["best"]["params"] != c["best"]["params"]
    # no subsampling at all: only the CMA-ES streams differ between seeds (kills a "CMA seed ignored" mutant)
    d, e = go("d", 3), go("e", 4)
    assert d["stage1"]["pairsRendered"] == d["stage1"]["fullPairs"] == 12
    assert d["best"]["captures"] == e["best"]["captures"]             # same discrete choice, deterministic screening
    assert d["best"]["params"] != e["best"]["params"]


def test_listening_stereo_with_di_r(tmp_path):
    """--di-r path: L and R renders are hard-panned into one stereo float file; no reference -> no loudness matching and
    no peak normalisation (gain 0 dB)."""
    from sawblade_match.matcher.run import _listening
    fs = 44100
    rng = np.random.default_rng(0)
    yl = (rng.standard_normal(fs) * 0.1).astype(np.float32)
    yr = (rng.standard_normal(fs - 7) * 0.05).astype(np.float32)        # R can be a little shorter
    cfg = Config(di=tmp_path / "x", ref=None, pool=None, out=tmp_path)
    info = _listening(tmp_path, {"best_L": (yl, fs, {}), "best_R": (yr, fs, {})}, cfg, lambda *_: None)
    st, rate = sf.read(info["wav"], dtype="float32")
    assert rate == fs and st.shape == (fs - 7, 2) and "cover_guitars_L-R" in info["wav"]
    assert info["loudnessMatched"] is False and info["fullLengthGainDb"] == 0.0
    assert np.array_equal(st[:, 0], yl[:fs - 7]) and np.array_equal(st[:, 1], yr)
    assert np.corrcoef(st[:, 0], st[:, 1])[0, 1] < 0.2                  # not a mono copy
    assert Path(info.get("mp3", info["wav"])).exists()
    # without R: mono-in-both
    info2 = _listening(tmp_path, {"best_L": (yl, fs, {})}, cfg, lambda *_: None)
    st2, _ = sf.read(info2["wav"], dtype="float32")
    assert np.array_equal(st2[:, 0], st2[:, 1])


def test_di_r_end_to_end_writes_stereo(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "single")
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2, plan=mkplan(top_k={"blend": 0, "single": 1, "single2": 0}),
                 di_r=di, write_audio=True, refine_offsets=False)
    res = run_match(cfg, Log())
    st, _ = sf.read(res["listening"]["wav"], dtype="float32")
    assert st.ndim == 2 and st.shape[1] == 2
    assert set(res["best"]["fullLengthPeakDbfs"]) == {"best_L", "best_R"}


def test_profiles_are_flagged_and_loadable():
    from sawblade_match.matcher.profile import load_profile
    cal = {"swedish_death_hm2": True}
    for pid in ("swedish_death_hm2", "chainsaw_grind", "chainsaw_hardcore", "chainsaw_crust", "us_death",
                "cavernous_death", "melodic_death"):
        p = load_profile(pid)
        assert p["schema"] == "sawblade.profile" and p["id"] == pid and p["calibrated"] is cal.get(pid, False)
        assert len(p["rules"]) == 10
        if p["provenance"]["kind"] == "hypothesis":
            assert p["provenance"]["calibration"] is None
    base, g = load_profile("swedish_death_hm2"), load_profile("chainsaw_grind")
    assert [r["expr"] for r in g["rules"]] == [r["expr"] for r in base["rules"]]
    assert all(a["toleranceDb"] > b["toleranceDb"] for a, b in zip(g["rules"], base["rules"]))   # loosened only


def test_class_override_in_manifest(tmp_path):
    mp = _fake_cache(tmp_path)
    m = json.loads(mp.read_text())
    m["tones"][1]["classOverride"] = "fuzz"                 # tone 2 (HM-2 title) is declared a fuzz
    mp.write_text(json.dumps(m))
    p = load_pool(mp)
    assert {c.key: c.kind for c in p.pedals} == {"2/20": "fuzz", "3/30": "drive"}
    m["tones"][1]["classOverride"] = "amp_low"
    mp.write_text(json.dumps(m))
    with pytest.raises(ValueError, match="classOverride"):
        load_pool(mp)


def test_generic_starter_needs_only_one_amp_and_one_cab(tmp_path):
    from sawblade_match.matcher.pool import starter_choice
    from sawblade_match.matcher.run import starter_preset
    full = fixture_pool()
    assert starter_choice(full)["amp"].key == full.amps[0].key and starter_choice(full)["cab"].key == full.cabs[0].key
    with pytest.raises(ValueError):
        starter_choice(Pool([], [], full.cabs))
    pool = Pool([], full.amps[:1], full.cabs[:1])           # no pedals at all
    p, caps = starter_preset(pool, gate_preset(-60.0))
    assert caps["kind"] == "generic starter baseline"
    assert p["paths"]["a"]["eq"] == [] and p["postEq"] == [] and len(p["paths"]["a"]["blocks"]) == 1
    core.render(p, np.zeros(2048, np.float32), 48000.0)
    # the whole run works on such a pool
    from sawblade_match.matcher.reference import load_reference
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    di = tmp_path / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    refwav = tmp_path / "ref.wav"
    sf.write(str(refwav), x * 0.4, fs, subtype="FLOAT")
    cfg = Config(di=di, ref=load_reference(refwav, channel="mid"), pool=pool, out=tmp_path / "out", seed=1,
                 excerpt_s=2.0, threads=2, write_audio=False,
                 plan=mkplan(top_k={"blend": 0, "single": 1, "single2": 0}, gens_linear=2, gens_gain=1, gens_final=1))
    res = run_match(cfg, Log())
    assert res["starter"]["label"] == "generic starter baseline" and res["best"]["topology"] == "single"


def test_gate_preset_passes_phase35_fields_through():
    base = gate_preset(-50.0)
    assert "mode" not in base  # default dict is unchanged: the core defaults preserve v1 behaviour
    g = gate_preset(-50.0, {"mode": "expander", "ratio": 4.0, "keyHighPassHz": 120.0, "releaseCurve": "linear-db"})
    assert g["mode"] == "expander" and g["ratio"] == 4.0 and g["keyHighPassHz"] == 120.0
    assert g["releaseCurve"] == "linear-db" and g["releaseMs"] == base["releaseMs"]


def test_gate_preset_clamps_digital_silence_floor():
    assert gate_preset(-200.0)["thresholdDb"] == -86.0          # inside the schema range [-120, 0]
    assert gate_preset(-90.0)["thresholdDb"] == -86.0
    assert gate_preset(-37.1)["thresholdDb"] == pytest.approx(-33.1)   # normal floors are unchanged


def test_stage2_first_linear_block_is_ltas_only(tmp_path, monkeypatch):
    """Staged objective: refine's first linear block (L1) is scored without the feel term; the start score, the gain
    block, the final linear block and the final score see the full target."""
    from sawblade_match.matcher import refine as R

    seen: list[bool] = []                        # per refine-side loss evaluation: does the target carry feel?

    class Spy:
        def __getattr__(self, name):
            return getattr(L, name)

        def evaluate(self, out, tgt, eq=None):
            seen.append(tgt.feel is not None)
            return L.evaluate(out, tgt, eq)

    monkeypatch.setattr(R, "L", Spy())
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    plan = mkplan(top_k={"blend": 1, "single": 0, "single2": 0}, gens_linear=3, gens_gain=2, gens_final=2,
                  pop_linear=8, pop_gain=4)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=7, excerpt_s=2.0, threads=2, plan=plan,
                 write_audio=False, refine_offsets=False)
    res = run_match(cfg, Log())
    n_l1 = 1 + plan.gens_linear * plan.pop_linear          # CMA-ES: initial point + gens x population
    assert seen[0] is True                                  # start score: full target
    assert seen[1:1 + n_l1] == [False] * n_l1               # L1: LTAS-only
    assert len(seen) > 1 + n_l1 and all(seen[1 + n_l1:])    # gain block, L2 and the final score: feel present
    assert res["best"]["breakdown"]["feelTerms"] is not None
