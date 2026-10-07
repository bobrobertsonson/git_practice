"""v0.6 Task C: A2 export (packed trainer): files, per-submodel metrics, metadata, resume, core playback and the A2 fast-path shape."""
from __future__ import annotations

import json
import os
import sys
import types
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.export import a2shape as A2S
from sawblade_match.export import plan as P
from sawblade_match.export import resume as R
from sawblade_match.export import signal as S
from sawblade_match.export import train as T
from sawblade_match.export import validate as V

try:
    from sawblade_match.export import chain as C
except ImportError as exc:  # pragma: no cover - depends on the local build
    pytest.skip(f"sawblade_core is not built: {exc}", allow_module_level=True)

REPO = Path(__file__).resolve().parents[2]
FIX = REPO / "tests" / "fixtures" / "a2"
PRESETS = REPO / "tests" / "fixtures" / "presets"
TRAIN = os.environ.get("SAWBLADE_TEST_TRAIN") == "1"
needs_train = pytest.mark.skipif(not TRAIN, reason="SAWBLADE_TEST_TRAIN=1 runs a real 1-2 epoch NAM training")


def _need_nam():
    try:
        T.import_nam()               # stubs tkinter on headless machines
    except RuntimeError as e:
        pytest.skip(str(e))


def _signals():
    rng = np.random.default_rng(1)
    x = (0.3 * rng.standard_normal(96000)).astype(np.float32)           # 2 s
    v = (0.3 * rng.standard_normal(24000)).astype(np.float32)
    f = lambda a: np.tanh(3 * a).astype(np.float32)
    return x, f(x), v, f(v)


# ---------------------------------------------------------------- pure logic (no trainer needed)

def test_arch_size_matrix_and_defaults():
    assert T.check_arch_size("a2", None) == "full" and T.check_arch_size("a1", None) == "standard"
    assert T.check_arch_size("a2", "lite") == "lite" and T.check_arch_size("a1", "feather") == "feather"
    for arch, size in (("a2", "standard"), ("a2", "feather"), ("a1", "full"), ("a3", "full")):
        with pytest.raises(ValueError, match="not valid for --arch|unknown --arch"):
            T.check_arch_size(arch, size)


def test_cli_refuses_bad_arch_size_pairs_before_any_work(capsys, tmp_path):
    from sawblade_match.export.cli import main
    prog = tmp_path / "p.json"
    for argv in (["--arch", "a2", "--size", "standard"], ["--arch", "a1", "--size", "full"]):
        rc = main([str(PRESETS / "golden_shared.json"), *argv, "--out", str(tmp_path / "o"), "--progress-json", str(prog)])
        assert rc == 1 and "is not valid for --arch" in capsys.readouterr().err
        assert json.loads(prog.read_text())["stage"] == "error"
    assert not (tmp_path / "o").exists()


def test_a2_shape_port_accepts_fixtures_and_rejects_variants():
    full = json.loads((FIX / "a2_full.nam").read_text())
    lite = json.loads((FIX / "a2_lite.nam").read_text())
    a1 = json.loads((FIX / "a1_standard.nam").read_text())
    cont = json.loads((FIX / "a2_container.nam").read_text())
    assert A2S.nam_file_fast_path(full) == 8 and A2S.nam_file_fast_path(lite) == 3
    assert A2S.nam_file_fast_path(a1) is None and A2S.nam_file_fast_path(cont) is None
    assert [A2S.nam_file_fast_path(s["model"]) for s in cont["config"]["submodels"]] == [3, 8]
    bad = json.loads(json.dumps(full))
    bad["config"]["layers"][0]["activation"][3]["negative_slope"] = 0.02
    assert A2S.nam_file_fast_path(bad) is None
    bad = json.loads(json.dumps(full))
    bad["config"]["layers"][0]["channels"] = bad["config"]["layers"][0]["bottleneck"] = 5
    assert A2S.nam_file_fast_path(bad) is None


