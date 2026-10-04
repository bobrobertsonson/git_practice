"""Tests for the NAM export (phase 4): modes/refusals, cab + post EQ fold, gate bypass, metadata, training signal.

The training smoke tests (real NAM trainer, a few seconds of audio) run only with SAWBLADE_TEST_TRAIN=1.
Needs the built sawblade_core (skipped otherwise); the smoke tests also need ``match[export]``.
"""
from __future__ import annotations

import copy
import json
import types
import os
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal as sps

from sawblade_match.export import plan as P
from sawblade_match.export import signal as S
from sawblade_match.tonecheck.rules import load_targets

try:
    from sawblade_match.core import CaptureCache
    from sawblade_match.export import chain as C
except ImportError as exc:  # pragma: no cover - depends on the local build
    pytest.skip(f"sawblade_core is not built: {exc}", allow_module_level=True)

REPO = Path(__file__).resolve().parents[2]
PRESETS = REPO / "tests" / "fixtures" / "presets"
TRAIN = os.environ.get("SAWBLADE_TEST_TRAIN") == "1"
SOURCE = {"provider": "tone3000", "id": "12345", "modelId": "678", "url": "https://www.tone3000.com/tones/x-12345",
          "title": "Some Amp", "creator": "someone", "license": "cc-by"}


def load(name: str) -> dict:
    return json.loads((PRESETS / f"{name}.json").read_text())


@pytest.fixture(scope="module")
def cache():
    return CaptureCache()


@pytest.fixture
def shared():
    p = load("golden_shared")
    p["busComp"]["enabled"] = False            # the live-compatible, exactly exportable variant
    return p


def db(num: float, den: float) -> float:
    return 10 * np.log10(max(num, 1e-300) / max(den, 1e-300))


# ---------------------------------------------------------------- modes / refusals

def test_nocab_shared_without_comp_is_exact_and_reports_gate_bypass(shared):
    pl = P.make_plan(shared, "nocab")
    assert pl.exact and not pl.inexact
    assert [b["what"] for b in pl.bypassed] == ["gate"]
    assert pl.bypassed[0]["original"]["thresholdDb"] == -55.0


def test_nocab_with_comp_refused_unless_allow_inexact():
    p = load("golden_shared")                  # bus comp on
    with pytest.raises(P.ExportRefused, match="bus compressor"):
        P.make_plan(p, "nocab")
    pl = P.make_plan(p, "nocab", allow_inexact=True)
    assert not pl.exact and pl.inexact[0]["what"] == "busComp"
    assert {b["what"] for b in pl.bypassed} == {"gate", "busComp"}


def test_perpath_nocab_refused_with_ui_message_withcab_allowed():
    p = load("golden_perpath")
    with pytest.raises(P.ExportRefused) as e:
        P.make_plan(p, "nocab")
    assert "only the with-cab export is exact for studio blends" in str(e.value)
    with pytest.raises(P.ExportRefused):
        P.make_plan(p, "nocab", allow_inexact=True)      # not overridable
    pl = P.make_plan(p, "withcab")
    assert pl.exact


def test_withcab_always_allowed_but_long_release_comp_refused():
    p = load("golden_shared")
    assert P.make_plan(p, "withcab").exact                  # comp release 100 ms
    p["busComp"]["releaseMs"] = 400.0
    with pytest.raises(P.ExportRefused, match="not NAM-trainable"):
        P.make_plan(p, "withcab")
    P.make_plan(p, "nocab", allow_inexact=True)              # comp is dropped, so its release does not matter


def test_unknown_mode_and_untrainable_block_refused(shared):
    with pytest.raises(P.ExportRefused):
        P.make_plan(shared, "bogus")
    p = copy.deepcopy(shared)
    p["paths"]["a"]["blocks"].append({"id": "fx1", "type": "reverb"})
    with pytest.raises(P.ExportRefused, match="fx1"):
        P.make_plan(p, "nocab")
    p["paths"]["a"]["blocks"][-1]["bypass"] = True          # bypassed blocks are not part of the chain
    P.make_plan(p, "nocab")


def test_non_commercial_capture_is_allowed_and_marked(shared):
    p = copy.deepcopy(shared)
    p["paths"]["a"]["blocks"][0]["model"]["source"] = {**SOURCE, "license": "cc-by-nc-sa"}
    P.make_plan(p, "withcab")                                  # plans fine
    att = P.attribution(p)
    assert [a["nonCommercial"] for a in att if a["license"] == "cc-by-nc-sa"] == [True]
    blk = P.sawblade_block(p, P.make_plan(p, "withcab"), "lite", 0, 1, "0" * 64, {}, None)
    note = P.licence_note(p)
    assert blk["nonCommercial"] is True and blk["licenceNote"] == note
    assert "NON-COMMERCIAL" in note and "Some Amp" in note and "personal use only" in note


