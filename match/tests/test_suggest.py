"""Task C pool rule (pure) and the `suggest-body` CLI on a fixture pool. Offline."""
from __future__ import annotations

import hashlib
import json

import pytest

from sawblade_match.t3k import cli
from sawblade_match.t3k.cache import Cache
from sawblade_match.t3k.suggest import family_key, pool_candidates, suggest_body


def rec(tone, model, title, name="Standard", cached=False):
    return {"tone_id": tone, "model_id": model, "title": title, "name": name, "cached": cached}


@pytest.mark.parametrize("title,fam", [
    ("Peavey 5150 Block Letter", "5150"), ("Peavey Invective", "peavey"), ("Peavey XXX", "peavey"),
    ("Triple Rectifier", "recto"), ("Peavey 6505+ Dual", "5150"),
     ("6505+ Lead", "5150"), ("5153 Red Stripe", "5150"),
    ("Mesa Dual Rectifier", "recto"), ("Mesa Boogie Recto", "recto"), ("Bogner Uberschall", "bogner"),
    ("EVH 5150III", "5150"), ("Marshall JCM800", "marshall"), ("  ", ""), ("", ""), (None, ""), ("1987 !!", ""),
    ("Fortin Cali Crush", "fortin"),
])
def test_family_key(title, fam):
    assert family_key(title) == fam


def test_different_family_preferred_even_if_uncached():
    pool = [rec(1, 11, "5150 Block Lead", cached=True), rec(2, 21, "Diezel VH4 Ch3")]
    assert suggest_body(pool, "6505 Plus")["tone_id"] == "2"      # 6505 and 5150 are one family
    assert suggest_body(pool, "Marshall JCM800 Lead")["tone_id"] == "1"   # both differ -> cached wins


def test_cached_preferred_within_tier_then_stable_pool_order():
    pool = [rec(1, 11, "Diezel VH4 Lead"), rec(2, 21, "Bogner Ecstasy Lead"), rec(3, 31, "Friedman BE-100 Lead", cached=True),
            rec(4, 41, "Revv G3 Lead", cached=True)]
    assert suggest_body(pool, "Mesa Dual Recto Lead") == {"tone_id": "3", "model_id": "31",
                                                          "title": "Friedman BE-100 Lead", "cached": True}
    uncached = [dict(r, cached=False) for r in pool]
    assert suggest_body(uncached, "Mesa Dual Recto Lead")["tone_id"] == "1"      # pool order
    assert suggest_body(list(reversed(uncached)), "Mesa Dual Recto Lead")["tone_id"] == "4"


def test_same_family_only_is_still_returned():
    pool = [rec(1, 11, "Mesa Rectifier Lead")]
    assert suggest_body(pool, "Dual Recto Lead")["tone_id"] == "1"


def test_no_amp_high_returns_none():
    assert suggest_body([], "5150") is None
    assert suggest_body([rec(1, 11, "Fender Twin Clean", "Clean"), rec(2, 21, "Vox AC30 crunch")], "5150") is None


def test_unknown_or_empty_a_title_uses_cached_then_order():
    pool = [rec(1, 11, "Diezel VH4 Lead"), rec(2, 21, "Bogner Ecstasy Lead", cached=True)]
    for a in ("", None, "   ", "1987"):
        assert suggest_body(pool, a)["tone_id"] == "2"
    assert suggest_body([dict(r, cached=False) for r in pool], "")["tone_id"] == "1"


def test_model_name_can_disqualify():
    pool = [rec(1, 11, "Peavey 5150 pack", "Clean channel"), rec(2, 21, "Peavey 5150 pack", "Lead")]
    assert suggest_body(pool, "")["model_id"] == "21"


# ---- fixture pool + CLI -------------------------------------------------------------------------

def _entry(tone, title, models, gear="amp", status="included"):
    return {"tone_id": tone, "title": title, "gear": gear, "slot": gear, "status": status,
            "models": [{"id": i, "name": n, "size": "standard"} for i, n in models]}


@pytest.fixture
def pool_dir(tmp_path):
    root = tmp_path / "cc"
    root.mkdir()
    manifest = {"tones": [
        _entry(1, "5150 Block Letter", [(11, "Lead")]),
        _entry(2, "Diezel VH4", [(21, "Clean"), (22, "Ch3 Lead")]),
        _entry(3, "Hot Rod Cab IR", [(31, "IR")], gear="ir"),
        _entry(4, "Bogner Uberschall", [(41, "Lead")]),
        _entry(5, "Revv G3 excluded", [(51, "Lead")], status="excluded"),
    ]}
    (root / "pool_manifest.json").write_text(json.dumps(manifest))
    cache = Cache(root)
    # mark tone 4 / model 41 as cached (file + matching sha256 in meta.json)
    d = root / "4"
    d.mkdir()
    (d / "41.nam").write_bytes(b"FILE")
    (d / "meta.json").write_text(json.dumps({"tone": {}, "models": {"41": {
        "model": {}, "sha256": hashlib.sha256(b"FILE").hexdigest(), "file": "41.nam", "fetched_at": "x"}}}))
    assert cache.get(4, 41) is not None
    return root