def test_acceptance_rules_a2_full_and_lite_and_a1_unchanged():
    assert V.acceptance("full", 0.019, 0.49, "a2")["status"] == "met"
    assert V.acceptance("full", 0.021, 0.49, "a2")["status"] == "NOT MET"
    lite = V.acceptance("lite", 0.04, 0.9, "a2")
    assert lite["status"] == "met" and lite["esrLimit"] == 0.05 and lite["ltasLimitDb"] == 1.0 and lite["evaluated"]
    assert V.acceptance("lite", 0.06, 0.9, "a2")["status"] == "NOT MET"
    assert "a2 lite" in lite["summary"]
    # A1 standard rule unchanged; non-standard sizes not judged
    assert V.acceptance("standard", 0.019, 0.49)["status"] == "met"
    assert V.acceptance("lite", 0.001, 0.1)["status"] == "not judged (non-standard size)"


def test_a1_sawblade_block_has_no_arch_key_a2_appends_it():
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    pl = P.make_plan(p, "withcab")
    a1 = P.sawblade_block(p, pl, "lite", 0, 1, "0" * 64, {}, None)
    a2 = P.sawblade_block(p, pl, "full", 0, 1, "0" * 64, {}, None, arch="a2")
    assert "arch" not in a1 and list(a2)[-1] == "arch" and a2["arch"] == "a2"
    assert {k: v for k, v in a2.items() if k not in ("arch", "size")} == {k: v for k, v in a1.items() if k != "size"}


# ---------------------------------------------------------------- trainer (needs neural-amp-modeler; tiny budget)

@pytest.fixture(scope="module")
def a2_run(tmp_path_factory):
    _need_nam()
    tmp = tmp_path_factory.mktemp("a2run")
    x, y, v, yv = _signals()
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    meta = {"sawblade": P.sawblade_block(p, P.make_plan(p, "nocab", True), "full", 3, 1, "0" * 64, {}, None, arch="a2")}
    from nam.models.metadata import GearType, ToneType, UserMetadata
    um = UserMetadata(name="t (nocab, A2)", modeled_by="Sawblade", gear_type=GearType("pedal_amp"), tone_type=ToneType.HI_GAIN)
    cfg = T.TrainConfig(size="full", arch="a2", epochs=2, seed=3, threads=2, max_minutes=10)
    res = T.train_nam(x, y, v, yv, cfg, tmp / "out", tmp / "scratch", user_metadata=um, other_metadata=meta, basename="m")
    return types.SimpleNamespace(tmp=tmp, res=res, v=v, x=x, y=y, yv=yv, cfg=cfg)


def test_a2_training_writes_container_and_two_standalone_files(a2_run):
    r = a2_run.res
    assert r.arch == "a2" and r.epochs_done == 2
    assert {k: v.name for k, v in r.files.items()} == {"container": "m.a2.nam", "full": "m.a2_full.nam", "lite": "m.a2_lite.nam"}
    assert r.nam_path == r.files["full"]                                   # --size full: the primary file
    cont = json.loads(r.files["container"].read_text())
    full = json.loads(r.files["full"].read_text())
    lite = json.loads(r.files["lite"].read_text())
    assert cont["architecture"] == "SlimmableContainer" and full["architecture"] == lite["architecture"] == "WaveNet"
    assert [s["max_value"] for s in cont["config"]["submodels"]] == [0.5, 1.0]
    # the standalone files are the container's submodels (same weights and config), each with its own loudness
    for name, f, i in (("lite", lite, 0), ("full", full, 1)):
        sub = cont["config"]["submodels"][i]["model"]
        assert f["weights"] == sub["weights"] and f["config"] == sub["config"], name
        assert f["metadata"]["loudness"] == sub["metadata"]["loudness"]
        assert f["sample_rate"] == 48000
    assert full["metadata"]["loudness"] == cont["metadata"]["loudness"]       # the container carries the Full submodel's
    assert lite["metadata"]["loudness"] != full["metadata"]["loudness"]
    assert r.submodels["full"]["parameters"] == 12145 and r.submodels["lite"]["parameters"] == 1870 and r.params == 22783
    assert r.receptive_field == 6347