def test_cc_by_only_preset_has_no_non_commercial_marker(shared):
    p = copy.deepcopy(shared)
    p["paths"]["a"]["blocks"][0]["model"]["source"] = dict(SOURCE)          # cc-by
    assert not any("nonCommercial" in a for a in P.attribution(p))
    blk = P.sawblade_block(p, P.make_plan(p, "withcab"), "lite", 0, 1, "0" * 64, {}, None)
    assert blk["nonCommercial"] is False and "NON-COMMERCIAL" not in blk["licenceNote"]


def test_acceptance_status_strings():
    from sawblade_match.export import validate as V
    assert V.acceptance("standard", 0.4, 4.0)["status"] == "NOT MET"
    assert V.acceptance("standard", 0.01, 0.3)["status"] == "met"
    a = V.acceptance("lite", 0.01, 0.3)
    assert a["status"] == "not judged (non-standard size)" and "held-out ESR" in a["summary"]


def test_device_resolution_without_gpu():
    from sawblade_match.export import train as T
    assert T.resolve_device("auto", cuda=True, mps=True) == ("cuda", "gpu")
    assert T.resolve_device("auto", cuda=False, mps=True) == ("mps", "mps")
    assert T.resolve_device("auto", cuda=False, mps=False) == ("cpu", "cpu")
    assert T.resolve_device("cpu", cuda=True, mps=True) == ("cpu", "cpu")
    with pytest.raises(RuntimeError):
        T.resolve_device("cuda", cuda=False, mps=False)
    with pytest.raises(ValueError):
        T.resolve_device("tpu", cuda=False, mps=False)


def test_core_trainability_warning_detected():
    rep = {"warnings": ["block 'x' (type 'reverb') is not NAM-trainable", "something else"]}
    assert len(P.core_trainability_problems(rep)) == 1


# ---------------------------------------------------------------- training chain / gate bypass

def test_training_preset_strips_gate_and_for_nocab_cab_posteq_comp():
    p = load("golden_shared")
    t = P.training_preset(p, P.make_plan(p, "nocab", allow_inexact=True))
    assert t["gate"]["enabled"] is False and t["cab"]["enabled"] is False
    assert t["postEq"] == [] and t["busComp"]["enabled"] is False
    assert t["output"] == p["output"] and t["paths"] == p["paths"]
    w = P.training_preset(p, P.make_plan(p, "withcab"))
    assert w["gate"]["enabled"] is False and w["cab"].get("enabled", True) is True
    assert w["postEq"] == p["postEq"] and w["busComp"]["enabled"] is True
    assert p["gate"]["enabled"] is True                       # input untouched


def test_gate_is_bypassed_in_the_rendered_training_chain(shared, cache):
    rng = np.random.default_rng(0)
    x = (rng.standard_normal(24000) * 10 ** (-70 / 20)).astype(np.float32)     # below the -55 dB gate threshold
    x[12000:14000] += 0.2 * np.sin(2 * np.pi * 220 * np.arange(2000) / 48000).astype(np.float32)
    pl = P.make_plan(shared, "withcab")
    t = P.training_preset(shared, pl)
    ungated = copy.deepcopy(shared)
    ungated["gate"]["enabled"] = False
    y_t, _ = C.render48(t, x, PRESETS, cache)
    y_u, _ = C.render48(ungated, x, PRESETS, cache)
    y_g, _ = C.render48(shared, x, PRESETS, cache)
    assert np.array_equal(y_t, y_u)
    assert not np.allclose(y_t, y_g, atol=1e-6)               # the gate does change the (quiet) signal


# ---------------------------------------------------------------- IR (*) post EQ fold

def _fold_preset(shared):
    """Empty paths: the chain is cab -> post EQ -> output gain only (oracle for the fold)."""
    p = P.folding_preset(shared)
    p["output"] = {"gainDb": 0.0}
    return p


def test_fold_equals_cab_plus_post_eq_to_minus_100_db(shared, cache):
    h, info = C.fold_cab_post_eq(shared, PRESETS, cache)
    rng = np.random.default_rng(3)
    x = (0.3 * rng.standard_normal(48000)).astype(np.float32)
    ref, _ = C.render48(_fold_preset(shared), x, PRESETS, cache)       # core: cab, then post EQ
    y = sps.fftconvolve(x.astype(np.float64), h.astype(np.float64))[: len(x)]
    err = db(np.sum((y - ref) ** 2), np.sum(ref.astype(np.float64) ** 2))
    assert err < -100.0, err
    assert info["postEqBands"] == 2 and info["samples"] > 9000          # cab 9600 samples at 48 kHz + EQ tail


