import json
import pytest

import numpy as np

from sawblade_match.t3k.cache import Cache
from sawblade_match.t3k.filter import FilterConfig, evaluate, parse_ts
from sawblade_match.t3k.pool import build_pool, write_manifest
from sawblade_match.t3k.types import Tone
from conftest import NOW, model_json, tone_json


def tones_of(*js):
    return {j["id"]: Tone.from_json(j) for j in js}


def run(js, favorited=(), cfg=None, gears=None):
    ts = tones_of(*js)
    src = {i: (["favorited"] if i in favorited else ["trending"]) for i in ts}
    ds, th = evaluate(ts, src, cfg or FilterConfig(), NOW, gears)
    return {d.tone.id: d for d in ds}, th


def amps():
    # favorites / downloads spread; defaults: floors 10 favs / 200 dl, p75 within gear
    return [
        tone_json(1, fav=200, dl=3000),
        tone_json(2, fav=120, dl=2000),
        tone_json(3, fav=80, dl=1500),
        tone_json(4, fav=60, dl=900),
        tone_json(5, fav=40, dl=700),
        tone_json(6, fav=30, dl=500),
        tone_json(7, fav=3, dl=50),
    ]


def test_default_is_absolute_floors_only():
    ds, th = run(amps())          # floors 100 favs / 1000 downloads, no percentile
    assert th["amp"]["favorites"] == 100 and "favorites_percentile_value" not in th["amp"]
    assert {i for i, d in ds.items() if d.status == "included"} == {1, 2}
    assert ds[3].reasons[0].startswith("below_popularity")


def test_popularity_uses_75th_percentile_within_gear():
    ds, th = run(amps() + [tone_json(50, gear="pedal", fav=5, dl=10)],
                 cfg=FilterConfig(popularity_percentile=75, min_favorites=10, min_downloads=200))
    p75f = np.percentile([200, 120, 80, 60, 40, 30, 3], 75)   # 100.0
    assert th["amp"]["favorites"] == p75f
    assert [i for i in range(1, 8) if ds[i].status == "included"] == [1, 2]
    assert any(r.startswith("below_popularity") for r in ds[3].reasons)
    assert "favorites 80 < 100" in ds[3].reasons[0]
    # pedal group is evaluated against its own (1-element) set, so its percentile is its own value,
    # but the absolute floors still exclude it
    assert th["pedal"]["n"] == 1 and ds[50].status == "excluded"
    assert "below_popularity" in ds[50].reasons[0]


def test_absolute_floor_overrides_low_percentile():
    cfg = FilterConfig(popularity_percentile=0, min_favorites=50, min_downloads=1000)
    ds, th = run(amps(), cfg=cfg)
    assert th["amp"]["favorites"] == 50 and th["amp"]["downloads"] == 1000
    assert [i for i, d in ds.items() if d.status == "included"] == [1, 2, 3]


def test_favorited_below_floor_excluded_unless_opted_in():
    ds, _ = run(amps(), favorited={7, 3})
    assert ds[7].status == "excluded" and ds[3].status == "excluded"
    assert ds[7].reasons[0].startswith("below_popularity")
    ds, _ = run(amps(), favorited={7, 3}, cfg=FilterConfig(keep_favorites_below_floor=True))
    assert ds[7].status == "included" and "below_popularity_floor" in ds[7].flags
    assert ds[3].status == "included" and "below_popularity_floor" in ds[3].flags
    assert ds[1].flags == []                                  # passing tone: no flag


def test_recency_18_months_published_fallback_updated():
    old = tone_json(10, fav=500, dl=9000, pub="2025-03-01T00:00:00Z")      # 19 months before NOW
    edge = tone_json(11, fav=500, dl=9000, pub="2025-05-01T00:00:00Z")     # 17 months
    nopub = tone_json(12, fav=500, dl=9000, published_at=None, updated_at="2026-09-01T00:00:00Z")
    nodate = tone_json(13, fav=500, dl=9000, published_at=None, updated_at=None)
    ds, _ = run([old, edge, nopub, nodate], cfg=FilterConfig(min_favorites=0, min_downloads=0))
    assert ds[10].reasons == ["too_old:2025-03-01"]
    assert ds[11].status == "included" and ds[12].status == "included"
    assert ds[13].reasons == ["no_publish_date"]
    # favorites do not bypass recency by default; the option exists
    ds, _ = run([old], favorited={10}, cfg=FilterConfig(min_favorites=0, min_downloads=0))
    assert ds[10].status == "excluded"
    ds, _ = run([old], favorited={10}, cfg=FilterConfig(min_favorites=0, min_downloads=0,
                                                         favorites_bypass_recency=True))
    assert ds[10].status == "included"


