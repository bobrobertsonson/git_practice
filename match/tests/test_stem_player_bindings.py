"""Tests for the StemPlayer / StemSet part of the sawblade_core bindings (spec 5.1, section 5).

Skipped (with the reason) when the extension has not been built.
"""
from __future__ import annotations

import numpy as np
import pytest

try:
    from sawblade_match import core
except ImportError as exc:  # pragma: no cover - depends on the local build
    pytest.skip(f"sawblade_core is not built: {exc}", allow_module_level=True)

if core.StemPlayer is None:  # pragma: no cover - an older build of the module
    pytest.skip("sawblade_core was built without StemPlayer", allow_module_level=True)

FS = 48000.0


def const(n: int, v: float) -> np.ndarray:
    return np.full(n, v, dtype=np.float32)


def make_player(stems: dict, max_latency: int = 0):
    ss = core.stem_set_from_arrays(stems, FS)
    p = core.StemPlayer()
    p.prepare(FS, 512, max_latency)
    p.set_stem_set(ss)
    p.process(0)  # adopt the set (transport stopped)
    return p, ss


def test_stem_set_from_arrays_properties():
    ss = core.stem_set_from_arrays(
        {"drums": const(1000, 0.1), "guitar": np.zeros((2, 1500), dtype=np.float32), "bass": const(10, 1.0)}, FS
    )
    assert ss.sample_rate == FS
    assert ss.length == 1500
    assert ss.present == ["drums", "bass", "guitar"]
    assert ss.warnings == []
    assert "StemSet" in repr(ss)


def test_stem_set_from_arrays_errors():
    with pytest.raises(ValueError):
        core.stem_set_from_arrays({"banjo": const(10, 1.0)}, FS)
    with pytest.raises(ValueError):
        core.stem_set_from_arrays({"drums": np.zeros(10, dtype=np.int16)}, FS)  # integer PCM rejected
    with pytest.raises(ValueError):
        core.stem_set_from_arrays({"drums": np.zeros((3, 10), dtype=np.float32)}, FS)  # not (n,) or (2, n)
    with pytest.raises(RuntimeError):
        core.stem_set_from_arrays({}, FS)  # no stems
    with pytest.raises(RuntimeError):
        core.stem_set_from_arrays({"drums": np.zeros(0, dtype=np.float32)}, FS)  # zero frames
    with pytest.raises(RuntimeError):
        core.stem_set_from_arrays({"drums": const(10, 1.0)}, 0.0)


def test_process_shape_and_dtype():
    p, _ = make_player({"drums": const(20000, 0.5)})
    p.play()
    y = p.process(1000)
    assert y.shape == (2, 1000) and y.dtype == np.float32
    assert y[0, 999] == pytest.approx(0.5, abs=1e-6) and np.array_equal(y[0], y[1])
    assert p.process(0).shape == (2, 0)
    assert p.position == 1000 and p.is_playing and p.latency_samples == 0
    with pytest.raises(ValueError):
        p.process(-1)


def test_unprepared_player_raises():
    p = core.StemPlayer()
    with pytest.raises(RuntimeError):
        p.process(10)
    with pytest.raises(RuntimeError):
        p.play()


def test_solo_and_mute_effect():
    p, _ = make_player({"drums": const(40000, 0.1), "bass": const(40000, 0.2), "vocals": const(40000, 0.3)})
    p.play()
    assert p.process(3000)[0, -1] == pytest.approx(0.6, abs=1e-6)
    p.set_stem_mute("bass", True)
    assert p.process(3000)[0, -1] == pytest.approx(0.4, abs=1e-6)
    p.set_stem_mute("bass", False)
    p.set_stem_solo("vocals", True)
    assert p.process(3000)[0, -1] == pytest.approx(0.3, abs=1e-6)
    p.set_stem_mute("vocals", True)  # mute wins over solo
    assert p.process(3000)[0, -1] == 0.0
    p.set_stem_mute("vocals", False)
    p.set_stem_solo("vocals", False)
    p.set_stem_gain_db("drums", -20.0)
    p.set_master_level_db(-6.0)
    expect = (0.1 * 10 ** (-20 / 20) + 0.2 + 0.3) * 10 ** (-6 / 20)
    assert p.process(3000)[0, -1] == pytest.approx(expect, abs=1e-6)
    with pytest.raises(ValueError):
        p.set_stem_gain_db("banjo", 0.0)


def test_guitar_muted_by_default():
    p, _ = make_player({"drums": const(40000, 0.1), "guitar": const(40000, 0.4)})
    p.play()
    assert p.process(3000)[0, -1] == pytest.approx(0.1, abs=1e-6)
    p.set_guitar_mode("ghost")
    assert p.process(3000)[0, -1] == pytest.approx(0.1 + 0.4 * 10 ** (-12 / 20), abs=1e-6)
    p.set_guitar_mode("full")
    assert p.process(3000)[0, -1] == pytest.approx(0.5, abs=1e-6)
    p.set_guitar_mode("muted")
    assert p.process(3000)[0, -1] == pytest.approx(0.1, abs=1e-6)
    with pytest.raises(ValueError):
        p.set_guitar_mode("loud")


