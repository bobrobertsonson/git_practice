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
from sawblade_match.tonecheck.render import find_tonerender
from sawblade_match.tonecheck.rules import classify, evaluate_rules, load_targets, parse_expr, summarize

FS = 48000
REPO_ROOT = Path(__file__).resolve().parents[2]
TARGETS = load_targets(Path(__file__).parent / "fixtures" / "tone_targets_v1.json")   # frozen v1
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
    blk = n // 8                                    # 4 playing / 4 silent blocks (>= 3 gaps and >= 3 s needed)
    di, xo = np.zeros(n), np.zeros(n)
    for k in range(8):
        sl = slice(k * blk, (k + 1) * blk)
        if k % 2 == 0:
            di[sl], xo[sl] = 0.1 * pink(blk), 0.1 * pink(blk)
        else:
            di[sl] = 3e-5 * rng.standard_normal(blk)
            xo[sl] = 1e-7 * rng.standard_normal(blk)
    g = A.gap_noise_db(xo, di, FS)
    assert g["value"] < -60 and g["gapCount"] == 4
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
    assert np.max(np.abs(detected - np.array(onsets))) < 0.010
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
                   "--targets", str(Path(__file__).parent / "fixtures" / "tone_targets_v1.json")])
    assert rc == 0
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    assert {r["id"] for r in rep["rules"]} >= {r["id"] for r in TARGETS["rules"]}
    assert rep["references"][0]["aWeightedErrorDb"] == pytest.approx(0, abs=1e-6)
    assert (tmp_path / "o" / "report.png").stat().st_size > 1000


def _tonerender():
    for rel in ("build/cli/tonerender", "build-lead/cli/tonerender"):
        if (REPO_ROOT / rel).exists():
            return REPO_ROOT / rel
    return None


@pytest.fixture(autouse=True)
def _env(monkeypatch):
    monkeypatch.setenv("SAWBLADE_TARGETS", str(Path(__file__).parent / "fixtures" / "tone_targets_v1.json"))
    tr = _tonerender()
    if tr:
        monkeypatch.setenv("SAWBLADE_TONERENDER", str(tr))


@pytest.mark.skipif(_tonerender() is None, reason="tonerender binary not built")
def test_end_to_end_fixtures(tmp_path):
    preset = REPO_ROOT / "tests/fixtures/presets/golden_shared.json"
    di = REPO_ROOT / "tests/fixtures/di_riff.wav"
    rc = cli.main([str(preset), "--di", str(di), "--out", str(tmp_path), "--tonerender", str(_tonerender())])
    assert rc == 0
    rep = json.loads((tmp_path / "report.json").read_text())
    assert rep["tonerender"]["renderRate"] > 0
    assert len(rep["rules"]) == len(TARGETS["rules"]) + 2          # + gap_noise + fizz_texture (phase 3.4)
    assert all(r["status"] in ("pass", "marginal", "fail") for r in rep["rules"] if r["id"] not in ("fizz_texture", "gap_noise"))   # gap_noise: n/a when the DI has < 1 s of real silence
    fz = [r for r in rep["rules"] if r["id"] == "fizz_texture"][0]
    assert fz["status"] == "n/a" and fz["valueStatus"] == "pending lead approval" and fz["value"] is not None
    assert (tmp_path / "report.png").exists() and (tmp_path / "render.wav").exists()
    # batch mode
    rc = cli.main(["--presets", str(preset), str(REPO_ROOT / "tests/fixtures/presets/golden_perpath.json"),
                   "--di", str(di), "--out", str(tmp_path / "b"), "--tonerender", str(_tonerender())])
    assert rc == 0
    assert len(json.loads((tmp_path / "b" / "summary.json").read_text())["presets"]) == 2


def test_gap_noise_from_di_robust_and_na():
    rng = np.random.default_rng(SEED)
    n = FS * 20
    # DI: bursts with a -40 dB hiss floor between them; gate (ideal) closes output in gaps
    di = 0.001 * rng.standard_normal(n)
    out = np.zeros(n)
    for k in range(10):
        s = int((1 + 2 * k) * FS)
        di[s:s + FS] += 0.3 * rng.standard_normal(FS)
        out[s:s + FS] = 0.3 * rng.standard_normal(FS)
    out += 1e-4 * rng.standard_normal(n)  # output residue in gaps: -70 dB re active
    g = A.gap_noise_db(out, di, FS)
    assert g["value"] == pytest.approx(20 * np.log10(1e-4 / 0.3), abs=1.5)
    assert -65 < g["diNoiseFloorDb"] < -55
    # no DI -> null
    assert A.gap_noise_db(out, None, FS)["value"] is None
    # steady DI: no frame can be < ... all frames equal -> everything within 6 dB (not "no gaps"); a DI
    # whose quietest 5 % are far below the rest still yields a measurable gap set
    assert g["gapCount"] == 10 and g["gapTotalS"] > 8.0


