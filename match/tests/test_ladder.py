"""Gain-ladder parser. Fixtures in fixtures/t3k_ladder/ are HAND-WRITTEN from the real naming patterns
(no network recording was available), shaped like the `GET /api/v1/models` response."""
from __future__ import annotations

import json
from pathlib import Path

import httpx
import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.auth import Session, TokenManager
from sawblade_match.t3k.client import T3KClient
from sawblade_match.t3k.ladder import Rung, gain_ladder, parse_ladder
from sawblade_match.t3k.types import Model

FIX = Path(__file__).parent / "fixtures" / "t3k_ladder"
BASE = "https://t3k.test"


def load(name: str) -> list[dict]:
    return json.loads((FIX / f"{name}.json").read_text())["data"]


def models(*names: str, size: str = "standard") -> list[Model]:
    return [Model.from_json({"id": 100 + i, "tone_id": 1, "name": n, "size": size, "model_url": "https://x/y",
                             "architecture_version": "2"}) for i, n in enumerate(names)]


def parse(name: str, size: str = "standard"):
    return parse_ladder([Model.from_json(d) for d in load(name)], size)


def test_clean_ladder_sorted_by_gain():
    r = parse("clean_ladder")
    assert r == [Rung(10011, 3.0, "Amp Gain 3"), Rung(10012, 5.0, "Amp Gain 5"),
                 Rung(10013, 7.0, "Amp Gain 7"), Rung(10014, 9.0, "Amp Gain 9")]


def test_mixed_sizes_only_requested_size_counts():
    assert [x.model_id for x in parse("mixed_sizes", "standard")] == [10021, 10023]
    assert [x.model_id for x in parse("mixed_sizes", "lite")] == [10022, 10024]
    assert parse("mixed_sizes", "feather") is None          # one model of that size
    assert parse("mixed_sizes", "xstandard") is None


@pytest.mark.parametrize("name", ["channel_names", "descriptive_names", "duplicate_gains", "single_model"])
def test_ambiguous_fixtures_are_no_ladder(name):
    assert parse(name) is None


def test_decimals():
    assert [x.gain for x in parse("decimals")] == [4.25, 6.5, 8.0]


@pytest.mark.parametrize("pair", [
    ("Gain 6", "Gain 8"), ("G6", "G8"), ("gain=6", "gain=8"), ("6 gain", "8 gain"), ("Drive 7", "Drive 3"),
    ("@7", "@3"), ("G 6.5", "G 2"), ("Amp - Gain: 6", "Amp - Gain: 8"), ("6g", "8g"), ("GAIN6", "GAIN 9"),
    ("5150 Gain 6", "5150 Gain 8"), ("Gain 10", "Gain 0"),
])
def test_naming_patterns_accepted(pair):
    r = parse_ladder(models(*pair), "standard")
    assert r is not None and len(r) == 2


@pytest.mark.parametrize("names", [
    ("Gain 6", "Gain 6"),                        # duplicate gains
    ("Gain 6", "Gain 8 Bright"),                 # remainders differ
    ("Lead", "Crunch"),                          # no numbers
    ("Gain 6", "Crunch"),                        # one unparsable model poisons the ladder
    ("Gain 5 Drive 7", "Gain 6 Drive 7"),        # two gain-like numbers
    ("Gain 6 / Gain 7", "Gain 8 / Gain 9"),
    ("Clean gain 5", "Lead gain 7"),
    ("Gain 2020", "Gain 2021"),                  # not a knob position
    ("6 gauge", "8 gauge"),
    ("Rhythm @ 6", "Lead @ 8"),
])
def test_never_guess(names):
    assert parse_ladder(models(*names), "standard") is None


def test_other_knobs_are_not_gain_tokens():
    assert parse_ladder(models("Gain 6 Treble 7", "Gain 8 Treble 7"), "standard") is not None
    assert parse_ladder(models("Gain 6 Treble 7", "Gain 8 Treble 5"), "standard") is None


def test_needs_two_models_and_ignores_empty():
    assert parse_ladder([], "standard") is None
    assert parse_ladder(models("Gain 6"), "standard") is None


def test_architecture_filter():
    ms = models("Gain 3", "Gain 6")
    ms.append(Model.from_json({"id": 999, "tone_id": 1, "name": "Gain 9", "size": "standard",
                               "model_url": "https://x/y", "architecture_version": "1"}))
    assert [r.model_id for r in parse_ladder(ms, "standard", "2")] == [100, 101]
    assert parse_ladder(ms, "standard", "1") is None
    assert len(parse_ladder(ms, "standard")) == 3


# ---- client / CLI against a mock transport (no network) -----------------------------------------

def _client(fixture: str, seen: list) -> T3KClient:
    def handler(req: httpx.Request) -> httpx.Response:
        seen.append(req)
        assert req.url.path == "/api/v1/models" and "architecture" in req.url.params
        return httpx.Response(200, json=json.loads((FIX / f"{fixture}.json").read_text()))
    http = httpx.Client(base_url=BASE, transport=httpx.MockTransport(handler))
    tm = TokenManager("t3k_pub_test", http, _NoStore(), None, now=lambda: 1000.0)
    tm.set_session(Session("tok", "ref", 10 ** 9))
    return T3KClient(tm, BASE, http=http, sleep=lambda s: None, clock=lambda: 1000.0)


class _NoStore:
    def load(self): return None
    def save(self, *_a, **_k): pass
    def clear(self): pass


def test_gain_ladder_lists_models_with_architecture():
    seen: list = []
    r = gain_ladder(_client("clean_ladder", seen), 1001, "standard", "2")
    assert [x.gain for x in r] == [3, 5, 7, 9]
    assert seen[0].url.params["tone_id"] == "1001" and seen[0].url.params["architecture"] == "2"
    assert gain_ladder(_client("channel_names", []), "1003", "standard") is None