def test_a2_preferred_a1_only_fallback_and_none():
    free = FilterConfig(min_favorites=0, min_downloads=0)
    both = tone_json(20, a2=2, a1=3)
    a1only = tone_json(21, a2=0, a1=2)
    none = tone_json(22, a2=0, a1=0, custom_models_count=4)
    ds, _ = run([both, a1only, none], cfg=free)
    assert ds[20].architecture == "2" and ds[20].flags == []
    assert ds[21].architecture == "1" and ds[21].flags == ["a1_only"] and ds[21].status == "included"
    assert ds[22].status == "excluded" and ds[22].reasons == ["no_a2_models"]
    ds, _ = run([a1only], cfg=FilterConfig(min_favorites=0, min_downloads=0, allow_a1_fallback=False))
    assert ds[21].reasons == ["a1_fallback_disabled"]


def test_slot_fit_by_gear():
    free = FilterConfig(min_favorites=0, min_downloads=0)
    js = [tone_json(30, gear="pedal"), tone_json(31, gear="amp"), tone_json(32, gear="amp-cab"),
          tone_json(33, gear="cab", fmt="ir", a2=0), tone_json(34, gear="cab", fmt="nam"),
          tone_json(35, gear="outboard"), tone_json(36, gear="space"), tone_json(37, gear="experimental"),
          tone_json(38, gear="full-rig")]
    ds, _ = run(js, cfg=free)
    assert (ds[30].slot, ds[31].slot, ds[33].slot) == ("pedal", "amp", "cab")
    assert all(ds[i].status == "included" for i in (30, 31, 33))
    for i in (32, 38):
        assert ds[i].status == "reference" and ds[i].reasons == ["full_rig_reference_only"]
    for i in (34, 35, 36, 37):
        assert ds[i].status == "excluded" and ds[i].reasons[0].startswith("not_slot_eligible")


def test_gear_restriction_to_slots():
    free = FilterConfig(min_favorites=0, min_downloads=0)
    ds, _ = run([tone_json(30, gear="pedal"), tone_json(31, gear="amp")], cfg=free, gears=["amp"])
    assert ds[31].status == "included" and ds[30].reasons == ["slot_not_requested:pedal"]


def test_all_failing_reasons_are_recorded():
    ds, _ = run([tone_json(40, a2=0, a1=0, pub="2020-01-01T00:00:00Z", fav=0, dl=0)])
    kinds = [r.split(":")[0] for r in ds[40].reasons]
    assert kinds == ["no_a2_models", "too_old", "below_popularity"]


def test_parse_ts_variants():
    assert parse_ts("2026-01-01T00:00:00Z").year == 2026
    assert parse_ts("2026-01-01T00:00:00.123+00:00").tzinfo is not None
    assert parse_ts("garbage") is None and parse_ts(None) is None


