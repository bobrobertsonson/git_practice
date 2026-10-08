#!/usr/bin/env python3
"""Regenerates the calibrated_*.notes.json / calibrated_*.export_notes.txt parity fixtures (v0.8 I4b) with the matcher's export code.

    cd <repo> && PYTHONPATH=match python3 plugin/tests/fixtures/export_notes/generate_calibration.py

Each case is `build_export_notes(preset, plan)` with `notes["calibration"] = plan.reference_levels(...)` set exactly as
`write_export_notes(calibration=...)` does, and `format_notes_txt(notes, preset name)`. The same notes JSON goes to the C++
`formatNotesTxt` (plugin/tests/test_export_notes.cpp) and to pytest (match/tests/test_export_notes_parity.py); both must give the
committed text, so the calibration lines cannot drift apart between the two sides.
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / "match"))
from sawblade_match.export import notes as N  # noqa: E402
from sawblade_match.export import plan as P  # noqa: E402


def probe(device=12.0, assumed=True, a_out=-3.0, b_out=-3.0, mode="calibrated"):
    blk = lambda out, i: [{"id": i, "kind": "nam", "captureInputDbu": 5.0, "captureOutputDbu": out,
                           "inputMissing": False, "outputMissing": out is None}]
    cal = {"enabled": mode == "calibrated", "mode": mode, "deviceDbu": device, "deviceAssumed": assumed, "anyUncalibrated": False,
           "paths": {"a": blk(a_out, "a1"), "b": blk(b_out, "b1")}}
    return {"warnings": [], "calibration": cal}


def single(preset):  # the audible path is A alone
    p = json.loads(json.dumps(preset))
    p["blend"] = 0.0
    p["paths"]["b"]["enabled"] = False
    return p


CASES = {
    # name: (preset file, tweak, render report, post-capture gain (dB, or None = not measured))
    "calibrated_assumed": ("nocab_preset.json", single, probe(), 6.0),
    "calibrated_given": ("nocab_preset.json", single, probe(device=9.5, assumed=False, a_out=-1.25), 0.0),
    "calibrated_blend_input_only": ("nocab_preset.json", lambda p: p, probe(), 0.0),
    "calibrated_unmeasured": ("nocab_preset.json", single, probe(), None),
    "calibrated_off": ("nocab_preset.json", single, probe(mode="legacy"), 0.0),
}


def render_case(name: str) -> tuple[str, str]:
    """(notes JSON text, export_notes.txt text) of one case, exactly as they are committed."""
    preset_file, tweak, report, g = CASES[name]
    preset = tweak(json.loads((HERE / preset_file).read_text()))
    post = (lambda k, last: (g, None)) if g is not None else None
    notes = N.build_export_notes(preset, {"mode": "nocab", "bypassed": []}, "nocab-standard.nam", "nocab.ir.wav")
    notes["calibration"] = P.reference_levels(preset, report, post=post)
    return json.dumps(notes, indent=2, sort_keys=True) + "\n", N.format_notes_txt(notes, preset.get("name"))


def main() -> None:
    for name in CASES:
        notes_json, txt = render_case(name)
        (HERE / f"{name}.notes.json").write_text(notes_json)
        (HERE / f"{name}.export_notes.txt").write_text(txt, encoding="utf-8")


if __name__ == "__main__":
    main()
