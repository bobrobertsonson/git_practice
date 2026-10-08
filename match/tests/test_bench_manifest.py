"""v0.7 Task A: the committed benchmark manifest (docs/benchmark/cases.json) loads with the runner's loader and is consistent."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from sawblade_match.bench import manifest as MF

REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "docs" / "benchmark"
AUDIO = {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg", ".m4a", ".nam", ".aac"}


@pytest.fixture(scope="module")
def m():
    return MF.load_manifest(BENCH_DIR / "cases.json")


def test_manifest_loads_with_the_runners_loader(m):
    assert m["schema"] == MF.SCHEMA and m["version"] == 1 and m["rootHint"] == "/Users/notsch/Desktop/NailTheMix"
    ids = [c["id"] for c in m["cases"]]
    assert len(ids) == len(set(ids))
    assert MF.DEFAULT_MANIFEST.resolve() == (BENCH_DIR / "cases.json").resolve()


def test_pathcheck_parents_exist_and_are_blend_cases(m):
    by = {c["id"]: c for c in m["cases"]}
    pcs = [c for c in m["cases"] if c["kind"] == "pathcheck"]
    assert {c["id"] for c in pcs} == {"bloodbath_hm2", "bloodbath_ubr"}
    for c in pcs:
        assert by[c["reference"]["parent"]]["kind"] == "blend" and not c["counts"]
        assert by[c["reference"]["parent"]]["topology"] == "blend"            # the forced blend carries the per-path answer
    assert {by[c["id"]]["reference"]["path"] for c in pcs} == {"a", "b"}


def test_counted_cases_are_the_eight_the_spec_lists(m):
    counted = [c["id"] for c in m["cases"] if c["counts"]]
    assert counted == ["bloodbath_blend", "bloodbath_mz", "immortal_disfig", "veil_of_maya", "sylosis_57", "sylosis_reamp", "haunted", "jinjer"]
    by = {c["id"]: c for c in m["cases"]}
    forced = by["bloodbath_blend_forced"]
    primary = by["bloodbath_blend"]
    assert forced["topology"] == "blend" and not forced["counts"] and primary["topology"] == "auto" and primary["counts"]
    assert forced["di"] == primary["di"] and forced["reference"] == primary["reference"]
    assert [(t["role"], t["gainDb"]) for t in primary["reference"]["tracks"]] == [("a", 0.0), ("b", 0.0)]
    assert all(c["tier"] == 2 and not c["counts"] and c["offsetMs"] is None for c in m["cases"] if c["tier"] == 2)
    assert sum(c["tier"] == 2 for c in m["cases"]) == 12
    assert by["jinjer"]["reference"]["channel"] == "left" and by["jinjer"]["transfer"][0]["reference"]["channel"] == "right"


def test_every_style_the_spec_lists_as_covered_has_a_counted_case(m):
    text = " | ".join(c["style"].lower() for c in m["cases"] if c["counts"])
    for style in ("hm-2", "death metal", "thrash", "djent", "deathcore"):
        assert style in text, style
    assert any("non-hm-2" in c["style"].lower() for c in m["cases"] if c["counts"])


def test_gaps_name_the_uncovered_styles(m):
    gaps = " | ".join(g["style"].lower() for g in m["gaps"])
    for style in ("black metal", "doom", "crust", "clean-ish"):
        assert style in gaps, style


def test_confirmed_flags_follow_the_rule(m):
    by = {c["id"]: c for c in m["cases"]}
    for cid in ("bloodbath_blend", "bloodbath_blend_forced", "bloodbath_hm2", "bloodbath_ubr"):
        assert by[cid]["confirmed"] is True
    for c in m["cases"]:
        if c["id"] not in ("bloodbath_blend", "bloodbath_blend_forced", "bloodbath_hm2", "bloodbath_ubr"):
            assert c["confirmed"] is False, c["id"]
    assert by["bloodbath_blend"]["di"]["path"] == "NailtheMix_March2023_Bloodbath_44k24b/17 GTR RHY L DI.wav"


def test_no_absolute_paths_and_no_audio_under_docs_benchmark(m):
    raw = json.loads((BENCH_DIR / "cases.json").read_text())

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k == "path" and isinstance(v, str) and o.get("parent") is None:
                    yield v
                yield from walk(v)
        elif isinstance(o, list):
            for v in o:
                yield from walk(v)
    paths = list(walk(raw["cases"]))
    assert paths and all(MF.is_relative_path(p) for p in paths), [p for p in paths if not MF.is_relative_path(p)]
    assert not [p for p in BENCH_DIR.rglob("*") if p.suffix.lower() in AUDIO]


@pytest.mark.parametrize("mutate,match", [
    (lambda d: d.update(schema="x"), "not a benchmark manifest"),
    (lambda d: d["cases"].append(dict(d["cases"][0])), "duplicate"),
    (lambda d: d["cases"][0]["di"].update(path="/abs/di.wav"), "relative"),
    (lambda d: d["cases"][2]["reference"].update(parent="bloodbath_mz"), "not a blend"),
    (lambda d: d["cases"][0].update(fit=[0, 10], heldOut=[5, 20]), "overlap"),
])
def test_validation_rejects_bad_manifests(mutate, match):
    d = json.loads((BENCH_DIR / "cases.json").read_text())
    mutate(d)
    with pytest.raises(MF.ManifestError, match=match):
        MF.validate(d)