def test_fold_without_post_eq_is_the_normalised_cab_ir(shared, cache):
    shared["postEq"] = []
    h, _ = C.fold_cab_post_eq(shared, PRESETS, cache)
    ir, fs = sf.read(PRESETS.parent / "ir" / "ir_a.wav", dtype="float64")
    ir = ir[:, 0] if ir.ndim > 1 else ir
    assert fs == 48000
    ir = ir / np.sqrt(np.sum(ir * ir))                                   # core: L2-normalised
    n = min(len(ir), len(h))
    assert db(np.sum((h[:n] - ir[:n]) ** 2), np.sum(ir ** 2)) < -100.0


def test_nocab_chain_then_folded_ir_equals_full_chain(shared, cache):
    """model-side chain (gate off, no cab/post EQ) followed by the exported IR == the original chain, gate off."""
    rng = np.random.default_rng(5)
    x = (0.25 * np.sin(2 * np.pi * 110 * np.arange(36000) / 48000) + 0.05 * rng.standard_normal(36000)).astype(np.float32)
    pl = P.make_plan(shared, "nocab")
    pre, _ = C.render48(P.training_preset(shared, pl), x, PRESETS, cache)
    full, _ = C.render48(P.reference_preset(shared, pl), x, PRESETS, cache)
    h, _ = C.fold_cab_post_eq(shared, PRESETS, cache)
    y = sps.fftconvolve(pre.astype(np.float64), h.astype(np.float64))[: len(x)]
    assert db(np.sum((y - full) ** 2), np.sum(full.astype(np.float64) ** 2)) < -100.0


def test_fold_with_cab_disabled_is_post_eq_only(shared, cache):
    shared["cab"]["enabled"] = False
    h, info = C.fold_cab_post_eq(shared, PRESETS, cache)
    assert info["cabEnabled"] is False and info["samples"] < 9600


def test_validation_harness_is_exact_for_a_perfect_model(shared, cache, tmp_path):
    """Known answer: if the 'exported model' is the chain's own NAM, model + folded IR must reproduce the chain
    (ESR ~ -100 dB), through the same export_check_preset / compare_signals path the real validation uses."""
    from sawblade_match.export import validate as V
    nam = (PRESETS.parent / "nam" / "wavenet.nam").resolve()
    p = copy.deepcopy(shared)
    p["paths"] = {"a": {"role": "saw", "blocks": [{"id": "a1", "type": "nam", "model": {"file": str(nam)}}],
                        "levelDb": -2.0}, "b": {"role": "body", "enabled": False, "blocks": []}}
    p["blend"] = 0.0
    p["align"] = {"mode": "off"}
    p["cab"]["ir"]["file"] = str((PRESETS.parent / "ir" / "ir_a.wav").resolve())
    pl = P.make_plan(p, "nocab")
    h, _ = C.fold_cab_post_eq(p, PRESETS, cache)
    ir = tmp_path / "ir.wav"
    sf.write(ir, h, 48000, subtype="FLOAT")
    # the model-side chain also holds the path level / output gain; the 'model' here is the NAM, so move them into
    # the check preset the way a trained model would carry them
    chk = V.export_check_preset(nam, ir, tmp_path)
    chk["paths"]["a"]["levelDb"] = -2.0
    chk["output"] = copy.deepcopy(p["output"])
    rng = np.random.default_rng(2)
    x = (0.2 * rng.standard_normal(36000)).astype(np.float32)
    targets = load_targets(V.targets_path())
    res, ref, out = V.compare_signals("t", x, 48000, P.reference_preset(p, pl), PRESETS, chk, cache, targets, drop=4800)
    assert res["esr"] < 1e-9, res["esr"]                                     # -90 dB
    assert res["ltas"]["aWeightedErrorDb"] < 0.01


# ---------------------------------------------------------------- metadata / attribution

