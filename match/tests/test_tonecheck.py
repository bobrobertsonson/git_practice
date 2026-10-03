"""Tone-check tests: synthetic signals only (plus an end-to-end test when tonerender exists)."""
import json
import shutil
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.tonecheck import analysis as A
from sawblade_match.tonecheck import cli
from sawblade_match.tonecheck.render import REPO_ROOT, find_tonerender
from sawblade_match.tonecheck.rules import classify, evaluate_rules, load_targets, parse_expr

FS = 48000
TARGETS = load_targets(REPO_ROOT / "docs" / "tone_targets.json")
SEED = 12345


def pink(n, seed=SEED):
    rng = np.random.default_rng(seed)
    X = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / FS)
    f[0] = f[1]
    X /= np.sqrt(f)
    X[0] = 0
    x = np.fft.irfft(X, n)
    return x / np.std(x)


def analytic_rel_db(sos, centres):
    """Band levels (re 1 kHz band) of pink noise through sos, by numerical integration of |H|^2/f."""
    f = np.geomspace(10, 20000, 400000)
    _, h = signal.sosfreqz(sos, worN=f, fs=FS)
    p = np.abs(h) ** 2 / f
    out = []
    for c in centres:
        ec = A._exact_centre(c)
        sel = (f >= ec * 2 ** (-1 / 6)) & (f < ec * 2 ** (1 / 6))
        out.append(10 * np.log10(np.trapezoid(p[sel], f[sel])))
    out = np.array(out)
    return out - out[centres.index(1000)]


@pytest.mark.parametrize("name,sos", [
    ("flat", None),
    ("lowpass2k", signal.butter(2, 2000, fs=FS, output="sos")),
    ("highpass200", signal.butter(2, 200, "highpass", fs=FS, output="sos")),
    ("peak1k", signal.tf2sos(*signal.iirpeak(1000, 2.0, fs=FS))),
])
def test_pink_through_biquads_band_levels(name, sos):
    if sos is None:
        sos = np.array([[1, 0, 0, 1, 0, 0.0]])
    x = signal.sosfilt(sos, pink(FS * 120))
    mask = np.ones(len(x), bool)
    freqs, psd, _ = A.ltas_psd(x, FS, mask)
    _, rel = A.band_levels_db(freqs, psd)
    exp = analytic_rel_db(sos, A.NOMINAL_CENTRES)
    c = np.array(A.NOMINAL_CENTRES)
    sel = (c >= 100) & (c <= 10000) & (exp > -35)  # skip bands too low in level / too few FFT bins
    assert np.max(np.abs(rel[sel] - exp[sel])) < 0.5, (name, rel[sel] - exp[sel])


def test_group_level_is_power_mean():
    rel = np.full(len(A.NOMINAL_CENTRES), -100.0)
    cs = A.NOMINAL_CENTRES
    rel[cs.index(80)] = 0.0
    g = A.group_levels(rel, {"thump": [80, 100, 125, 160]})
    assert g["thump"] == pytest.approx(10 * np.log10(0.25), abs=1e-6)


def test_parse_expr():
    e = parse_expr("sub <= thump - 12")
    assert (e.lhs, e.op, e.rhs, e.offset) == ("sub", "<=", "thump", -12.0)
    e = parse_expr("thump <= saw + 4")
    assert e.offset == 4.0
    assert parse_expr("lowMid <= thump").offset == 0.0
    with pytest.raises(ValueError):
        parse_expr("a < b")


def test_classify_boundaries():
    assert classify(0.0, 2) == "pass"
    assert classify(-0.01, 2) == "marginal"
    assert classify(-2.0, 2) == "marginal"
    assert classify(-2.01, 2) == "fail"


