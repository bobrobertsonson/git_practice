"""D.1 synthetic known answer for the v0.4M feel terms: a hidden chain (fixture captures + pedal.ts boost + 24 dB/oct post
high-/low-pass + a gate above the DI floor) on a synthetic DI with real gaps and a -70 dBFS noise floor (made here, nothing
committed). The matcher must recover a chain whose LTAS error and feel features are within the D.1 tolerances.

Report (not a test): ``python tests/test_known_answer_feel.py OUT_DIR`` runs the same case with each --ablate item, with feel
weights 0 and with three weight sets, and writes ``OUT_DIR/feel_report.json`` (about 10 runs, several minutes).

TODO(D.1, later tasks): hidden irMix pair (offsetSamplesB / invertB, B2.1), fast bus comp with the studio detector firing
(B2.3) and not firing on the plain chain, hidden pre-EQ (B4). Add them to ``known_answer.feel_hidden``.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("sawblade_core")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sawblade_match.matcher import feel as F, known_answer as K      # noqa: E402
from sawblade_match.matcher.engine import RATE                         # noqa: E402
from sawblade_match.matcher.reference import build_target, make_excerpt, load_reference   # noqa: E402
from test_matcher import fixture_pool, mkplan                           # noqa: E402


def small_plan():
    return mkplan(top_k={"blend": 0, "single": 2, "single2": 0}, gens_linear=20, gens_gain=4, gens_final=10,
                  n_rescore_single=8, n_cab_single=2)


def test_gap_di_has_real_gaps_and_the_requested_noise_floor():
    x = K.gap_di(8.0, seed=3, floor_db=-70.0)
    assert x.dtype == np.float32 and len(x) == 8 * RATE and np.abs(x).max() < 1.0
    assert np.array_equal(x, K.gap_di(8.0, seed=3, floor_db=-70.0))           # seeded
    blocks = x[: len(x) // 4800 * 4800].astype(np.float64).reshape(-1, 4800)
    rms = 20 * np.log10(np.sqrt(np.mean(blocks ** 2, axis=1)))
    assert -72.0 < rms.min() < -68.0 and rms.max() > -40.0                      # real gaps (the floor) between audible notes


def test_feel_deltas_are_zero_for_the_reference_itself(tmp_path):
    pool = fixture_pool()
    di = K.gap_di(6.0)
    combo, v, preset, gate = K.feel_hidden(pool, di, seed=1)
    assert combo.boost and preset["gate"]["thresholdDb"] > K.gate_preset(K.gate_envelope_floor_db(di, RATE))["thresholdDb"] + 5
    assert v["post.hp_slope"] >= 0.5 and v["post.lp_slope"] >= 0.5 and "highPass" in {b["type"] for b in preset["postEq"]}
    hidden, _ = K.render(preset, di, float(RATE))
    import soundfile as sf
    sf.write(str(tmp_path / "h.wav"), hidden, RATE, subtype="FLOAT")
    ref = load_reference(tmp_path / "h.wav", channel="mid", matched="mono", offset_ms=0.0)
    ft = build_target(ref, make_excerpt(di, len(di) / RATE, window=(0, len(di)))).feel
    assert ft is not None and ft.mode == "paired" and ft.fizz_on and ft.plan.gap_ok
    d = K.feel_deltas(hidden, ft)
    assert all(d[k] is not None for k in K.FEEL_TOLERANCES if k != "aWeightedErrorDb")
    assert d["t12Ms"] == pytest.approx(0.0, abs=1e-6) and d["hfRatioDb"] == pytest.approx(0.0, abs=1e-6)
    assert d["fluxDb"] == pytest.approx(0.0, abs=1e-6) and d["floorDb"] == pytest.approx(0.0, abs=1e-6)
    d2 = K.feel_deltas(np.asarray(hidden) * 0 + np.convolve(hidden, np.ones(8) / 8, mode="same"), ft)       # darker render
    assert d2["hfRatioDb"] > 1.0


def test_feel_weights_context_restores_them():
    old = (F.W_TIGHT, F.W_FIZZ, F.W_POLISH)
    with K.feel_weights((0.0, 0.0, 0.0)):
        assert (F.W_TIGHT, F.W_FIZZ, F.W_POLISH) == (0.0, 0.0, 0.0)
    assert (F.W_TIGHT, F.W_FIZZ, F.W_POLISH) == old


def test_feel_known_answer_recovers_ltas_and_feel_within_the_d1_tolerances(tmp_path):
    row = K.feel_case(fixture_pool(), tmp_path / "ka", seed=1, plan=small_plan(), log=lambda m: None)
    tol = K.FEEL_TOLERANCES
    assert row["aWeightedErrorDb"] <= tol["aWeightedErrorDb"], row
    assert row["t12Ms"] <= tol["t12Ms"] and row["sustainDb"] <= tol["sustainDb"], row
    assert row["hfRatioDb"] <= tol["hfRatioDb"] and row["hfFlat"] <= tol["hfFlat"], row
    assert row["fluxDb"] <= tol["fluxDb"] and row["floorDb"] <= tol["floorDb"], row
    assert row["pass"] and row["topology"] == "single" and row["beforeStarterDb"] > row["aWeightedErrorDb"] + 3.0


if __name__ == "__main__":       # report script: python tests/test_known_answer_feel.py OUT_DIR
    import json
    out = Path(sys.argv[1])
    tab = K.feel_report(fixture_pool(), out, seed=1, plan=small_plan(), log=lambda m: None)
    keys = ("aWeightedErrorDb", "t12Ms", "sustainDb", "hfRatioDb", "hfFlat", "fluxDb", "floorDb", "pass")
    print(json.dumps({n: {k: r[k] for k in keys} for n, r in tab.items()}, indent=1, default=float))