def test_attribution_and_sawblade_block(shared):
    p = copy.deepcopy(shared)
    p["paths"]["a"]["blocks"][0]["model"]["source"] = dict(SOURCE)
    p["paths"]["b"]["blocks"][1]["model"]["source"] = dict(SOURCE)                # same tone+model: deduplicated
    p["cab"]["ir"]["source"] = {**SOURCE, "id": "99", "modelId": "1", "title": "A cab", "license": "t3k"}
    att = P.attribution(p)
    ids = {(a["toneId"], a["modelId"]) for a in att}
    assert ("12345", "678") in ids and ("99", "1") in ids
    amp = next(a for a in att if a["toneId"] == "12345")
    assert (amp["title"], amp["creator"], amp["license"], amp["url"]) == (
        "Some Amp", "someone", "cc-by", "https://www.tone3000.com/tones/x-12345")
    assert len(amp["roles"]) == 2
    nofile = [a for a in att if a["toneId"] is None]
    assert nofile and all(a["license"] == "unknown" for a in nofile)              # captures without a source
    blk = P.sawblade_block(p, P.make_plan(p, "nocab"), "lite", 0, 1, "ab" * 32, {"inputLevelDbu": None}, "x.ir.wav")
    assert blk["preset"]["name"] == p["name"] and len(blk["preset"]["sha256"]) == 64
    assert blk["exportMode"] == "nocab" and blk["gateBypassed"] and blk["ir"] == "x.ir.wav"
    assert blk["attribution"] == att
    assert "personal use only" in blk["licenceNote"] and "TONE3000" in blk["licenceNote"]
    assert "NON-COMMERCIAL" not in blk["licenceNote"]


def test_preset_hash_ignores_machine_paths_but_not_content(shared):
    a = copy.deepcopy(shared)
    b = copy.deepcopy(shared)
    b["paths"]["a"]["blocks"][0]["model"]["file"] = "/somewhere/else/wavenet.nam"
    assert P.preset_hash(a) == P.preset_hash(b)
    b["blend"] = 0.5
    assert P.preset_hash(a) != P.preset_hash(b)


# ---------------------------------------------------------------- training signal

SMALL = S.SignalSpec(seed=7, train_plucks_s=4.0, valid_plucks=1)


def test_signal_is_deterministic_and_seeded():
    a, va, ia = S.generate(SMALL)
    b, vb, ib = S.generate(SMALL)
    assert np.array_equal(a, b) and np.array_equal(va, vb)
    assert ia["trainSha256"] == ib["trainSha256"] == S.signal_hash(a)
    c, _, ic = S.generate(S.SignalSpec(seed=8, train_plucks_s=4.0, valid_plucks=1))
    assert ic["trainSha256"] != ia["trainSha256"]
    assert ia["spec"]["seed"] == 7 and ia["spec"]["version"] == S.SIGNAL_VERSION


def test_signal_content_and_levels():
    a, v, info = S.generate(SMALL)
    assert a.dtype == np.float32 and v.dtype == np.float32 and np.isfinite(a).all()
    assert np.max(np.abs(a)) <= 10 ** (S.PEAK_CEILING_DB / 20) + 1e-6
    names = [s["name"] for s in info["trainSections"]]
    assert {"pink_steps", "white_steps", "plucks", "silence"} <= set(names) and sum(n.startswith("sweep") for n in names) == 6
    assert np.all(a[: int(0.9 * S.RATE)] == 0)                       # leading silence
    assert not np.array_equal(a[: len(v)], v)                         # validation is a different segment
    sec = {s["name"]: s for s in info["validSections"]}
    assert "plucks" in sec and "sweep" in sec


@pytest.mark.skipif(not TRAIN, reason="SAWBLADE_TEST_TRAIN=1 (generates the full >= 3 min signal, ~25 s)")
def test_default_signal_is_at_least_three_minutes():
    a, v, info = S.generate()
    assert info["trainSeconds"] >= 180.0 and info["validSeconds"] >= 20.0


# ---------------------------------------------------------------- trainer configuration (no training)

def test_train_config_defaults_and_architectures():
    from sawblade_match.export import train as T
    c = T.TrainConfig(size="lite").resolved()
    assert c.epochs == T.DEFAULT_EPOCHS["lite"] and c.max_minutes == T.DEFAULT_MAX_MINUTES["lite"]
    assert 0.8 <= c.lr_gamma <= 0.994 and abs(c.lr_gamma ** c.epochs - 0.05) < 1e-6      # anneals to 5 %
    assert T.TrainConfig(size="standard", epochs=1000).resolved().lr_gamma == 0.994      # the trainer's own recipe
    assert T.TrainConfig(size="feather", lr_gamma=0.9).resolved().lr_gamma == 0.9
    for size, (c1, h1, c2) in T.SIZES.items():
        a, b = T.wavenet_config(size)["layers_configs"]
        assert (a["channels"], a["head"]["out_channels"], b["channels"], b["input_size"]) == (c1, h1, c2, c1)
        assert h1 == c2 and b["head"]["out_channels"] == 1                              # array-1 head feeds array 2
    assert T.TrainConfig().size == "standard"                                           # default size


