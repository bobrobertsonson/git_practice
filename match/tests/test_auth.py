import json
import stat

import httpx
import pytest

from sawblade_match.t3k.auth import (DEVICE_GRANT, Session, TokenManager, TokenStore, poll_for_session,
                                     request_device_code)
from sawblade_match.t3k.errors import DeviceFlowError, ReauthRequired
from conftest import BASE, FakeClock

TOKEN_URL = f"{BASE}/api/v1/oauth/token"
DEV_URL = f"{BASE}/api/v1/oauth/device_authorization"


def http():
    return httpx.Client(base_url=BASE)


def device_body():
    return {"device_code": "DEV", "user_code": "BCDF-GHJK", "verification_uri": "https://www.tone3000.com/activate",
            "verification_uri_complete": "https://www.tone3000.com/activate?c=BCDFGHJK",
            "expires_in": 600, "interval": 5}


def tokens(access="a1", refresh="r1", exp=3600):
    return {"access_token": access, "refresh_token": refresh, "token_type": "bearer", "expires_in": exp,
            "scope": "read"}


def test_device_request_sends_only_client_id_and_scope(respx_mock):
    route = respx_mock.post(DEV_URL).respond(200, json=device_body())
    dc = request_device_code(http(), "t3k_pub_x")
    body = route.calls[0].request.content.decode()
    assert "client_id=t3k_pub_x" in body and "scope=read" in body
    for forbidden in ("redirect_uri", "code_challenge", "prompt"):
        assert forbidden not in body
    assert dc.user_code == "BCDF-GHJK" and dc.interval == 5


def test_device_request_error(respx_mock):
    respx_mock.post(DEV_URL).respond(400, json={"error": "invalid_client"})
    with pytest.raises(DeviceFlowError, match="invalid_client"):
        request_device_code(http(), "bad")


def test_poll_pending_slow_down_then_success(respx_mock):
    clock = FakeClock()
    pend = httpx.Response(400, json={"error": "authorization_pending"})
    slow = httpx.Response(400, json={"error": "slow_down"})
    route = respx_mock.post(TOKEN_URL).mock(side_effect=[pend, slow, pend, httpx.Response(200, json=tokens())])
    dc = request_device_code_obj()
    s = poll_for_session(http(), "cid", dc, sleep=clock.sleep, now=clock, mono=clock)
    assert s.access_token == "a1" and s.refresh_token == "r1"
    assert clock.sleeps == [5, 5, 10, 10]        # slow_down adds 5 s permanently
    body = route.calls[0].request.content.decode()
    assert DEVICE_GRANT.replace(":", "%3A") in body and "device_code=DEV" in body


def request_device_code_obj():
    from sawblade_match.t3k.auth import DeviceCode
    return DeviceCode("DEV", "BCDF-GHJK", "u", None, 600, 5)


@pytest.mark.parametrize("err,msg", [("access_denied", "declined"), ("expired_token", "expired"),
                                     ("invalid_grant", "invalid_grant")])
def test_poll_terminal_errors(respx_mock, err, msg):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).respond(400, json={"error": err})
    with pytest.raises(DeviceFlowError, match=msg):
        poll_for_session(http(), "cid", request_device_code_obj(), sleep=clock.sleep, now=clock, mono=clock)


def test_poll_local_deadline(respx_mock):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).respond(400, json={"error": "authorization_pending"})
    with pytest.raises(DeviceFlowError, match="expired"):
        poll_for_session(http(), "cid", request_device_code_obj(), sleep=clock.sleep, now=clock, mono=clock)
    assert clock.t > 1600


def test_token_file_is_0600_and_dir_0700(store):
    store.save(Session("a", "r", 123.0))
    assert stat.S_IMODE(store.path.stat().st_mode) == 0o600
    assert stat.S_IMODE(store.path.parent.stat().st_mode) == 0o700
    store.save(Session("a2", "r2", 456.0))   # overwrite keeps mode
    assert stat.S_IMODE(store.path.stat().st_mode) == 0o600
    assert store.load().refresh_token == "r2"


