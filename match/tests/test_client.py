import hashlib
import json
import logging

import httpx
import pytest

from sawblade_match.t3k.auth import Session
from sawblade_match.t3k.cache import Cache
from sawblade_match.t3k.client import TokenBucket
from sawblade_match.t3k.errors import RateLimitedError, ReauthRequired
from sawblade_match.t3k.fetch import ensure_capture
from conftest import BASE, SECRET_ACCESS, SECRET_REFRESH, FakeClock, model_json, tone_json

TOKEN_URL = f"{BASE}/api/v1/oauth/token"


def test_401_refresh_retry(respx_mock, make_client, api, store):
    client = make_client(access="stale-token", refresh="r-old")
    respx_mock.post(TOKEN_URL).respond(200, json={"access_token": SECRET_ACCESS, "refresh_token": "r-new",
                                                  "expires_in": 3600, "token_type": "bearer"})
    u = client.get_user()
    assert u.username == "alice"
    users = api.requests(r"/api/v1/user$")
    assert [r.headers["authorization"] for r in users] == ["Bearer stale-token", f"Bearer {SECRET_ACCESS}"]
    assert store.load().refresh_token == "r-new"          # rotation persisted


def test_401_after_refresh_gives_up(respx_mock, make_client, api):
    client = make_client(access="stale")
    respx_mock.post(TOKEN_URL).respond(200, json={"access_token": "still-bad", "refresh_token": "r2", "expires_in": 3600})
    with pytest.raises(ReauthRequired):
        client.get_user()


def test_proactive_refresh_before_request(respx_mock, make_client, api, clock):
    client = make_client(access="soon-expired", expires_at=clock() + 10)
    route = respx_mock.post(TOKEN_URL).respond(200, json={"access_token": SECRET_ACCESS, "refresh_token": "r2",
                                                          "expires_in": 3600})
    client.get_user()
    assert route.call_count == 1
    assert len(api.requests(r"/api/v1/user$")) == 1


def test_429_backoff_exponential_then_success(respx_mock, make_client, clock):
    client = make_client()
    route = respx_mock.get(f"{BASE}/api/v1/user").mock(side_effect=[
        httpx.Response(429), httpx.Response(429), httpx.Response(200, json={"id": 1, "username": "u"})])
    assert client.get_user().id == 1
    assert route.call_count == 3
    assert clock.sleeps == [1.0, 2.0]


def test_429_honours_retry_after(respx_mock, make_client, clock):
    client = make_client()
    respx_mock.get(f"{BASE}/api/v1/user").mock(side_effect=[
        httpx.Response(429, headers={"Retry-After": "7"}), httpx.Response(200, json={"id": 1, "username": "u"})])
    client.get_user()
    assert clock.sleeps == [7.0]


def test_429_gives_up(respx_mock, make_client, clock):
    client = make_client(max_retries=3)
    route = respx_mock.get(f"{BASE}/api/v1/user").respond(429)
    with pytest.raises(RateLimitedError):
        client.get_user()
    assert route.call_count == 4 and clock.sleeps == [1.0, 2.0, 4.0]


def test_token_bucket_throttles_to_100_per_minute():
    clock = FakeClock()
    b = TokenBucket(100, 60.0, clock, clock.sleep)
    for _ in range(100):
        b.acquire()
    assert clock.sleeps == []                      # burst of 100 is free
    b.acquire()
    assert clock.sleeps and clock.t - 1000 == pytest.approx(0.6, abs=1e-6)   # 1 token / 0.6 s
    for _ in range(99):
        b.acquire()
    assert clock.t - 1000 == pytest.approx(60.0, abs=0.7)   # 200 requests need ~60 s


def test_pagination_follows_total_pages(respx_mock, make_client, api):
    api.favorited = [tone_json(i) for i in range(1, 251)]
    client = make_client()
    tones = client.list_favorited()
    assert [t.id for t in tones] == list(range(1, 251))
    reqs = api.requests("favorited")
    assert [r.url.params["page"] for r in reqs] == ["1", "2", "3"]
    assert all(r.url.params["page_size"] == "100" for r in reqs)


def test_list_limit_and_gear_param(respx_mock, make_client, api):
    api.favorited = [tone_json(i) for i in range(1, 30)]
    client = make_client()
    assert len(client.list_favorited(gear="amp", limit=5)) == 5
    assert api.requests("favorited")[0].url.params["gear"] == "amp"


