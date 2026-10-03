import json

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


def test_popularity_uses_75th_percentile_within_gear():
    ds, th = run(amps() + [tone_json(50, gear="pedal", fav=5, dl=10)])
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


def test_favorited_bypasses_popularity_but_is_flagged():
    ds, _ = run(amps(), favorited={7, 3})
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
    free = FilterConfig(min_favorites=0, min_downloads=0, popularity_percentile=0)
    both = tone_json(20, a2=2, a1=3)
    a1only = tone_json(21, a2=0, a1=2)
    none = tone_json(22, a2=0, a1=0, custom_models_count=4)
    ds, _ = run([both, a1only, none], cfg=free)
    assert ds[20].architecture == "2" and ds[20].flags == []
    assert ds[21].architecture == "1" and ds[21].flags == ["a1_only"] and ds[21].status == "included"
    assert ds[22].status == "excluded" and ds[22].reasons == ["no_a2_models"]
    ds, _ = run([a1only], cfg=FilterConfig(min_favorites=0, min_downloads=0, popularity_percentile=0,
                                           allow_a1_fallback=False))
    assert ds[21].reasons == ["a1_fallback_disabled"]


def test_slot_fit_by_gear():
    free = FilterConfig(min_favorites=0, min_downloads=0, popularity_percentile=0)
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
    free = FilterConfig(min_favorites=0, min_downloads=0, popularity_percentile=0)
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
    fav_low = tone_json(102, gear="amp", fav=1, dl=1)           # low pop, favorited -> kept + flagged
    rig = tone_json(103, gear="amp-cab", fav=999, dl=9999)
    ir = tone_json(104, gear="cab", fmt="ir", a2=0, fav=150, dl=2000, title="V30 IR")
    api.favorited = [fav_low, rig]
    api.trending = {"amp": [good, pop2], "pedal": [], "cab": [ir]}
    api.latest = [tone_json(105, gear="pedal", fav=140, dl=1800, a2=0, a1=1)]
    for t in (good, fav_low, ir, api.latest[0]):
        arch = "1" if t["a2_models_count"] == 0 else "2"
        api.add_tone(t, [model_json(t["id"] * 10, t["id"], size="lite", arch=arch),
                         model_json(t["id"] * 10 + 1, t["id"], size="standard", arch=arch)])
    for t in (pop2, rig):
        api.add_tone(t, [])
    client = make_client()
    cache = Cache(tmp_path / "c")
    m = build_pool(client, cache, FilterConfig(), now=NOW)
    out = tmp_path / "pool.json"
    write_manifest(m, out)
    m = json.loads(out.read_text())

    inc = {t["tone_id"]: t for t in m["tones"]}
    assert set(inc) == {100, 102, 104, 105}
    g = inc[100]
    assert (g["title"], g["creator"], g["license"], g["gear"], g["slot"]) == ("Good Amp", "bob", "cc-by-sa", "amp", "amp")
    assert g["url"].endswith("tone-100") and g["published_at"] and g["favorites_count"] == 300
    assert g["chosen_model"]["id"] == 1001 and g["chosen_model"]["size"] == "standard"
    assert g["chosen_model"]["architecture_queried"] == "2"
    assert g["sha256"] and g["cached_path"].endswith("100/1001.nam") and "favorited" not in g["sources"]
    assert inc[102]["flags"] == ["below_popularity_floor"] and inc[102]["sources"] == ["favorited"]
    assert inc[104]["cached_path"].endswith("104/1041.wav")
    assert inc[105]["flags"] == ["a1_only"] and inc[105]["chosen_model"]["architecture_queried"] == "1"
    assert [t["tone_id"] for t in m["references"]] == [103]
    ex = {t["tone_id"]: t for t in m["excluded"]}
    assert set(ex) == {101} and ex[101]["reasons"][0].startswith("below_popularity")
    assert m["config"]["max_age_months"] == 18 and "amp" in m["popularity_thresholds"]
    assert m["sources"]["search"] is False
    # only included tones were downloaded (FakeApi also asserts every models query had `architecture`)
    downloaded = sorted(int(r.url.path.split("/")[-2]) for r in api.requests("download"))
    assert downloaded == [1001, 1021, 1041, 1051]
    assert not (tmp_path / "c" / "101").exists() and not (tmp_path / "c" / "103").exists()


def test_search_is_opt_in(make_client, api):
    from sawblade_match.t3k.pool import collect
    api.search_results = [tone_json(9)]
    client = make_client()
    collect(client)
    assert not api.requests("search")
    _, src = collect(client, search_query="plexi")
    assert len(api.requests("search")) == 1 and src[9] == ["search"]
