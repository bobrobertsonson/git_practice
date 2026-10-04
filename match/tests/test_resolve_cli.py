import functools
import hashlib
import json
import stat

import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.cache import Cache
from sawblade_match.t3k.resolve import resolve_preset
from conftest import BASE, SECRET_ACCESS, FakeClock, model_json, tone_json

PRESET = {
    "schema": "sawblade.preset", "version": 1,
    "paths": {"a": {"blocks": [
        {"id": "a1", "type": "nam", "model": {"file": "captures/old.nam",
                                              "source": {"provider": "tone3000", "id": "200", "modelId": "2001"}}},
        {"id": "a2", "type": "nam", "model": {"file": "captures/local.nam"}},
        {"id": "a3", "type": "nam", "model": {"file": "captures/other.nam",
                                              "source": {"provider": "tone3000", "id": "300"}}}]}},
    "cab": {"ir": {"file": "captures/cab.wav", "source": {"provider": "somewhere", "id": "x"}}},
}


@pytest.fixture
def world(api):
    t1 = tone_json(200, gear="amp", title="Resolved Amp", user="carol", license="cc-by-sa")
    t1["user"]["display_name"] = "Carol Verified"
    api.add_tone(t1, [model_json(2001, 200, size="lite"), model_json(2002, 200, size="standard")])
    t2 = tone_json(300, gear="pedal", title="Pedal", user="dave", license="t3k")
    api.add_tone(t2, [model_json(3001, 300, size="lite"), model_json(3002, 300, size="standard")])
    return api


def test_resolve_rewrites_preset(make_client, world, tmp_path):
    cache = Cache(tmp_path / "cache")
    preset = json.loads(json.dumps(PRESET))
    done = resolve_preset(make_client(), cache, preset, first_model=True)
    assert done == ["paths.a.blocks[0].model", "paths.a.blocks[2].model"]

    m1 = preset["paths"]["a"]["blocks"][0]["model"]
    assert m1["file"] == str(tmp_path / "cache" / "200" / "2001.nam")
    assert m1["sha256"] == hashlib.sha256(b"FILE-2001").hexdigest()
    assert m1["source"] == {"provider": "tone3000", "id": "200", "modelId": "2001",
                            "url": "https://www.tone3000.com/tones/tone-200", "title": "Resolved Amp",
                            "creator": "Carol Verified", "license": "cc-by-sa"}
    m3 = preset["paths"]["a"]["blocks"][2]["model"]           # no modelId + --first-model -> first
    assert m3["source"]["modelId"] == "3001" and m3["file"].endswith("300/3001.nam")
    assert m3["source"]["creator"] == "dave" and m3["source"]["license"] == "t3k"
    assert preset["paths"]["a"]["blocks"][1]["model"] == {"file": "captures/local.nam"}   # untouched
    assert preset["cab"]["ir"]["file"] == "captures/cab.wav"                              # other provider
    assert (tmp_path / "cache" / "200" / "2001.nam").read_bytes() == b"FILE-2001"


def test_resolve_second_run_is_offline(make_client, world, tmp_path):
    cache = Cache(tmp_path / "cache")
    once = json.loads(json.dumps(PRESET))
    resolve_preset(make_client(), cache, once, first_model=True)
    n = len(world.calls)
    resolve_preset(make_client(), cache, once)          # every capture now has modelId and is cached
    assert len(world.calls) == n