def test_a2_files_keep_metadata_at_top_level_and_take_the_core_fast_path(a2_run):
    r = a2_run.res
    for key in ("container", "full", "lite"):
        m = json.loads(r.files[key].read_text())
        md = m["metadata"]
        assert md["name"] == "t (nocab, A2)" and md["modeled_by"] == "Sawblade" and md["gear_type"] == "pedal_amp", key
        assert md["sawblade"]["arch"] == "a2" and md["sawblade"]["exportMode"] == "nocab" and "licenceNote" in md["sawblade"]
        assert {"loudness", "date"} <= set(md)
    # the exported standalone files have the shape the pinned core's A2 fast path accepts
    assert A2S.nam_file_fast_path(json.loads(r.files["full"].read_text())) == 8
    assert A2S.nam_file_fast_path(json.loads(r.files["lite"].read_text())) == 3
    cont = json.loads(r.files["container"].read_text())
    assert [A2S.nam_file_fast_path(s["model"]) for s in cont["config"]["submodels"]] == [3, 8]


def test_a2_history_uses_per_submodel_esr_never_the_sum(a2_run):
    for row in a2_run.res.history:
        sub = row["submodels"]
        assert row["valEsr"] == sub["full"]["valEsr"] and row["valLoss"] == sub["full"]["valLoss"]
        assert sub["lite"]["valEsr"] != sub["full"]["valEsr"]
    for size in ("full", "lite"):
        best = min(a2_run.res.history, key=lambda r: r["submodels"][size]["valLoss"])
        assert a2_run.res.submodels[size]["bestEpoch"] == best["epoch"]
        assert a2_run.res.submodels[size]["bestValEsr"] == best["submodels"][size]["valEsr"]
    assert a2_run.res.best_val_esr == a2_run.res.submodels["full"]["bestValEsr"]


def test_a2_files_load_in_the_core_and_container_plays_the_full_submodel(a2_run, tmp_path):
    r = a2_run.res
    outs = {}
    for key in ("container", "full", "lite"):
        chk = V.export_check_preset(r.files[key], None, tmp_path)
        y, rep = C.render48(chk, a2_run.v[:8000], tmp_path, None)
        assert len(y) == 8000 and np.isfinite(y).all()
        outs[key] = y
    assert np.max(np.abs(outs["container"] - outs["full"])) < 1e-5            # the core plays the container's last submodel
    assert np.max(np.abs(outs["lite"] - outs["full"])) > 1e-4


def test_a2_resume_after_1_epoch_equals_uninterrupted_2_epochs(a2_run, tmp_path):
    x, y, v, yv = a2_run.x, a2_run.y, a2_run.v, a2_run.yv
    kw = dict(size="full", arch="a2", seed=3, threads=1, max_minutes=10, lr_gamma=0.9)
    ident = {"presetSha256": "p", "signalSha256": "s", "validSha256": "v", "mode": "nocab", "size": "full", "arch": "a2",
             "layout": T.A2_LAYOUT}
    full = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=2, **kw), tmp_path / "full", tmp_path / "sf", basename="m", identity=ident)
    T.train_nam(x, y, v, yv, T.TrainConfig(epochs=1, **kw), tmp_path / "res", tmp_path / "sr", basename="m", identity=ident)
    cd = tmp_path / "sr" / "checkpoint"
    prog = R.read_progress(cd)
    assert prog["arch"] == "a2" and prog["layout"] == T.A2_LAYOUT and prog["epoch"] == 1
    assert {"last.ckpt", "progress.json", "packed_best_submodel_0.ckpt", "packed_best_submodel_1.ckpt"} <= {f.name for f in cd.iterdir()}
    res = T.train_nam(x, y, v, yv, T.TrainConfig(epochs=2, **kw), tmp_path / "res", tmp_path / "sr", basename="m",
                      identity=ident, resume=True)
    assert res.epochs_done == 2
    for key in ("full", "lite", "container"):
        a = json.loads(full.files[key].read_text())
        b = json.loads(res.files[key].read_text())
        if key == "container":
            a, b = a["config"]["submodels"][1]["model"], b["config"]["submodels"][1]["model"]
        assert a["weights"] == b["weights"], key
    assert [r["submodels"] for r in full.history] == [r["submodels"] for r in res.history]


