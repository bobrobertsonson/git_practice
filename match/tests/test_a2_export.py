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
from sawblade_match.export import notes as N
from sawblade_match.export import plan as P
from sawblade_match.export import reamp as RP
from sawblade_match.export import standard_input as SI
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
        rc = main([str(PRESETS / "golden_shared.json"), *argv, "--signal", "sawblade", "--out", str(tmp_path / "o"), "--progress-json", str(prog)])
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
def test_end_to_end_a2_nocab_export_on_fixture_preset(tmp_path, size, monkeypatch):
    _need_nam()
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    di = tmp_path / "di.wav"
    sf.write(di, (0.2 * np.sin(2 * np.pi * 196 * np.arange(44100 * 4) / 44100)).astype(np.float32), 44100)
    out = tmp_path / "out"
    prog = tmp_path / "progress.json"
    standin = _standin_input(tmp_path / "nam_input.wav", seconds=30)
    _register_standin(monkeypatch, standin)
    rep = run_export(pj, mode="nocab", arch="a2", size=size, out=out, epochs=1, max_minutes=10, threads=2, di=di,
                     signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None,
                     progress_json=prog, reamp_pair=standin)
    stem = f"golden-shared-live-compatible-nocab-{size}"
    assert rep["arch"] == "a2" and rep["size"] == size
    reamp = rep["files"].pop("reampPair")                                      # training + --reamp-pair: both
    assert reamp["output"] == f"{stem}.reamp_output.wav" and (out / reamp["output"]).is_file() and (out / reamp["notes"]).is_file()
    assert rep["files"] == {"primary": f"{stem}.a2.nam", "container": f"{stem}.a2.nam", "full": f"{stem}.a2_full.nam",
                            "lite": f"{stem}.a2_lite.nam"}
    assert list(rep["files"])[2] == size                                       # --size: that extra is listed first
    assert rep["training"]["namFile"] == f"{stem}.a2.nam"
    for f in rep["files"].values():
        assert (out / f).is_file()
    assert json.loads((out / rep["files"]["container"]).read_text())["architecture"] == "SlimmableContainer"
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
    # notes: generic + anagram, named by stem; the model line points at the primary file (the container)
    assert rep["exportNotes"]["file"] == f"{stem}.export_notes.txt" and (out / f"{stem}.anagram_notes.txt").is_file()
    prof = rep["exportNotes"]["deviceProfiles"]["anagram"]
    assert prof["file"] == f"{stem}.anagram_notes.txt"
    assert [s["block"] for s in prof["stages"]] == ["Gate", "Neural Amp", "IR"]
    assert prof["stages"][1]["settings"]["model"] == f"{stem}.a2.nam"
    assert "any NAM A2 block" in prof["stages"][1]["hardware"] and prof["stages"][1]["hardware"].count("A2 container") == 1
    for txtf in (out / f"{stem}.export_notes.txt", out / f"{stem}.anagram_notes.txt"):
        assert TN in txtf.read_text(encoding="utf-8")
    assert rep["exportNotes"]["trainingNote"] == TN and rep["trainingSignal"]["id"] == "sawblade-synthetic v1"
    assert "not the standard NAM signal" in TN and "use the reamp pair" in TN
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
    out = main([str(PRESETS / "golden_shared.json"), "--arch", "a2", "--size", size, "--require-accept", "--signal", "sawblade", "--out", str(tmp_path / "o")])
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
    # everything but weights and loudness / gain is exact: sample_rate, a LeakyReLU slope, loudness beyond 1e-4
    for edit, key in ((lambda d: d.__setitem__("sample_rate", d["sample_rate"] + 1), "/sample_rate"),
                      (lambda d: d["config"]["layers"][0]["activation"][0].__setitem__(
                          "negative_slope", d["config"]["layers"][0]["activation"][0]["negative_slope"] + 5e-5), "negative_slope"),
                      (lambda d: d["config"]["layers"][0]["activation"][0].__setitem__(
                          "negative_slope", d["config"]["layers"][0]["activation"][0]["negative_slope"] + 1e-5), "negative_slope"),
                      (lambda d: d["metadata"].__setitem__("loudness", d["metadata"]["loudness"] + 0.5), "/metadata/loudness")):
        shutil.copy(FIX / "a2_lite.nam", new / "a2_lite.nam")
        d = json.loads((new / "a2_lite.nam").read_text())
        edit(d)
        (new / "a2_lite.nam").write_text(json.dumps(d))
        lines.clear()
        assert "a2_lite.nam" in gen.check_dirs(new, FIX, log=lines.append) and key in "\n".join(lines), key
    shutil.copy(FIX / "a2_lite.nam", new / "a2_lite.nam")
    # manifest: forward-pass diagnostics (run 247: packedForwardMaxAbsDiff 2.05e-8 vs 0.0) absolute 1e-6; other numbers exact
    man = json.loads((new / "manifest.json").read_text())
    man["models"]["a2_lite"]["packedForwardMaxAbsDiff"] = 2.05e-8
    man["models"]["a2_lite"]["referencePeak"] *= 1 + 1e-6
    man["input"]["rmsDbfs"] += 1.78e-15                     # CI run 248: numpy summation rounding on another CPU
    (new / "manifest.json").write_text(json.dumps(man))
    assert gen.check_dirs(new, FIX, log=lines.append) == ["a2_full.nam"]
    lines.clear()
    shutil.copy(FIX / "a2_full.nam", new / "a2_full.nam")
    assert gen.check_dirs(new, FIX, log=lines.append) == []
    man["models"]["a2_lite"]["packedForwardMaxAbsDiff"] = 1e-3
    man["models"]["a2_lite"]["parameters"] += 1
    (new / "manifest.json").write_text(json.dumps(man))
    assert gen.check_dirs(new, FIX, log=lines.append) == ["manifest.json"]
    text = "\n".join(lines)
    assert "packedForwardMaxAbsDiff" in text and "/parameters" in text
    shutil.copy(FIX / "manifest.json", new / "manifest.json")
    (new / "a2_full.nam").unlink()
    lines.clear()
    assert "a2_full.nam" in gen.check_dirs(new, FIX, log=lines.append) and "only in the committed directory" in "\n".join(lines)


