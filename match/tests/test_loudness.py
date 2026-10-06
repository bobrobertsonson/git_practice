"""BS.1770-4 loudness / true peak (pure numpy/scipy) and the matcher's loudness-matched listen/ folder."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher.loudness import (choose_listen_section, integrated_lufs, k_weighting_sos, match_gain_db,
                                             true_peak_db)

FS = 48000


def sine(f, db_peak, seconds, fs=FS):
    t = np.arange(int(seconds * fs)) / fs
    return 10 ** (db_peak / 20) * np.sin(2 * np.pi * f * t)


def test_k_weighting_matches_the_bs1770_48k_table():
    sos = k_weighting_sos(48000)
    assert sos[0] == pytest.approx([1.53512485958697, -2.69169618940638, 1.19839281085285, 1.0,
                                    -1.69065929318241, 0.73248077421585], abs=1e-6)
    assert sos[1] == pytest.approx([1.0, -2.0, 1.0, 1.0, -1.99004745483398, 0.99007225036621], abs=1e-6)


def test_mono_997hz_sine_minus20_dbfs_is_minus23_lufs():
    assert integrated_lufs(sine(997, -20.0, 20), FS) == pytest.approx(-23.0, abs=0.1)


def test_ebu_tech3341_case1_stereo_1khz_minus23_dbfs():
    s = sine(1000, -23.0, 20)
    assert integrated_lufs(np.stack([s, s], axis=1), FS) == pytest.approx(-23.0, abs=0.1)


def test_other_sample_rates_and_level_linearity():
    assert integrated_lufs(sine(997, -20.0, 20, 44100), 44100) == pytest.approx(-23.0, abs=0.1)
    a = integrated_lufs(sine(997, -30.0, 10), FS)
    b = integrated_lufs(sine(997, -24.0, 10), FS)
    assert b - a == pytest.approx(6.0, abs=0.01)


def test_silence_segments_are_gated_out():
    tone = sine(997, -20.0, 15)
    quiet = sine(997, -80.0, 10)
    gated = integrated_lufs(np.concatenate([tone, quiet, tone]), FS)
    assert gated == pytest.approx(integrated_lufs(tone, FS), abs=0.1)
    assert gated == pytest.approx(-23.0, abs=0.1)


def test_unmeasurable_signals_are_minus_inf_and_match_gain_is_zero():
    assert integrated_lufs(np.zeros(FS), FS) == float("-inf")
    assert integrated_lufs(sine(997, -20.0, 0.2), FS) == float("-inf")          # shorter than one 400 ms block
    assert match_gain_db(np.zeros(FS), sine(997, -20.0, 2), FS)[0] == 0.0


def test_match_gain_brings_loudness_together():
    ref = sine(997, -20.0, 5)
    y = sine(997, -9.0, 5)
    g, lr, ly = match_gain_db(ref, y, FS)
    assert g == pytest.approx(-11.0, abs=0.02)
    assert integrated_lufs(y * 10 ** (g / 20), FS) == pytest.approx(lr, abs=0.01)


def test_true_peak_sees_intersample_peaks():
    n = np.arange(FS)
    x = np.sin(2 * np.pi * (FS / 4) * n / FS + np.pi / 4)            # every sample is +-0.7071, the waveform peaks at 1.0
    assert 20 * np.log10(np.max(np.abs(x))) == pytest.approx(-3.01, abs=0.01)
    assert -0.5 < true_peak_db(x) < 0.8
    assert true_peak_db(np.zeros(100)) == float("-inf")


def test_listen_section_is_moved_into_the_reference():
    rng = np.random.default_rng(1)
    di = (rng.standard_normal(40 * FS) * 0.1).astype(np.float32)
    a, b, _ = choose_listen_section(di, None, 0)
    assert b - a == 30 * FS
    off = 5 * FS
    a2, b2, _ = choose_listen_section(di, 30 * FS, off)              # reference only covers DI samples < 25 s
    assert 0 <= a2 and b2 + off <= 30 * FS and b2 - a2 == 25 * FS
    a3, b3, _ = choose_listen_section(di[: 10 * FS], None, 0)       # shorter than 30 s: all of it
    assert (a3, b3) == (0, 10 * FS)


# ---- the listen/ folder ----------------------------------------------------------------------------------------------

def _bursty_di(seconds=40, seed=0):
    rng = np.random.default_rng(seed)
    n = seconds * FS
    env = np.repeat(rng.uniform(0.2, 1.0, seconds * 4), FS // 4)
    return (rng.standard_normal(n) * 0.1 * env).astype(np.float32)


def test_listen_folder_is_aligned_and_loudness_matched(tmp_path):
    from sawblade_match.matcher.reference import Reference
    from sawblade_match.matcher.run import Config, _listening
    di = _bursty_di()
    off = 4800                                                      # reference index of DI sample 0
    ref_sig = np.concatenate([np.zeros(off, np.float32), 0.05 * di]).astype(np.float32)
    ref = Reference("r", str(tmp_path / "r.wav"), "left", ref_sig, 0.0, matched_sig=ref_sig, matched_channel="mono",
                    offset_samples=off, offset_given=True)
    render = (0.5 * di).astype(np.float32)                          # 20 dB louder than the reference
    starter = (0.25 * di).astype(np.float32)
    cfg = Config(di=tmp_path / "x", ref=None, pool=None, out=tmp_path)
    logs = []
    info = _listening(tmp_path, {"best_L": (render, FS, {}), "starter_L": (starter, FS, {})}, cfg, logs.append,
                      ref=ref, di48=di, offset=off)
    d = tmp_path / "listen"
    r, fs_r = sf.read(d / "ref.wav", dtype="float64")
    y, fs_y = sf.read(d / "render.wav", dtype="float64")
    b, _ = sf.read(d / "before.wav", dtype="float64")
    assert fs_r == fs_y == FS and r.ndim == y.ndim == 1                              # mono pair stays mono, 48 kHz
    assert len(r) == len(y) == len(b) == 30 * FS
    s0, s1 = info["section"]
    assert s1 - s0 == pytest.approx(30.0) and info["offsetMs"] == pytest.approx(100.0)
    assert info["gainDb"] == pytest.approx(-20.0, abs=0.02)
    assert info["gainBeforeDb"] == pytest.approx(20 * np.log10(0.05 / 0.25), abs=0.02)
    assert info["lufsRenderRaw"] + info["gainDb"] == pytest.approx(info["lufsRef"], abs=1e-6)
    assert integrated_lufs(y, FS) == pytest.approx(integrated_lufs(r, FS), abs=0.1)  # same loudness over the same section
    assert integrated_lufs(b, FS) == pytest.approx(integrated_lufs(r, FS), abs=0.1)
    assert np.allclose(y, r, atol=1e-5)                                              # aligned: this DI is its own reference
    assert not np.isclose(np.max(np.abs(y)), 10 ** (-1 / 20), atol=1e-3)             # not peak-normalised
    assert set(info["truePeakDb"]) == {"ref", "render", "before"}
    assert info["truePeakDb"]["render"] == pytest.approx(info["truePeakDb"]["ref"], abs=0.1)
    assert any("render was 20.0 dB louder than the reference before matching" in m for m in logs)
    # the full-length file carries the same gain, float, unclipped
    full, _ = sf.read(info["wav"], dtype="float64")
    assert info["fullLengthGainDb"] == info["gainDb"] and np.allclose(full[:, 0], 0.5 * di * 10 ** (info["gainDb"] / 20),
                                                                     atol=1e-5)


def test_listen_folder_without_before_and_unmatched_reference(tmp_path):
    from sawblade_match.matcher.reference import Reference
    from sawblade_match.matcher.run import Config, _listening
    di = _bursty_di(20, seed=3)
    guitars = (0.02 * _bursty_di(20, seed=4)).astype(np.float32)     # a guitar isolation, no time alignment with the DI
    ref = Reference("r", "r.wav", "side", guitars, 3.0)
    cfg = Config(di=tmp_path / "x", ref=None, pool=None, out=tmp_path)
    info = _listening(tmp_path, {"best_L": (di, FS, {})}, cfg, lambda *_: None, ref=ref, di48=di, offset=0)
    d = tmp_path / "listen"
    assert (d / "ref.wav").exists() and (d / "render.wav").exists() and not (d / "before.wav").exists()
    assert "lufsBefore" not in info and info["offsetMs"] is None and info["section"] == pytest.approx([0.0, 20.0])
    y, _ = sf.read(d / "render.wav", dtype="float64")
    r, _ = sf.read(d / "ref.wav", dtype="float64")
    assert integrated_lufs(y, FS) == pytest.approx(integrated_lufs(r, FS), abs=0.1)