def test_resolve_ambiguous_models_fails_with_listing(make_client, world, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    p = {"m": {"file": "x", "source": {"provider": "tone3000", "id": "300"}}}
    with pytest.raises(T3KError) as e:
        resolve_preset(make_client(), Cache(tmp_path / "c"), p)
    msg = str(e.value)
    assert "3001 (m3001)" in msg and "3002 (m3002)" in msg and "--first-model" in msg
    assert "sha256" not in p["m"] and not (tmp_path / "c" / "300").exists()


def test_resolve_single_model_tone_needs_no_modelid(make_client, world, tmp_path):
    t = tone_json(500, gear="amp")
    world.add_tone(t, [model_json(5001, 500)])
    p = {"m": {"file": "x", "source": {"provider": "tone3000", "id": "500"}}}
    resolve_preset(make_client(), Cache(tmp_path / "c"), p)
    assert p["m"]["source"]["modelId"] == "5001" and p["m"]["file"].endswith("500/5001.nam")


def test_resolve_rejects_model_of_other_tone(make_client, world, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    p = {"m": {"file": "x", "source": {"provider": "tone3000", "id": "300", "modelId": "2001"}}}
    with pytest.raises(T3KError, match="belongs to tone"):
        resolve_preset(make_client(), Cache(tmp_path / "c"), p)


@pytest.fixture
def cli_env(monkeypatch, tmp_path, respx_mock):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_pub_test")
    monkeypatch.setenv("TONE3000_BASE_URL", BASE)
    monkeypatch.setenv("SAWBLADE_T3K_TOKEN_FILE", str(tmp_path / "tok" / "t3k_tokens.json"))
    monkeypatch.delenv("TONE3000_REFRESH_TOKEN", raising=False)
    clock = FakeClock()
    monkeypatch.setattr(cli, "poll_for_session",
                        functools.partial(cli.poll_for_session, sleep=clock.sleep, mono=clock))
    return tmp_path / "tok" / "t3k_tokens.json"


def test_cli_login_whoami_resolve(cli_env, respx_mock, api, world, tmp_path, capsys):
    import httpx
    respx_mock.post(f"{BASE}/api/v1/oauth/device_authorization").respond(200, json={
        "device_code": "DEVSECRET", "user_code": "BCDF-GHJK", "verification_uri": "https://www.tone3000.com/activate",
        "verification_uri_complete": "https://www.tone3000.com/activate?c=1", "expires_in": 600, "interval": 5})
    respx_mock.post(f"{BASE}/api/v1/oauth/token").mock(side_effect=[
        httpx.Response(400, json={"error": "authorization_pending"}),
        httpx.Response(200, json={"access_token": SECRET_ACCESS, "refresh_token": "REFRESH-ONCE",
                                  "expires_in": 3600, "token_type": "bearer", "scope": "read"})])
    assert cli.main(["login"]) == 0
    out = capsys.readouterr().out
    assert "BCDF-GHJK" in out and "TONE3000_REFRESH_TOKEN" in out and "REFRESH-ONCE" in out
    assert SECRET_ACCESS not in out and "DEVSECRET" not in out
    assert stat.S_IMODE(cli_env.stat().st_mode) == 0o600

    assert cli.main(["whoami"]) == 0
    out = capsys.readouterr()
    assert "alice" in out.out and SECRET_ACCESS not in out.out + out.err

    p = tmp_path / "preset.json"
    p.write_text(json.dumps(PRESET))
    o = tmp_path / "out.json"
    assert cli.main(["resolve", str(p), "-o", str(o), "--cache-dir", str(tmp_path / "cc")]) == 1
    assert "3001 (m3001)" in capsys.readouterr().err and not o.exists()   # ambiguous tone 300
    assert cli.main(["resolve", str(p), "-o", str(o), "--cache-dir", str(tmp_path / "cc"), "--first-model"]) == 0
    assert json.loads(p.read_text()) == PRESET                         # input untouched with -o
    res = json.loads(o.read_text())
    assert res["paths"]["a"]["blocks"][0]["model"]["sha256"]
    assert SECRET_ACCESS not in capsys.readouterr().out

    # default output: <name>.resolved.json next to the input; input never rewritten
    assert cli.main(["resolve", str(p), "--cache-dir", str(tmp_path / "cc"), "--first-model"]) == 0
    assert json.loads(p.read_text()) == PRESET
    assert json.loads((tmp_path / "preset.resolved.json").read_text()) == res


def test_cache_meta_stores_creator_username(make_client, world, tmp_path):
    cache = Cache(tmp_path / "cache")
    resolve_preset(make_client(), cache, json.loads(json.dumps(PRESET)), first_model=True)
    assert cache.read_meta(200)["creatorUsername"] == "carol"


def test_store_reads_lead_login_format(tmp_path):
    from sawblade_match.t3k.auth import TokenStore
    p = tmp_path / "t.json"
    p.write_text(json.dumps({"access_token": "a", "refresh_token": "r", "expires_in": 3600,
                             "token_type": "bearer", "scope": "read", "obtained_at": 1000.0}))
    s = TokenStore(p).load()
    assert s.expires_at == 4600.0 and s.refresh_token == "r"


def test_cli_pull_writes_manifest_and_table(cli_env, api, tmp_path, capsys):
    cli_env.parent.mkdir(parents=True)
    from sawblade_match.t3k.auth import Session, TokenStore
    import time
    TokenStore(cli_env).save(Session(SECRET_ACCESS, "r", time.time() + 3600))
    t = tone_json(400, gear="amp", fav=150, dl=2000, title="Pull Amp")
    api.favorited = [t]
    api.add_tone(t, [model_json(4001, 400)])
    # published_at is relative to the real clock in the CLI, so make the fixture recent
    from datetime import datetime, timezone
    t["published_at"] = datetime.now(timezone.utc).isoformat()
    mp = tmp_path / "m.json"
    assert cli.main(["pull", "--favorites", "--gear", "amp", "--no-trending", "--no-latest",
                     "--cache-dir", str(tmp_path / "cc"), "--manifest", str(mp)]) == 0
    out = capsys.readouterr().out
    assert "Pull Amp" in out and "1 included" in out
    m = json.loads(mp.read_text())
    assert m["tones"][0]["tone_id"] == 400 and (tmp_path / "cc" / "400" / "4001.nam").exists()
    assert SECRET_ACCESS not in out


def test_cli_missing_client_id(monkeypatch, capsys):
    monkeypatch.delenv("TONE3000_CLIENT_ID", raising=False)
    assert cli.main(["whoami"]) == 1
    assert "TONE3000_CLIENT_ID" in capsys.readouterr().err


def test_cli_rejects_secret_key_as_client_id(monkeypatch, capsys):
    monkeypatch.setenv("TONE3000_CLIENT_ID", "t3k_cs_nope")
    assert cli.main(["whoami"]) == 1
    assert "SECRET" in capsys.readouterr().err


def test_cli_search_flag_warns(cli_env, api, tmp_path, capsys):
    from sawblade_match.t3k.auth import Session, TokenStore
    import time
    TokenStore(cli_env).save(Session(SECRET_ACCESS, "r", time.time() + 3600))
    assert cli.main(["pull", "--search", "plexi", "--no-trending", "--no-latest", "--no-download",
                     "--cache-dir", str(tmp_path / "cc"), "--manifest", str(tmp_path / "m.json")]) == 0
    assert "personal, non-commercial" in capsys.readouterr().err


def _nc_world(api, lic="cc-by-nc"):
    t = tone_json(800, gear="amp", license=lic)
    api.add_tone(t, [model_json(8001, 800)])
    return {"m": {"file": "x", "source": {"provider": "tone3000", "id": "800", "modelId": "8001"}}}


@pytest.mark.parametrize("lic", ["cc-by-nc", "cc-by-nc-sa", "cc-by-nc-nd", "weird"])
def test_resolve_refuses_disallowed_license_without_download(make_client, api, tmp_path, lic):
    from sawblade_match.t3k.errors import T3KError
    p = _nc_world(api, lic)
    with pytest.raises(T3KError, match="refused"):
        resolve_preset(make_client(), Cache(tmp_path / "c"), p)
    assert not api.requests("download") and not (tmp_path / "c" / "800").exists()


def test_resolve_refuses_disallowed_license_on_cache_hit(make_client, api, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    import json as _json
    cache = Cache(tmp_path / "c")
    p = _nc_world(api, "cc-by")
    resolve_preset(make_client(), cache, p)
    meta = cache.read_meta(800)
    meta["tone"]["license"] = "cc-by-nc"          # e.g. the tone's license was changed upstream
    (cache.tone_dir(800) / "meta.json").write_text(_json.dumps(meta))
    n = len(api.calls)
    p2 = {"m": {"file": "x", "source": {"provider": "tone3000", "id": "800", "modelId": "8001"}}}
    with pytest.raises(T3KError, match="non_commercial"):
        resolve_preset(make_client(), cache, p2)
    assert len(api.calls) == n


@pytest.mark.parametrize("bad", ["../..", "12/../3", "abc", "", "-1", "1.5"])
def test_resolve_rejects_non_numeric_ids(make_client, api, tmp_path, bad):
    from sawblade_match.t3k.errors import T3KError
    for src in ({"provider": "tone3000", "id": bad or " "}, {"provider": "tone3000", "id": "5", "modelId": bad or " "}):
        with pytest.raises(T3KError, match="invalid"):
            resolve_preset(make_client(), Cache(tmp_path / "c"), {"m": {"file": "x", "source": src}})
    assert not api.calls


def test_cache_paths_reject_traversal(tmp_path):
    from sawblade_match.t3k.errors import T3KError
    with pytest.raises(T3KError):
        Cache(tmp_path).path_for("../x", 1, "nam")


def test_resolve_output_equal_to_input_refused(make_client, world, tmp_path):
    from sawblade_match.t3k.errors import T3KError
    from sawblade_match.t3k.resolve import resolve_file
    p = tmp_path / "preset.json"
    p.write_text(json.dumps(PRESET))
    with pytest.raises(T3KError, match="equals the input"):
        resolve_file(make_client(), Cache(tmp_path / "c"), p, tmp_path / "." / "preset.json", True)
    assert json.loads(p.read_text()) == PRESET and not world.calls