def test_pool_candidates_shape_and_cached_flag(pool_dir):
    manifest = json.loads((pool_dir / "pool_manifest.json").read_text())
    c = pool_candidates(manifest, Cache(pool_dir))
    assert [(r["tone_id"], r["model_id"], r["cached"]) for r in c] == [
        (1, 11, False), (2, 21, False), (2, 22, False), (4, 41, True)]       # IR and excluded dropped


def run(capsys, *argv):
    rc = cli.main(list(argv))
    return rc, json.loads(capsys.readouterr().out)


def test_cli_suggest_body(pool_dir, capsys):
    rc, out = run(capsys, "suggest-body", "--a-title", "5150 Block Letter", "--json", "--cache-dir", str(pool_dir))
    assert rc == 0 and out == {"tone_id": "4", "model_id": "41", "title": "Bogner Uberschall", "cached": True}
    rc, out = run(capsys, "suggest-body", "--a-title", "Bogner Uberschall", "--json", "--cache-dir", str(pool_dir))
    assert out == {"tone_id": "1", "model_id": "11", "title": "5150 Block Letter", "cached": False}   # clean 21 is amp_low
    rc, out = run(capsys, "suggest-body", "--json", "--cache-dir", str(pool_dir))                 # no A title
    assert out["tone_id"] == "4"


def test_cli_suggest_body_null_without_pool_or_candidates(tmp_path, capsys):
    rc, out = run(capsys, "suggest-body", "--a-title", "x", "--json", "--cache-dir", str(tmp_path / "none"))
    assert rc == 0 and out is None
    (tmp_path / "pool_manifest.json").write_text("{not json")
    assert run(capsys, "suggest-body", "--json", "--cache-dir", str(tmp_path))[1] is None
    (tmp_path / "pool_manifest.json").write_text(json.dumps({"tones": [_entry(1, "Fender Twin", [(1, "Clean")])]}))
    assert run(capsys, "suggest-body", "--json", "--cache-dir", str(tmp_path))[1] is None


def test_alias_families_merge_but_peavey_does_not_fold():
    assert family_key("Peavey 5150 Block Letter") == family_key("6505+ Lead")
    assert family_key("Peavey Invective") != family_key("6505")
    assert family_key("Mesa Dual Rectifier") == family_key("Recto")
    pool = [rec(1, 11, "Peavey 5150 Lead", cached=True), rec(2, 21, "Diezel VH4 Ch3")]
    assert suggest_body(pool, "6505+ Lead")["tone_id"] == "2"
    assert suggest_body([rec(1, 11, "Peavey Invective Lead", cached=True), rec(2, 21, "Diezel VH4")],
                        "6505")["tone_id"] == "1"


def test_dual_and_recto_word_boundaries():
    assert family_key("Dualist") != "recto"
    assert family_key("Revv G3 Dual") == "recto"
    assert family_key("Triple Crown") == "recto" and family_key("Tripled") != "recto"
    assert family_key("Mesa Rectifier") == "recto" and family_key("Rev EVH") == "5150"
    assert family_key("Prevh Amp") != "5150"


def test_cli_corrupt_manifest_entries_are_skipped(tmp_path, capsys):
    root = tmp_path / "cc"
    root.mkdir()
    good = _entry(7, "Diezel VH4", [(71, "Lead")])
    bad_tone = _entry(1, "Bogner Ecstasy", [(11, "Lead")])
    bad_tone["tone_id"] = "abc"
    bad_model = _entry(2, "Bogner Uberschall", [(21, "Lead")])
    bad_model["models"].append({"id": "x", "name": "Lead"})
    bad_model["models"].append("junk")
    bad_models_type = dict(_entry(3, "Revv G3", []), models=5)
    meta_no_file = _entry(4, "Friedman BE-100", [(41, "Lead")])
    (root / "pool_manifest.json").write_text(json.dumps(
        {"tones": [bad_tone, bad_model, bad_models_type, meta_no_file, "junk", good]}))
    (root / "4").mkdir()
    (root / "4" / "meta.json").write_text(json.dumps({"models": {"41": {"sha256": "x"}}}))   # missing "file"
    rc, out = run(capsys, "suggest-body", "--json", "--cache-dir", str(root))
    assert rc == 0 and out["model_id"] == "21"          # first valid candidate, uncached, pool order
    (root / "pool_manifest.json").write_text(json.dumps({"tones": [bad_tone, meta_no_file, good]}))
    rc, out = run(capsys, "suggest-body", "--json", "--cache-dir", str(root))
    assert rc == 0 and out["model_id"] == "41" and out["cached"] is False
    (root / "4" / "meta.json").write_text("[1, 2]")                       # meta that is a list
    assert run(capsys, "suggest-body", "--json", "--cache-dir", str(root))[0] == 0
    (root / "4" / "meta.json").write_text(json.dumps({"models": {"41": 5}}))   # entry not a dict
    assert run(capsys, "suggest-body", "--json", "--cache-dir", str(root))[0] == 0
    (root / "pool_manifest.json").write_text(json.dumps({"tones": [bad_tone, good]}))
    assert run(capsys, "suggest-body", "--json", "--cache-dir", str(root))[1]["tone_id"] == "7"