# ---------------------------------------------------------------- decision 18: the files are standard NAM A2 files

@needs_train
def test_exported_a2_files_load_in_the_core_only_check(tmp_path):
    """Exports a tiny A2 and runs the core-only loader tool (``tests/tools/nam_load_check``: NeuralAmpModelerCore only, no
    Sawblade code) on the container and both standalone files; exit 0 = all load and process finite audio."""
    exe = os.environ.get("SAWBLADE_NAM_LOAD_CHECK")
    if not exe:
        pytest.skip("SAWBLADE_NAM_LOAD_CHECK is not set (path of the nam_load_check executable built from tests/tools)")
    import subprocess
    _need_nam()
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    out = tmp_path / "out"
    rep = run_export(pj, mode="nocab", arch="a2", size="full", out=out, epochs=1, max_minutes=10, threads=2, di="builtin",
                     signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None,
                     validate=False)
    files = [str(out / rep["files"][k]) for k in ("container", "full", "lite")]
    r = subprocess.run([exe, *files], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, f"nam_load_check failed ({r.returncode}):\n{r.stdout}\n{r.stderr}"


# ---------------------------------------------------------------- decision 20: the reamp pair

TN = N.training_sentence(f"{N.SYNTHETIC_SIGNAL} v{S.SIGNAL_VERSION}")


def _standin_input(path, seconds=30.0, rate=48000, subtype="PCM_24"):
    rng = np.random.default_rng(7)
    x = (0.25 * rng.standard_normal(int(seconds * rate))).astype(np.float32)
    sf.write(str(path), x, rate, subtype=subtype)
    return path


def _v3_standin(path, seconds_train=20.0, amp=0.25):
    """Synthetic stand-in with the V3 layout the trainer's pipeline needs (never the official audio): 9 s validation noise,
    1 s silence, blips at 10.5 s / 11.5 s, chirp/noise filler, training noise, 0.5 s silence, the same 9 s validation again."""
    r = 48000
    rng = np.random.default_rng(11)
    val = amp * rng.standard_normal(9 * r)
    parts = [val, np.zeros(r), np.zeros(2 * r)]
    parts[-1][int(0.5 * r)] = 0.9
    parts[-1][int(1.5 * r)] = 0.9
    parts += [amp * rng.standard_normal(5 * r), amp * rng.standard_normal(int(seconds_train * r)), np.zeros(r // 2), val]
    x = np.concatenate(parts).astype(np.float32)
    sf.write(str(path), x, r, subtype="PCM_24")
    return path


def _register_standin(monkeypatch, path):
    """Make the synthetic stand-in count as the standard input (the official file never lives in the repo)."""
    x, rate = SI.read_standard_samples(path)
    monkeypatch.setattr(SI, "STANDARD_INPUT_STRONG_MD5", {})
    monkeypatch.setattr(SI, "STANDARD_INPUT_WEAK", {"3.0.0": {(SI._md5_array(x[:17 * rate]), SI._md5_array(x[-9 * rate:]))}})


def test_reamp_input_validation_follows_the_trainer(monkeypatch, tmp_path):
    from sawblade_match.export.plan import ExportRefused
    good = _standin_input(tmp_path / "good.wav")
    with pytest.raises(ExportRefused, match="not a known NAM standard input file.*standard input file as used by the NAM trainer"):
        SI.recognise(good)                                              # real tables: a stand-in is unknown
    _register_standin(monkeypatch, good)
    info = SI.recognise(good)
    assert info["version"] == "3.0.0" and info["match"] == "weak" and info["rate"] == 48000
    monkeypatch.setattr(SI, "STANDARD_INPUT_STRONG_MD5", {info["md5"]: "3.0.0"})
    assert SI.recognise(good)["match"] == "strong"                       # strong (file bytes) wins first
    with pytest.raises(ExportRefused, match="44100 Hz.*48000 Hz"):
        SI.recognise(_standin_input(tmp_path / "r44.wav", rate=44100))
    with pytest.raises(ExportRefused, match="10.0 s long.*at least 26 s"):
        SI.recognise(_standin_input(tmp_path / "short.wav", seconds=10))
    sf.write(str(tmp_path / "st.wav"), np.zeros((48000 * 30, 2), np.float32), 48000, subtype="PCM_24")
    with pytest.raises(ExportRefused, match="2 channels"):
        SI.recognise(tmp_path / "st.wav")
    sf.write(str(tmp_path / "f.wav"), np.zeros(48000 * 30, np.float32), 48000, subtype="FLOAT")
    with pytest.raises(ExportRefused, match="integer PCM"):
        SI.recognise(tmp_path / "f.wav")
    with pytest.raises(ExportRefused, match="no such file"):
        SI.recognise(tmp_path / "missing.wav")
    other = _standin_input(tmp_path / "other.wav")                           # right format, other content: unknown
    x, _ = SI.read_standard_samples(other)
    x2 = x.copy()
    x2[100] += 0.5
    sf.write(str(other), x2.astype(np.float32), 48000, subtype="PCM_24")
    monkeypatch.setattr(SI, "STANDARD_INPUT_STRONG_MD5", {})
    with pytest.raises(ExportRefused, match="not a known NAM standard input file"):
        SI.recognise(other)


def test_reamp_weak_hash_matches_the_trainers_own_function(tmp_path):
    """Our weak hash must equal what ``nam.train.core`` computes (trainer 0.13.0), else a real file would be rejected."""
    _need_nam()
    from nam.train import core as NC
    f = _standin_input(tmp_path / "a.wav", seconds=30)
    x, rate = SI.read_standard_samples(f)
    from nam.data import wav_to_np
    ref = wav_to_np(f)
    assert ref.dtype == x.dtype and np.array_equal(ref, x)
    import hashlib
    assert hashlib.md5(ref[:17 * rate]).hexdigest() == SI._md5_array(x[:17 * rate])
    assert NC._V3_DATA_INFO.rate == SI.RATE


def test_reamp_cli_argument_rules(capsys, tmp_path):
    from sawblade_match.export.cli import main
    pj = str(PRESETS / "golden_shared.json")
    assert main([pj, "--no-train"]) == 1 and "--no-train needs --reamp-pair" in capsys.readouterr().err
    assert main([pj, "--no-train", "--reamp-pair", "x.wav", "--require-accept"]) == 1
    assert main([pj, "--reamp-pair", str(tmp_path / "nope.wav"), "--no-train", "--out", str(tmp_path / "o")]) == 1
    assert "no such file" in capsys.readouterr().err


def test_reamp_pair_only_renders_the_exportable_chain(monkeypatch, tmp_path):
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    src = _standin_input(tmp_path / "nam_input.wav", seconds=30)
    _register_standin(monkeypatch, src)
    out = tmp_path / "out"
    prog = tmp_path / "progress.json"
    rep = run_export(pj, mode="nocab", arch="a2", size="full", out=out, reamp_pair=src, no_train=True, log=lambda *_: None,
                     progress_json=prog)
    stem = "golden-shared-live-compatible-nocab-full"
    assert rep["trained"] is False and "validation" not in rep and "training" not in rep
    assert rep["files"] == {"reampPair": {"input": f"{stem}.reamp_input.wav", "output": f"{stem}.reamp_output.wav",
                                          "notes": f"{stem}.reamp_notes.txt"}}
    assert (out / f"{stem}.reamp_input.wav").read_bytes() == src.read_bytes()            # copied unchanged
    y, rate = sf.read(str(out / f"{stem}.reamp_output.wav"), dtype="float64")
    x, _ = SI.read_standard_samples(src)
    info = sf.info(str(out / f"{stem}.reamp_output.wav"))
    assert rate == 48000 and info.subtype == "PCM_24" and info.channels == 1 and len(y) == len(x)
    assert np.max(np.abs(y)) < 1.0 and np.sqrt(np.mean(y ** 2)) > 1e-3
    # same chain as the model export: the training preset rendered at 48 kHz (no gate, no cab / post EQ in nocab)
    from sawblade_match.core import CaptureCache
    pre, base = C.load_preset(pj)
    tp = P.training_preset(pre, P.make_plan(pre, "nocab", False))
    want, _ = C.render48(tp, x.astype(np.float32), base, CaptureCache())
    g = np.max(np.abs(want))
    ref = want * min(1.0, SI.CLIP_CEILING / g)
    assert np.max(np.abs(y - ref)) < 2e-7 + 1e-6
    assert (out / f"{stem.rsplit('-', 1)[0]}.ir.wav").is_file()                          # nocab: the IR for the loader
    txt = (out / f"{stem}.reamp_notes.txt").read_text(encoding="utf-8")
    assert "PERSONAL USE ONLY" in txt and "never upload or share" in txt and "standard input file" in txt
    assert json.loads((out / "export_report.json").read_text())["reamp"]["inputVersion"] == "3.0.0"
    st = json.loads(prog.read_text())
    assert st["stage"] == "done" and st["arch"] == "a2"
    assert not list(out.glob("*.nam"))                                                   # no training happened


def test_reamp_pair_refuses_an_unknown_input_before_any_work(tmp_path):
    from sawblade_match.export.plan import ExportRefused
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    src = _standin_input(tmp_path / "nam_input.wav", seconds=30)
    with pytest.raises(ExportRefused, match="standard input file as used by the NAM trainer"):
        run_export(pj, mode="nocab", arch="a2", out=tmp_path / "o", reamp_pair=src, no_train=True, log=lambda *_: None)
    assert not (tmp_path / "o").exists()
    with pytest.raises(ExportRefused, match="--no-train needs --reamp-pair"):
        run_export(pj, mode="nocab", arch="a2", out=tmp_path / "o", no_train=True, log=lambda *_: None)


# ---------------------------------------------------------------- decision 22: training on the NAM standard input

def test_standard_input_tables_cover_every_version_the_trainer_knows():
    _need_nam()
    import inspect
    from nam.train import core as NC
    src = inspect.getsource(NC._detect_input_version)
    for md5 in SI.STANDARD_INPUT_STRONG_MD5:
        assert md5 in src
    for ver, pairs in SI.STANDARD_INPUT_WEAK.items():
        for a, b in pairs:
            assert a in src and b in src, ver
    assert set(SI.STANDARD_INPUT_STRONG_MD5.values()) == {"1.0.0", "1.1.1", "2.0.0", "3.0.0"}
    # validation split per version matches the trainer's own data config
    from nam.train._version import Version
    for ver in ("1.0.0", "2.0.0", "3.0.0"):
        cfg = NC._get_data_config(Version.from_string(ver), Path("a"), Path("b"), 8192, 0)
        v = cfg["validation"]
        n = 9_600_000
        sl = SI.validation_slice(ver, n)
        if "start_samples" in v:
            assert (v["start_samples"] % n) == sl.start, ver


def test_official_data_pipeline_uses_the_trainers_calibration_and_split(monkeypatch, tmp_path):
    _need_nam()
    from sawblade_match.export import official as OFF
    src = _v3_standin(tmp_path / "in.wav")
    _register_standin(monkeypatch, src)
    info = SI.recognise(src)
    x, _ = SI.read_standard_samples(src)
    y = np.tanh(2 * x) * 0.5
    sf.write(str(tmp_path / "out.wav"), y, 48000, subtype="PCM_24")
    od = OFF.OfficialData(src, tmp_path / "out.wav", info["version"], log=lambda *_: None)
    dtr, dva = od.build(rf=1023, ny=8192)
    assert od.info["version"] == "3.0.0" and od.info["latencySamples"] == -1          # blip at the input sample: delay 0, safety factor 1
    assert od.info["calibrationDelays"] == [0] and od.info["dataChecks"] == {"passed": True}
    assert od.info["trainSplit"]["start_samples"] == 480000 and od.info["validationSplit"]["start_samples"] == -432000
    assert len(dtr) > 100 and len(dva) == 1 and float(dtr.sample_rate) == 48000.0
    # a chain that does not repeat (validation replicates differ) is refused by the trainer's check
    y2 = y.copy()
    y2[-432000:] *= 0.3
    sf.write(str(tmp_path / "out2.wav"), y2, 48000, subtype="PCM_24")
    from sawblade_match.export.plan import ExportRefused
    with pytest.raises(ExportRefused, match="validation replicates"):
        OFF.OfficialData(src, tmp_path / "out2.wav", "3.0.0", log=lambda *_: None).build(1023, 8192)


def test_cli_demands_an_explicit_training_signal(capsys, tmp_path):
    from sawblade_match.export.cli import main
    pj = str(PRESETS / "golden_shared.json")
    assert main([pj, "--out", str(tmp_path / "o")]) == 1
    err = capsys.readouterr().err
    assert "--nam-input" in err and "--signal sawblade" in err and not (tmp_path / "o").exists()
    assert main([pj, "--nam-input", "a.wav", "--signal", "sawblade"]) == 1 and "contradict" in capsys.readouterr().err
    assert main([pj, "--nam-input", str(tmp_path / "missing.wav"), "--out", str(tmp_path / "o")]) == 1
    assert "no such file" in capsys.readouterr().err


@needs_train
def test_end_to_end_a2_on_the_standard_input_standin(monkeypatch, tmp_path):
    _need_nam()
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    src = _v3_standin(tmp_path / "nam_input.wav")
    _register_standin(monkeypatch, src)
    out = tmp_path / "out"
    rep = run_export(pj, mode="nocab", arch="a2", size="lite", out=out, epochs=1, max_minutes=10, threads=2, di="builtin",
                     nam_input=src, log=lambda *_: None)
    assert rep["trainingSignal"]["id"] == "nam-standard v3.0.0" and rep["trainingSignal"]["version"] == "3.0.0" and rep["signal"]["kind"] == "nam-standard"
    assert rep["training"]["officialData"]["latencySamples"] in (-1, 0, 1, 2) and "validationSplit" in rep
    assert rep["exportNotes"]["trainingSignal"] == "nam-standard v3.0.0"
    assert "Trained on the standard NAM signal (nam-standard v3.0.0)" in (out / rep["exportNotes"]["file"]).read_text()
    for k in ("container", "full", "lite"):
        meta = json.loads((out / rep["files"][k]).read_text())["metadata"]
        assert meta["sawblade"]["trainingSignal"] == "nam-standard v3.0.0"
    assert set(rep["validation"]) == {"full", "lite"} and rep["validation"]["lite"]["heldOut"]["esr"] >= 0
    # the other signal is the labelled fallback
    out2 = tmp_path / "out2"
    rep2 = run_export(pj, mode="nocab", arch="a2", size="full", out=out2, epochs=1, max_minutes=10, threads=2, validate=False,
                      signal="sawblade", signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None)
    assert rep2["trainingSignal"]["id"].startswith("sawblade-synthetic v")
    assert "not the standard NAM signal" in (out2 / rep2["exportNotes"]["file"]).read_text()


def test_resume_reuses_the_recorded_signal_and_the_cli_does_not_demand_one(monkeypatch, tmp_path):
    from sawblade_match.export import run as RUN
    from sawblade_match.export.cli import main
    from sawblade_match.export.plan import ExportRefused
    run_dir = tmp_path / "run"
    cd = R.ckpt_dir(run_dir)
    cd.mkdir(parents=True)
    (cd / R.LAST).write_bytes(b"x")
    R.write_progress(cd, {"trainingSignal": "nam-standard v3.0.0", "namInputPath": str(tmp_path / "gone.wav"), "epoch": 1})
    pj = _fixture_preset(tmp_path)
    with pytest.raises(ExportRefused, match="not there any more.*--nam-input"):
        RUN.run_export(pj, mode="nocab", arch="a2", resume=str(run_dir), log=lambda *_: None)
    seen = {}
    monkeypatch.setattr(RUN, "run_export", lambda preset, **kw: seen.update(kw) or (_ for _ in ()).throw(ExportRefused("stop")))
    assert main([str(pj), "--resume", str(run_dir)]) == 1 and seen["nam_input"] is None and seen["signal"] is None
    assert main([str(pj), "--resume", "auto"]) == 1                                  # auto cannot know the signal: asks
    # the plugin's pair-only call: no --device / --di / --require-accept / training signal
    assert main([str(pj), "--mode", "nocab", "--arch", "a2", "--size", "full", "--reamp-pair", "x.wav", "--no-train"]) == 1
    assert seen["no_train"] is True and seen["reamp_pair"] == "x.wav"


def test_resume_with_a_different_signal_refuses(tmp_path):
    from sawblade_match.export.plan import ExportRefused
    from sawblade_match.export.run import run_export
    pj = _fixture_preset(tmp_path)
    pre = json.loads(pj.read_text())
    run_dir = tmp_path / "run"
    cd = R.ckpt_dir(run_dir)
    cd.mkdir(parents=True)
    (cd / R.LAST).write_bytes(b"x")
    R.write_progress(cd, {"trainingSignal": "nam-standard v3.0.0", "namInputPath": "x.wav", "presetSha256": P.preset_hash(pre),
                          "signalSha256": "a" * 32, "validSha256": "a" * 32, "mode": "nocab", "size": "full", "arch": "a2",
                          "layout": T.layout_of("a2"), "epoch": 1})
    with pytest.raises(ExportRefused, match="cannot resume.*training-signal sha256 differs"):
        run_export(pj, mode="nocab", arch="a2", size="full", resume=str(run_dir), signal="sawblade",
                   signal_spec=S.SignalSpec(seed=1, train_plucks_s=3.0, valid_plucks=1), log=lambda *_: None)
