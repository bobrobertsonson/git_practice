"""Spec 8a: `models`, `fetch`, `list`, `whoami --json`, `login --json-events`, `--json` error objects.

Fully offline (respx fake API from conftest)."""
import functools
import hashlib
import json
import time

import httpx
import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.auth import Session, TokenStore
from conftest import BASE, SECRET_ACCESS, FakeClock, model_json, tone_json


@pytest.fixture
def run(make_client, monkeypatch, capsys):
    """Run the CLI with the fake-API client; returns (exit status, parsed stdout JSON, stderr)."""
    monkeypatch.setattr(cli, "make_client", make_client)

    def _run(*argv):
        rc = cli.main(list(argv))
        cap = capsys.readouterr()
        out = json.loads(cap.out) if cap.out.strip() else None   # exactly one document, nothing else
        return rc, out, cap.err
    return _run


@pytest.fixture
def world(api):
    api.add_tone(tone_json(10, gear="amp", title="Body Amp", user="carol", license="cc-by-sa"),
                 [model_json(101, 10, size="lite"), model_json(102, 10, size="standard")])
    api.add_tone(tone_json(11, gear="ir", fmt="ir", title="Cab", user="dave", license="cco"),
                 [model_json(111, 11, size="standard")])
    api.add_tone(tone_json(12, gear="pedal", license="cc-by-nc", title="NC Pedal"), [model_json(121, 12)])
    api.add_tone(tone_json(13, gear="pedal", license="mystery", title="Odd Pedal"), [model_json(131, 13)])
    api.add_tone(tone_json(14, gear="amp", a2=0, a1=2, title="Old Amp"),
                 [model_json(141, 14, arch="1"), model_json(142, 14, arch="1", size="lite")])
    return api


def test_models_shape(run, world):
    rc, out, _ = run("models", "10", "--json")
    assert rc == 0
    assert out == {"tone_id": 10, "architecture": "2", "models": [
        {"model_id": 101, "name": "m101", "size": "lite"}, {"model_id": 102, "name": "m102", "size": "standard"}]}
    rc, out, _ = run("models", "14", "--json")          # A1 fallback when there is no A2
    assert out["architecture"] == "1" and [m["model_id"] for m in out["models"]] == [141, 142]
    rc, out, _ = run("models", "11", "--json")          # IR tone
    assert out["tone_id"] == 11 and out["models"] == [{"model_id": 111, "name": "m111", "size": "standard"}]
    assert not world.requests("download")                # read-only


def test_models_empty_size_is_null(run, world):
    world.add_tone(tone_json(15, gear="amp"), [model_json(151, 15, size="")])
    assert run("models", "15", "--json")[1]["models"][0]["size"] is None


def test_models_unknown_tone_not_found(run, world):
    rc, out, _ = run("models", "999", "--json")
    assert rc == 1 and out["code"] == "not_found" and out["error"]


def test_fetch_result_shape_and_cache_hit(run, world, tmp_path):
    cache = tmp_path / "cc"
    rc, out, _ = run("fetch", "10", "--model", "102", "--json", "--cache-dir", str(cache))
    assert rc == 0
    path = cache / "10" / "102.nam"
    assert out == {
        "tone_id": 10, "model_id": 102, "path": str(path.resolve()),
        "sha256": hashlib.sha256(b"FILE-102").hexdigest(), "kind": "nam", "gear": "amp",
        "source": {"provider": "tone3000", "id": "10", "modelId": "102",
                   "url": "https://www.tone3000.com/tones/tone-10", "title": "Body Amp",
                   "creator": "carol", "license": "cc-by-sa"}}
    assert path.read_bytes() == b"FILE-102"
    assert len(world.requests("download")) == 1

    rc, again, _ = run("fetch", "10", "--model", "102", "--json", "--cache-dir", str(cache))
    assert rc == 0 and again == out
    assert len(world.requests("download")) == 1          # cache hit: no second download


