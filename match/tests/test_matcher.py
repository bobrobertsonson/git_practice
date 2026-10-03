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
    gear = {"hm2": "pedal", "boost": "pedal", "amp": "amp", "cab": "cab"}[kind]
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
    for has_boost in (True, False):
        sp = Space(has_boost)
        assert len(sp.indices("gain")) == (4 if has_boost else 3)
        d = sp.default()
        u = sp.encode(d)
        back = sp.decode(u)
        for k in d:
            assert back[k] == pytest.approx(d[k], rel=1e-9, abs=1e-9), k
        lo, hi = sp.decode(np.zeros(len(sp))), sp.decode(np.ones(len(sp)))
        for p in sp.params:
            assert lo[p.name] == pytest.approx(p.lo) and hi[p.name] == pytest.approx(p.hi)
        assert sp.decode(np.full(len(sp), 7.0))["blend"] == pytest.approx(0.95)   # clipped into the box
    sp = Space(True)
    assert 0.0 < sp.encode({"a.f0": 200.0})[sp.idx["a.f0"]] < 1.0
    assert sp.decode(sp.encode({"gain.a.amp": -12.0}))["gain.a.amp"] == pytest.approx(-12.0)
    assert len(sp.eq_gains(sp.default())) == 9


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
    assert [c.key for c in p.hm2] == ["2/20"]
    assert [c.key for c in p.boost] == ["3/30"]
    assert sorted(c.key for c in p.amps) == ["1/10", "1/11"]       # nc amp excluded, undownloaded models skipped
    assert [c.key for c in p.cabs] == ["4/40"]


# ---- presets / emulation (need the C++ core) ----------------------------------------------------------------------------------------
core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from sawblade_match.matcher.engine import Engine, to48          # noqa: E402
from sawblade_match.matcher.run import (Config, Log, Plan, choose, pick_output_gain, portable, run_match)  # noqa: E402
from sawblade_match.matcher.reference import Reference            # noqa: E402
from sawblade_match.matcher.screen import Scored                    # noqa: E402


def fixture_pool() -> Pool:
    n = lambda f: FIX / "nam" / f
    i = lambda f: FIX / "ir" / f
    hm2 = [cap(n("wavenet.nam"), 1, 1, "hm2", "HM-2 a"), cap(n("lstm.nam"), 1, 2, "hm2", "HM-2 b")]
    amps = [cap(n("lstm.nam"), 2, 3, "amp", "Amp lstm"), cap(n("wavenet.nam"), 2, 4, "amp", "Amp wavenet"),
            cap(n("linear_identity.nam"), 2, 5, "amp", "Amp lin")]
    boost = [cap(n("linear_identity_loud24.nam"), 3, 6, "boost", "TS")]
    cabs = [cap(i("ir_a.wav"), 4, 7, "cab", "cab a"), cap(i("ir_b.wav"), 4, 8, "cab", "cab b")]
    return Pool(hm2, boost, amps, cabs)


def hidden(pool: Pool):
    combo = Combo(pool.hm2[1], pool.amps[1], pool.boost[0], pool.amps[0], pool.cabs[1])
    sp = Space(True)
    v = sp.default()
    v.update({"blend": 0.4, "levelB": -2.0, "a.g1": 4.0, "b.g2": -3.0, "post.g1": 2.0, "gain.a.pedal": 3.0})
    return combo, sp, v


def test_preset_emission_is_schema_valid_via_cpp_parser():
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    preset = build_preset(combo, v, gate=gate_preset(-50.0), align=manual_align(2, False))
    y, rep = core.render(preset, np.zeros(2048, np.float32), 48000.0)           # strict parse + render
    assert rep["liveCompatible"] is True and len(y) == 2048
    assert preset["cab"]["mode"] == "shared"
    src = preset["paths"]["a"]["blocks"][0]["model"]["source"]
    assert src["provider"] == "tone3000" and src["id"] == "1" and src["modelId"] == "2"
    assert all(b["type"] == "nam" for p in preset["paths"].values() for b in p["blocks"])   # nothing non-trainable
    bad = json.loads(json.dumps(preset))
    bad["paths"]["a"]["blocks"][0]["bogus"] = 1
    with pytest.raises(ValueError):
        core.render(bad, np.zeros(2048, np.float32), 48000.0)
    port = portable(preset)
    assert port["cab"]["ir"]["file"] == "captures/4_8.wav" and "sha256" not in port["cab"]["ir"]
    # no boost variant also valid
    combo2 = Combo(combo.hm2, combo.saw_amp, None, combo.body_amp, combo.cab)
    p2 = build_preset(combo2, Space(False).default(), gate=None, align=manual_align())
    core.render(p2, np.zeros(2048, np.float32), 48000.0)


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


