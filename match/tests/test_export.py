"""Tests for the NAM export (phase 4): modes/refusals, cab + post EQ fold, gate bypass, metadata, training signal.

The training smoke tests (real NAM trainer, a few seconds of audio) run only with SAWBLADE_TEST_TRAIN=1.
Needs the built sawblade_core (skipped otherwise); the smoke tests also need ``match[export]``.
"""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal as sps

from sawblade_match.export import plan as P
from sawblade_match.export import signal as S

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


def test_non_commercial_capture_refused(shared):
    p = copy.deepcopy(shared)
    p["paths"]["a"]["blocks"][0]["model"]["source"] = {**SOURCE, "license": "cc-by-nc-sa"}
    with pytest.raises(P.ExportRefused, match="non-commercial"):
        P.make_plan(p, "withcab")


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
