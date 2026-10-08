"""``sawblade-calibrate device-null`` on synthetic data: a known delay, gain, polarity and gentle filter must be recovered."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.calibrate import device_null as DN
from sawblade_match.calibrate.cli import main

RATE = DN.RATE
REPO = Path(__file__).resolve().parents[2]
PRESETS = REPO / "tests" / "fixtures" / "presets"


def guitar_like(seconds: float, seed: int = 5) -> np.ndarray:
    """Seeded bursty, harmonic, decaying notes + a little noise: has the envelope structure of a played riff."""
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    y = np.zeros(n)
    t = np.arange(int(0.5 * RATE)) / RATE
    pos = 0
    while pos < n - len(t):
        f0 = rng.choice([82.4, 110.0, 146.8, 196.0, 247.0])
        note = sum(np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28)) / h for h in range(1, 9)) * np.exp(-t * rng.uniform(4, 9))
        y[pos:pos + len(t)] += note * rng.uniform(0.3, 1.0)
        pos += int(rng.uniform(0.18, 0.42) * RATE)
    y += 0.003 * rng.standard_normal(n)
    return 0.3 * y / np.max(np.abs(y))


def device_of(ref: np.ndarray, delay_samples: float, gain_db: float, invert: bool = False, lowpass_hz: float | None = 8000.0,
              noise_db: float = -60.0, tail: int = 20000, seed: int = 9) -> np.ndarray:
    """What a gentle device path does to the render: a delay (fractional), a gain, optional polarity flip, a 1st-order low-pass
    and a little noise; plus leading/trailing silence of a real recording."""
    y = DN.shift(ref, delay_samples, len(ref) + int(np.ceil(delay_samples)) + tail)
    if lowpass_hz:
        b, a = signal.butter(1, lowpass_hz, fs=RATE)
        y = signal.lfilter(b, a, y)
    y = y * 10 ** (gain_db / 20) * (-1 if invert else 1)
    rng = np.random.default_rng(seed)
    return y + rng.standard_normal(len(y)) * 10 ** ((noise_db + gain_db) / 20) * np.sqrt(np.mean(ref ** 2))


def test_recovers_delay_gain_and_gentle_filter():
    ref = guitar_like(12.0)
    d = 1795.37 + RATE / (2 * np.pi * 8000.0)             # 37.4 ms fractional delay + the 8 kHz 1st-order LP's low-frequency group delay (0.95 sample)
    rep = DN.analyse(device_of(ref, 1795.37, -7.3), ref)
    al = rep["alignment"]
    assert abs(al["latencySamples"] - d) < 0.2 and abs(al["latencyMs"] - d / RATE * 1000) < 0.005     # < 0.2 sample
    assert al["peakConfidence"] > 1.05 and al["correlation"] > 0.9
    assert abs(rep["gain"]["deviceVsRenderDb"] - (-7.3)) < 0.5            # the gentle LP changes the LS gain a little
    assert rep["polarity"]["applied"] == "normal"
    # the filter shows up where it is: ~-3 dB at the 8 kHz octave, ~0 dB at <= 1 kHz
    by = {b["centreHz"]: b for b in rep["bands"]}
    assert abs(by[500]["levelDiffDb"]) < 0.8 and abs(by[1000]["levelDiffDb"]) < 0.8
    assert by[8000]["levelDiffDb"] < -2.0
    # residual: only the filter + noise are left, and it is explained by per-band level
    assert -50.0 < rep["residual"]["overallDb"] < -30.0
    assert rep["residual"]["afterPerBandGainDb"] < rep["residual"]["overallDb"] - 5.0
    assert not rep["warnings"]


def test_no_filter_nulls_deep_and_detects_inverted_polarity():
    ref = guitar_like(10.0, seed=6)
    rep = DN.analyse(device_of(ref, 2500.8, 3.0, invert=True, lowpass_hz=None, noise_db=-70.0), ref)
    assert rep["polarity"]["applied"] == "inverted" and rep["polarity"]["detectedFromCorrelation"] == "inverted"
    assert abs(rep["gain"]["deviceVsRenderDb"] - 3.0) < 0.1 and abs(rep["alignment"]["latencySamples"] - 2500.8) < 0.05
    assert rep["residual"]["overallDb"] < -55.0                           # the noise floor is -70 dB
    # forcing the wrong polarity makes the null terrible instead of silently flipping it
    wrong = DN.analyse(device_of(ref, 2500.8, 3.0, invert=True, lowpass_hz=None), ref, polarity="normal")
    assert wrong["residual"]["overallDb"] > -3.0 and any("forced polarity" in w for w in wrong["warnings"])
    v = DN.verdict(rep["residual"]["overallDb"], -30.0, rep)
    assert v["withinTolerance"] is True and "indistinguishable" in v["summary"]
    v2 = DN.verdict(wrong["residual"]["overallDb"], -30.0, wrong)
    assert v2["withinTolerance"] is False and "clearly different" in v2["summary"]


def test_a_nonlinear_device_is_flagged_as_not_a_plain_filter_difference():
    ref = guitar_like(10.0, seed=7)
    dev = device_of(ref, 900.0, 0.0, lowpass_hz=None, noise_db=-80.0)
    dev = np.tanh(8.0 * dev) / 8.0                                        # a saturating device stage
    rep = DN.analyse(dev, ref)
    assert -25.0 < rep["residual"]["overallDb"] < -5.0
    hints = DN.verdict(rep["residual"]["overallDb"], -30.0, rep)["whatDiffers"]
    assert any("non-linear" in h for h in hints)


def test_refuses_mismatched_and_silent_recordings():
    ref = guitar_like(6.0)
    with pytest.raises(ValueError, match="silent"):
        DN.analyse(np.zeros(len(ref)), ref)
    with pytest.raises(ValueError, match="shorter than 1 s"):
        DN.analyse(ref[:1000], ref)
    clipped = DN.analyse(np.clip(device_of(ref, 400.0, 6.0, lowpass_hz=None) * 4.0, -1.0, 1.0), ref)
    assert any("clips" in w for w in clipped["warnings"])


def test_cli_with_render_files_writes_report_and_30s_listening_pair(tmp_path, capsys):
    ref = guitar_like(40.0, seed=8)
    dev = device_of(ref, 1234.5, -4.0)
    sf.write(tmp_path / "render.wav", ref.astype(np.float32), RATE, subtype="FLOAT")
    sf.write(tmp_path / "rec_stereo.wav", np.stack([dev, dev * 0.1], axis=1).astype(np.float32), RATE, subtype="FLOAT")   # stereo, left used
    out = tmp_path / "out"
    rc = main(["device-null", "--recording", str(tmp_path / "rec_stereo.wav"), "--render", str(tmp_path / "render.wav"),
               "--di", str(tmp_path / "render.wav"), "--out", str(out)])
    assert rc == 0
    rep = json.loads((out / "device_null_report.json").read_text())
    assert abs(rep["alignment"]["latencySamples"] - (1234.5 + RATE / (2 * np.pi * 8000.0))) < 0.2 and abs(rep["gain"]["deviceVsRenderDb"] + 4.0) < 0.5
    assert rep["schema"] == "sawblade.device_null" and len(rep["bands"]) == 9 and rep["verdict"]["toleranceDb"] == -30.0
    assert rep["alignment"]["lagVsDiMs"] == pytest.approx(rep["alignment"]["coarseEnvelopeLagMs"], abs=1.5)
    lst = out / "listen"
    assert {f.name for f in lst.iterdir() if f.suffix == ".wav"} == {"ab_render_then_device.wav", "render_aligned.wav",
                                                                    "device_aligned_levelmatched.wav"}
    a, fs = sf.read(str(lst / "render_aligned.wav"))
    b, _ = sf.read(str(lst / "device_aligned_levelmatched.wav"))
    ab, _ = sf.read(str(lst / "ab_render_then_device.wav"))
    assert fs == RATE and len(a) == len(b) == 30 * RATE and len(ab) == 2 * 30 * RATE + int(0.8 * RATE)
    # level-matched and aligned: the two excerpts correlate ~1 at lag 0 and have the same RMS
    assert np.corrcoef(a, b)[0, 1] > 0.95
    assert abs(20 * np.log10(np.sqrt(np.mean(a ** 2)) / np.sqrt(np.mean(b ** 2)))) < 1.0
    out_txt = capsys.readouterr().out
    assert "latency +25." in out_txt


def test_cli_mismatched_render_warns_and_missing_inputs_error(tmp_path, capsys):
    ref = guitar_like(8.0)
    sf.write(tmp_path / "r.wav", ref.astype(np.float32), RATE)
    sf.write(tmp_path / "rec.wav", device_of(ref, 300.0, 0.0).astype(np.float32), RATE)
    assert main(["device-null", "--recording", str(tmp_path / "rec.wav"), "--out", str(tmp_path / "o")]) == 3
    assert "--render" in capsys.readouterr().err
    # the DI is 5 ms off the render: the cross-check warns that the render is not sample-aligned to the DI
    di = np.concatenate([np.zeros(int(0.012 * RATE)), ref])
    sf.write(tmp_path / "di.wav", di.astype(np.float32), RATE)
    assert main(["device-null", "--recording", str(tmp_path / "rec.wav"), "--render", str(tmp_path / "r.wav"),
                 "--di", str(tmp_path / "di.wav"), "--out", str(tmp_path / "o2")]) == 0
    assert "not sample-aligned to the DI" in capsys.readouterr().out


def test_preset_plus_di_renders_through_the_core_and_nulls(tmp_path):
    pytest.importorskip("sawblade_match.core")
    di = guitar_like(8.0, seed=11)
    sf.write(tmp_path / "di.wav", di.astype(np.float32), RATE, subtype="FLOAT")
    ref = DN.render_preset(PRESETS / "golden_shared.json", tmp_path / "di.wav")
    sf.write(tmp_path / "rec.wav", device_of(ref, 2400.0, -2.0, lowpass_hz=None, noise_db=-80.0).astype(np.float32), RATE,
             subtype="FLOAT")
    out = tmp_path / "o"
    assert main(["device-null", "--recording", str(tmp_path / "rec.wav"), "--preset", str(PRESETS / "golden_shared.json"),
                 "--di", str(tmp_path / "di.wav"), "--out", str(out)]) == 0
    rep = json.loads((out / "device_null_report.json").read_text())
    assert (out / "render.wav").is_file()
    assert abs(rep["alignment"]["latencySamples"] - 2400.0) < 0.2 and rep["residual"]["overallDb"] < -45.0
    assert rep["verdict"]["withinTolerance"] is True


def _band_edit(x: np.ndarray, lo: float, hi: float, gain_db: float) -> np.ndarray:
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    X[(f >= lo) & (f < hi)] *= 10 ** (gain_db / 20)
    return np.fft.irfft(X, len(x))


def test_band_powers_are_normalised_and_an_empty_band_does_not_enter_the_spread():
    rng = np.random.default_rng(3)
    n = 10 * RATE
    sos = signal.butter(8, 4000, btype="low", fs=RATE, output="sos")
    ref = signal.sosfilt(sos, guitar_like(10.0, seed=12) + 0.0)
    burst = np.zeros(n)
    for k in range(0, n - 2400, RATE // 2):                              # content in the 8 kHz octave: sparse tone bursts
        burst[k:k + 2400] = 0.05 * np.sin(2 * np.pi * 8000 * np.arange(2400) / RATE) * np.hanning(2400)
    ref = ref + burst
    ref = _band_edit(ref, 11314, RATE / 2, -200.0)                        # the 16 kHz octave is empty
    dev = _band_edit(ref, 5657, 11314, +6.0)                              # only the 8 kHz octave differs (+6 dB)
    dev = dev + _band_edit(rng.standard_normal(n) * 1e-7, 0, 11314, -200.0)          # garbage far below the content
    rep = DN.analyse(dev, ref)
    by = {b["centreHz"]: b for b in rep["bands"]}
    # normalised: the band powers add up to the time-domain power (no length-dependent offset)
    tot = 10 * np.log10(sum(10 ** (b["renderDb"] / 10) for b in rep["bands"]))
    assert abs(tot - rep["gain"]["rmsRenderDbfs"]) < 0.5
    assert by[8000]["levelDiffDb"] > 4.0 and abs(by[1000]["levelDiffDb"]) < 0.5
    assert by[16000]["renderDb"] < by[500]["renderDb"] - 60                # genuinely (near) empty
    assert DN.content_bands(rep["bands"]) and 16000 not in [b["centreHz"] for b in DN.content_bands(rep["bands"])]
    assert 4.0 < rep["bandLevelSpreadDb"] < 8.0                            # the one real difference, not the empty band's noise
    v = DN.verdict(rep["residual"]["overallDb"], -30.0, rep)
    assert any("largest at 8000 Hz" in h for h in v["whatDiffers"]) and v["bandsOutsideTolerance"] == [8000]
    assert v["withinTolerance"] is False


def test_verdict_needs_the_bands_as_well_as_the_overall_residual():
    ref = guitar_like(12.0)
    rep = DN.analyse(device_of(ref, 1795.37, -7.3), ref)                   # overall about -41 dB, but the 8 / 16 kHz bands are low
    assert rep["residual"]["overallDb"] < -30.0
    v = DN.verdict(rep["residual"]["overallDb"], -30.0, rep)
    assert v["overallOk"] is True and v["bandsOk"] is False and v["withinTolerance"] is False
    assert DN.verdict(rep["residual"]["overallDb"], -30.0, rep, band_tolerance_db=20.0)["withinTolerance"] is True


def test_a_tanh_distorted_render_is_not_a_match():
    ref = guitar_like(10.0, seed=7)
    dev = np.tanh(8.0 * device_of(ref, 900.0, 0.0, lowpass_hz=None, noise_db=-80.0)) / 8.0
    rep = DN.analyse(dev, ref)
    v = DN.verdict(rep["residual"]["overallDb"], -30.0, rep)
    assert v["withinTolerance"] is False and v["overallOk"] is False


@pytest.mark.parametrize("rate", [44100, 96000])
def test_recordings_at_other_sample_rates_go_through_the_resample_path(tmp_path, rate):
    from math import gcd
    ref = guitar_like(14.0, seed=13)
    dev = device_of(ref, 1500.25, -3.0, lowpass_hz=None, noise_db=-70.0)
    g = gcd(rate, RATE)
    rec = signal.resample_poly(dev, rate // g, RATE // g)
    sf.write(tmp_path / "rec.wav", rec.astype(np.float32), rate, subtype="FLOAT")
    sf.write(tmp_path / "render.wav", ref.astype(np.float32), RATE, subtype="FLOAT")
    out = tmp_path / "o"
    assert main(["device-null", "--recording", str(tmp_path / "rec.wav"), "--render", str(tmp_path / "render.wav"),
                 "--out", str(out)]) == 0
    r = json.loads((out / "device_null_report.json").read_text())
    assert r["inputs"]["recordingRateHz"] == rate
    assert abs(r["alignment"]["latencySamples"] - 1500.25) < 0.3 and abs(r["gain"]["deviceVsRenderDb"] + 3.0) < 0.2
    assert r["residual"]["overallDb"] < -40.0 and r["verdict"]["withinTolerance"] is True


def test_cli_model_and_ir_render_the_exported_files_through_the_core(tmp_path, capsys):
    pytest.importorskip("sawblade_match.core")
    di = guitar_like(8.0, seed=14)
    sf.write(tmp_path / "di.wav", di.astype(np.float32), RATE, subtype="FLOAT")
    nam, ir = REPO / "tests" / "fixtures" / "nam" / "wavenet.nam", REPO / "tests" / "fixtures" / "ir" / "ir_a.wav"
    ref = DN.render_model(nam, ir, tmp_path / "di.wav")
    assert len(ref) == len(di) and np.max(np.abs(ref)) > 1e-3
    sf.write(tmp_path / "rec.wav", device_of(ref, 2000.0, -5.0, lowpass_hz=None, noise_db=-80.0).astype(np.float32), RATE, subtype="FLOAT")
    out = tmp_path / "o"
    assert main(["device-null", "--recording", str(tmp_path / "rec.wav"), "--model", str(nam), "--ir", str(ir),
                 "--di", str(tmp_path / "di.wav"), "--out", str(out)]) == 0
    r = json.loads((out / "device_null_report.json").read_text())
    assert "model" in r["inputs"]["render"] and "IR" in r["inputs"]["render"]
    assert abs(r["alignment"]["latencySamples"] - 2000.0) < 0.2 and r["verdict"]["withinTolerance"] is True
    txt = capsys.readouterr().out
    assert txt.index("octave band") < txt.index("latency +")                # the band table comes first
    # the no-IR form renders the model alone (different from model + IR)
    assert not np.allclose(DN.render_model(nam, None, tmp_path / "di.wav"), ref)