def test_gap_noise_rule_na(tmp_path):
    x = (0.1 * pink(FS * 10)).astype(np.float32)
    wav = tmp_path / "x.wav"
    sf.write(wav, x, FS, subtype="FLOAT")
    rc = cli.main(["--audio", str(wav), "--out", str(tmp_path / "o")])  # no DI
    assert rc == 0
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    gap = [r for r in rep["rules"] if r["id"] == "gap_noise"][0]
    assert gap["status"] == "n/a" and gap["value"] is None
    # independent expectation: evaluate the spectral rules directly, without summarize()
    xa = sf.read(wav)[0]
    exp = evaluate_rules(A.analyze(xa, FS, TARGETS).groups, TARGETS["rules"])
    for k in ("pass", "marginal", "fail"):
        assert rep["summary"][k] == sum(r["status"] == k for r in exp)
    assert rep["summary"]["n/a"] == 2        # gap_noise (no DI) + fizz_texture (ceiling pending lead approval)
    # n/a does not influence the overall verdict
    expected_overall = ("fail" if any(r["status"] == "fail" for r in exp) else
                        "marginal" if any(r["status"] == "marginal" for r in exp) else "pass")
    assert rep["summary"]["overall"] == expected_overall


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


# ---------------------------------------------------------------------------------------------------
# Absolute-level, boundary and CLI-robustness tests
# ---------------------------------------------------------------------------------------------------
def _sine(f, a, n, fs=FS):
    return a * np.sin(2 * np.pi * f * np.arange(n) / fs)


def _abs_bands(x):
    freqs, psd, _ = A.ltas_psd(x, FS, np.ones(len(x), bool))
    return A.band_levels_db(freqs, psd)[0]


def test_absolute_levels_of_sines():
    n = FS * 20
    x = _sine(250, 1.0, n) + _sine(1000, 0.5, n) + _sine(4000, 0.25, n)
    ab = _abs_bands(x)
    c = A.NOMINAL_CENTRES
    for f, amp in ((250, 1.0), (1000, 0.5), (4000, 0.25)):
        assert ab[c.index(f)] == pytest.approx(10 * np.log10(amp ** 2 / 2), abs=0.05)
    assert ab[c.index(500)] < -60 and ab[c.index(2000)] < -60


def test_band_edge_placement():
    c = A.NOMINAL_CENTRES
    n = FS * 20
    ab = _abs_bands(_sine(1100, 1.0, n))      # 1 kHz band upper edge is 1122 Hz
    assert ab[c.index(1000)] == pytest.approx(-3.01, abs=0.1)
    assert ab[c.index(1250)] < -35
    ab = _abs_bands(_sine(1140, 1.0, n))      # above the edge -> 1.25 kHz band
    assert ab[c.index(1250)] == pytest.approx(-3.01, abs=0.1)
    assert ab[c.index(1000)] < -35


def test_white_noise_absolute_band_levels():
    rng = np.random.default_rng(SEED)
    ab = _abs_bands(rng.standard_normal(FS * 60))
    idx = {500: -3, 630: -2, 800: -1, 1000: 0, 1600: 2, 2000: 3, 4000: 6}  # 10-per-decade band numbers
    for nom, k in idx.items():
        fc = 1000 * 10 ** (k / 10)
        bw = fc * (2 ** (1 / 6) - 2 ** (-1 / 6))
        assert ab[A.NOMINAL_CENTRES.index(nom)] == pytest.approx(10 * np.log10(bw / (FS / 2)), abs=0.3)


def _frames_signal(levels_db, f=1000.0):
    """One 50 ms frame (integer number of cycles) of a sine per entry, mean-square level in dB."""
    n = int(0.05 * FS)
    return np.concatenate([_sine(f, np.sqrt(2 * 10 ** (L / 10)), n) for L in levels_db])


