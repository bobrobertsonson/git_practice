"""`sawblade-t3k pack`, `resolve --progress-json` and the exit-code contract (4 = not logged in). No network."""
from __future__ import annotations

import json
import time

import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.auth import Session, TokenStore
from sawblade_match.t3k.cache import Cache
from sawblade_match.t3k.errors import ReauthRequired, T3KError
from sawblade_match.t3k.pack import build_pack
from conftest import BASE, SECRET_ACCESS, model_json, tone_json


@pytest.fixture
def cli_env(monkeypatch, tmp_path):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_test")
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(tmp_path / "tok" / "t3k_tokens.json"))
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    return tmp_path / "tok" / "t3k_tokens.json"


def login(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    TokenStore(path).save(Session(SECRET_ACCESS, "r", time.time() + 3600))


@pytest.fixture
def ir_world(api):
    t = tone_json(900, gear="ir", fmt="ir", title="Open 4x12 Pack", user="erin", license="cc-by")
    t["user"]["display_name"] = "Erin IR"
    api.add_tone(t, [model_json(9001, 900), model_json(9002, 900), model_json(9003, 900)])
    api.models[900][1]["name"] = "V30 UL SM57 0.5in"
    return api


def test_build_pack_manifest(make_client, ir_world, tmp_path):
    seen = []
    m = build_pack(make_client(), Cache(tmp_path / "c"), "900", progress=lambda i, n, name: seen.append((i, n, name)))
    assert {k: m[k] for k in ("toneId", "title", "creator", "license", "url")} == {
        "toneId": "900", "title": "Open 4x12 Pack", "creator": "Erin IR", "license": "cc-by",
        "url": "https://www.tone3000.com/tones/tone-900"}
    assert [x["modelId"] for x in m["models"]] == ["9001", "9002", "9003"]
    assert [x["name"] for x in m["models"]][1] == "V30 UL SM57 0.5in"
    for x in m["models"]:
        assert set(x) == {"modelId", "name", "file", "sha256"}
        assert x["file"].startswith(str(tmp_path / "c")) and x["file"].startswith("/")
        assert (tmp_path / "c" / "900" / f"{x['modelId']}.wav").read_bytes() == f"FILE-{x['modelId']}".encode()
    assert seen == [(1, 3, "m9001"), (2, 3, "V30 UL SM57 0.5in"), (3, 3, "m9003")]


def test_build_pack_rejects_non_ir_tone(make_client, api, tmp_path):
    api.add_tone(tone_json(901, gear="amp"), [model_json(9011, 901)])
    with pytest.raises(T3KError, match="not an IR pack"):
        build_pack(make_client(), Cache(tmp_path / "c"), "901")


def test_cli_pack_writes_manifest_and_progress_lines(cli_env, ir_world, tmp_path, capsys):
    login(cli_env)
    out = tmp_path / "sub" / "manifest.json"
    assert cli.main(["pack", "900", "--cache-dir", str(tmp_path / "cc"), "-o", str(out), "--progress-json"]) == 0
    cap = capsys.readouterr()
    lines = [json.loads(l) for l in cap.out.splitlines()]
    assert lines == [{"done": 1, "total": 3, "name": "m9001"}, {"done": 2, "total": 3, "name": "V30 UL SM57 0.5in"},
                     {"done": 3, "total": 3, "name": "m9003"}]          # stdout is JSON lines only
    m = json.loads(out.read_text())
    assert m["toneId"] == "900" and len(m["models"]) == 3
    assert SECRET_ACCESS not in cap.out + cap.err


def test_cli_pack_without_progress_flag_is_human_output(cli_env, ir_world, tmp_path, capsys):
    login(cli_env)
    assert cli.main(["pack", "900", "--cache-dir", str(tmp_path / "cc"), "-o", str(tmp_path / "m.json")]) == 0
    assert "3 model(s)" in capsys.readouterr().out


def test_cli_resolve_progress_json(cli_env, api, tmp_path, capsys):
    login(cli_env)
    api.add_tone(tone_json(200, gear="amp", title="Amp Two"), [model_json(2001, 200)])
    api.add_tone(tone_json(300, gear="pedal", title="Pedal Three"), [model_json(3001, 300)])
    preset = {"paths": {"a": {"blocks": [
        {"id": "a1", "type": "nam", "model": {"file": "x.nam", "source": {"provider": "tone3000", "id": "300"}}},
        {"id": "a2", "type": "nam", "model": {"file": "y.nam", "source": {"provider": "tone3000", "id": "200"}}},
        {"id": "a3", "type": "nam", "model": {"file": "local.nam"}}]}}}
    p = tmp_path / "p.json"
    p.write_text(json.dumps(preset))
    o = tmp_path / "p.resolved.json"
    assert cli.main(["resolve", str(p), "-o", str(o), "--cache-dir", str(tmp_path / "cc"), "--progress-json"]) == 0
    out = capsys.readouterr().out
    lines = [json.loads(l) for l in out.splitlines()]
    assert lines == [{"done": 1, "total": 2, "capture": "paths.a.blocks[0].model", "title": "Pedal Three"},
                     {"done": 2, "total": 2, "capture": "paths.a.blocks[1].model", "title": "Amp Two"}]
    assert json.loads(o.read_text())["paths"]["a"]["blocks"][0]["model"]["sha256"]


def test_cli_resolve_without_flag_prints_no_json(cli_env, api, tmp_path, capsys):
    login(cli_env)
    api.add_tone(tone_json(200, gear="amp"), [model_json(2001, 200)])
    p = tmp_path / "p.json"
    p.write_text(json.dumps({"m": {"file": "x", "source": {"provider": "tone3000", "id": "200"}}}))
    assert cli.main(["resolve", str(p), "-o", str(tmp_path / "o.json"), "--cache-dir", str(tmp_path / "cc")]) == 0
    assert "Resolved 1 capture(s)" in capsys.readouterr().out


# ---------------------------------------------------------------- exit codes

def test_exit_4_when_no_stored_token(cli_env, api, tmp_path, capsys):
    assert not cli_env.exists()
    for argv in (["whoami"], ["pack", "900", "-o", str(tmp_path / "m.json"), "--cache-dir", str(tmp_path / "c")],
                 ["resolve", str(tmp_path / "p.json"), "--cache-dir", str(tmp_path / "c")],
                 ["pull", "--no-trending", "--no-latest", "--cache-dir", str(tmp_path / "c")]):
        (tmp_path / "p.json").write_text(json.dumps({"m": {"file": "x", "source": {"provider": "tone3000", "id": "1"}}}))
        assert cli.main(argv) == 4, argv
        assert "login" in capsys.readouterr().err
    assert not api.calls and not (tmp_path / "m.json").exists()


def test_exit_4_on_reauth_required_from_the_client(cli_env, monkeypatch, tmp_path, capsys):
    def boom(*_a, **_k):
        raise ReauthRequired("API rejected the token after refresh; run `sawblade-t3k login`")
    monkeypatch.setattr(cli, "build_pack", boom)
    monkeypatch.setattr(cli, "make_client", lambda: None)
    assert cli.main(["pack", "1", "-o", str(tmp_path / "m.json")]) == 4
    assert "sawblade-t3k login" in capsys.readouterr().err


def test_exit_1_for_other_errors(cli_env, ir_world, tmp_path, capsys):
    login(cli_env)
    ir_world.add_tone(tone_json(901, gear="amp"), [model_json(9011, 901)])
    assert cli.main(["pack", "901", "-o", str(tmp_path / "m.json"), "--cache-dir", str(tmp_path / "c")]) == 1
    assert "not an IR pack" in capsys.readouterr().err