def test_rule_evaluation_with_tolerance():
    groups = {"sub": -20.0, "thump": 0.0, "saw": 0.0, "lowMid": -3.0, "dipMid": -5.0, "presence": -4.0, "fizz": -20.0}
    res = {r["id"]: r for r in evaluate_rules(groups, TARGETS["rules"])}
    assert all(r["status"] == "pass" for r in res.values())
    assert res["sub_rolled_off"]["margin"] == pytest.approx(8.0)
    groups.update(sub=-10.0, dipMid=-0.5, fizz=-8.0)  # sub: 2 over (marginal); dip: 1.5-... see below
    res = {r["id"]: r for r in evaluate_rules(groups, TARGETS["rules"])}
    assert res["sub_rolled_off"]["status"] == "marginal"      # -10 <= -12 violated by 2 dB, tol 2
    assert res["sub_rolled_off"]["margin"] == pytest.approx(-2.0)
    assert res["saw_over_dip"]["status"] == "marginal"        # -0.5 <= -2 violated by 1.5, tol 1.5
    assert res["fizz_rolled_off"]["status"] == "fail"         # -8 <= -15 violated by 7, tol 3
    # >= rule direction
    groups.update(thump=-8.0, saw=0.0)
    res = {r["id"]: r for r in evaluate_rules(groups, TARGETS["rules"])}
    assert res["thump_present"]["margin"] == pytest.approx(-2.0)
    assert res["thump_present"]["status"] == "marginal"


