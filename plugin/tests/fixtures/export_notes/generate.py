#!/usr/bin/env python3
"""Regenerates the *.notes.json / *.export_notes.txt parity fixtures with the v0.4M Python notes module.

    git show origin/claude/sawblade-v0_4m-matcher-feel:match/sawblade_match/export/notes.py > /some/dir/notes.py
    python3 generate.py /some/dir/notes.py          # run from anywhere; writes next to this script

(notes.py is pure Python, no other imports from the matcher; v0.4M commit 9074c0b, NOTES_VERSION 1.)
Each case is one call of build_export_notes(preset_dict, {"mode": ..., "bypassed": [...]}, nam_name, ir_name) and
format_notes_txt(notes, preset["name"]). The C++ port (plugin/src/ExportNotes.cpp) must reproduce them
(plugin/tests/test_export_notes.cpp). When v0.4M changes the format, regenerate and follow in the port.
"""
import importlib.util
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
CASES = {
    # name: (preset file, plan, nam_name, ir_name)
    "nocab": ("nocab_preset.json", {"mode": "nocab", "bypassed": []}, None, None),
    "nocab_named": ("nocab_preset.json", {"mode": "nocab", "bypassed": []}, "nocab-standard.nam", "nocab.ir.wav"),
    "withcab_dropcomp": ("withcab_preset.json", {"mode": "withcab", "bypassed": [{"what": "busComp"}]}, None, None),
    "withcab_keepcomp": ("withcab_preset.json", {"mode": "withcab", "bypassed": []}, None, None),
    "irmix_named": ("irmix_preset.json", {"mode": "nocab", "bypassed": []}, "irmix-nocab-standard.nam", "irmix-nocab.ir.wav"),
}


def main() -> None:
    spec = importlib.util.spec_from_file_location("notes", sys.argv[1])
    notes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(notes)
    assert notes.NOTES_VERSION == 1
    for name, (preset_file, plan, nam, ir) in CASES.items():
        preset = json.loads((HERE / preset_file).read_text())
        n = notes.build_export_notes(preset, plan, nam, ir)
        (HERE / f"{name}.notes.json").write_text(json.dumps(n, indent=2, sort_keys=True) + "\n")
        (HERE / f"{name}.export_notes.txt").write_text(notes.format_notes_txt(n, preset.get("name")), encoding="utf-8")


if __name__ == "__main__":
    main()