def test_refresh_when_near_expiry_persists_rotation(respx_mock, store):
    clock = FakeClock()
    route = respx_mock.post(TOKEN_URL).respond(200, json=tokens("a2", "r2"))
    tm = TokenManager("cid", http(), store, now=clock)
    tm.set_session(Session("a1", "r1", clock() + 30))     # inside the 60 s margin
    assert tm.get_access_token() == "a2"
    assert route.call_count == 1
    body = route.calls[0].request.content.decode()
    assert "grant_type=refresh_token" in body and "refresh_token=r1" in body and "client_id=cid" in body
    on_disk = json.loads(store.path.read_text())
    assert on_disk["refresh_token"] == "r2" and on_disk["access_token"] == "a2"
    assert tm.get_access_token() == "a2" and route.call_count == 1   # fresh now


def test_no_refresh_when_token_fresh(respx_mock, store):
    clock = FakeClock()
    route = respx_mock.post(TOKEN_URL).respond(200, json=tokens())
    tm = TokenManager("cid", http(), store, now=clock)
    tm.set_session(Session("a1", "r1", clock() + 600))
    assert tm.get_access_token() == "a1" and route.call_count == 0


def test_refresh_keeps_old_refresh_token_if_not_rotated(respx_mock, store):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).respond(200, json={"access_token": "a2", "expires_in": 3600})
    tm = TokenManager("cid", http(), store, now=clock)
    tm.set_session(Session("a1", "r1", 0))
    tm.get_access_token()
    assert store.load().refresh_token == "r1"


@pytest.mark.parametrize("status", [400, 401])
def test_refresh_failure_means_relogin(respx_mock, store, status):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).respond(status, json={"error": "invalid_grant"})
    tm = TokenManager("cid", http(), store, now=clock)
    tm.set_session(Session("a1", "r1", 0))
    with pytest.raises(ReauthRequired, match="login"):
        tm.get_access_token()
    assert store.load() is None


def test_env_seed_used_when_no_stored_tokens_and_stored_preferred(respx_mock, store):
    clock = FakeClock()
    route = respx_mock.post(TOKEN_URL).respond(200, json=tokens("a9", "r9"))
    tm = TokenManager("cid", http(), store, env_refresh_token="seed", now=clock)
    assert tm.get_access_token() == "a9"
    assert "refresh_token=seed" in route.calls[0].request.content.decode()
    assert store.load().refresh_token == "r9"
    # a new manager prefers the stored session over the env seed
    tm2 = TokenManager("cid", http(), store, env_refresh_token="seed", now=clock)
    assert tm2.get_access_token() == "a9" and route.call_count == 1


def test_not_logged_in(store):
    tm = TokenManager("cid", http(), store)
    with pytest.raises(ReauthRequired):
        tm.get_access_token()


def test_session_repr_hides_tokens():
    assert "SECRET" not in repr(Session("SECRET-a", "SECRET-r", 1.0))


def test_save_chmods_existing_sawblade_dir(tmp_path):
    d = tmp_path / "sawblade"
    d.mkdir(mode=0o755)
    d.chmod(0o755)
    TokenStore(d / "t.json").save(Session("a", "r", 1.0))
    assert stat.S_IMODE(d.stat().st_mode) == 0o700


def test_save_does_not_chmod_unrelated_existing_dir(tmp_path):
    d = tmp_path / "shared"
    d.mkdir()
    d.chmod(0o755)
    TokenStore(d / "t.json").save(Session("a", "r", 1.0))
    assert stat.S_IMODE(d.stat().st_mode) == 0o755