def test_activity_gate_excludes_silence():
    n = FS * 20
    x = np.zeros(n)
    x[: n // 2] = 0.1 * pink(n // 2)  # active first half, then digital silence + -90 dB hiss
    rng = np.random.default_rng(SEED)
    x[n // 2:] = 3e-5 * rng.standard_normal(n - n // 2)
    mask, frames, _ = A.activity_mask(x, FS)
    assert 0.45 < frames.mean() < 0.52
    assert not mask[int(n * 0.55):].any()
    # LTAS of gated signal matches LTAS of the active part alone
    freqs, psd, _ = A.ltas_psd(x, FS, mask)
    freqs2, psd2, _ = A.ltas_psd(x[: n // 2], FS, np.ones(n // 2, bool))
    _, r1 = A.band_levels_db(freqs, psd)
    _, r2 = A.band_levels_db(freqs2, psd2)
    assert np.max(np.abs(r1 - r2)[8:-6]) < 0.5
    # gap noise: hiss is ~ -90 dB re active RMS ~ -20 dBFS => about -70 dB
    di = np.zeros(n)
    di[: n // 2] = 0.1 * pink(n // 2)
    di[n // 2:] = 3e-5 * rng.standard_normal(n - n // 2)
    g = A.gap_noise_db(x, di, FS)
    assert g["value"] < -60
    assert g["diNoiseFloorDb"] < -80


def test_flatness_white_vs_sine():
    rng = np.random.default_rng(SEED)
    white = rng.standard_normal(FS * 30)
    t = np.arange(FS * 30) / FS
    sine = np.sin(2 * np.pi * 1500 * t) + 1e-4 * rng.standard_normal(len(t))
    m = np.ones(len(t), bool)
    fw, pw, _ = A.ltas_psd(white, FS, m)
    fs_, ps, _ = A.ltas_psd(sine, FS, m)
    assert A.buzz_flatness(fw, pw) > 0.9
    assert A.buzz_flatness(fs_, ps) < 0.05


def _synthetic_decays(tau, onsets_s, dur=8.0, f0=120.0):
    n = int(dur * FS)
    t = np.arange(n) / FS
    rng = np.random.default_rng(SEED)
    di = 1e-4 * rng.standard_normal(n)
    out = np.zeros(n)
    for o in onsets_s:
        i = int(o * FS)
        m = n - i
        tt = np.arange(m) / FS
        burst = np.exp(-tt / 0.15) * rng.standard_normal(m)  # DI: noisy pluck, 150 ms decay
        di[i:] += 0.3 * burst
        out[i:] += np.exp(-tt / tau) * np.sin(2 * np.pi * f0 * (tt + o))
    return di, out


@pytest.mark.parametrize("tau", [0.05, 0.1, 0.2])
def test_low_tightness_known_decay(tau):
    onsets = [0.5, 2.0, 3.5, 5.0, 6.5]
    di, out = _synthetic_decays(tau, onsets)
    detected = A.detect_onsets(di, FS)
    assert len(detected) == len(onsets)
    assert np.max(np.abs(detected - np.array(onsets))) < 0.03
    r = A.low_tightness_ms(di, out, FS)
    expected = 1000 * tau * np.log(10)  # amplitude to 0.1 = -20 dB
    assert r["nMeasured"] == len(onsets)
    assert r["valueMs"] == pytest.approx(expected, rel=0.10)


def test_low_tightness_without_di_is_null():
    x = pink(FS * 5) * 0.1
    a = A.analyze(x, FS, TARGETS)
    assert a.metrics["lowTightnessMs"]["valueMs"] is None


def test_crest_factor_sine():
    t = np.arange(FS * 2) / FS
    x = 0.5 * np.sin(2 * np.pi * 440 * t)
    assert A.crest_factor_db(x, np.ones(len(x), bool)) == pytest.approx(3.01, abs=0.05)


def test_loudness_range_two_levels():
    n = FS * 20
    x = 0.5 * pink(n) * 0.1
    x[n // 2:] *= 10 ** (-10 / 20)  # 10 dB quieter second half
    lra = A.loudness_range_lu(x)
    assert 8.0 < lra < 11.0
    assert A.loudness_range_lu(x[: FS * 2]) is None


def test_44k_input_is_resampled_consistently():
    x = pink(44100 * 60)
    a = A.analyze(x, 44100, TARGETS)
    c = A.NOMINAL_CENTRES
    assert abs(a.rel_db[c.index(250)]) < 0.5  # pink is flat per 1/3-octave


def test_reference_comparison_and_aweighting():
    x = pink(FS * 60)
    sos = signal.butter(2, 3000, fs=FS, output="sos")
    a1 = A.analyze(x, FS, TARGETS)
    a2 = A.analyze(signal.sosfilt(sos, x), FS, TARGETS)
    same = cli.compare_to_reference(a1, a1)
    assert same["aWeightedErrorDb"] == pytest.approx(0, abs=1e-9)
    diff = cli.compare_to_reference(a2, a1)
    assert diff["aWeightedErrorDb"] > 1.0
    assert cli.a_weight_db(np.array([1000.0]))[0] == pytest.approx(0.0, abs=0.05)


def test_audio_mode_writes_report(tmp_path):
    x = (0.1 * pink(FS * 20)).astype(np.float32)
    wav = tmp_path / "x.wav"
    sf.write(wav, x, FS, subtype="FLOAT")
    rc = cli.main(["--audio", str(wav), "--ref", str(wav), "--ref-channel", "left", "--out", str(tmp_path / "o"),
                   "--targets", str(REPO_ROOT / "docs/tone_targets.json")])
    assert rc == 0
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    assert {r["id"] for r in rep["rules"]} >= {r["id"] for r in TARGETS["rules"]}
    assert rep["references"][0]["aWeightedErrorDb"] == pytest.approx(0, abs=1e-6)
    assert (tmp_path / "o" / "report.png").stat().st_size > 1000


def _tonerender():
    try:
        return find_tonerender()
    except Exception:
        return None


@pytest.mark.skipif(_tonerender() is None, reason="tonerender binary not built")
def test_end_to_end_fixtures(tmp_path):
    preset = REPO_ROOT / "tests/fixtures/presets/golden_shared.json"
    di = REPO_ROOT / "tests/fixtures/di_riff.wav"
    rc = cli.main([str(preset), "--di", str(di), "--out", str(tmp_path)])
    assert rc == 0
    rep = json.loads((tmp_path / "report.json").read_text())
    assert rep["tonerender"]["renderRate"] > 0
    assert len(rep["rules"]) == len(TARGETS["rules"]) + 1
    assert all(r["status"] in ("pass", "marginal", "fail") for r in rep["rules"])
    assert (tmp_path / "report.png").exists() and (tmp_path / "render.wav").exists()
    # batch mode
    rc = cli.main(["--presets", str(preset), str(REPO_ROOT / "tests/fixtures/presets/golden_perpath.json"),
                   "--di", str(di), "--out", str(tmp_path / "b")])
    assert rc == 0
    assert len(json.loads((tmp_path / "b" / "summary.json").read_text())["presets"]) == 2


def test_gap_noise_from_di_robust_and_na():
    rng = np.random.default_rng(SEED)
    n = FS * 20
    # DI: bursts with a -40 dB hiss floor between them; gate (ideal) closes output in gaps
    di = 0.01 * rng.standard_normal(n)
    out = np.zeros(n)
    for k in range(10):
        s = int((1 + 2 * k) * FS)
        di[s:s + FS] += 0.3 * rng.standard_normal(FS)
        out[s:s + FS] = 0.3 * rng.standard_normal(FS)
    out += 1e-4 * rng.standard_normal(n)  # output residue in gaps: -70 dB re active
    g = A.gap_noise_db(out, di, FS)
    assert g["value"] == pytest.approx(20 * np.log10(1e-4 / 0.3), abs=1.5)
    assert -45 < g["diNoiseFloorDb"] < -35
    # no DI -> null
    assert A.gap_noise_db(out, None, FS)["value"] is None
    # steady DI: no frame can be < ... all frames equal -> everything within 6 dB (not "no gaps"); a DI
    # whose quietest 5 % are far below the rest still yields a measurable gap set
    assert g["gapFrameFraction"] >= 0.01


def test_gap_noise_rule_na(tmp_path):
    x = (0.1 * pink(FS * 10)).astype(np.float32)
    wav = tmp_path / "x.wav"
    sf.write(wav, x, FS, subtype="FLOAT")
    rc = cli.main(["--audio", str(wav), "--out", str(tmp_path / "o")])  # no DI
    assert rc == 0
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    gap = [r for r in rep["rules"] if r["id"] == "gap_noise"][0]
    assert gap["status"] == "n/a" and gap["value"] is None
    assert rep["summary"]["fail"] == rep["summary"]["fail"] and rep["summary"]["n/a"] == 1


@pytest.mark.parametrize("tau", [0.05, 0.1, 0.2])
def test_low_decay_slope(tau):
    di, out = _synthetic_decays(tau, [0.5, 2.0, 3.5, 5.0, 6.5])
    _, d = A.low_end_decay(di, out, FS)
    expected = -20 * np.log10(np.e) / (tau * 1000)  # dB per ms of an exp(-t/tau) amplitude
    assert d["nMeasured"] == 5
    # The causal 80-160 Hz band-pass (4th order) has a ~20-30 ms transient that flattens the first part
    # of the window, so the measured slope is shallower than the true one (-13 % at tau 100 ms, -23 % at
    # 200 ms): tolerance 25 %, and the ordering by tau must hold (see test below).
    assert d["value"] == pytest.approx(expected, rel=0.25)
    assert d["value"] < 0
    assert d["p25"] <= d["value"] <= d["p75"]


def test_low_decay_measurable_with_dense_onsets():
    # onsets every 120 ms: 20 dB fall is cut off (censored) but the 5-35 ms slope window is measurable
    onsets = list(np.arange(0.5, 6.0, 0.12))
    di, out = _synthetic_decays(0.1, onsets)
    t, d = A.low_end_decay(di, out, FS)
    assert d["nMeasured"] > 0.8 * d["nOnsets"] > 0
    assert d["nMeasured"] > t["nMeasured"]


def test_multiple_refs(tmp_path):
    x = (0.1 * pink(FS * 20)).astype(np.float32)
    st = np.stack([x, 0.5 * x], axis=1)
    wav, ref = tmp_path / "x.wav", tmp_path / "ref.wav"
    sf.write(wav, x, FS, subtype="FLOAT")
    sf.write(ref, st, FS, subtype="FLOAT")
    rc = cli.main(["--audio", str(wav), "--ref", str(ref), "--ref", str(ref), "--ref-channel", "left",
                   "--ref-channel", "right", "--out", str(tmp_path / "o")])
    assert rc == 0
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    assert [r["channel"] for r in rep["references"]] == ["left", "right"]
    assert all(r["aWeightedErrorDb"] == pytest.approx(0, abs=1e-6) for r in rep["references"])
    assert (tmp_path / "o" / "report.png").stat().st_size > 1000


def test_low_decay_orders_by_tau():
    vals = []
    for tau in (0.05, 0.1, 0.2):
        di, out = _synthetic_decays(tau, [0.5, 2.0, 3.5])
        vals.append(A.low_end_decay(di, out, FS)[1]["value"])
    assert vals[0] < vals[1] < vals[2] < 0  # steeper (more negative) for faster decays