def test_activity_gate_boundary():
    levels = [0.0] * 38 + [-29.0, -31.0]
    mask, frames, n = A.activity_mask(_frames_signal(levels), FS)
    assert frames[38] and not frames[39] and frames[:38].all()
    assert mask.sum() == 39 * n


def test_activity_gate_uses_95th_percentile():
    # 20 frames: p95 = +10 dB, p90 = +1 dB. A -25 dB frame is outside p95-30 (-20) but inside p90-30 (-29).
    levels = [-25.0] + [0.0] * 17 + [10.0, 10.0]
    _, frames, _ = A.activity_mask(_frames_signal(levels), FS)
    assert not frames[0] and frames[1:].all()


@pytest.mark.parametrize("k,expected_segments", [(2000, 1), (2700, 2)])
def test_segment_active_fraction(k, expected_segments):
    x = np.concatenate([_sine(1000, 1.0, 8192), _sine(2000, 1.0, 8192)])
    mask = np.zeros(len(x), bool)
    mask[: 8192 + k] = True  # segment 1 (4096..12288) has (4096+k)/8192 active: 0.74 or 0.83
    _, _, nseg = A.ltas_psd(x, FS, mask)
    assert nseg == expected_segments


def test_ltas_fallback_warns_when_no_segment_passes():
    x = pink(FS)
    w = []
    _, _, nseg = A.ltas_psd(x, FS, np.zeros(len(x), bool), w)
    assert nseg > 0 and w


def test_a_weighting_reference_values():
    got = cli.a_weight_db(np.array([100.0, 1000.0, 4000.0, 8000.0]))
    assert got == pytest.approx([-19.14, 0.0, 0.96, -1.15], abs=0.02)


def _fake(rel):
    c = list(A.NOMINAL_CENTRES)
    return A.Analysis(FS, c, np.array(rel), np.array(rel), {}, {}, 1, 1.0)


def test_reference_diff_sign_and_hand_calculated_aweighted_error():
    c = np.array(A.NOMINAL_CENTRES, float)
    ref = np.zeros(len(c))
    tilt = 2.0 * np.log2(c / 1000.0)  # output tilted: +2 dB/octave
    cmp = cli.compare_to_reference(_fake(ref + tilt), _fake(ref))
    assert cmp["diff_db"][-1] > 0 > cmp["diff_db"][0]  # sign = output - reference
    # hand calculation with the standard A-weighting formula, 80 Hz..8 kHz bands
    num = den = 0.0
    for f, d in zip(c, tilt):
        if 80 <= f <= 8000:
            ra = (12194 ** 2 * f ** 4) / ((f ** 2 + 20.6 ** 2) * np.sqrt((f ** 2 + 107.7 ** 2) * (f ** 2 + 737.9 ** 2)) * (f ** 2 + 12194 ** 2))
            w = 10 ** ((20 * np.log10(ra) + 2.0) / 10)
            num += w * d * d
            den += w
    assert cmp["aWeightedErrorDb"] == pytest.approx(np.sqrt(num / den), rel=1e-3)
    assert cmp["aWeightedErrorDb"] < cmp["unweightedRmsErrorDb"]  # extremes (low end) are down-weighted
    flat = cli.compare_to_reference(_fake(ref + 3.0), _fake(ref))
    assert flat["aWeightedErrorDb"] == pytest.approx(3.0, abs=1e-9) and flat["meanDiffDb"] == pytest.approx(3.0)


def _single_band_diff(freq, d=10.0):
    c = A.NOMINAL_CENTRES
    rel = np.zeros(len(c))
    rel[c.index(freq)] = d
    return cli.compare_to_reference(_fake(rel), _fake(np.zeros(len(c))))["aWeightedErrorDb"]


def test_reference_error_range_is_80hz_to_8khz():
    assert _single_band_diff(63) == 0.0 and _single_band_diff(10000) == 0.0
    assert _single_band_diff(80) > 0 and _single_band_diff(8000) > 0  # inclusive edges
    assert _single_band_diff(100) < _single_band_diff(1000)            # low band down-weighted by A


