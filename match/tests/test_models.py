"""sawblade-models: paths, sha256 sidecar, pins, download verify/cleanup, CLI, status. No network, no torch."""
from __future__ import annotations

import hashlib
from pathlib import Path

import httpx
import pytest
import respx

from sawblade_match.models import cli, download, paths, store


def test_models_dir_env_wins():
    d = paths.models_dir({"SAWBLADE_MODELS_DIR": "/x/m", "XDG_DATA_HOME": "/y"}, "linux", Path("/h"))
    assert d == Path("/x/m")


def test_models_dir_macos():
    assert paths.models_dir({}, "darwin", Path("/Users/a")) == Path("/Users/a/Library/Application Support/Sawblade/models")


def test_models_dir_linux_xdg_and_fallback():
    assert paths.models_dir({"XDG_DATA_HOME": "/data"}, "linux", Path("/h")) == Path("/data/sawblade/models")
    assert paths.models_dir({}, "linux", Path("/h")) == Path("/h/.local/share/sawblade/models")
    # a relative XDG_DATA_HOME is invalid per the XDG spec and ignored
    assert paths.models_dir({"XDG_DATA_HOME": "rel"}, "linux", Path("/h")) == Path("/h/.local/share/sawblade/models")
    assert paths.models_dir({"SAWBLADE_MODELS_DIR": ""}, "linux", Path("/h")) == Path("/h/.local/share/sawblade/models")


def test_file_names_match_cpp_contract(tmp_path):
    assert paths.onnx_path("htdemucs_6s", tmp_path).name == "htdemucs_6s-core-opset17.onnx"
    assert paths.sidecar_path("htdemucs", tmp_path).name == "htdemucs-core-opset17.onnx.sha256"
    assert paths.checkpoint_path("a.th", tmp_path) == tmp_path / "checkpoints" / "hub" / "checkpoints" / "a.th"


def test_sidecar_format(tmp_path):
    f = tmp_path / "htdemucs-core-opset17.onnx"
    f.write_bytes(b"abc")
    d = store.sha256_file(f)
    assert d == hashlib.sha256(b"abc").hexdigest()
    p = store.write_sidecar("htdemucs", tmp_path, d)
    assert p.read_text() == d + "\n" and len(d) == 64
    assert store.read_sidecar("htdemucs", tmp_path) == d
    with pytest.raises(ValueError):
        store.write_sidecar("htdemucs", tmp_path, d.upper())
    p.write_text("garbage\n")
    assert store.read_sidecar("htdemucs", tmp_path) is None


def test_pins_present():
    assert store.PINNED_ONNX_SHA256["htdemucs"].startswith("79189af3")
    assert store.PINNED_ONNX_SHA256["htdemucs_6s"].startswith("d23996ba")
    assert set(store.CHECKPOINTS) == set(paths.MODEL_IDS)


def test_model_status(tmp_path):
    st = store.model_status("htdemucs", tmp_path)
    assert not st.present and not st.sha_ok and not st.pinned_match
    f = paths.onnx_path("htdemucs", tmp_path)
    f.write_bytes(b"model")
    st = store.model_status("htdemucs", tmp_path)
    assert st.present and not st.sha_ok            # no sidecar yet
    store.write_sidecar("htdemucs", tmp_path, store.sha256_file(f))
    st = store.model_status("htdemucs", tmp_path)
    assert st.sha_ok and not st.pinned_match
    store.write_sidecar("htdemucs", tmp_path, "0" * 64)
    assert not store.model_status("htdemucs", tmp_path).sha_ok


def test_pinned_match(tmp_path, monkeypatch):
    f = paths.onnx_path("htdemucs", tmp_path)
    f.write_bytes(b"model")
    monkeypatch.setitem(store.PINNED_ONNX_SHA256, "htdemucs", store.sha256_file(f))
    assert store.model_status("htdemucs", tmp_path).pinned_match


