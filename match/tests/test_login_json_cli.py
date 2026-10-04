import functools
import io
import json

import httpx
import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.auth import poll_for_session
from conftest import BASE, SECRET_ACCESS, SECRET_REFRESH, FakeClock

DEV_URL = f"{BASE}/api/v1/oauth/device_authorization"
TOK_URL = f"{BASE}/api/v1/oauth/token"
NEW_ACCESS, NEW_REFRESH = "acc-NEW-9999", "ref-NEW-9999"


@pytest.fixture(autouse=True)
def env(monkeypatch, tmp_path):
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_test")
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(tmp_path / "cfg" / "tokens.json"))
    clock = FakeClock()
    monkeypatch.setattr(cli, "poll_for_session",
                        functools.partial(poll_for_session, sleep=clock.sleep, now=clock, mono=clock))


def mock_flow(respx_mock, complete="https://www.tone3000.com/activate?c=X"):
    respx_mock.post(DEV_URL).respond(200, json={
        "device_code": "DEV", "user_code": "BCDF-GHJK", "verification_uri": "https://www.tone3000.com/activate",
        "verification_uri_complete": complete, "expires_in": 600, "interval": 5})
    respx_mock.post(TOK_URL).respond(200, json={
        "access_token": NEW_ACCESS, "refresh_token": NEW_REFRESH, "token_type": "bearer",
        "expires_in": 3600, "scope": "read"})
    respx_mock.get(f"{BASE}/api/v1/user").mock(side_effect=lambda req: httpx.Response(
        200, json={"id": 7, "username": "alice", "display_name": "Alice A"})
        if req.headers.get("authorization") == f"Bearer {NEW_ACCESS}" else httpx.Response(401, json={}))


def lines(capsys):
    out = capsys.readouterr().out
    return out, [json.loads(l) for l in out.splitlines()]