def test_build_pool_manifest_end_to_end(respx_mock, make_client, api, tmp_path):
    good = tone_json(100, gear="amp", fav=300, dl=5000, title="Good Amp", user="bob", license="cc-by-sa")
    pop2 = tone_json(101, gear="amp", fav=10, dl=100)           # low pop, not favorited
    fav_low = tone_json(102, gear="amp", fav=1, dl=1)           # low pop, favorited
    rig = tone_json(103, gear="amp-cab", fav=999, dl=9999)
    ir = tone_json(104, gear="cab", fmt="ir", a2=0, fav=150, dl=2000, title="V30 IR")
    api.favorited = [fav_low, rig]
    api.trending = {"amp": [good, pop2], "pedal": [], "cab": [ir]}
    api.latest = [tone_json(105, gear="pedal", fav=140, dl=1800, a2=0, a1=1)]
    api.add_tone(good, [model_json(1000 + i, 100, arch="2") for i in range(5)])   # 5 models
    api.add_tone(fav_low, [model_json(1020, 102)])
    api.add_tone(ir, [model_json(1040 + i, 104, arch=None) for i in range(168)])
    api.add_tone(api.latest[0], [model_json(1050, 105, arch="1")])
    for t in (pop2, rig):
        api.add_tone(t, [])
    cache = Cache(tmp_path / "c")
    m = build_pool(make_client(), cache, FilterConfig(), now=NOW)
    out = tmp_path / "pool.json"
    write_manifest(m, out)
    m = json.loads(out.read_text())

    inc = {t["tone_id"]: t for t in m["tones"]}
    assert set(inc) == {100, 104, 105}
    g = inc[100]
    assert (g["title"], g["creator"], g["license"], g["gear"], g["slot"]) == ("Good Amp", "bob", "cc-by-sa", "amp", "amp")
    assert g["url"].endswith("tone-100") and g["published_at"] and g["favorites_count"] == 300
    # manifest lists ALL models as candidates; downloads: 3 per pedal/amp tone, ALL models of an IR tone (v0.4M B3)
    assert [x["id"] for x in g["models"]] == [1000, 1001, 1002, 1003, 1004]
    assert {"id", "name", "architecture_version"} <= set(g["models"][0])
    assert [x["model_id"] for x in g["downloads"]] == [1000, 1001, 1002]
    assert g["downloads"][0]["path"].endswith("100/1000.nam") and g["downloads"][0]["sha256"]
    assert len(inc[104]["models"]) == 168 and len(inc[104]["downloads"]) == 168
    assert inc[104]["downloads"][0]["path"].endswith("104/1040.wav")
    assert inc[105]["flags"] == ["a1_only"] and inc[105]["models"][0]["architecture_queried"] == "1"
    assert [t["tone_id"] for t in m["references"]] == [103]
    ex = {t["tone_id"]: t for t in m["excluded"]}
    assert set(ex) == {101, 102} and ex[102]["reasons"][0].startswith("below_popularity")
    assert m["config"]["popularity_percentile"] is None and m["max_models_per_tone"] == 3
    downloaded = sorted(int(r.url.path.split("/")[-2]) for r in api.requests("download"))
    assert downloaded == sorted([1000, 1001, 1002, 1050, *range(1040, 1208)])
    assert not (tmp_path / "c" / "101").exists() and not (tmp_path / "c" / "103").exists()

    m2 = build_pool(make_client(), Cache(tmp_path / "c2"), FilterConfig(keep_favorites_below_floor=True),
                    download=False, max_models_per_tone=1, now=NOW)
    assert {t["tone_id"] for t in m2["tones"]} == {100, 102, 104, 105}
    assert [t for t in m2["tones"] if t["tone_id"] == 102][0]["flags"] == ["below_popularity_floor"]
    assert all(t["downloads"] == [] for t in m2["tones"])


def test_search_is_opt_in(make_client, api):
    from sawblade_match.t3k.pool import collect
    api.search_results = [tone_json(9)]
    client = make_client()
    collect(client)
    assert not api.requests("search")
    _, src = collect(client, searches=["plexi"])
    assert len(api.requests("search")) == 1 and src[9] == ["search"]


def test_license_allowlist_no_favorites_bypass():
    cfg = FilterConfig(min_favorites=0, min_downloads=0, keep_favorites_below_floor=True,
                       favorites_bypass_recency=True)
    allowed = ["t3k", "cc-by", "cc-by-sa", "cc-by-nd", "cco", "cc-by-nc", "cc-by-nc-sa", "cc-by-nc-nd"]
    bad = {"": "unknown_license:", "gpl-3": "unknown_license:gpl-3"}
    js = [tone_json(600 + i, license=lic, fav=99999, dl=999999) for i, lic in enumerate(allowed)]
    js += [tone_json(700 + i, license=lic, fav=99999, dl=999999) for i, lic in enumerate(bad)]
    ds, _ = run(js, favorited={j["id"] for j in js}, cfg=cfg)
    for i in range(len(allowed)):
        assert ds[600 + i].status == "included", allowed[i]
        assert ("non_commercial" in ds[600 + i].flags) == allowed[i].startswith("cc-by-nc"), allowed[i]
    for i, (lic, reason) in enumerate(bad.items()):
        assert ds[700 + i].status == "excluded" and reason in ds[700 + i].reasons, lic