def test_a2_lite_primary_and_budget_defaults():
    c = T.TrainConfig(size="lite", arch="a2").resolved()
    assert (c.epochs, c.max_minutes) == (T.A2_DEFAULT_EPOCHS, T.A2_DEFAULT_MAX_MINUTES)
    c1 = T.TrainConfig(size="standard").resolved()
    assert (c1.epochs, c1.max_minutes) == (22, 55.0)                           # A1 budget unchanged


# ---------------------------------------------------------------- end to end (opt-in: SAWBLADE_TEST_TRAIN=1)

def _fixture_preset(tmp_path):
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    p["busComp"]["enabled"] = False
    for k in ("a", "b"):
        for blk in p["paths"][k]["blocks"]:
            blk["model"]["file"] = str((PRESETS / blk["model"]["file"]).resolve())
    p["cab"]["ir"]["file"] = str((PRESETS / p["cab"]["ir"]["file"]).resolve())
    pj = tmp_path / "preset.json"
    pj.write_text(json.dumps(p))
    return pj


@needs_train
@pytest.mark.parametrize("size", ["full", "lite"])
def test_end_to_end_a2_nocab_export_on_fixture_preset(tmp_path, size):
    _need_nam()
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    di = tmp_path / "di.wav"
    sf.write(di, (0.2 * np.sin(2 * np.pi * 196 * np.arange(44100 * 4) / 44100)).astype(np.float32), 44100)
    out = tmp_path / "out"
    prog = tmp_path / "progress.json"
    rep = run_export(pj, mode="nocab", arch="a2", size=size, out=out, epochs=1, max_minutes=10, threads=2, di=di,
                     signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None,
                     progress_json=prog)
    stem = f"golden-shared-live-compatible-nocab-{size}"
    assert rep["arch"] == "a2" and rep["size"] == size
    assert rep["files"] == {"primary": f"{stem}.a2_{size}.nam", "container": f"{stem}.a2.nam", "full": f"{stem}.a2_full.nam",
                            "lite": f"{stem}.a2_lite.nam"}
    for f in rep["files"].values():
        assert (out / f).is_file()
    assert rep["a2FastPath"] == {"full": True, "lite": True}
    assert set(rep["validation"]) == {"full", "lite"}                          # both standalone files validated through the core
    for sz in ("full", "lite"):
        v = rep["validation"][sz]
        assert v["heldOut"]["esr"] >= 0 and v["acceptance"]["evaluated"] is True and v["acceptance"]["appliesTo"] == f"a2 {sz}"
        assert v["acceptance"]["status"] in ("met", "NOT MET") and v["exportedModelLoadsInCore"] is True
        nam = json.loads((out / rep["files"][sz]).read_text())
        assert nam["metadata"]["sawblade"]["size"] == sz and nam["metadata"]["sawblade"]["arch"] == "a2"
        assert nam["metadata"]["sawblade"]["validation"]["heldOutEsr"] == v["heldOut"]["esr"]
        assert nam["metadata"]["training"]["validation_esr"] == rep["training"]["submodels"][sz]["bestValEsr"]
        assert nam["metadata"]["name"].endswith(f"A2 {sz.capitalize()})")
    assert rep["training"]["validationEsr"] == rep["training"]["submodels"][size]["bestValEsr"]       # per-submodel, not the sum
    assert (out / "listen" / "ab_original_then_export.wav").is_file()
    assert (out / "listen" / f"ab_original_then_export_{'lite' if size == 'full' else 'full'}.wav").is_file()
    # notes: generic + anagram, named by stem; the model line points at the primary file
    assert rep["exportNotes"]["file"] == f"{stem}.export_notes.txt" and (out / f"{stem}.anagram_notes.txt").is_file()
    prof = rep["exportNotes"]["deviceProfiles"]["anagram"]
    assert prof["file"] == f"{stem}.anagram_notes.txt"
    assert [s["block"] for s in prof["stages"]] == ["Gate", "Neural Amp", "IR"]
    assert prof["stages"][1]["settings"]["model"] == f"{stem}.a2_{size}.nam"
    assert json.loads(prog.read_text())["arch"] == "a2" and json.loads(prog.read_text())["stage"] == "done"
    assert "personal use only" in rep["licenceNote"]