@pytest.fixture
def fake_ckpt(monkeypatch):
    blob = b"checkpoint-bytes" * 1000
    name = "fake-00000000.th"
    monkeypatch.setitem(store.CHECKPOINTS, "htdemucs", (name, hashlib.sha256(blob).hexdigest()))
    return name, blob


@respx.mock
def test_download_ok_and_cached(tmp_path, fake_ckpt):
    name, blob = fake_ckpt
    route = respx.get(f"{store.CHECKPOINT_BASE_URL}/{name}").respond(200, content=blob)
    p = download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None)
    assert p.read_bytes() == blob and route.call_count == 1
    assert not list(p.parent.glob("*.partial"))
    download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None)
    assert route.call_count == 1                   # cached and hash-correct: no second download


@respx.mock
def test_download_replaces_corrupt_cache(tmp_path, fake_ckpt):
    name, blob = fake_ckpt
    dest = paths.checkpoint_path(name, tmp_path)
    dest.parent.mkdir(parents=True)
    dest.write_bytes(b"corrupt")
    respx.get(f"{store.CHECKPOINT_BASE_URL}/{name}").respond(200, content=blob)
    assert download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None).read_bytes() == blob


@respx.mock
def test_download_hash_mismatch_cleans_up(tmp_path, fake_ckpt):
    name, _ = fake_ckpt
    respx.get(f"{store.CHECKPOINT_BASE_URL}/{name}").respond(200, content=b"wrong")
    with pytest.raises(download.DownloadError, match="sha256"):
        download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None)
    d = paths.checkpoint_path(name, tmp_path).parent
    assert not list(d.glob("*"))


@respx.mock
def test_download_http_error_and_interrupted_stream_clean_up(tmp_path, fake_ckpt):
    name, blob = fake_ckpt
    d = paths.checkpoint_path(name, tmp_path).parent
    respx.get(f"{store.CHECKPOINT_BASE_URL}/{name}").respond(404)
    with pytest.raises(download.DownloadError, match="404"):
        download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None)
    assert not list(d.glob("*"))

    def stream():
        yield blob[:100]
        raise httpx.ReadError("connection reset")
    respx.get(f"{store.CHECKPOINT_BASE_URL}/{name}").mock(return_value=httpx.Response(200, content=stream()))
    with pytest.raises(download.DownloadError):
        download.ensure_checkpoint("htdemucs", tmp_path, log=lambda m: None)
    assert not list(d.glob("*"))


def test_cli_parsing():
    p = cli.build_parser()
    a = p.parse_args(["fetch"])
    assert (a.cmd, a.model, a.dir, a.force) == ("fetch", "htdemucs_6s", None, False)
    a = p.parse_args(["fetch", "--model", "all", "--dir", "/m"])
    assert (a.model, a.dir) == ("all", "/m")
    assert p.parse_args(["status"]).cmd == "status"
    with pytest.raises(SystemExit):
        p.parse_args(["fetch", "--model", "nope"])
    with pytest.raises(SystemExit):
        p.parse_args([])


def test_status_output(tmp_path, capsys):
    f = paths.onnx_path("htdemucs", tmp_path)
    f.write_bytes(b"x")
    store.write_sidecar("htdemucs", tmp_path, store.sha256_file(f))
    assert cli.main(["status", "--dir", str(tmp_path)]) == 0
    out = capsys.readouterr().out
    assert str(tmp_path) in out
    assert "htdemucs: present  sha ok  pinned differs" in out
    assert "htdemucs_6s: missing" in out and "match/.venv/bin/sawblade-models fetch --model htdemucs_6s" in out