def test_fetch_defaults_to_first_candidate_and_ir_kind(run, world, tmp_path):
    cache = str(tmp_path / "cc")
    assert run("fetch", "10", "--json", "--cache-dir", cache)[1]["model_id"] == 101
    out = run("fetch", "11", "--json", "--cache-dir", cache)[1]
    assert out["kind"] == "ir" and out["gear"] == "ir" and out["path"].endswith("11/111.wav")


@pytest.mark.parametrize("tone", ["12", "13"])        # cc-by-nc and an unknown licence
def test_fetch_refuses_nc_and_unknown_license(run, world, tmp_path, tone):
    cache = tmp_path / "cc"
    rc, out, _ = run("fetch", tone, "--json", "--cache-dir", str(cache))
    assert rc == 1 and out["code"] == "license" and "refused" in out["error"]
    assert not world.requests("download") and not world.requests("/models")
    assert not cache.exists() or not any(cache.rglob("*.*"))


def test_fetch_model_not_in_list_is_not_found(run, world, tmp_path):
    cache = tmp_path / "cc"
    rc, out, _ = run("fetch", "10", "--model", "111", "--json", "--cache-dir", str(cache))   # other tone's model
    assert rc == 1 and out["code"] == "not_found"
    assert not world.requests("download") and not cache.exists()


def test_error_codes_auth_network_error(make_client, monkeypatch, capsys, world):
    monkeypatch.setattr(cli, "make_client", lambda: make_client(access=None))   # not logged in
    assert cli.main(["models", "10", "--json"]) == 1
    assert json.loads(capsys.readouterr().out)["code"] == "auth"

    monkeypatch.delenv("TONE3000_CLIENT_ID", raising=False)
    monkeypatch.undo()
    monkeypatch.delenv("TONE3000_CLIENT_ID", raising=False)
    assert cli.main(["whoami", "--json"]) == 1
    out = json.loads(capsys.readouterr().out)
    assert out["code"] == "auth" and "TONE3000_CLIENT_ID" in out["error"]

    def boom():
        raise httpx.ConnectError("no route")
    monkeypatch.setattr(cli, "make_client", boom)
    assert cli.main(["search", "x", "--json"]) == 1
    out = json.loads(capsys.readouterr().out)
    assert out["code"] == "network" and "ConnectError" in out["error"]

    def odd():
        from sawblade_match.t3k.errors import T3KError
        raise T3KError("something else")
    monkeypatch.setattr(cli, "make_client", odd)
    assert cli.main(["fetch", "1", "--json"]) == 1
    assert json.loads(capsys.readouterr().out)["code"] == "error"


def test_text_errors_unchanged_without_json(run, world, capsys):
    rc, out, err = run("fetch", "12")
    assert rc == 1 and out is None and err.startswith("error: ") and "non_commercial" in err


def test_whoami_json(run, world):
    rc, out, _ = run("whoami", "--json")
    assert rc == 0 and out == {"id": 7, "username": "alice", "display_name": None}


def test_list_favorites_shape_filter_and_query(run, world):
    good = tone_json(20, gear="amp", fav=300, dl=5000, title="Fav Amp", license="cc-by")
    nc = tone_json(21, gear="pedal", fav=300, dl=5000, title="Fav NC", license="cc-by-nc")
    ir = tone_json(22, gear="ir", fmt="ir", fav=300, dl=5000, title="Fav IR", license="t3k")
    world.favorited = [good, nc, ir]
    rc, out, _ = run("list", "--source", "favorites", "--json")
    assert rc == 0
    assert [(r["tone_id"], r["passes"]) for r in out][:2] == [(20, True), (21, False)]
    assert out[1]["reasons"][0] == "non_commercial_license:cc-by-nc"
    # same record shape as `search --json`
    world.search_results = [good]
    search_rec = run("search", "x", "--json")[1][0]
    assert set(out[0]) == set(search_rec) and out[0] == search_rec
    assert world.requests("favorited")[0].url.params.get("query") is None

    out = run("list", "--source", "favorites", "--query", "amp", "--gear", "amp", "--limit", "5", "--json")[1]
    assert world.requests("favorited")[-1].url.params["query"] == "amp"
    assert [r["tone_id"] for r in out] == [20]
    assert [r["tone_id"] for r in run("list", "--source", "favorites", "--gear", "ir", "--json")[1]] == [22]
    assert len(run("list", "--source", "favorites", "--limit", "1", "--json")[1]) == 1


