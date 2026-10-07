"""v0.3.0.1: the CLI needs a client id only to log in; stored tokens work without TONE3000_CLIENT_ID."""
import functools
import json
import time

import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.auth import Session, TokenStore, poll_for_session as real_poll
from conftest import BASE, SECRET_ACCESS, FakeClock, model_json, tone_json

TOKEN_URL = f"{BASE}/api/v1/oauth/token"
DEV_URL = f"{BASE}/api/v1/oauth/device_authorization"
DEV_BODY = {"device_code": "D", "user_code": "U", "verification_uri": "https://x", "expires_in": 600, "interval": 5}
TOK_BODY = {"access_token": "new", "refresh_token": "ref2", "expires_in": 3600}


@pytest.fixture
def env(monkeypatch, tmp_path, respx_mock):
    monkeypatch.delenv("TONE3000_CLIENT_ID", raising=False)
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    path = tmp_path / "cfg" / "t3k_tokens.json"
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(path))
    clock = FakeClock()
    monkeypatch.setattr(cli, "poll_for_session", functools.partial(real_poll, sleep=clock.sleep, now=clock, mono=clock))
    return TokenStore(path)


def fresh(client_id=None):
    return Session(SECRET_ACCESS, "ref", time.time() + 3600, client_id=client_id)


def expired(client_id=None):
    return Session("old", "ref1", 0.0, client_id=client_id)


def test_whoami_with_stored_token_no_env(env, api, capsys):
    env.save(fresh())   # not even a client_id in the file: a valid token needs none
    assert cli.main(["whoami", "--json"]) == 0
    assert "username" in json.loads(capsys.readouterr().out)


def test_fetch_with_stored_token_no_env(env, api, tmp_path):
    api.add_tone(tone_json(10, gear="amp", title="Amp", user="c", license="cc-by"), [model_json(101, 10)])
    env.save(fresh("t3k_pub_file"))
    rc = cli.main(["fetch", "10", "--model", "101", "--json", "--cache-dir", str(tmp_path / "cc")])
    assert rc == 0 and list((tmp_path / "cc").rglob("*.nam"))


def test_refresh_uses_token_file_client_id(env, respx_mock):
    env.save(expired("t3k_pub_file"))
    route = respx_mock.post(TOKEN_URL).respond(200, json=TOK_BODY)
    assert cli.make_client().tokens.get_access_token() == "new"
    assert "client_id=t3k_pub_file" in route.calls[0].request.content.decode()
    assert json.loads(env.path.read_text())["client_id"] == "t3k_pub_file"   # kept across rotation


def test_refresh_env_wins_over_file(env, monkeypatch, respx_mock):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_env")
    env.save(expired("t3k_pub_file"))
    route = respx_mock.post(TOKEN_URL).respond(200, json=TOK_BODY)
    cli.make_client().tokens.get_access_token()
    assert "client_id=t3k_pub_env" in route.calls[0].request.content.decode()


def test_refresh_without_any_client_id_is_a_clear_error(env, capsys):
    env.save(expired())
    assert cli.main(["whoami", "--json"]) == 1
    err = json.loads(capsys.readouterr().out)["error"]
    assert "export TONE3000_CLIENT_ID" in err and "Settings" in err


def test_login_writes_client_id_from_env(env, monkeypatch, respx_mock):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_login")
    respx_mock.post(DEV_URL).respond(200, json=DEV_BODY)
    respx_mock.post(TOKEN_URL).respond(200, json=TOK_BODY)
    assert cli.main(["login"]) == 0
    assert json.loads(env.path.read_text())["client_id"] == "t3k_pub_login"


def test_login_without_any_client_id_names_both_fixes(env, capsys):
    assert cli.main(["login"]) == 1
    err = capsys.readouterr().err
    assert "export TONE3000_CLIENT_ID" in err and "Settings" in err


def test_login_falls_back_to_token_file_client_id(env, respx_mock):
    env.save(fresh("t3k_pub_file"))
    dev = respx_mock.post(DEV_URL).respond(200, json=DEV_BODY)
    respx_mock.post(TOKEN_URL).respond(200, json=TOK_BODY)
    assert cli.main(["login"]) == 0
    assert "client_id=t3k_pub_file" in dev.calls[0].request.content.decode()


def test_secret_never_accepted(env, monkeypatch, capsys):
    env.path.parent.mkdir(parents=True)
    env.path.write_text(json.dumps({"access_token": "a", "refresh_token": "r", "expires_at": 0,
                                    "client_id": "t3k_cs_nope"}))
    assert cli.main(["whoami"]) == 1   # expired, secret in file ignored -> no client id
    capsys.readouterr()
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_cs_nope")
    assert cli.main(["whoami"]) == 1
    assert "SECRET" in capsys.readouterr().err


def test_revoked_refresh_keeps_client_id_so_login_still_works(env, respx_mock):
    env.save(expired("t3k_pub_file"))
    respx_mock.post(TOKEN_URL).respond(400, json={"error": "invalid_grant"})
    assert cli.main(["whoami"]) == cli.EXIT_NOT_LOGGED_IN
    assert json.loads(env.path.read_text()) == {"client_id": "t3k_pub_file"}   # tokens gone, id kept
    assert env.load() is None
    respx_mock.post(DEV_URL).respond(200, json=DEV_BODY)
    respx_mock.post(TOKEN_URL).respond(200, json=TOK_BODY)
    assert cli.main(["login"]) == 0   # no env var needed
    assert json.loads(env.path.read_text())["client_id"] == "t3k_pub_file"


def test_revoked_refresh_without_client_id_removes_file(env, respx_mock, monkeypatch):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_env")
    env.save(expired())
    respx_mock.post(TOKEN_URL).respond(401, json={})
    assert cli.main(["whoami"]) == cli.EXIT_NOT_LOGGED_IN
    assert not env.path.exists()
