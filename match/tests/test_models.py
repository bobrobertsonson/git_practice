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
