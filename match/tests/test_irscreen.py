"""Analytic IR screen (v0.4M Task B3): agreement with full renders, ranking, speed, prefilter."""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

pytest.importorskip("sawblade_core")
from sawblade_match.matcher import feel as F, irlib, irscreen, loss as L      # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FIX = REPO / "tests" / "fixtures" / "ir"
FS = 48000


def preset_with_cab(path: str) -> dict:
    return {"schema": "sawblade.preset", "version": 1, "name": "t", "gate": {"enabled": False},
            "paths": {"a": {"role": "saw", "blocks": []}, "b": {"role": "body", "enabled": False, "blocks": []}},
            "align": {"mode": "off"}, "blend": 0.0, "cab": {"mode": "shared", "ir": {"file": path}, "enabled": True},
            "postEq": [], "output": {"gainDb": 0.0}}


def pre_cab_signal(seconds=6.0, seed=0) -> np.ndarray:
    """Distorted-guitar-like excitation: bursts of band-limited noise with a rolled-off spectrum."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    x = rng.standard_normal(n)
    x = signal.sosfilt(signal.butter(2, [90, 9000], btype="band", fs=FS, output="sos"), x)
    env = np.ones(n)
    for t in np.arange(0.0, seconds, 0.4):
        i = int(t * FS)
        env[i + int(0.3 * FS): i + int(0.4 * FS)] = 0.02
    return (x * env * 0.1).astype(np.float32)


def random_iss(n: int, seed: int) -> list[np.ndarray]:
    """Random 2nd-order-filtered IRs (low/high-pass or band-pass resonances of random frequency and width)."""
    rng = np.random.default_rng(seed)
    out = []
    imp = np.zeros(2048)
    imp[8] = 1.0
    for _ in range(n):
        kind = rng.integers(3)
        if kind == 0:
            sos = signal.butter(1, [20.0, rng.uniform(1500, 9000)], btype="band", fs=FS, output="sos")      # ~low-pass
        elif kind == 1:
            sos = signal.butter(1, [rng.uniform(60, 600), 20000.0], btype="band", fs=FS, output="sos")      # ~high-pass
        else:
            c = rng.uniform(600, 6000)
            sos = signal.butter(1, [c / rng.uniform(1.3, 2.5), c * rng.uniform(1.3, 2.5)], btype="band", fs=FS, output="sos")
        out.append(signal.sosfilt(sos, imp).astype(np.float32))
    return out


def make_target(x: np.ndarray, ref: np.ndarray, fizz: bool = False) -> L.Target:
    starts = L.segment_starts(len(x), None)
    ft = None
    if fizz:
        ft = F.make_target(x, np.arange(0.2, len(x) / FS - 0.5, 0.4), None, None, ref_sig=ref)
    return L.Target(starts, L.features(ref, starts, None), None, None, None, feel=ft)


def write_irs(tmp: Path, irs) -> list[str]:
    paths = []
    for i, ir in enumerate(irs):
        p = tmp / f"ir{i:04d}.wav"
        sf.write(str(p), ir, FS, subtype="FLOAT")
        paths.append(str(p))
    return paths


def full_ltas(path: str, x: np.ndarray, tgt: L.Target) -> float:
    from sawblade_match.core import render
    y, _ = render(preset_with_cab(path), x, float(FS))
    return L.evaluate(np.asarray(y, np.float64), tgt).ltas


def bank_of(paths: list[str]) -> irscreen.Bank:
    h = np.stack([irlib.response_from_ir(irlib._render_ir(p)) for p in paths])
    return irscreen.Bank.from_arrays([None] * len(paths), h)


def test_ltas_batch_equals_the_scalar_loss_function():
    rng = np.random.default_rng(0)
    ref = rng.normal(-40, 5, len(L.BAND_CENTRES))
    pred = rng.normal(-40, 5, (7, len(L.BAND_CENTRES)))
    for hf in (None, 4500.0):
        e, off = irscreen.ltas_batch(pred, ref, hf)
        for i in range(7):
            s, so = L.ltas_error(pred[i], ref, hf)
            assert e[i] == pytest.approx(s) and off[i] == pytest.approx(so)


def test_analytic_ltas_matches_the_full_render_within_0_3_db(tmp_path):
    x = pre_cab_signal()
    ref = np.asarray(__import__("sawblade_match.core", fromlist=["render"]).render(
        preset_with_cab(str(FIX / "ir_b.wav")), x * 0.5, float(FS))[0], np.float64)
    tgt = make_target(x, ref)
    irs = [str(FIX / "ir_a.wav"), str(FIX / "ir_b.wav")] + write_irs(tmp_path, random_iss(6, 5))
    bank = bank_of(irs)
    s = irscreen.screen(bank, x, tgt)
    for i, p in enumerate(irs):
        assert abs(s.ltas[i] - full_ltas(p, x, tgt)) < 0.3, (p, s.ltas[i])


def test_rank_correlation_over_200_random_second_order_irs(tmp_path):
    x = pre_cab_signal(seed=1)
    ref = signal.sosfilt(signal.butter(2, [100, 5500], btype="band", fs=FS, output="sos"), x.astype(np.float64))
    tgt = make_target(x, ref)
    paths = write_irs(tmp_path, random_iss(200, 7))
    bank = bank_of(paths)
    s = irscreen.screen(bank, x, tgt)
    full = np.array([full_ltas(p, x, tgt) for p in paths])
    assert irscreen.rank_correlation(s.ltas, full) >= 0.9
    assert np.abs(s.ltas - full).max() < 0.3
    assert set(s.order()[:3]) & set(np.argsort(full)[:6])        # the analytic favourites are really good ones


def test_fizz_prediction_tracks_the_feel_fizz_term(tmp_path):
    x = pre_cab_signal(seed=2)
    ref = signal.sosfilt(signal.butter(2, [20, 4500], btype="band", fs=FS, output="sos"), x.astype(np.float64))
    tgt = make_target(x, ref, fizz=True)
    paths = write_irs(tmp_path, random_iss(40, 11))
    bank = bank_of(paths)
    s = irscreen.screen(bank, x, tgt)
    assert s.fizz_on and s.fizz.max() > 0
    from sawblade_match.core import render
    actual = []
    for p in paths:
        y, _ = render(preset_with_cab(p), x, float(FS))
        t = F.evaluate(np.asarray(y, np.float64), tgt.feel)[1]
        actual.append((t["fizzHfRatioSoft"] + t["fizzHfFlatSoft"]) * F.W_FIZZ * tgt.feel.scale)
    assert irscreen.rank_correlation(s.fizz, np.array(actual)) >= 0.85


def synth_bank(n: int, seed: int = 0) -> irscreen.Bank:
    """n synthetic |H| (analytic biquad responses, no files and no FFTs) with tags."""
    rng = np.random.default_rng(seed)
    f = np.fft.rfftfreq(irlib.NFFT, 1 / FS)
    w = f / FS * 2 * np.pi
    z = np.exp(-1j * w)
    h = np.empty((n, irlib.H_BINS), np.float32)
    for i in range(n):
        fc, q = rng.uniform(800, 9000), rng.uniform(0.5, 3.0)
        w0 = 2 * np.pi * fc / FS
        a = np.sin(w0) / (2 * q)
        b = np.array([(1 - np.cos(w0)) / 2, 1 - np.cos(w0), (1 - np.cos(w0)) / 2])
        d = np.array([1 + a, -2 * np.cos(w0), 1 - a])
        H = (b[0] + b[1] * z + b[2] * z ** 2) / (d[0] + d[1] * z + d[2] * z ** 2)
        h[i] = np.abs(H) * rng.uniform(0.5, 2.0) ** 0
    cycle = ["v30", "greenback", "g12t75", "1960", "mesa"]
    caps = [type("C", (), {"key": f"local/{i:05d}", "title": f"ir{i}", "tags": (cycle[i % 5],) if i % 3 == 0 else (),
                           "provider": "local"})() for i in range(n)]
    return irscreen.Bank.from_arrays(caps, h)


def test_2000_synthetic_irs_screen_in_under_60_seconds():
    x = pre_cab_signal(seed=3)
    ref = signal.sosfilt(signal.butter(2, [100, 5500], btype="band", fs=FS, output="sos"), x.astype(np.float64))
    tgt = make_target(x, ref, fizz=True)
    bank = synth_bank(2000)
    t0 = time.time()
    rows, info = irscreen.prefilter(bank, irscreen.SCREEN_MAX, set())
    s = irscreen.screen(bank, x, tgt)
    dt = time.time() - t0
    assert info["prefiltered"] is False and len(rows) == 2000 and s.fizz_on and len(s.score) == 2000
    assert np.all(np.isfinite(s.score)) and dt < 60.0
    print(f"2000 IRs screened in {dt:.2f} s")


def test_prefilter_is_deterministic_tag_first_and_proportional():
    bank = synth_bank(900, seed=1)
    a, ia = irscreen.prefilter(bank, 300, {"v30"}, seed=4)
    b, ib = irscreen.prefilter(bank, 300, {"v30"}, seed=4)
    assert np.array_equal(a, b) and ia == ib and len(a) == 300 and len(set(a)) == 300
    assert ia["prefiltered"] and ia["method"] == "tags+kmeans32" and ia["before"] == 900 and ia["after"] == 300
    tagged = [i for i, t in enumerate(bank.tags) if "v30" in t]
    assert ia["tagKept"] == len(tagged) and set(tagged) <= set(a.tolist())      # every v30 IR is kept first
    c, ic = irscreen.prefilter(bank, 300, set(), seed=4)
    assert ic["method"] == "kmeans32" and len(c) == 300 and ic["tagKept"] == 0
    d, _ = irscreen.prefilter(bank, 2000, {"v30"})
    assert len(d) == 900                                                         # under the cap: everything stays
    sel = irscreen.kmeans_select(bank.shape_db(), 100, 32, 0)                    # keeps every spectral family
    lab, _ = irscreen.kmeans(bank.shape_db(), 32, 0)
    assert len(set(lab[sel].tolist())) >= 28 and len(sel) == 100


def _lib_run(tmp: Path, monkeypatch, n_local=5, **cfg_kw):
    from test_matcher import mkplan
    from test_matcher_v04m import _known, distinct_tone_pool
    from sawblade_match.matcher.run import Config, Log, run_match
    from sawblade_match.matcher.space import Combo, Space
    h = tmp / "home"
    h.mkdir()
    monkeypatch.setattr(irlib, "cache_dir", lambda: h / ".cache" / "sawblade")
    root = tmp / "my irs" / "Pack A"
    root.mkdir(parents=True)
    for i, ir in enumerate(random_iss(n_local, 21)):
        sf.write(str(root / f"V30 SM57 cap {i}.wav"), ir, FS, subtype="FLOAT")
    lib = irlib.scan([tmp / "my irs"], workers=2)
    pool = distinct_tone_pool()
    combo = Combo((pool.pedals[0],), pool.amps[1], None, None, pool.cabs[1])
    v = Space.for_combo(combo).default()
    v.update({"post.g1": 1.5})
    di, ref = _known(tmp, pool, combo, v)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0}, gens_linear=4, gens_gain=2, gens_final=3,
                  n_rescore_single=6, n_cab_single=2)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp / "out", seed=3, excerpt_s=2.0, threads=2, plan=plan, write_audio=False,
                 refine_offsets=False, ir_library=lib,
                 ir_dirs=({"path": str(tmp / "my irs"), "source": "cli"},), **cfg_kw)
    return run_match(cfg, Log()), lib, pool


def test_run_screens_library_irs_with_the_pool_cabs_and_records_irpool(tmp_path, monkeypatch):
    res, lib, pool = _lib_run(tmp_path, monkeypatch)
    ip = res["irPool"]
    assert ip["local"] == len(lib.records) == 5 and ip["tone3000"] == len(pool.cabs) and ip["total"] == 5 + len(pool.cabs)
    assert ip["dirs"] == [{"path": str(tmp_path / "my irs"), "source": "cli"}]
    assert ip["screened"] is True and ip["skipped"] == 0 and ip["prefiltered"] is False and ip["screenSeconds"] >= 0
    assert ip["winner"]["source"] in ("local", "tone3000") and ip["winner"]["key"]
    for c in res["cabSweep"]["candidates"]:
        keys = {i["cab"] for i in c["irs"]}
        assert any(k.startswith("local/") for k in keys) and c["screen"]["pool"] == 5 + len(pool.cabs) and len(c["screen"]["top6"]) == 6
        assert c["nCabs"] == 5 + len(pool.cabs) and c["screen"]["fullTop6"] and all(i["source"] == "local" for i in c["irs"] if i["cab"].startswith("local/"))
    w = res["best"]["captures"]["cab"]
    assert (w.get("source") == "local") == (ip["winner"]["source"] == "local")


def test_prefiltered_pool_is_recorded_and_irsweep_ablation_keeps_the_old_sweep(tmp_path, monkeypatch):
    res, lib, pool = _lib_run(tmp_path, monkeypatch, n_local=12, ir_screen_max=6)
    ip = res["irPool"]
    assert ip["prefiltered"] is True and ip["total"] == len(lib.records) + len(pool.cabs), (ip, len(lib.records), lib.report)
    c = res["cabSweep"]["candidates"][0]["screen"]["prefilter"]
    assert c["prefiltered"] and c["before"] == len(lib.records) + len(pool.cabs) and c["after"] == 6 and c["method"].endswith("kmeans32")
    assert c["tagHints"] == []                         # the fixture cabs carry no cab tags
    tmp2 = tmp_path / "second"
    tmp2.mkdir()
    res2, _, _ = _lib_run(tmp2, monkeypatch, ablate=("irsweep",))
    assert res2["irPool"]["ablated"] is True and res2["irPool"]["screened"] is False and res2["cabSweep"]["ablated"] is True