# ---------------------------------------------------------------- CLI

def test_cli_refuses_studio_nocab_before_any_work(capsys, tmp_path):
    from sawblade_match.export.cli import main
    rc = main([str(PRESETS / "golden_perpath.json"), "--mode", "nocab", "--out", str(tmp_path / "o")])
    err = capsys.readouterr().err
    assert rc == 2 and "only the with-cab export is exact for studio blends" in err


def test_cli_refuses_comp_nocab(capsys, tmp_path):
    from sawblade_match.export.cli import main
    rc = main([str(PRESETS / "golden_shared.json"), "--out", str(tmp_path / "o")])
    assert rc == 2 and "bus compressor" in capsys.readouterr().err


# ---------------------------------------------------------------- training smoke (slow, opt-in)

def _need_nam(T):
    try:
        T.import_nam()               # stubs tkinter on headless machines
    except RuntimeError as e:
        pytest.skip(str(e))


needs_train = pytest.mark.skipif(not TRAIN, reason="SAWBLADE_TEST_TRAIN=1 runs a real 1-2 epoch NAM training")


@needs_train
def test_feather_smoke_training_exports_a_loadable_nam(tmp_path, shared):
    from sawblade_match.export import train as T
    _need_nam(T)
    from sawblade_match.export import validate as V
    rng = np.random.default_rng(1)
    x = (0.3 * rng.standard_normal(96000)).astype(np.float32)           # 2 s
    v = (0.3 * rng.standard_normal(24000)).astype(np.float32)
    f = lambda a: np.tanh(3 * a).astype(np.float32)
    cfg = T.TrainConfig(size="feather", epochs=2, seed=3, threads=2, max_minutes=5)
    meta = {"sawblade": P.sawblade_block(shared, P.make_plan(shared, "nocab"), "feather", 3, 1, "0" * 64, {}, None)}
    r1 = T.train_nam(x, f(x), v, f(v), cfg, tmp_path / "a", tmp_path / "sa", other_metadata=meta, basename="m")
    r2 = T.train_nam(x, f(x), v, f(v), cfg, tmp_path / "b", tmp_path / "sb", other_metadata=meta, basename="m")
    a = json.loads(r1.nam_path.read_text())
    b = json.loads(r2.nam_path.read_text())
    assert a["architecture"] == "WaveNet" and a["sample_rate"] == 48000
    assert a["metadata"]["sawblade"]["exportMode"] == "nocab" and "licenceNote" in a["metadata"]["sawblade"]
    assert np.allclose(a["weights"], b["weights"], atol=0, rtol=0), "same seed + threads must repeat"
    assert r1.epochs_done == 2 and np.isfinite(r1.best_val_esr)
    # loads in the C++ core as a NamBlock and renders
    chk = V.export_check_preset(r1.nam_path, None, tmp_path)
    y, rep = C.render48(chk, v, tmp_path, None)
    assert len(y) == len(v) and np.isfinite(y).all()


@needs_train
def test_end_to_end_nocab_export_on_fixture_preset(tmp_path):
    from sawblade_match.export import train as T
    _need_nam(T)
    from sawblade_match.export.run import run_export
    p = load("golden_shared")
    p["busComp"]["enabled"] = False
    pj = tmp_path / "preset.json"
    for k in ("a", "b"):
        for blk in p["paths"][k]["blocks"]:
            blk["model"]["file"] = str((PRESETS / blk["model"]["file"]).resolve())
    p["cab"]["ir"]["file"] = str((PRESETS / p["cab"]["ir"]["file"]).resolve())
    pj.write_text(json.dumps(p))
    di = tmp_path / "di.wav"
    sf.write(di, (0.2 * np.sin(2 * np.pi * 196 * np.arange(44100 * 4) / 44100)).astype(np.float32), 44100)
    rep = run_export(pj, mode="nocab", size="feather", out=tmp_path / "out", epochs=1, max_minutes=5, threads=2,
                     di=di, signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None)
    out = tmp_path / "out"
    assert (out / "export_report.json").is_file() and (out / rep["training"]["namFile"]).is_file()
    assert (out / rep["ir"]["file"]).is_file() and (out / "listen").is_dir()
    assert rep["validation"]["heldOut"]["esr"] >= 0 and rep["validation"]["acceptance"]["evaluated"] is False
    nam = json.loads((out / rep["training"]["namFile"]).read_text())
    assert nam["metadata"]["training"]["validation_esr"] == rep["training"]["validationEsr"]
    assert nam["metadata"]["sawblade"]["validation"]["heldOutEsr"] == rep["validation"]["heldOut"]["esr"]
    assert rep["validation"]["acceptance"]["status"] == "not judged (non-standard size)"
    assert rep["training"]["config"]["device"] and "approximations" in rep["training"]["config"]["sizesNote"]
    assert "personal use only" in rep["licenceNote"]