def test_drive_only_is_a_positive_rule_and_sets_gear_type_pedal():
    p = json.loads((PRESETS / "golden_shared.json").read_text())
    p["gate"] = {"enabled": False}
    nam = lambda i, **kw: {"id": i, "type": "nam", "model": {"file": "../nam/wavenet.nam"}, **kw}
    p["paths"]["a"]["blocks"] = [nam("a1", slot="pedal"), {"id": "a2", "type": "pedal.ts", "params": {}}]
    p["paths"]["b"]["blocks"] = [nam("b1", slot="boost"), {"id": "e", "type": "eq", "bands": []}]
    pl = P.make_plan(p, "nocab", True)
    assert P.drive_only(p, pl) is True and P.gear_type(pl, p) == "pedal"
    p["paths"]["b"]["blocks"].append(nam("b2"))                               # unlabelled NAM block: an amp
    assert P.drive_only(p, pl) is False and P.gear_type(pl, p) == "pedal_amp"
    p["paths"]["b"]["blocks"][-1]["slot"] = "fx"                              # any other slot too
    assert P.drive_only(p, pl) is False
    p["paths"]["b"]["blocks"][-1]["bypass"] = True                            # a bypassed block does not count
    assert P.drive_only(p, pl) is True
    assert P.gear_type(P.make_plan(p, "withcab", True), p) == "amp_pedal_cab"


def _fake_report(full_status, lite_status):
    def block(st):
        return {"heldOut": {"esr": 0.01}, "diExcerpt": {"ltas": {"aWeightedErrorDb": 0.1}},
                "acceptance": {"status": st, "summary": f"acceptance {st}"}}
    return {"validation": {"full": block(full_status), "lite": block(lite_status)}, "licenceNote": "note"}


@pytest.mark.parametrize("size,full,lite,rc", [("lite", "met", "NOT MET", 2), ("full", "met", "NOT MET", 0),
                                               ("full", "NOT MET", "met", 2), ("lite", "NOT MET", "met", 0)])
def test_cli_require_accept_judges_the_primary_a2_size_only(monkeypatch, tmp_path, size, full, lite, rc):
    from sawblade_match.export import run as RUN
    from sawblade_match.export.cli import main
    seen = {}

    def fake(preset, **kw):
        seen.update(kw)
        return _fake_report(full, lite)
    monkeypatch.setattr(RUN, "run_export", fake)
    out = main([str(PRESETS / "golden_shared.json"), "--arch", "a2", "--size", size, "--require-accept", "--out", str(tmp_path / "o")])
    assert out == rc and seen["arch"] == "a2" and seen["size"] == size


def test_fixture_check_tolerates_float_noise_and_reports_real_differences(tmp_path):
    import importlib.util
    import shutil
    spec = importlib.util.spec_from_file_location("a2gen", FIX / "generate.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    shutil.copytree(FIX, tmp_path / "f")
    new = tmp_path / "f"
    lines = []
    assert gen.check_dirs(new, FIX, log=lines.append) == []
    m = json.loads((new / "a2_full.nam").read_text())
    m["metadata"]["loudness"] += 1e-6                       # another CPU's forward pass: tolerated
    m["weights"][0] += 1e-8
    (new / "a2_full.nam").write_text(json.dumps(m))
    assert gen.check_dirs(new, FIX, log=lines.append) == []
    m["weights"][5] += 1e-3                                 # a real weight change: reported with path and size
    m["config"]["layers"][0]["channels"] = 9
    (new / "a2_full.nam").write_text(json.dumps(m))
    assert gen.check_dirs(new, FIX, log=lines.append) == ["a2_full.nam"]
    text = "\n".join(lines)
    assert "a2_full.nam/weights: max abs diff 0.001, 1 value(s)" in text and "/config/layers[0]/channels" in text