def test_ref_channel_selects_left_or_right(tmp_path):
    x = (0.1 * pink(FS * 10)).astype(np.float32)
    ref = tmp_path / "ref.wav"
    sf.write(ref, np.stack([x, 0.1 * x], axis=1), FS, subtype="FLOAT")  # right is 20 dB quieter
    wav = tmp_path / "x.wav"
    sf.write(wav, x, FS, subtype="FLOAT")
    assert cli.main(["--audio", str(wav), "--ref", str(ref), "--ref", str(ref), "--ref", str(ref),
                     "--ref-channel", "left", "--ref-channel", "right", "--ref-channel", "mid",
                     "--out", str(tmp_path / "o")]) == 0
    refs = json.loads((tmp_path / "o" / "report.json").read_text())["references"]
    lv = [r["analysis"]["ltasDbAbs"][A.NOMINAL_CENTRES.index(1000)] for r in refs]
    assert lv[0] - lv[1] == pytest.approx(20.0, abs=0.1)
    assert lv[2] - lv[1] == pytest.approx(20 * np.log10(0.55 / 0.1), abs=0.1)  # mid = (1 + 0.1)/2


def test_summarize_mixes():
    def s(*st):
        return summarize([{"status": x} for x in st])
    assert s("pass", "pass") == {"pass": 2, "marginal": 0, "fail": 0, "n/a": 0, "overall": "pass"}
    assert s("pass", "marginal", "n/a") == {"pass": 1, "marginal": 1, "fail": 0, "n/a": 1, "overall": "marginal"}
    assert s("pass", "marginal", "fail", "fail", "n/a")["overall"] == "fail"
    assert s("pass", "marginal", "fail", "fail", "n/a")["fail"] == 2
    assert s("n/a", "pass")["overall"] == "pass" and s("n/a")["overall"] == "pass"


def _gap_case(di_runs):
    """DI/output from runs of (n_frames, di_level, out_level, out_level_first_frame) in 50 ms frames."""
    di, out = [], []
    for nfr, dl, ol, first in di_runs:
        di += [dl] * nfr
        out += [first] + [ol] * (nfr - 1)
    return _frames_signal(di), _frames_signal(out)


def test_gap_regions_real_silence_only():
    di, out = _gap_case([(20, -20.0, -10.0, -10.0),
                         (40, -60.0, -80.0, -45.0),     # 2.0 s real gap (first 50 ms = ringing tail, output -45)
                         (20, -20.0, -10.0, -10.0),
                         (10, -45.0, -10.0, -10.0),     # above -50 dBFS: quiet playing, not a gap
                         (20, -20.0, -10.0, -10.0),
                         (2, -60.0, -80.0, -45.0),      # 100 ms < 120 ms: not a gap
                         (20, -20.0, -10.0, -10.0),
                         (3, -60.0, -80.0, -45.0),      # 150 ms: a gap, 100 ms after the skip
                         (20, -20.0, -10.0, -10.0),
                         (30, -60.0, -80.0, -45.0),     # 1.5 s gap: brings the count to 3 and the total over 3 s
                         (20, -20.0, -10.0, -10.0)])
    g = A.gap_noise_db(out, di, FS)
    assert g["diNoiseFloorDb"] == pytest.approx(-60.0, abs=0.01)
    assert g["gapCount"] == 3
    assert g["gapTotalS"] == pytest.approx(1.95 + 0.10 + 1.45, abs=0.01)
    assert g["value"] == pytest.approx(-80.0 + 10.0, abs=0.05)    # tail frames (-45) are skipped
    leg = A.gap_noise_legacy_db(out, di, FS)
    assert leg["value"] > g["value"]                               # the old definition counted tails


def test_gap_noise_is_na_on_a_dense_di_with_quiet_playing():
    """Floor is -40 dBFS (quiet playing, no silence): nothing is below the absolute -50 dBFS, so n/a."""
    di, out = _gap_case([(40, -20.0, -10.0, -10.0), (30, -40.0, -30.0, -30.0), (40, -20.0, -10.0, -10.0)])
    g = A.gap_noise_db(out, di, FS)
    assert g["value"] is None and g["reason"] == "no gaps" and g["gapCount"] == 0


def test_gap_noise_na_with_fewer_than_three_gaps_or_under_three_seconds():
    loud = (20, -20.0, -10.0, -10.0)
    # two long gaps (4 s total): count rule
    di, out = _gap_case([loud, (40, -60.0, -80.0, -80.0), loud, (40, -60.0, -80.0, -80.0), loud])
    g = A.gap_noise_db(out, di, FS)
    assert g["value"] is None and g["reason"] == "no gaps" and g["gapCount"] == 2 and g["gapTotalS"] > 3.0
    # three gaps, 1.8 s total: total rule
    di, out = _gap_case([loud] + [(14, -60.0, -80.0, -80.0), loud] * 3)
    g = A.gap_noise_db(out, di, FS)
    assert g["value"] is None and g["gapCount"] == 3 and g["gapTotalS"] < 3.0
    # three gaps, > 3 s: measured
    di, out = _gap_case([loud] + [(25, -60.0, -80.0, -80.0), loud] * 3)
    assert A.gap_noise_db(out, di, FS)["value"] == pytest.approx(-70.0, abs=0.1)