def test_transport_loop_count_in_latency():
    ramp = (np.arange(30000) * 1e-5).astype(np.float32)
    p, _ = make_player({"drums": ramp}, max_latency=64)
    assert p.set_loop(1000, 5000) is True and p.set_loop(1000, 1100) is False
    p.clear_loop()
    p.seek(2000)
    p.play()
    y = p.process(1000)
    assert y[0, 400] == pytest.approx(ramp[2400], abs=1e-6)
    p.pause()
    p.process(1000)
    assert not p.is_playing and p.position == 3000 + 240

    # count-in: clicks first, stems after round(bars * beats * 60 * fs / bpm) samples
    p.seek(0)
    p.set_count_in(1, 240.0, 4)
    p.play()
    y = p.process(1000)
    assert p.is_counting_in and np.abs(y[0]).max() > 0.1
    p.pause()
    p.process(2000)
    assert not p.is_counting_in

    # rig latency delays everything
    p.seek(0)
    p.set_count_in(0, 120.0)
    p.set_rig_latency_samples(50)
    assert p.rig_latency_samples == 50
    p.play()
    y = p.process(1000)
    assert np.all(y[0, :50] == 0.0) and y[0, 400] == pytest.approx(ramp[350], abs=1e-6)
    p.set_rig_latency_samples(1000)
    assert p.rig_latency_samples == 64  # clamped


def test_set_adoption_and_rate_mismatch():
    p, _ = make_player({"drums": const(1000, 0.5)})
    assert p.stem_set_length == 1000
    other = core.stem_set_from_arrays({"bass": const(5000, 0.25)}, FS)
    p.play()
    p.process(10)  # playing now
    p.set_stem_set(other)
    p.process(100)
    assert p.stem_set_length == 1000  # waits while playing
    p.pause()
    p.process(512)  # the pause fade completes within this block
    p.process(10)
    assert p.stem_set_length == 5000 and p.position == 0
    wrong = core.stem_set_from_arrays({"bass": const(100, 0.25)}, 44100.0)
    with pytest.raises(ValueError):
        p.set_stem_set(wrong)


def test_host_follow():
    ramp = (np.arange(30000) * 1e-5).astype(np.float32)
    p, _ = make_player({"drums": ramp})
    p.set_transport_mode("host_follow")
    for b in range(20):
        p.set_host_position(b * 64, True)
        y = p.process(64)
    assert p.position == 20 * 64 and y[0, -1] == pytest.approx(ramp[20 * 64 - 1], abs=1e-6)
    p.play()  # ignored in host-follow mode
    p.set_host_position(20 * 64, False)
    p.process(512)
    assert not p.is_playing
    with pytest.raises(ValueError):
        p.set_transport_mode("sideways")


def test_load_stems_directory(tmp_path):
    sf = pytest.importorskip("soundfile")
    sf.write(tmp_path / "drums.wav", np.stack([const(4410, 0.1), const(4410, -0.1)], axis=1), 44100, subtype="FLOAT")
    sf.write(tmp_path / "Guitars.wav", const(3000, 0.2), 48000, subtype="FLOAT")
    sf.write(tmp_path / "piano.wav", const(3000, 0.3), 48000, subtype="FLOAT")
    ss = core.load_stems(tmp_path, FS)
    assert ss.sample_rate == FS
    assert ss.present == ["drums", "other", "guitar"]
    assert ss.length == 4800  # 4410 frames at 44.1 kHz -> 4800 at 48 kHz
    assert not any("piano.wav" in w for w in ss.warnings)  # piano is a recognised stem name: no warning
    p = core.StemPlayer()
    p.prepare(FS, 256, 0)
    p.set_stem_set(ss)
    p.process(0)
    assert p.stem_set_length == 4800
    with pytest.raises(RuntimeError, match="nope"):
        core.load_stems(tmp_path / "nope", FS)


def test_backing_loudness():
    n = int(FS * 3)
    tone = (0.0708 * np.sin(2 * np.pi * 1000 * np.arange(n) / FS)).astype(np.float32)  # about -23 dBFS peak
    v = core.integrated_loudness_lufs(tone, FS)
    assert v == pytest.approx(-23.0, abs=0.1)  # EBU Tech 3341 case 1: -23 dBFS peak sine on both channels
    assert core.integrated_loudness_lufs(np.stack([tone, tone]), FS) == pytest.approx(v)
    assert core.integrated_loudness_lufs(np.zeros(n, dtype=np.float32), FS) is None
    loud_guitar = 0.5 * tone / 0.0708
    ss = core.stem_set_from_arrays({"drums": tone, "guitar": loud_guitar.astype(np.float32)}, FS)
    assert ss.backing_loudness_lufs == pytest.approx(v)  # guitar excluded
    assert core.stem_set_from_arrays({"guitar": loud_guitar.astype(np.float32)}, FS).backing_loudness_lufs is None


def test_load_stems_other_role(tmp_path):
    sf = pytest.importorskip("soundfile")
    sf.write(tmp_path / "bass.wav", const(3000, 0.1), 48000, subtype="FLOAT")
    sf.write(tmp_path / "other.wav", const(3000, 0.2), 48000, subtype="FLOAT")
    g = core.load_stems(tmp_path, FS)  # default: 4-stem other is the guitar
    assert g.present == ["bass", "guitar"] and g.other_mapped_to_guitar
    g2 = core.load_stems(tmp_path, FS, other_role="guitar")
    assert g2.present == ["bass", "guitar"]
    o = core.load_stems(tmp_path, FS, other_role="other")
    assert o.present == ["bass", "other"] and not o.other_mapped_to_guitar
    with pytest.raises(ValueError):
        core.load_stems(tmp_path, FS, other_role="keys")
    sf.write(tmp_path / "guitars.wav", const(3000, 0.3), 48000, subtype="FLOAT")  # a real guitar file wins
    w = core.load_stems(tmp_path, FS)
    assert w.present == ["bass", "other", "guitar"] and not w.other_mapped_to_guitar