def test_poll_survives_transient_errors(respx_mock):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).mock(side_effect=[
        httpx.ConnectError("x"), httpx.Response(503), httpx.Response(200, json=tokens())])
    s = poll_for_session(http(), "cid", request_device_code_obj(), sleep=clock.sleep, now=clock, mono=clock)
    assert s.access_token == "a1"


# ---- client_id persisted in the token file (read by the C++ settings) ----

def _login(respx_mock, monkeypatch, tmp_path, cid):
    import functools
    from sawblade_match.t3k import cli
    from sawblade_match.t3k.auth import poll_for_session as real_poll
    clock = FakeClock()
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    monkeypatch.setenv("TONE3000_CLIENT_ID", cid)
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    path = tmp_path / "cfg" / "tokens.json"
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(path))
    monkeypatch.setattr(cli, "poll_for_session",
                        functools.partial(real_poll, sleep=clock.sleep, now=clock, mono=clock))
    respx_mock.post(DEV_URL).respond(200, json=device_body())
    respx_mock.post(TOKEN_URL).respond(200, json=tokens())
    return cli, path


def test_login_stores_publishable_client_id(respx_mock, monkeypatch, tmp_path, capsys):
    cli, path = _login(respx_mock, monkeypatch, tmp_path, "t3k_pub_abc")
    assert cli.main(["login"]) == 0
    assert json.loads(path.read_text())["client_id"] == "t3k_pub_abc"
    assert TokenStore(path).load().client_id == "t3k_pub_abc"


def test_login_with_secret_key_stores_nothing(respx_mock, monkeypatch, tmp_path, capsys):
    cli, path = _login(respx_mock, monkeypatch, tmp_path, "t3k_cs_secret")
    assert cli.main(["login"]) != 0           # refused up front
    assert not path.exists()


def test_store_never_writes_secret_looking_client_id(store):
    store.save(Session("a", "r", 1.0, client_id="t3k_cs_secret"))
    assert "client_id" not in json.loads(store.path.read_text())
    assert "t3k_cs_" not in store.path.read_text()


def test_load_ignores_secret_client_id_in_file(store):
    store.path.parent.mkdir(parents=True)
    store.path.write_text(json.dumps({"access_token": "a", "refresh_token": "r", "expires_at": 1.0,
                                      "client_id": "t3k_cs_secret"}))
    assert store.load().client_id is None


def test_old_token_file_without_client_id_loads_and_refreshes(respx_mock, store):
    clock = FakeClock()
    store.path.parent.mkdir(parents=True)
    store.path.write_text(json.dumps({"access_token": "a1", "refresh_token": "r1", "expires_at": 0.0,
                                      "token_type": "bearer", "scope": "read"}))
    s = store.load()
    assert s.access_token == "a1" and s.client_id is None
    respx_mock.post(TOKEN_URL).respond(200, json=tokens("a2", "r2"))
    tm = TokenManager("cid", http(), store, now=clock)
    assert tm.get_access_token() == "a2"
    on_disk = json.loads(store.path.read_text())
    assert on_disk["refresh_token"] == "r2" and "client_id" not in on_disk


def test_refresh_preserves_client_id(respx_mock, store):
    clock = FakeClock()
    respx_mock.post(TOKEN_URL).respond(200, json=tokens("a2", "r2"))
    tm = TokenManager("cid", http(), store, now=clock)
    tm.set_session(Session("a1", "r1", 0.0, client_id="t3k_pub_abc"))
    assert tm.get_access_token() == "a2"
    assert json.loads(store.path.read_text())["client_id"] == "t3k_pub_abc"
    # a fresh manager (loads from disk) also keeps it across another rotation
    respx_mock.post(TOKEN_URL).respond(200, json=tokens("a3", "r3"))
    store.save(Session("a2", "r2", 0.0, client_id="t3k_pub_abc"))
    tm2 = TokenManager("cid", http(), store, now=clock)
    assert tm2.get_access_token() == "a3"
    assert json.loads(store.path.read_text())["client_id"] == "t3k_pub_abc"