# ---------------------------------------------------------------- resumable training (phase 4.1)

def _tiny_signals():
    rng = np.random.default_rng(1)
    x = (0.3 * rng.standard_normal(96000)).astype(np.float32)           # 2 s
    v = (0.3 * rng.standard_normal(24000)).astype(np.float32)
    f = lambda a: np.tanh(3 * a).astype(np.float32)
    return x, f(x), v, f(v)


@needs_train
def test_resume_after_2_epochs_equals_uninterrupted_4_epochs(tmp_path):
    from sawblade_match.export import resume as R
    from sawblade_match.export import train as T
    _need_nam(T)
    x, y, v, yv = _tiny_signals()
    kw = dict(size="feather", seed=3, threads=1, max_minutes=10, lr_gamma=0.9)
    ident = {"presetSha256": "p", "signalSha256": "s", "validSha256": "v", "mode": "nocab", "size": "feather"}
    full = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=4, **kw), tmp_path / "full", tmp_path / "sf", basename="m",
                       identity=ident)
    part = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=2, **kw), tmp_path / "res", tmp_path / "sr", basename="m",
                       identity=ident)
    assert part.epochs_done == 2
    prog = R.read_progress(tmp_path / "sr" / "checkpoint")
    assert prog["epoch"] == 2 and prog["presetSha256"] == "p" and prog["elapsedTrainingS"] > 0
    assert {"last.ckpt", "best.ckpt", "progress.json"} <= {f.name for f in (tmp_path / "sr" / "checkpoint").iterdir()}
    assert not list((tmp_path / "sr" / "checkpoint").glob("*.tmp"))              # atomic: no temp files left
    res = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=4, **kw), tmp_path / "res", tmp_path / "sr", basename="m",
                      identity=ident, resume=True)
    assert res.epochs_done == 4 and [r["epoch"] for r in res.history] == [1, 2, 3, 4]
    a = json.loads(full.nam_path.read_text())["weights"]
    b = json.loads(res.nam_path.read_text())["weights"]
    assert a == b, "resumed run must be bit-identical to the uninterrupted one (CPU, 1 thread)"
    assert [r["valEsr"] for r in full.history] == [r["valEsr"] for r in res.history]
    assert res.best_epoch == full.best_epoch and res.wall_s >= part.wall_s       # total time spans the resume


@needs_train
def test_resume_time_cap_counts_total_training_time(tmp_path):
    from sawblade_match.export import train as T
    _need_nam(T)
    x, y, v, yv = _tiny_signals()
    kw = dict(size="feather", seed=3, threads=1, lr_gamma=0.9)
    part = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=2, max_minutes=10, **kw), tmp_path / "o", tmp_path / "s",
                       basename="m")
    # the cap is already used up by the first session: resuming trains nothing more and says why
    res = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=6, max_minutes=part.wall_s / 120.0, **kw), tmp_path / "o",
                      tmp_path / "s", basename="m", resume=True)
    assert res.epochs_done == 2 and res.stopped_by == "max_time" and res.nam_path.is_file()


class _FakeTrain:
    """Stands in for ``train.train_nam``: records the call, writes a minimal .nam and a checkpoint like the real one."""

    def __init__(self):
        self.calls = []

    def __call__(self, x, y, v, yv, cfg, outdir, scratch, user_metadata=None, other_metadata=None, log=print,
                 basename="model", ckpt_dir=None, resume=False, identity=None):
        from sawblade_match.export import resume as R
        self.calls.append({"outdir": Path(outdir), "resume": resume, "ckpt_dir": Path(ckpt_dir), "identity": identity,
                           "cfg": cfg})
        Path(ckpt_dir).mkdir(parents=True, exist_ok=True)
        (Path(ckpt_dir) / R.LAST).write_bytes(b"x")
        r = cfg.resolved()
        R.write_progress(ckpt_dir, {**identity, "epoch": 2, "complete": False, "elapsedTrainingS": 1.0,
                                    "config": {"seed": cfg.seed, "batchSize": cfg.batch_size, "epochs": r.epochs,
                                               "lrGamma": r.lr_gamma}})
        p = Path(outdir) / f"{basename}.nam"
        p.write_text(json.dumps({"architecture": "WaveNet", "weights": [0.0],
                                 "metadata": {"sawblade": dict(other_metadata["sawblade"])}}))
        from sawblade_match.export.train import TrainResult
        return TrainResult(nam_path=p, epochs_done=2, best_epoch=2, best_val_esr=0.5, wall_s=1.0, stopped_by="max_epochs",
                           params=1, receptive_field=1, history=[], config={})


