#!/usr/bin/env python3
"""Writes the synthetic calibration-metadata fixtures used by tests/test_chain_calibration.cpp (v0.8 I1).

Stdlib only; deterministic (sorted keys off, fixed field order). Run from anywhere:
    python3 tests/fixtures/nam/make_cal_fixtures.py [--check]

Each file is a NeuralAmpModelerCore "Linear" architecture model with weights [1, 0, 0, 0] (an exact identity, receptive
field 4) and invented input_level_dbu / output_level_dbu / gear_type metadata, so a test can read the gain a block
applies straight from the rendered signal. No TONE3000 capture and no third-party audio is involved.
"""
import json
import pathlib
import sys

FIXTURES = {
    # name: (gear_type, input_level_dbu, output_level_dbu)
    "cal_pedal_a.nam": ("pedal", 6.0, 10.0),
    "cal_pedal_b.nam": ("pedal", 6.0, 4.0),     # same input as pedal_a, output 6 dB lower
    "cal_amp_hi.nam": ("amp", 12.0, 0.0),
    "cal_amp_lo.nam": ("amp", 18.0, 0.0),       # input 6 dB higher than amp_hi
    "cal_amp_nometa.nam": ("amp", None, None),  # no calibration metadata at all
}


def build(gear, in_dbu, out_dbu, name):
    meta = {"name": name, "gear_type": gear, "modeled_by": "sawblade-tests"}
    if in_dbu is not None:
        meta["input_level_dbu"] = in_dbu
    if out_dbu is not None:
        meta["output_level_dbu"] = out_dbu
    return {"version": "0.5.4", "architecture": "Linear", "config": {"receptive_field": 4, "bias": False},
            "weights": [1.0, 0.0, 0.0, 0.0], "sample_rate": 48000, "metadata": meta}


def main():
    here = pathlib.Path(__file__).resolve().parent
    check = "--check" in sys.argv
    bad = 0
    for fn, (gear, i, o) in FIXTURES.items():
        text = json.dumps(build(gear, i, o, fn[:-4]), separators=(",", ":")) + "\n"
        p = here / fn
        if check:
            if not p.exists() or p.read_text() != text:
                print("DIFFERS:", fn)
                bad = 1
        else:
            p.write_text(text)
    return bad


if __name__ == "__main__":
    sys.exit(main())