# ---- phase 3.7: search, lead picks, pool_sources.json -------------------------------------------

def _pool(make_client, tmp_path, **kw):
    return build_pool(make_client(), Cache(tmp_path / "c"), FilterConfig(), download=False, now=NOW, **kw)


def test_multi_search_merge_dedupes(make_client, api, tmp_path):
    a = tone_json(10, gear="amp", fav=500, dl=5000)
    b = tone_json(11, gear="pedal", fav=500, dl=5000)
    api.search_results = [a, b]          # the fake returns the same list for every query
    for t in (a, b):
        api.add_tone(t, [model_json(t["id"] * 10, t["id"])])
    m = _pool(make_client, tmp_path, searches=["plexi", "big muff", "plexi"], trending=False, latest=False)
    assert len(api.requests("search")) == 2                       # duplicate query dropped
    assert sorted(t["tone_id"] for t in m["tones"]) == [10, 11]   # not listed twice
    assert m["sources"]["search"] == ["plexi", "big muff"]
    assert all(t["sources"] == ["search"] for t in m["tones"])


def test_add_tone_is_lead_pick_and_filtered(make_client, api, tmp_path):
    ok = tone_json(20, gear="pedal", fav=500, dl=5000)
    old = tone_json(21, gear="pedal", fav=500, dl=5000, pub="2020-01-01T00:00:00Z")
    nc = tone_json(22, gear="pedal", fav=500, dl=5000, license="cc-by-nc")
    for t in (ok, old, nc):
        api.add_tone(t, [model_json(t["id"] * 10, t["id"])])
    m = _pool(make_client, tmp_path, add_tones=[20, 21, 22], trending=False, latest=False)
    assert [t["tone_id"] for t in m["tones"]] == [20, 22] and m["tones"][0]["sources"] == ["lead-pick"]
    assert m["tones"][1]["flags"] == ["non_commercial"]
    ex = {t["tone_id"]: t["reasons"] for t in m["excluded"]}
    assert ex[21][0].startswith("too_old") and 22 not in ex
    assert m["sources"]["lead_picks"] == [20, 21, 22]


def test_force_tone_skips_quality_but_not_license(make_client, api, tmp_path):
    old = tone_json(30, gear="pedal", fav=1, dl=1, pub="2020-01-01T00:00:00Z")
    nc = tone_json(31, gear="pedal", fav=999, dl=9999, license="cc-by-nc-sa")
    for t in (old, nc):
        api.add_tone(t, [model_json(t["id"] * 10, t["id"])])
    m = _pool(make_client, tmp_path, force_tones=[30, 31], trending=False, latest=False)
    assert [t["tone_id"] for t in m["tones"]] == [30, 31]
    assert m["tones"][0]["flags"] == ["forced"] and m["tones"][0]["sources"] == ["lead-pick"]
    assert m["tones"][1]["flags"] == ["non_commercial", "forced"] and not m["excluded"]


def test_pool_sources_file_merged_into_pull(make_client, api, tmp_path, monkeypatch, capsys):
    from sawblade_match.t3k import cli
    srcs = tmp_path / "pool_sources.json"
    srcs.write_text(json.dumps({"searches": ["hm-2"], "tones": [41]}))
    monkeypatch.setenv("SAWBLADE_POOL_SOURCES", str(srcs))
    monkeypatch.setattr(cli, "make_client", make_client)
    s = tone_json(40, gear="pedal", fav=500, dl=5000)
    p = tone_json(41, gear="pedal", fav=500, dl=5000)
    api.search_results = [s]
    for t in (s, p):
        api.add_tone(t, [model_json(t["id"] * 10, t["id"])])
    man = tmp_path / "m.json"
    rc = cli.main(["pull", "--no-trending", "--no-latest", "--no-download", "--search", "hm-2",
                   "--add-tone", "41", "--cache-dir", str(tmp_path / "c"), "--manifest", str(man)])
    assert rc == 0 and str(srcs) in capsys.readouterr().out
    m = json.loads(man.read_text())
    assert {t["tone_id"]: t["sources"] for t in m["tones"]} == {40: ["search"], 41: ["lead-pick"]}
    assert len(api.requests("search")) == 1                      # flag + file de-duplicated