def test_list_pool_without_manifest_is_empty_and_offline(run, world, tmp_path):
    n = len(world.calls)
    rc, out, _ = run("list", "--source", "pool", "--json", "--cache-dir", str(tmp_path / "none"))
    assert rc == 0 and out == [] and len(world.calls) == n


def test_list_pool_with_manifest(run, world, tmp_path):
    cache = tmp_path / "cc"
    cache.mkdir()
    entry = {"tone_id": 30, "title": "Pool Amp", "creator": "erin", "gear": "amp", "format": "nam",
             "license": "cc-by", "url": "https://www.tone3000.com/tones/tone-30", "favorites_count": 200,
             "downloads_count": 3000, "published_at": "2026-05-01T00:00:00Z", "a2_models_count": 2,
             "a1_models_count": 0, "irs_count": 0, "status": "included", "reasons": [], "flags": ["x"],
             "models": [{"id": 1, "name": "a", "size": "lite"}, {"id": 2, "name": "b", "size": "standard"}]}
    sparse = {"tone_id": 31, "title": "Bare IR", "gear": "ir"}
    (cache / "pool_manifest.json").write_text(json.dumps({"tones": [entry, sparse], "excluded": [
        {"tone_id": 32, "title": "no", "gear": "amp"}]}))
    n = len(world.calls)
    rc, out, _ = run("list", "--source", "pool", "--json", "--cache-dir", str(cache))
    assert rc == 0 and len(world.calls) == n
    assert [r["tone_id"] for r in out] == [30, 31]
    assert out[0] == {
        "tone_id": 30, "title": "Pool Amp", "creator": "erin", "gear": "amp", "format": "nam", "license": "cc-by",
        "favorites_count": 200, "downloads_count": 3000, "created_at": "2026-05-01T00:00:00Z", "models_count": 2,
        "a2_models_count": 2, "a1_models_count": 0, "irs_count": 0, "sizes": ["lite", "standard"],
        "url": "https://www.tone3000.com/tones/tone-30", "passes": True, "status": "included", "reasons": [],
        "flags": ["x"]}
    assert out[1]["creator"] is None and out[1]["license"] is None and out[1]["models_count"] is None
    assert set(out[0]) == set(out[1])
    assert [r["tone_id"] for r in run("list", "--source", "pool", "--gear", "ir", "--json",
                                     "--cache-dir", str(cache))[1]] == [31]
    assert [r["tone_id"] for r in run("list", "--source", "pool", "--query", "POOL", "--json",
                                     "--cache-dir", str(cache))[1]] == [30]
    (cache / "pool_manifest.json").write_text("{not json")        # corrupt manifest: still not an error
    assert run("list", "--source", "pool", "--json", "--cache-dir", str(cache))[1] == []


def test_search_json_error_object(run, world, monkeypatch):
    from sawblade_match.t3k.errors import ReauthRequired
    monkeypatch.setattr(cli, "make_client", lambda: (_ for _ in ()).throw(ReauthRequired("not logged in")))
    rc, out, _ = run("search", "x", "--json")
    assert rc == 1 and out == {"error": "not logged in", "code": "auth"}


@pytest.fixture
def login_env(monkeypatch, tmp_path, respx_mock):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_test")
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(tmp_path / "tok" / "t3k_tokens.json"))
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    clock = FakeClock()
    monkeypatch.setattr(cli, "poll_for_session",
                        functools.partial(cli.poll_for_session, sleep=clock.sleep, mono=clock))
    return tmp_path / "tok" / "t3k_tokens.json"


