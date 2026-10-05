"""Phase 7.1: pedal.hm fitting against a synthetic reference (render pedal.hm at known params; the fit must recover
them) plus unit tests of the analysis helpers. Needs the tonerender binary (skipped without it)."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.calibrate import pedal_fit as PF

REPO = PF.REPO
DI = REPO / "tests" / "fixtures" / "di_riff.wav"
LAYOUT = PF.ProbeLayout(sweep_s=2.0, steps_s=3.2, di_s=8.0)

try:
    PF.tonerender_path()
    HAVE_CLI = True
except RuntimeError:
    HAVE_CLI = False
needs_cli = pytest.mark.skipif(not HAVE_CLI, reason="tonerender binary not found")


def test_probe_layout_and_determinism():
    a, lay, slots = PF.build_probe(DI, LAYOUT)
    b, _, _ = PF.build_probe(DI, LAYOUT)
    assert a.dtype == np.float32 and len(a) == lay.n_total and np.array_equal(a, b)
    assert len(slots) == 16 and np.max(np.abs(a[lay.sweep_sl])) == pytest.approx(0.1, abs=2e-3)
    assert np.max(np.abs(a[slots[0][2]])) == pytest.approx(10 ** (-30 / 20), abs=2e-3)
    full = PF.ProbeLayout()
    assert full.n_total == 45 * PF.FS


def test_labels_and_bands():
    assert PF.parse_labels("Boss HM-2 Lv-7 L-9 H-9 D-2") == {"level": 7, "low": 9, "high": 9, "distortion": 2}
    assert PF.parse_labels("Throne Torcher Maxed V2") is None
    assert PF.BAND_CENTRES[0] > 60 and PF.BAND_CENTRES[-1] <= 12000 and len(PF.BAND_CENTRES) == 23


def test_eq_correction_recovers_known_curve():
    truth = [("peak", 1200.0, 4.0, 1.5), ("lowShelf", 150.0, -3.0, 0.7)]
    target = PF.eq_response_db(truth)
    r = PF.fit_eq_correction(target, [("peak", 1000.0, 1.0, 1.0), ("lowShelf", 120.0, 0.0, 0.7)], ["fgq", "fg"])
    assert r["rms_after_db"] < 0.1 < r["rms_before_db"]


def test_harmonic_profile_of_known_distortion():
    f0 = 220.0
    t = np.arange(int(0.3125 * PF.FS)) / PF.FS
    x = np.sin(2 * np.pi * f0 * t) + 0.1 * np.sin(2 * np.pi * 3 * f0 * t)   # H3 at -20 dB
    h = PF.harmonic_profile(x, [(f0, -10.0, slice(0, len(x)))])
    assert h[0, 1] == pytest.approx(-20.0, abs=0.5)           # H3
    assert h[0, 0] == PF.HARM_FLOOR_DB                        # H2 absent


@needs_cli
def test_level_is_a_pure_output_gain(tmp_path):
    """The closed-form level solve relies on pedal.hm ending in a linear 3 dB/level gain."""
    probe, lay, slots = PF.build_probe(DI, LAYOUT)
    wav = tmp_path / "p.wav"
    sf.write(str(wav), probe, PF.FS, subtype="FLOAT")
    y8 = PF.render(PF.hm_preset(7, 3, 6, 8.0), wav, tmp_path / "a.wav", tmp_path)
    y5 = PF.render(PF.hm_preset(7, 3, 6, 5.0), wav, tmp_path / "b.wav", tmp_path)
    assert np.max(np.abs(y5 - y8 * 10 ** (-9 / 20))) < 1e-5 * np.max(np.abs(y8)) + 1e-7


@needs_cli
def test_synthetic_self_fit_recovers_params(tmp_path):
    probe, lay, slots = PF.build_probe(DI, LAYOUT)
    wav = tmp_path / "p.wav"
    sf.write(str(wav), probe, PF.FS, subtype="FLOAT")
    truth = dict(level=6.5, low=7.0, high=3.0, distortion=8.0)
    yref = PF.render(PF.hm_preset(truth["low"], truth["high"], truth["distortion"], truth["level"]), wav,
                     tmp_path / "ref.wav", tmp_path)
    ref = PF.features(yref, probe, lay, slots)
    ev = PF.Evaluator(wav, probe, lay, slots, tmp_path, jobs=4)
    fit = PF.fit_free(ref, ev, seed=PF.SEED, restarts=3, popsize=8, generations=12)
    res, _ = PF.evaluate_params(ref, ev, fit["low"], fit["high"], fit["distortion"])
    for k in ("low", "high", "distortion"):
        assert abs(res["params"][k] - truth[k]) < 0.3, (k, res["params"], truth)
    assert abs(res["params"]["level"] - truth["level"]) < 0.3
    assert res["ltas_rms_db"] < 0.1
    # determinism: same seed, same answer
    ev2 = PF.Evaluator(wav, probe, lay, slots, tmp_path, jobs=2)
    fit2 = PF.fit_free(ref, ev2, seed=PF.SEED, restarts=3, popsize=8, generations=12)
    assert fit2["low"] == fit["low"] and fit2["distortion"] == fit["distortion"]
