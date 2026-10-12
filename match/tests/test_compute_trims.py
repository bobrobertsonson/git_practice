"""scripts/compute_trims.py: the text patch that writes output.autoTrimDb / autoTrimHash into a preset file.

Pure functions only (no tonerender, no sawblade_core): the formatting of the hand-written preset files must survive, the
patched file must stay valid JSON and the second patch must change nothing.
"""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("compute_trims", REPO / "scripts" / "compute_trims.py")
CT = importlib.util.module_from_spec(spec)
spec.loader.exec_module(CT)

HASH = "ab" * 32


def test_inline_output_object_is_extended_in_place():
    text = '{\n  "schema": "sawblade.preset",\n  "version": 1,\n  "name": "x",\n  "output": { "gainDb": -3.0 },\n  "blend": 0.5\n}\n'
    out = CT.patch_preset(text, -7.54567, HASH)
    j = json.loads(out)
    assert j["version"] == 3
    assert j["output"] == {"gainDb": -3.0, "autoTrimDb": -7.546, "autoTrimHash": HASH}
    assert '"output": { "gainDb": -3.0, "autoTrimDb": -7.546, "autoTrimHash": "' in out
    assert out.endswith('"blend": 0.5\n}\n')  # the rest of the file is byte for byte as it was
    assert out.startswith(text[: text.index('"output"')].replace('"version": 1', '"version": 3'))


def test_multi_line_output_object():
    text = '{\n    "version": 2,\n    "output": {\n        "gainDb": 1\n    }\n}\n'
    j = json.loads(CT.patch_preset(text, 2.0, HASH))
    assert j["output"] == {"gainDb": 1, "autoTrimDb": 2.0, "autoTrimHash": HASH}
    assert j["version"] == 3


def test_missing_output_object_is_appended_with_the_files_indent():
    text = '{\n    "schema": "sawblade.preset",\n    "version": 1,\n    "cab": {"mode": "shared"}\n}\n'
    out = CT.patch_preset(text, -5.0, HASH)
    assert json.loads(out)["output"] == {"autoTrimDb": -5.0, "autoTrimHash": HASH}
    assert '\n    "output": {\n        "autoTrimDb": -5.0' in out
    assert out.endswith("\n}\n")


def test_patch_is_idempotent_and_replaces_an_old_trim():
    text = '{\n  "version": 3,\n  "output": { "gainDb": 0.0 },\n  "blend": 0.5\n}\n'
    once = CT.patch_preset(text, -6.0, HASH)
    assert CT.patch_preset(once, -6.0, HASH) == once
    again = json.loads(CT.patch_preset(once, -4.0, "cd" * 32))
    assert again["output"]["autoTrimDb"] == -4.0
    assert again["output"]["autoTrimHash"] == "cd" * 32
    assert list(again["output"]).count("autoTrimDb") == 1


def test_a_nested_version_key_is_not_touched():
    text = '{\n  "version": 1,\n  "paths": {"a": {"blocks": [{"id": "a1", "type": "pedal.hm", "version": 1}]}}\n}\n'
    j = json.loads(CT.patch_preset(text, -1.0, HASH))
    assert j["version"] == 3
    assert j["paths"]["a"]["blocks"][0]["version"] == 1


def test_committed_presets_keep_their_formatting_when_repatched():
    """Every committed preset that has a trim: patching it with its own values changes nothing."""
    n = 0
    for f in sorted((REPO / "presets").rglob("*.json")):
        text = f.read_text()
        out = json.loads(text).get("output", {})
        if "autoTrimDb" not in out:
            continue
        assert CT.patch_preset(text, out["autoTrimDb"], out["autoTrimHash"]) == text, f
        n += 1
    assert n >= 30