def test_list_models_always_sends_architecture_and_paginates(respx_mock, make_client, api):
    api.add_tone(tone_json(5), [model_json(i, 5) for i in range(1, 4)])
    client = make_client()
    assert [m.id for m in client.list_models(5, "2")] == [1, 2, 3]
    assert api.requests("/models$")[0].url.params["architecture"] == "2"
    client.get_tone(5)
    assert "architecture" in api.requests(r"/tones/5$")[0].url.params


def test_deprecation_header_logged_once(respx_mock, make_client, caplog):
    client = make_client()
    respx_mock.get(f"{BASE}/api/v1/user").respond(
        200, json={"id": 1, "username": "u"}, headers={"X-Tone3000-Deprecations": "legacy_platform_key"})
    with caplog.at_level(logging.WARNING, logger="sawblade.t3k"):
        client.get_user()
        client.get_user()
    msgs = [r.getMessage() for r in caplog.records if "legacy_platform_key" in r.getMessage()]
    assert len(msgs) == 1


def test_download_bearer_header_cache_and_hit(respx_mock, make_client, api, tmp_path):
    t = tone_json(11, a2=1)
    m = model_json(101, 11)
    api.add_tone(t, [m])
    client = make_client()
    cache = Cache(tmp_path / "cache")
    from sawblade_match.t3k.types import Model, Tone
    tone, model = Tone.from_json(t), Model.from_json(m)

    e = ensure_capture(client, cache, tone, model)
    assert e.path == tmp_path / "cache" / "11" / "101.nam"
    assert e.path.read_bytes() == b"FILE-101"
    assert e.sha256 == hashlib.sha256(b"FILE-101").hexdigest()
    dl = api.requests("download")
    assert len(dl) == 1 and dl[0].headers["authorization"] == f"Bearer {SECRET_ACCESS}"
    meta = json.loads((tmp_path / "cache" / "11" / "meta.json").read_text())
    assert meta["models"]["101"]["sha256"] == e.sha256 and meta["tone"]["id"] == 11
    assert meta["models"]["101"]["fetched_at"] and meta["models"]["101"]["model"]["id"] == 101
    assert not [p for p in e.path.parent.iterdir() if p.name.startswith(".")]   # temp files gone

    n = len(api.calls)
    e2 = ensure_capture(client, cache, tone, model)       # cache hit: zero network
    assert e2.path == e.path and len(api.calls) == n

    e.path.write_bytes(b"corrupt")                         # tampered file -> re-download
    ensure_capture(client, cache, tone, model)
    assert e.path.read_bytes() == b"FILE-101" and len(api.calls) == n + 1


def test_download_failure_leaves_no_partial_file(respx_mock, make_client, tmp_path):
    client = make_client()
    respx_mock.get(f"{BASE}/api/v1/models/1/download").respond(404)
    dest = tmp_path / "x" / "1.nam"
    with pytest.raises(Exception):
        client.download_model(f"{BASE}/api/v1/models/1/download", dest)
    assert not dest.exists()


def test_no_bearer_sent_to_foreign_host(respx_mock, make_client, tmp_path):
    client = make_client()
    route = respx_mock.get("https://cdn.example/m.nam").respond(200, content=b"x")
    client.download_model("https://cdn.example/m.nam", tmp_path / "m.nam")
    assert "authorization" not in route.calls[0].request.headers


def test_search_uses_search_bucket_and_architecture(respx_mock, make_client, clock):
    client = make_client(search_rate_per_min=2)
    respx_mock.get(f"{BASE}/api/v1/tones/search").respond(
        200, json={"data": [tone_json(1)], "page": 1, "page_size": 25, "total": 1, "total_pages": 1})
    for _ in range(3):
        client.search("plexi")
    assert clock.sleeps and sum(clock.sleeps) == pytest.approx(30.0)   # 3rd call waits 30 s at 2/min
    assert "architecture" in respx_mock.calls[0].request.url.params


def test_no_token_or_auth_header_in_logs(respx_mock, make_client, api, store, caplog, capsys):
    caplog.set_level(logging.DEBUG)
    client = make_client(access="stale-SECRET-xyz", refresh="refresh-SECRET-xyz")
    respx_mock.post(TOKEN_URL).respond(200, json={"access_token": SECRET_ACCESS, "refresh_token": "ref-SECRET-new",
                                                  "expires_in": 3600})
    client.get_user()           # exercises 401 -> refresh -> retry with debug logging on
    api.latest = [tone_json(1)]
    client.list_latest()
    out = capsys.readouterr()
    text = caplog.text + out.out + out.err
    for secret in ("SECRET", "Bearer", "authorization", "Authorization"):
        assert secret not in text, secret