def test_gap_noise_na_below_one_second_of_gaps():
    di, out = _gap_case([(40, -20.0, -10.0, -10.0), (10, -60.0, -80.0, -80.0), (40, -20.0, -10.0, -10.0)])
    g = A.gap_noise_db(out, di, FS)       # 0.5 s of silence (0.45 s after the skip)
    assert g["value"] is None and g["reason"] == "no gaps" and g["gapCount"] == 1


def test_gap_noise_null_for_steady_di():
    rng = np.random.default_rng(SEED)
    di = 0.1 * rng.standard_normal(FS * 10)
    g = A.gap_noise_db(0.1 * di, di, FS)
    assert g["value"] is None and g["reason"] == "no gaps"      # nothing below -50 dBFS


def test_gap_rule_threshold_boundary():
    assert cli.gap_rule({"value": -60.0})["status"] == "pass"
    assert cli.gap_rule({"value": -59.99})["status"] == "fail"
    na = cli.gap_rule({"value": None, "reason": "no gaps"})
    assert na["status"] == "n/a" and na["reason"] == "no gaps" and na["margin"] is None


def test_low_decay_censors_onsets_closer_than_the_window(monkeypatch):
    di, out = _synthetic_decays(0.1, [0.5, 2.0])
    monkeypatch.setattr(A, "detect_onsets", lambda d, fs=FS: np.array([0.5, 0.51, 2.0]))
    _, d = A.low_end_decay(di, out, FS)
    assert d["nOnsets"] == 3 and d["nMeasured"] == 2 and d["nCensored"] == 1


def test_cli_unreadable_inputs_exit_3(tmp_path, capsys):
    good = tmp_path / "good.wav"
    sf.write(good, (0.1 * pink(FS * 5)).astype(np.float32), FS, subtype="FLOAT")
    garbage = tmp_path / "garbage.wav"
    garbage.write_bytes(b"this is not a wav file" * 10)
    out = str(tmp_path / "o")
    for argv in (["--audio", str(tmp_path / "missing.wav")],
                 ["--audio", str(good), "--ref", str(tmp_path / "missing.wav")],
                 ["--audio", str(garbage)],
                 ["--audio", str(good), "--di", str(garbage)]):
        capsys.readouterr()
        assert cli.main(argv + ["--out", out]) == 3
        err = capsys.readouterr().err
        assert err.startswith("error:") and err.count("\n") == 1 and "Traceback" not in err


def test_finders_have_no_package_relative_fallback(tmp_path, monkeypatch):
    from sawblade_match.tonecheck.render import RenderError, find_targets
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("SAWBLADE_TONERENDER", raising=False)
    monkeypatch.delenv("SAWBLADE_TARGETS", raising=False)
    with pytest.raises(RenderError, match="repo root"):
        find_tonerender()
    with pytest.raises(RenderError, match="repo root"):
        find_targets()
    monkeypatch.setenv("SAWBLADE_TARGETS", "x.json")
    assert str(find_targets()) == "x.json"


# --- review follow-ups ------------------------------------------------------------------------------
def _oracle_peak_index(out, t0, fs=FS):
    """Envelope peak the decay metric uses for an onset at t0 (80-160 Hz BP4, 10 ms mean square)."""
    sos = signal.butter(4, (80.0, 160.0), btype="bandpass", fs=fs, output="sos")
    y = signal.sosfilt(sos, out)
    k = int(0.010 * fs)
    env = np.convolve(y * y, np.ones(k) / k, mode="same")
    a, b = int(max(0, (t0 - 0.010) * fs)), int((t0 + 0.060) * fs)
    return a + int(np.argmax(env[a:b]))