def test_pick_output_gain_and_choose():
    g, clipped = pick_output_gain(0.1, offset_db=-10.0, level_offset_db=3.0)
    assert g == pytest.approx(13.0) and not clipped
    assert pick_output_gain(0.5, -20.0, 0.0)[1]            # +20 dB on a 0.5 peak clips
    pool = fixture_pool()
    from dataclasses import replace
    big = Combo(pool.hm2[0], pool.amps[0], None, pool.amps[1], pool.cabs[0])
    small = Combo(replace(pool.hm2[1], size_label="lite"), replace(pool.amps[2], size_label="lite"), None,
                  replace(pool.amps[2], size_label="lite"), pool.cabs[0])
    twin = Combo(pool.hm2[0], pool.amps[0], None, pool.amps[1], pool.cabs[1])    # same size category as ``big``
    mk = lambda c, l, clipped=False: Scored(c, l, 0.5, manual_align(), None, "refined", {"clipped": clipped})
    assert choose([mk(big, 1.0), mk(small, 1.04)]).combo is small         # within 0.05 dB -> lighter category
    assert choose([mk(big, 1.0), mk(small, 1.2)]).combo is big            # outside the tolerance -> lower loss
    assert choose([mk(big, 1.03), mk(twin, 1.0)]).combo is twin           # same category -> lower loss
    assert choose([mk(big, 1.0, clipped=True), mk(small, 1.4)]).combo is small   # clipping rejected
    assert choose([mk(big, float("nan")), mk(small, 2.0)]).combo is small          # non-finite loss dropped
    assert choose([mk(small, float("inf")), mk(big, 3.0)]).combo is big


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


def test_known_answer_recovery_fixtures(tmp_path):
    """Hidden preset from fixture captures -> reference = its render of the DI; the matcher must come within 0.5 dB
    A-weighted of the hidden preset (whose own error against the reference is 0)."""
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    di = tmp_path / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    floor_gate = gate_preset(-60.0)
    from sawblade_match.matcher.run import gate_envelope_floor_db
    gate = gate_preset(gate_envelope_floor_db(to48(x, fs), 48000))
    hid, _ = core.render(build_preset(combo, v, gate=gate, align=Engine(gate).probe_align(combo, v)), x, fs)
    refwav = tmp_path / "hidden.wav"
    sf.write(str(refwav), hid, fs, subtype="FLOAT")
    from sawblade_match.matcher.reference import load_reference
    ref = load_reference(refwav, channel="mid", matched="mono", offset_ms=0.0)
    plan = Plan(cap_a=6, cap_b=6, n_rescore=6, n_cab=2, top_k=1, gens_linear=40, gens_gain=8, gens_final=25,
                pop_linear=16, pop_gain=8)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=7, excerpt_s=2.0, threads=2, plan=plan,
                 write_audio=False, targets=REPO / "docs" / "tone_targets.json", refine_offsets=False)
    res = run_match(cfg, Log())
    after = res["after"][0]["aWeightedErrorDb"]
    before = res["before"][0]["aWeightedErrorDb"]
    assert after < 0.5, (before, after)
    assert after <= before
    # outputs
    out = tmp_path / "out"
    for name in ("best.preset.resolved.json", "best.preset.json", "result.json", "tonecheck/best_L/report.json"):
        assert (out / name).exists(), name
    best = json.loads((out / "best.preset.resolved.json").read_text())
    for blk in best["paths"]["a"]["blocks"] + best["paths"]["b"]["blocks"]:
        assert blk["model"]["source"]["provider"] == "tone3000" and blk["model"]["source"]["modelId"]
    assert best["cab"]["mode"] == "shared"
    core.render(best, np.zeros(2048, np.float32), 48000.0)


def test_determinism_with_seed(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    x, fs = sf.read(FIX / "di_riff.wav", dtype="float32")
    di = tmp_path / "di.wav"
    sf.write(str(di), x, fs, subtype="FLOAT")
    from sawblade_match.matcher.reference import load_reference
    ref_x = x * 0.5
    refwav = tmp_path / "ref.wav"
    sf.write(str(refwav), ref_x, fs, subtype="FLOAT")
    plan = Plan(cap_a=4, cap_b=4, n_rescore=3, n_cab=1, top_k=1, gens_linear=3, gens_gain=2, gens_final=2,
                pop_linear=8, pop_gain=4)
    outs = []
    for k, seed in enumerate((3, 3, 4)):
        ref = load_reference(refwav, channel="mid")
        cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / f"o{k}", seed=seed, excerpt_s=2.0, threads=2, plan=plan,
                     write_audio=False, targets=REPO / "docs" / "tone_targets.json")
        outs.append(run_match(cfg, Log()))
    a, b, c = outs
    assert a["best"]["params"] == b["best"]["params"]
    assert a["best"]["loss"] == b["best"]["loss"]
    assert a["best"]["captures"] == b["best"]["captures"]
    assert a["seed"] == 3 and "seed=3" in a["randomness"]
    assert a["best"]["breakdown"]["stft"] is None and a["starter"]["excerptLoss"]["stft"] is None   # unmatched
    # a different seed changes something observable (subset sampling in stage 1 and the CMA-ES streams)
    assert c["seed"] == 4
    assert (a["stage1"]["sampledA"], a["stage1"]["sampledB"]) != (c["stage1"]["sampledA"], c["stage1"]["sampledB"])
    assert a["stage1"]["sampledA"] == b["stage1"]["sampledA"]
    assert (a["stage1"]["screenTop"] != c["stage1"]["screenTop"] or a["best"]["params"] != c["best"]["params"]
            or a["best"]["loss"] != c["best"]["loss"])
    assert a["best"]["params"] != c["best"]["params"]