@pytest.fixture
def mocked_export(tmp_path, monkeypatch, shared):
    from sawblade_match.export import run as RUN
    from sawblade_match.export import train as T
    _need_nam(T)
    fake = _FakeTrain()
    sinfo = {"trainSha256": "t" * 64, "validSha256": "v" * 64,
             "train": {"rmsDbfs": -20.0, "peakDbfs": -3.0}, "valid": {}}
    state = {"sinfo": sinfo}
    monkeypatch.setattr(RUN.T, "train_nam", fake)
    monkeypatch.setattr(RUN, "probe_report", lambda *a, **k: {"warnings": []})
    monkeypatch.setattr(RUN, "_cached_signal", lambda spec, log: (np.zeros(10, np.float32), np.zeros(10, np.float32),
                                                                  state["sinfo"]))
    monkeypatch.setattr(RUN, "_cached_targets", lambda *a, **k: (np.zeros(10, np.float32), np.zeros(10, np.float32),
                                                                 {"levels": {"trainOutRmsDbfs": -20.0, "trainOutPeakDbfs": -3.0}}))
    monkeypatch.setattr(RUN, "fold_cab_post_eq", lambda *a, **k: (np.zeros(8, np.float32),
                                                                  {"samples": 8, "peakDb": 0.0}))
    p = copy.deepcopy(shared)
    for k in ("a", "b"):
        for blk in p["paths"][k]["blocks"]:
            blk["model"]["file"] = str((PRESETS / blk["model"]["file"]).resolve())
    p["cab"]["ir"]["file"] = str((PRESETS / p["cab"]["ir"]["file"]).resolve())
    pj = tmp_path / "preset.json"
    pj.write_text(json.dumps(p))

    def go(**kw):
        kw.setdefault("size", "feather")
        kw.setdefault("mode", "nocab")
        return RUN.run_export(pj, validate=False, log=lambda *_: None, exports_root=tmp_path / "exports", **kw)
    return types.SimpleNamespace(go=go, fake=fake, tmp=tmp_path, sinfo=state, preset=p, RUN=RUN)


def _unfinished(mocked, name, **over):
    """An unfinished run directory (checkpoint + progress) as the trainer leaves it."""
    from sawblade_match.export import plan as PL
    from sawblade_match.export import resume as R
    d = mocked.tmp / "exports" / name
    c = R.ckpt_dir(d)
    c.mkdir(parents=True)
    (c / R.LAST).write_bytes(b"x")
    rc = mocked.fake_cfg
    prog = {"presetSha256": PL.preset_hash(mocked.preset), "signalSha256": "t" * 64, "validSha256": "v" * 64,
            "mode": "nocab", "size": "feather", "epoch": 2, "complete": False,
            "config": {"seed": 0, "batchSize": 16, "epochs": rc.epochs, "lrGamma": rc.lr_gamma}}
    prog.update(over)
    R.write_progress(c, prog)
    return d


@pytest.fixture
def mx(mocked_export):
    from sawblade_match.export import train as T
    mocked_export.fake_cfg = T.TrainConfig(size="feather").resolved()
    return mocked_export


def test_mock_fresh_run_removes_checkpoint_unless_keep_scratch(mx):
    from sawblade_match.export import resume as R
    out = mx.tmp / "o1"
    rep = mx.go(out=str(out))
    assert rep["resume"] is None and mx.fake.calls[-1]["resume"] is False
    assert mx.fake.calls[-1]["ckpt_dir"] == out / "checkpoint"
    assert mx.fake.calls[-1]["identity"]["presetSha256"] == P.preset_hash(mx.preset)
    assert not (out / "checkpoint").exists() and not (out / "_scratch").exists()
    out2 = mx.tmp / "o2"
    mx.go(out=str(out2), keep_scratch=True)
    assert (out2 / "checkpoint").is_dir() and R.read_progress(out2 / "checkpoint")["complete"] is True