@pytest.mark.parametrize("margin_ms,measured", [(-1.0, 1), (+1.0, 2)])
def test_low_decay_minimum_window_is_15_ms(monkeypatch, margin_ms, measured):
    # one burst at 0.5 s; a second (fake) onset so that the first onset's slope window
    # [peak+5 ms, next onset] is 15 ms -/+ 1 ms long. Under 15 ms: censored; over: measured.
    n = 3 * FS
    tt = np.arange(n - int(0.5 * FS)) / FS
    out = np.zeros(n)
    out[int(0.5 * FS):] = np.exp(-tt / 0.1) * np.sin(2 * np.pi * 120.0 * tt)
    pk = _oracle_peak_index(out, 0.5)
    nxt = pk + int((0.005 + 0.015 + margin_ms / 1000) * FS)
    monkeypatch.setattr(A, "detect_onsets", lambda d, fs=FS: np.array([0.5, nxt / FS]))
    _, d = A.low_end_decay(np.zeros(n), out, FS)
    assert d["nOnsets"] == 2 and d["nMeasured"] == measured and d["nCensored"] == 2 - measured


def test_summarize_exactly_one_fail():
    s = summarize([{"status": "pass"}, {"status": "fail"}, {"status": "n/a"}])
    assert s == {"pass": 1, "marginal": 0, "fail": 1, "n/a": 1, "overall": "fail"}
    assert summarize([{"status": "fail"}])["overall"] == "fail"


def _noise_plucks(onsets, tau, seed, dur=8.0, floor=1e-4, amp=0.3):
    n = int(dur * FS)
    rng = np.random.default_rng(seed)
    di = floor * rng.standard_normal(n)
    for o in onsets:
        i = int(o * FS)
        di[i:] += amp * np.exp(-np.arange(n - i) / FS / tau) * rng.standard_normal(n - i)
    return di


def _match_onsets(det, truth, tol=0.015):
    det, tp = list(det), 0
    for t in truth:
        near = [d for d in det if abs(d - t) <= tol]
        if near:
            tp += 1
            det.remove(min(near, key=lambda d: abs(d - t)))
    return tp


@pytest.mark.parametrize("tau", [0.03, 0.05, 0.1])
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_no_spurious_onsets_in_noisy_decays(tau, seed):
    # regression: 5 noise plucks with a 30 ms decay used to give 10-12 onsets (flux peaks in the noisy tail)
    truth = [0.5, 2.0, 3.5, 5.0, 6.5]
    det = A.detect_onsets(_noise_plucks(truth, tau, seed), FS)
    assert len(det) == 5
    assert _match_onsets(det, truth) == 5


def test_onset_precision_recall_on_synthetic_set():
    # before the minimum-rise check: precision 0.545, recall 1.0 on this set (220 detections for 120 plucks)
    truth = [0.5, 2.0, 3.5, 5.0, 6.5]
    tp = nd = nt = 0
    for tau in (0.03, 0.05, 0.1, 0.15):
        for seed in (1, 2, 3):
            for amp in (0.3, 0.05):
                det = A.detect_onsets(_noise_plucks(truth, tau, seed, amp=amp), FS)
                tp += _match_onsets(det, truth)
                nd += len(det)
                nt += len(truth)
    assert tp / nd >= 0.97 and tp / nt >= 0.97


# --- onset minimum-rise edges ------------------------------------------------------------------------------
def _bed_with_pluck(rise_db, seed=7, dur=4.0, t0=2.0):
    """Steady noise bed plus one pluck whose initial power is (10^(rise/10) - 1) x the bed's."""
    n = int(dur * FS)
    rng = np.random.default_rng(seed)
    x = 0.05 * rng.standard_normal(n)
    i = int(t0 * FS)
    amp = 0.05 * np.sqrt(10 ** (rise_db / 10) - 1)
    x[i:] += amp * np.exp(-np.arange(n - i) / FS / 0.08) * rng.standard_normal(n - i)
    return x


def _has_onset_near(det, t, tol=0.02):
    return bool(np.any(np.abs(np.asarray(det) - t) <= tol))


def test_onset_min_rise_edges():
    assert A.ONSET_MIN_RISE_DB == 6.0
    # ~4 dB rise: below the 6 dB check -> suppressed; ~9 dB: kept (3 seeds each)
    for seed in (7, 8, 9):
        assert not _has_onset_near(A.detect_onsets(_bed_with_pluck(4.0, seed), FS), 2.0)
        assert not _has_onset_near(A.detect_onsets(_bed_with_pluck(5.0, seed), FS), 2.0)   # just under the 6 dB check
        assert _has_onset_near(A.detect_onsets(_bed_with_pluck(9.0, seed), FS), 2.0)