def test_pool_sources_missing_and_malformed(tmp_path):
    from sawblade_match.t3k.errors import T3KError
    from sawblade_match.t3k.sources import load_pool_sources
    assert load_pool_sources(tmp_path / "nope.json").searches == []
    bad = tmp_path / "bad.json"
    bad.write_text('{"tones": ["../x"]}')
    with pytest.raises(T3KError):
        load_pool_sources(bad)
    bad.write_text("{not json")
    with pytest.raises(T3KError):
        load_pool_sources(bad)


def test_search_output_and_verdicts(make_client, api, tmp_path, monkeypatch, capsys):
    from sawblade_match.t3k import cli
    monkeypatch.setattr(cli, "make_client", make_client)
    good = tone_json(50, gear="pedal", fav=300, dl=5000, title="Fuzz Good", user="bob", license="cc-by")
    bad = tone_json(51, gear="pedal", fav=5, dl=50, pub="2020-01-01T00:00:00Z", license="cc-by-nc",
                    title="Fuzz Bad")
    api.search_results = [good, bad]
    before = len(api.calls)
    assert cli.main(["search", "fuzz", "--gear", "pedal"]) == 0
    out = capsys.readouterr().out
    lines = {l.split()[0]: l for l in out.splitlines() if l[:2] in ("50", "51")}
    assert "Fuzz Good" in lines["50"] and "bob" in lines["50"] and "cc-by" in lines["50"]
    assert "300/5000" in lines["50"] and "2026-06-01" in lines["50"] and "PASS" in lines["50"]
    assert "A2:1" in lines["50"]
    assert "FAIL" in lines["51"] and "non_commercial_license" not in lines["51"]
    assert "too_old" in lines["51"] and "below_popularity" in lines["51"]
    assert "1 pass" in out
    q = api.requests("search")[0].url.params
    assert q["query"] == "fuzz" and q["gears"] == "pedal"
    # read-only: only the search call, no download / models / tone lookups
    assert [r.url.path for r in api.calls[before:]] == ["/api/v1/tones/search"]

    assert cli.main(["search", "fuzz", "--json"]) == 0
    recs = json.loads(capsys.readouterr().out)
    assert [(r["tone_id"], r["passes"]) for r in recs] == [(50, True), (51, False)]
    assert "non_commercial_license:cc-by-nc" not in recs[1]["reasons"]


def test_ir_tones_download_all_models_unless_capped_and_ir_search_adds_ir_tones(make_client, api, tmp_path):
    ir = tone_json(204, gear="cab", fmt="ir", a2=0, fav=150, dl=2000, title="Mesa pack")
    amp = tone_json(200, gear="amp", fav=300, dl=5000)
    api.search_results = [ir]
    api.trending = {"amp": [amp], "pedal": [], "cab": []}
    api.add_tone(ir, [model_json(2040 + i, 204, arch=None) for i in range(7)])
    api.add_tone(amp, [model_json(2000 + i, 200, arch="2") for i in range(5)])
    m = build_pool(make_client(), Cache(tmp_path / "c"), FilterConfig(), ir_searches=["mesa 4x12"], latest=False, now=NOW)
    inc = {t["tone_id"]: t for t in m["tones"]}
    assert len(inc[204]["downloads"]) == 7 and len(inc[200]["downloads"]) == 3 and inc[204]["sources"] == ["ir-search"]
    assert m["sources"]["ir_search"] == ["mesa 4x12"] and m["max_ir_models_per_tone"] is None
    assert api.requests("search") and "ir" in str(api.requests("search")[0].url)
    m = build_pool(make_client(), Cache(tmp_path / "c2"), FilterConfig(), ir_searches=["mesa"], latest=False,
                   max_ir_models_per_tone=2, now=NOW)
    assert len([t for t in m["tones"] if t["tone_id"] == 204][0]["downloads"]) == 2