def test_whoami_json_ok(api, make_client, monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(cli, "make_client", make_client)
    assert cli.main(["whoami", "--json"]) == 0
    out, objs = lines(capsys)
    assert len(out.splitlines()) == 1
    assert objs == [{"username": "alice", "display_name": None, "id": 7,
                     "token_file": str(tmp_path / "cfg" / "tokens.json")}]


def test_whoami_json_error_shape(respx_mock, monkeypatch, capsys):
    monkeypatch.delenv("TONE3000_CLIENT_ID")
    assert cli.main(["whoami", "--json"]) == 1
    out, objs = lines(capsys)
    assert len(objs) == 1 and list(objs[0]) == ["error"] and "TONE3000_CLIENT_ID" in objs[0]["error"]


def test_whoami_json_api_error(api, respx_mock, make_client, monkeypatch, capsys):
    respx_mock.post(TOK_URL).respond(400, json={"error": "invalid_grant"})
    monkeypatch.setattr(cli, "make_client", lambda: make_client(access="wrong"))
    assert cli.main(["whoami", "--json"]) == 1
    _, objs = lines(capsys)
    assert len(objs) == 1 and set(objs[0]) == {"error"} and objs[0]["error"]


def test_whoami_plain_unchanged(api, make_client, monkeypatch, capsys):
    monkeypatch.setattr(cli, "make_client", make_client)
    assert cli.main(["whoami"]) == 0
    assert capsys.readouterr().out == "alice (@alice, id 7)\n"


def test_login_json_event_sequence(respx_mock, tmp_path, capsys):
    mock_flow(respx_mock)
    assert cli.main(["login", "--json"]) == 0
    out, objs = lines(capsys)
    assert objs == [
        {"event": "device_code", "user_code": "BCDF-GHJK", "verification_uri": "https://www.tone3000.com/activate",
         "verification_uri_complete": "https://www.tone3000.com/activate?c=X", "expires_in": 600},
        {"event": "logged_in", "username": "alice", "display_name": "Alice A", "id": 7,
         "token_file": str(tmp_path / "cfg" / "tokens.json")}]
    assert NEW_REFRESH not in out and NEW_ACCESS not in out
    assert NEW_REFRESH in (tmp_path / "cfg" / "tokens.json").read_text()      # saved, just not printed


def test_login_json_null_complete_uri(respx_mock, capsys):
    mock_flow(respx_mock, complete=None)
    assert cli.main(["login", "--json"]) == 0
    _, objs = lines(capsys)
    assert objs[0]["verification_uri_complete"] is None


def test_login_json_failure_is_json(respx_mock, capsys):
    mock_flow(respx_mock)
    respx_mock.post(TOK_URL).respond(400, json={"error": "access_denied"})
    assert cli.main(["login", "--json"]) == 1
    out, objs = lines(capsys)
    assert [o["event"] for o in objs] == ["device_code", "error"] and "declined" in objs[1]["message"]
    assert NEW_REFRESH not in out


def test_login_json_device_error_and_missing_env(respx_mock, monkeypatch, capsys):
    respx_mock.post(DEV_URL).respond(500, json={})
    assert cli.main(["login", "--json"]) == 1
    _, objs = lines(capsys)
    assert len(objs) == 1 and objs[0]["event"] == "error"
    monkeypatch.delenv("TONE3000_CLIENT_ID")
    assert cli.main(["login", "--json"]) == 1
    _, objs = lines(capsys)
    assert objs[0]["event"] == "error" and "TONE3000_CLIENT_ID" in objs[0]["message"]


def test_login_plain_still_prints_refresh_token(respx_mock, capsys):
    mock_flow(respx_mock)
    assert cli.main(["login"]) == 0
    out = capsys.readouterr().out
    assert NEW_REFRESH in out and "BCDF-GHJK" in out


def test_login_json_unexpected_exception_is_one_error_line(monkeypatch, capsys):
    def boom(*a, **k):
        raise RuntimeError("boom")
    monkeypatch.setattr(cli, "request_device_code", boom)
    assert cli.main(["login", "--json"]) == 1
    out, objs = lines(capsys)
    assert objs == [{"event": "error", "message": "boom"}]


def test_whoami_json_unexpected_exception_is_one_error_line(monkeypatch, capsys):
    def boom(self):
        raise RuntimeError("boom")
    monkeypatch.setattr(cli.T3KClient, "get_user", boom)
    assert cli.main(["whoami", "--json"]) == 1
    out, objs = lines(capsys)
    assert objs == [{"error": "boom"}]


def test_login_json_token_store_failure(respx_mock, monkeypatch, capsys):
    mock_flow(respx_mock)

    def fail(self, s):
        raise OSError("disk full")
    monkeypatch.setattr(cli.TokenStore, "save", fail)
    assert cli.main(["login", "--json"]) == 1
    out, objs = lines(capsys)
    assert [o["event"] for o in objs] == ["device_code", "error"] and "disk full" in objs[1]["message"]
    assert NEW_REFRESH not in out and NEW_ACCESS not in out


def test_login_json_device_code_is_flushed_before_polling(respx_mock, monkeypatch):
    mock_flow(respx_mock)

    class Spy(io.StringIO):
        flushes = 0
        flushed_len = 0

        def flush(self):
            Spy.flushes += 1
            Spy.flushed_len = len(self.getvalue())
            super().flush()

    spy = Spy()
    seen = {}

    def fake_poll(http, cid, dc, **kw):
        # the device_code line must already have been flushed (not just written) at this point
        seen["flushed"] = spy.getvalue() != "" and Spy.flushed_len == len(spy.getvalue())
        raise cli.T3KError("stop")

    monkeypatch.setattr(cli, "poll_for_session", fake_poll)
    monkeypatch.setattr(cli.sys, "stdout", spy)
    assert cli.main(["login", "--json"]) == 1
    assert seen["flushed"] is True


def test_whoami_json_with_env_seed_stdout_all_json(api, respx_mock, monkeypatch, capsys):
    monkeypatch.setenv("TONE3000_REFRESH_TOKEN", SECRET_REFRESH)
    respx_mock.post(TOK_URL).respond(200, json={"access_token": SECRET_ACCESS, "refresh_token": "ref-ROT",
                                                "expires_in": 3600})
    cli.main(["whoami", "--json"])
    out, objs = lines(capsys)          # every line parses as JSON
    assert len(objs) == 1 and SECRET_REFRESH not in out and "ref-ROT" not in out