def test_status_uses_env_dir(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("SAWBLADE_MODELS_DIR", str(tmp_path / "e"))
    cli.main(["status"])
    assert str(tmp_path / "e") in capsys.readouterr().out


def test_fetch_skips_when_verified(tmp_path, capsys):
    f = paths.onnx_path("htdemucs_6s", tmp_path)
    f.write_bytes(b"x")
    store.write_sidecar("htdemucs_6s", tmp_path, store.sha256_file(f))
    assert cli.main(["fetch", "--dir", str(tmp_path)]) == 0
    assert "already present and verified" in capsys.readouterr().out


def test_full_pins_and_checkpoint_hashes():
    assert store.PINNED_ONNX_SHA256 == {
        "htdemucs": "79189af3c584b1a2145ae5e4182a50c0204f88b76e2829bd27e4d4a88ede427d",
        "htdemucs_6s": "d23996ba2e9396d393e2bd53c29f1411bd33b8cf3451854ad32d746ad3d06132",
    }
    assert store.CHECKPOINTS == {
        "htdemucs": ("955717e8-8726e21a.th", "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"),
        "htdemucs_6s": ("5c90dfd2-34c22ccb.th", "34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd"),
    }


def test_residual_db():
    np = pytest.importorskip("numpy")
    from sawblade_match.models.export_onnx import residual_db
    ref = np.random.default_rng(0).standard_normal(1000)
    assert residual_db(ref, ref) < -250
    assert residual_db(ref * 1.1, ref) == pytest.approx(-20.0, abs=1e-6)
    assert residual_db(ref + 0.01 * ref, ref) == pytest.approx(-40.0, abs=1e-6)


def test_verify_error_keeps_existing_pair_and_leaves_nothing_new(tmp_path):
    from sawblade_match.models import export_onnx as ex
    f, sc = paths.onnx_path("htdemucs", tmp_path), paths.sidecar_path("htdemucs", tmp_path)
    f.write_bytes(b"old-verified")
    store.write_sidecar("htdemucs", tmp_path, store.sha256_file(f))
    old_sc = sc.read_text()
    part = f.with_name(f.name + ".partial")
    part.write_bytes(b"new-bad")
    with pytest.raises(ex.VerifyError):
        ex.check_residuals("htdemucs", {"x_freq": -80.0, "x_time": -59.0, "composed": -80.0})
    # the exporter deletes the partial on any failure and never calls commit(): the old pair is untouched
    part.unlink()
    assert f.read_bytes() == b"old-verified" and sc.read_text() == old_sc
    assert store.model_status("htdemucs", tmp_path).sha_ok


def test_verify_error_on_fresh_dir_leaves_no_onnx_or_sidecar(tmp_path):
    from sawblade_match.models import export_onnx as ex
    with pytest.raises(ex.VerifyError):
        ex.check_residuals("htdemucs_6s", {"composed": float("nan")})
    assert not list(tmp_path.iterdir())


def test_commit_swaps_pair_and_writes_sidecar(tmp_path):
    from sawblade_match.models import export_onnx as ex
    f = paths.onnx_path("htdemucs", tmp_path)
    f.write_bytes(b"old")
    store.write_sidecar("htdemucs", tmp_path, store.sha256_file(f))
    part = f.with_name(f.name + ".partial")
    part.write_bytes(b"new")
    ex.commit("htdemucs", tmp_path, part)
    assert f.read_bytes() == b"new" and not part.exists()
    assert store.model_status("htdemucs", tmp_path).sha_ok


def test_fetch_all_continues_after_a_failure(tmp_path, monkeypatch, capsys):
    calls = []

    def fake_ensure(mid, d, log=print):
        calls.append(mid)
        if mid == "htdemucs_6s":
            raise ValueError("boom")
        raise KeyError("also")
    monkeypatch.setattr(download, "ensure_checkpoint", fake_ensure)
    assert cli.main(["fetch", "--model", "all", "--dir", str(tmp_path)]) == 4
    assert calls == ["htdemucs_6s", "htdemucs"]
    err = capsys.readouterr().err
    assert "htdemucs_6s: ValueError: boom" in err and "htdemucs: KeyError" in err