def test_login_json_events_never_prints_refresh_token(login_env, respx_mock, capsys):
    respx_mock.post(f"{BASE}/api/v1/oauth/device_authorization").respond(200, json={
        "device_code": "DEVSECRET", "user_code": "BCDF-GHJK", "verification_uri": "https://www.tone3000.com/activate",
        "verification_uri_complete": "https://www.tone3000.com/activate?c=1", "expires_in": 600, "interval": 5})
    respx_mock.post(f"{BASE}/api/v1/oauth/token").mock(side_effect=[
        httpx.Response(400, json={"error": "authorization_pending"}),
        httpx.Response(200, json={"access_token": SECRET_ACCESS, "refresh_token": "REFRESH-ONCE",
                                  "expires_in": 3600, "token_type": "bearer", "scope": "read"})])
    assert cli.main(["login", "--json-events"]) == 0
    cap = capsys.readouterr()
    lines = [json.loads(l) for l in cap.out.splitlines()]
    assert lines == [
        {"event": "device_code", "verification_uri": "https://www.tone3000.com/activate",
         "verification_uri_complete": "https://www.tone3000.com/activate?c=1", "user_code": "BCDF-GHJK",
         "expires_in": 600},
        {"event": "logged_in"}]
    for secret in ("REFRESH-ONCE", SECRET_ACCESS, "DEVSECRET"):
        assert secret not in cap.out + cap.err
    saved = TokenStore(login_env).load()                  # still persisted for the CLI itself
    assert saved is not None and saved.refresh_token == "REFRESH-ONCE"


def test_login_json_events_error_is_json(login_env, respx_mock, capsys):
    respx_mock.post(f"{BASE}/api/v1/oauth/device_authorization").respond(400, json={"error": "invalid_client"})
    assert cli.main(["login", "--json-events"]) == 1
    out = json.loads(capsys.readouterr().out)
    assert out["code"] == "auth" and "REFRESH" not in out["error"]


def test_whoami_json_after_login_store(login_env, api, capsys):
    login_env.parent.mkdir(parents=True)
    TokenStore(login_env).save(Session(SECRET_ACCESS, "r", time.time() + 3600))
    assert cli.main(["whoami", "--json"]) == 0
    cap = capsys.readouterr()
    assert json.loads(cap.out) == {"id": 7, "username": "alice", "display_name": None}
    assert SECRET_ACCESS not in cap.out + cap.err


def test_fetch_cache_hit_still_looks_up_tone_and_refuses_changed_license(run, world, tmp_path):
    cache = str(tmp_path / "cc")
    run("fetch", "10", "--model", "102", "--json", "--cache-dir", cache)
    before = len(world.requests(r"/tones/10$"))
    rc, out, _ = run("fetch", "10", "--model", "102", "--json", "--cache-dir", cache)
    assert rc == 0 and len(world.requests(r"/tones/10$")) == before + 1      # tone lookup happens
    assert len(world.requests("download")) == 1
    # the cached copy's tone licence is now non-commercial (and the live lookup agrees)
    meta_path = tmp_path / "cc" / "10" / "meta.json"
    meta = json.loads(meta_path.read_text())
    meta["tone"]["license"] = "cc-by-nc"
    meta_path.write_text(json.dumps(meta))
    world.tones[10]["license"] = "cc-by-nc"
    rc, out, _ = run("fetch", "10", "--model", "102", "--json", "--cache-dir", cache)
    assert rc == 1 and out["code"] == "license"
    assert len(world.requests("download")) == 1


def test_models_empty_result(run, world):
    world.add_tone(tone_json(16, gear="amp", a2=0, a1=0), [])
    rc, out, _ = run("models", "16", "--json")
    assert rc == 0 and out == {"tone_id": 16, "architecture": "", "models": []}


def test_json_catch_all(run, monkeypatch):
    def boom():
        raise RuntimeError("kaboom")
    monkeypatch.setattr(cli, "make_client", boom)
    rc, out, err = run("models", "1", "--json")
    assert rc == 1 and out == {"error": "RuntimeError: kaboom", "code": "error"} and "Traceback" not in err
    rc, out, err = run("-v", "models", "1", "--json")
    assert rc == 1 and out["code"] == "error" and "Traceback" in err
    with pytest.raises(RuntimeError):                      # text mode unchanged
        cli.main(["models", "1"])