def test_cli_ladder_json(monkeypatch, capsys):
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _client("clean_ladder", []))
    assert cli.main(["ladder", "1001", "--architecture", "2", "--json"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out == {"tone_id": "1001", "size": "standard", "rungs": [
        {"model_id": "10011", "gain": 3.0, "name": "Amp Gain 3"},
        {"model_id": "10012", "gain": 5.0, "name": "Amp Gain 5"},
        {"model_id": "10013", "gain": 7.0, "name": "Amp Gain 7"},
        {"model_id": "10014", "gain": 9.0, "name": "Amp Gain 9"}]}


def test_cli_ladder_null_and_size(monkeypatch, capsys):
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _client("mixed_sizes", []))
    assert cli.main(["ladder", "1002", "--size", "lite", "--architecture", "2", "--json"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["size"] == "lite" and [r["model_id"] for r in out["rungs"]] == ["10022", "10024"]
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _client("descriptive_names", []))
    assert cli.main(["ladder", "1004", "--architecture", "2", "--json"]) == 0
    assert json.loads(capsys.readouterr().out) == {"tone_id": "1004", "size": "standard", "rungs": None}


def test_cli_ladder_bad_id_is_json_error_without_login(monkeypatch, capsys):
    def boom(*a, **k):
        raise AssertionError("make_client must not be called for an invalid id")
    monkeypatch.setattr(cli, "make_client", boom)
    assert cli.main(["ladder", "12x", "--json"]) == 1
    assert json.loads(capsys.readouterr().out)["code"] == "error"


def _tone_and_models_client(fixture: str | None, seen: list, a2: int = 1, a1: int = 0) -> T3KClient:
    """Serves GET tones/{id} (with architecture counts) and GET models (one fixture for A2, empty for A1)."""
    from conftest import tone_json

    def handler(req: httpx.Request) -> httpx.Response:
        seen.append((req.url.path, dict(req.url.params)))
        if req.url.path.startswith("/api/v1/tones/"):
            return httpx.Response(200, json=tone_json(1001, a2=a2, a1=a1))
        arch = req.url.params["architecture"]
        if fixture and arch == "2":
            return httpx.Response(200, json=json.loads((FIX / f"{fixture}.json").read_text()))
        return httpx.Response(200, json={"data": [], "page": 1, "total_pages": 1})
    http = httpx.Client(base_url=BASE, transport=httpx.MockTransport(handler))
    tm = TokenManager("t3k_pub_test", http, _NoStore(), None, now=lambda: 1000.0)
    tm.set_session(Session("tok", "ref", 10 ** 9))
    return T3KClient(tm, BASE, http=http, sleep=lambda s: None, clock=lambda: 1000.0)


def test_cli_ladder_default_architecture_lists_models_once(monkeypatch, capsys):
    seen: list = []
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _tone_and_models_client("clean_ladder", seen))
    assert cli.main(["ladder", "1001", "--json"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert [r["gain"] for r in out["rungs"]] == [3.0, 5.0, 7.0, 9.0]
    model_calls = [c for c in seen if c[0] == "/api/v1/models"]
    assert len(model_calls) == 1 and model_calls[0][1]["architecture"] == "2"


def test_cli_ladder_no_models_is_null(monkeypatch, capsys):
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _tone_and_models_client(None, [], a2=0, a1=0))
    assert cli.main(["ladder", "1001", "--json"]) == 0
    assert json.loads(capsys.readouterr().out) == {"tone_id": "1001", "size": "standard", "rungs": None}
    monkeypatch.setattr(cli, "make_client", lambda *a, **k: _tone_and_models_client(None, [], a2=1, a1=0))
    assert cli.main(["ladder", "1001", "--json"]) == 0      # tone claims A2 models but lists none
    assert json.loads(capsys.readouterr().out)["rungs"] is None


# --- hyphen / underscore separators (v0.3 Task E) ---------------------------------------------------

def test_hyphen_and_underscore_separators():
    from sawblade_match.t3k.ladder import _gain_and_rest
    assert _gain_and_rest("Gain-06") == (6.0, "")
    assert _gain_and_rest("gain_6")[0] == 6.0
    assert _gain_and_rest("G-6")[0] == 6.0
    assert _gain_and_rest("APP-6505Plus-Scooped-Gain-06") == (6.0, "app 6505plus scooped")


@pytest.mark.parametrize("name", ["APP-6505+-Boost-S-OD1", "5150-III", "Mesa-Rec-2ch", "Big-5", "APP-6505+-Boost-S-MercilessDrive"])
def test_hyphenated_names_without_gain_token_do_not_parse(name):
    from sawblade_match.t3k.ladder import _gain_and_rest
    assert _gain_and_rest(name) is None


def test_boost_pack_names_are_no_ladder():
    assert parse_ladder(models("APP-6505+-Boost-S-MercilessDrive", "APP-6505+-Boost-S-OD1",
                               "APP-6505+-Boost-S-SickAs"), "standard") is None


def test_gain_range_pack_is_five_step_ladder():
    names = [f"APP-6505Plus-Scooped-Gain-{n}" for n in ("04", "05", "06", "07", "02")]
    r = parse_ladder(models(*names), "standard")
    assert r is not None and [x.gain for x in r] == [2.0, 4.0, 5.0, 6.0, 7.0] and len(r) == 5


def test_hyphen_ladder_still_needs_identical_remainder_and_distinct_gains():
    assert parse_ladder(models("Amp-Gain-3", "Lead-Gain-5"), "standard") is None
    assert parse_ladder(models("Amp-Gain-3", "Amp-Gain-03"), "standard") is None