def test_transport_error_retried_with_backoff(respx_mock, make_client, clock):
    client = make_client()
    respx_mock.get(f"{BASE}/api/v1/user").mock(side_effect=[
        httpx.ConnectError("boom"), httpx.Response(200, json={"id": 1, "username": "u"})])
    assert client.get_user().id == 1 and clock.sleeps == [1.0]


def _redirect_client(store, clock, **kw):
    from sawblade_match.t3k.auth import TokenManager
    from sawblade_match.t3k.client import T3KClient
    http = httpx.Client(base_url=BASE, timeout=5, follow_redirects=True)     # as in production
    tm = TokenManager("cid", http, store, now=clock)
    tm.set_session(Session(SECRET_ACCESS, SECRET_REFRESH, clock() + 3600))
    return T3KClient(tm, BASE, http=http, sleep=clock.sleep, clock=clock, **kw)


def test_cross_host_redirect_drops_bearer_and_saves_file(respx_mock, store, clock, tmp_path):
    client = _redirect_client(store, clock)
    first = respx_mock.get(f"{BASE}/api/v1/models/1/download").respond(
        302, headers={"Location": "https://api.tone3000.com/s/x.nam"})
    second = respx_mock.get("https://api.tone3000.com/s/x.nam").respond(200, content=b"NAMDATA")
    sha = client.download_model(f"{BASE}/api/v1/models/1/download", tmp_path / "x" / "1.nam")
    assert first.calls[0].request.headers["authorization"] == f"Bearer {SECRET_ACCESS}"
    assert "authorization" not in second.calls[0].request.headers
    assert (tmp_path / "x" / "1.nam").read_bytes() == b"NAMDATA"
    assert sha == hashlib.sha256(b"NAMDATA").hexdigest()


def test_non_https_model_url_refused(respx_mock, make_client, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    client = make_client()
    with pytest.raises(T3KError, match="non-https"):
        client.download_model("http://t3k.test/api/v1/models/1/download", tmp_path / "a.nam")
    assert not respx_mock.calls


def test_unfollowed_redirect_is_error_and_writes_nothing(respx_mock, make_client, tmp_path):
    from sawblade_match.t3k.errors import ApiError
    client = make_client()      # test client does not follow redirects
    respx_mock.get(f"{BASE}/api/v1/models/1/download").respond(302, content=b"<html>moved</html>",
                                                              headers={"Location": "https://elsewhere/x"})
    with pytest.raises(ApiError, match="redirect"):
        client.download_model(f"{BASE}/api/v1/models/1/download", tmp_path / "a.nam")
    assert not (tmp_path / "a.nam").exists()


def test_ensure_capture_refuses_unknown_license_without_download(make_client, api, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    from sawblade_match.t3k.types import Model, Tone
    t, m = tone_json(900, license="gpl-3"), model_json(9001, 900)
    api.add_tone(t, [m])
    cache = Cache(tmp_path / "c")
    with pytest.raises(T3KError, match="unknown_license:gpl-3"):
        ensure_capture(make_client(), cache, Tone.from_json(t), Model.from_json(m))
    assert not api.requests("download") and not (tmp_path / "c" / "900").exists()


def test_ensure_capture_allows_non_commercial_license(make_client, api, tmp_path):
    from sawblade_match.t3k.types import Model, Tone
    t, m = tone_json(902, license="cc-by-nc-sa"), model_json(9021, 902)
    api.add_tone(t, [m])
    ensure_capture(make_client(), Cache(tmp_path / "c"), Tone.from_json(t), Model.from_json(m))
    assert api.requests("download")


def test_ensure_capture_refuses_cached_nc_entry(make_client, api, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    from sawblade_match.t3k.types import Model, Tone
    t, m = tone_json(901, license="cc-by"), model_json(9011, 901)
    api.add_tone(t, [m])
    cache, client = Cache(tmp_path / "c"), make_client()
    ensure_capture(client, cache, Tone.from_json(t), Model.from_json(m))     # fine while cc-by
    meta = cache.read_meta(901)
    meta["tone"]["license"] = "gpl-3"                                          # cached meta now unusable
    (cache.tone_dir(901) / "meta.json").write_text(json.dumps(meta))
    n = len(api.calls)
    with pytest.raises(T3KError, match="unknown_license:gpl-3"):
        ensure_capture(client, cache, Tone.from_json(t), Model.from_json(m))  # fresh tone says cc-by
    assert len(api.calls) == n