def test_mock_resume_dir_continues_in_that_dir(mx):
    d = _unfinished(mx, "run-a")
    rep = mx.go(resume=str(d))
    c = mx.fake.calls[-1]
    assert c["resume"] is True and c["outdir"] == d and rep["resume"]["epoch"] == 2
    assert not (d / "checkpoint").exists()                                   # removed after the successful export
    assert (d / "export_report.json").is_file()


def test_mock_resume_refuses_sha_size_mode_mismatch_and_missing(mx):
    from sawblade_match.export.cli import main
    n = len(mx.fake.calls)
    d = _unfinished(mx, "run-a", presetSha256="0" * 64)
    with pytest.raises(P.ExportRefused, match="preset sha256 differs"):
        mx.go(resume=str(d))
    d2 = _unfinished(mx, "run-b", signalSha256="0" * 64)
    with pytest.raises(P.ExportRefused, match="training-signal sha256 differs"):
        mx.go(resume=str(d2))
    d3 = _unfinished(mx, "run-c", size="lite")
    with pytest.raises(P.ExportRefused, match="size differs"):
        mx.go(resume=str(d3))
    d4 = _unfinished(mx, "run-d", mode="withcab")
    with pytest.raises(P.ExportRefused, match="mode differs"):
        mx.go(resume=str(d4))
    d5 = _unfinished(mx, "run-e", config={"seed": 9, "batchSize": 16, "epochs": 40, "lrGamma": 0.9})
    with pytest.raises(P.ExportRefused, match="seed differs"):
        mx.go(resume=str(d5))
    with pytest.raises(P.ExportRefused, match="no checkpoint"):
        mx.go(resume=str(mx.tmp / "nowhere"))
    with pytest.raises(P.ExportRefused, match="differ"):
        mx.go(resume=str(d), out=str(mx.tmp / "other"))
    assert len(mx.fake.calls) == n and (d / "checkpoint").exists()          # nothing trained, nothing deleted


def test_mock_cli_refusal_exit_code_and_resume_flag(mx, capsys, monkeypatch):
    from sawblade_match.export import cli
    d = _unfinished(mx, "run-a", mode="withcab")
    pj = mx.tmp / "preset.json"
    rc = cli.main([str(pj), "--size", "feather", "--resume", str(d), "--no-validate"])
    assert rc == 2 and "mode differs" in capsys.readouterr().err
    assert cli.build_parser().parse_args([str(pj), "--resume", "auto"]).resume == "auto"


def test_mock_resume_auto_picks_the_matching_newest_unfinished_dir(mx):
    import os as _os
    from sawblade_match.export import resume as R
    other_preset = _unfinished(mx, "other-preset", presetSha256="1" * 64)
    other_size = _unfinished(mx, "other-size", size="standard")
    other_mode = _unfinished(mx, "other-mode", mode="withcab")
    finished = _unfinished(mx, "finished", complete=True)
    old = _unfinished(mx, "old-match")
    new = _unfinished(mx, "new-match")
    _os.utime(R.ckpt_dir(old) / R.PROGRESS, (1_000_000, 1_000_000))
    msgs = []
    rep = mx.RUN.run_export(mx.tmp / "preset.json", mode="nocab", size="feather", validate=False, log=msgs.append,
                            resume="auto", exports_root=mx.tmp / "exports")
    assert rep["resume"]["dir"] == str(new) and mx.fake.calls[-1]["outdir"] == new and mx.fake.calls[-1]["resume"]
    assert any("resuming" in m and "new-match" in m for m in msgs)
    for d in (other_preset, other_size, other_mode, finished, old):
        assert (d / "checkpoint").exists()                                   # untouched


def test_mock_resume_auto_without_match_starts_fresh_in_out(mx):
    _unfinished(mx, "other-preset", presetSha256="1" * 64)
    msgs = []
    out = mx.tmp / "fresh"
    rep = mx.RUN.run_export(mx.tmp / "preset.json", mode="nocab", size="feather", validate=False, log=msgs.append,
                            resume="auto", exports_root=mx.tmp / "exports", out=str(out))
    assert rep["resume"] is None and mx.fake.calls[-1]["resume"] is False and mx.fake.calls[-1]["outdir"] == out
    assert any("starting fresh" in m for m in msgs)


def test_resume_progress_is_written_atomically(tmp_path):
    from sawblade_match.export import resume as R
    R.write_progress(tmp_path, {"epoch": 1})
    R.write_progress(tmp_path, {"epoch": 2})
    assert R.read_progress(tmp_path) == {"epoch": 2} and [f.name for f in tmp_path.iterdir()] == ["progress.json"]
    (tmp_path / "progress.json").write_text("{trunc")
    assert R.read_progress(tmp_path) is None
