"""v0.4M Task F.1: the blend reference (sum of two same-take amp tracks). Synthetic audio generated here."""
from __future__ import annotations

import json

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher import refsum as RS
from sawblade_match.matcher.loudness import integrated_lufs

FS = 48000


def tracks(seconds=8.0, seed=0):
    """a: noise bursts (a 'guitar-ish' signal); b: a smoothed copy (a different spectrum, same take, zero delay)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    env = (np.sin(2 * np.pi * 1.5 * np.arange(n) / FS) > -0.3).astype(float)
    a = rng.normal(size=n) * env * 0.2
    b = np.convolve(a, [0.25, 0.5, 0.25], mode="same") * 0.7        # symmetric FIR: no delay, darker
    return a, b


def write(p, x, fs=FS):
    sf.write(str(p), x.astype(np.float32), fs, subtype="FLOAT")
    return p


def run(tmp_path, a, b, extra=(), fs_a=FS, fs_b=FS):
    pa, pb = write(tmp_path / "a.wav", a, fs_a), write(tmp_path / "b.wav", b, fs_b)
    out, js = tmp_path / "blend.wav", tmp_path / "r.json"
    rc = RS.main(["--a", str(pa), "--b", str(pb), "--out", str(out), "--json", str(js), *extra])
    return rc, out, (json.loads(js.read_text()) if js.exists() else None)


def test_unity_sum_float32_and_ratio(tmp_path):
    a, b = tracks()
    a32, b32 = a.astype(np.float32).astype(np.float64), b.astype(np.float32).astype(np.float64)
    rc, out, r = run(tmp_path, a, b)
    assert rc == 0
    y, fs = sf.read(str(out), dtype="float32")
    assert fs == FS and y.dtype == np.float32
    if hasattr(sf, "info"):
        assert sf.info(str(out)).subtype == "FLOAT"
    assert np.max(np.abs(y - (a32 + b32))) < 1e-6
    assert r["lagMs"] == 0.0 and r["corrPolarity"] == 1 and r["polarity"]["chosen"] == "asis" and r["corr"] > 0.7
    want = integrated_lufs(a32, FS) - integrated_lufs(b32, FS)
    assert r["refRatioDb"] == pytest.approx(want, abs=0.05)
    assert r["peakDb"] == pytest.approx(20 * np.log10(np.max(np.abs(y))), abs=1e-3)
    assert r["gainsDb"] == [0.0, 0.0] and r["warnings"] == []


def test_blend_db_applied_and_ratio_follows_faders(tmp_path):
    a, b = tracks()
    a32, b32 = a.astype(np.float32).astype(np.float64), b.astype(np.float32).astype(np.float64)
    rc, out, r = run(tmp_path, a, b, ["--blend-db", "-3,+2.5"])
    y, _ = sf.read(str(out), dtype="float32")
    ref = a32 * 10 ** (-3 / 20) + b32 * 10 ** (2.5 / 20)
    assert rc == 0 and np.max(np.abs(y - ref)) < 1e-6
    assert r["gainsDb"] == [-3.0, 2.5]
    rc0, _, r0 = run(tmp_path, a, b)
    assert r["refRatioDb"] - r0["refRatioDb"] == pytest.approx(-5.5, abs=0.05)


def test_delayed_b_warns_and_reports_lag(tmp_path):
    a, b = tracks()
    d = int(0.005 * FS)
    bd = np.concatenate([np.zeros(d), b[:-d]])
    rc, out, r = run(tmp_path, a, bd, ["--polarity", "asis"])     # (a 5 ms delay is half a cycle at 100 Hz: auto would flip)
    assert rc == 0 and out.exists()                                  # the sum is still written, unshifted
    assert r["lagMs"] == pytest.approx(5.0, abs=0.1) and r["corrPolarity"] == 1
    assert any(w.startswith("WARNING:") for w in r["warnings"])
    y, _ = sf.read(str(out), dtype="float32")
    assert np.max(np.abs(y - (a.astype(np.float32) + bd.astype(np.float32)))) < 1e-6


def test_mismatched_rates_exit_2(tmp_path):
    a, b = tracks(2.0)
    rc, out, r = run(tmp_path, a, b[:len(a) * 441 // 480], fs_b=44100)
    assert rc == 2 and not out.exists()


def test_bad_blend_db_and_missing_file_exit_2(tmp_path):
    a, b = tracks(1.0)
    assert run(tmp_path, a, b, ["--blend-db", "x,1"])[0] == 2
    assert run(tmp_path, a, b, ["--blend-db", "1"])[0] == 2
    assert RS.main(["--a", str(tmp_path / "nope.wav"), "--b", str(tmp_path / "nope2.wav"), "--out", str(tmp_path / "o.wav")]) == 2


def test_stereo_and_unequal_lengths(tmp_path):
    a, b = tracks(3.0)
    st = np.stack([a, a * 0.5], axis=1)                              # stereo -> channel mean
    rc, out, r = run(tmp_path, st, b[:-1000])
    y, _ = sf.read(str(out), dtype="float32")
    assert rc == 0 and len(y) == len(a) - 1000 and y.ndim == 1
    ref = (st.astype(np.float32).astype(np.float64).mean(axis=1)[:len(y)] + b.astype(np.float32).astype(np.float64)[:len(y)])
    assert np.max(np.abs(y - ref)) < 1e-6




def f32(x):
    return x.astype(np.float32).astype(np.float64)


def test_auto_polarity_flips_a_track_that_cancels_the_lows(tmp_path):
    a, b = tracks()
    rc, out, r = run(tmp_path, a, -b)                                    # default mode: auto
    y, _ = sf.read(str(out), dtype="float32")
    pol = r["polarity"]
    assert rc == 0 and pol["mode"] == "auto" and pol["chosen"] == "invert-b"
    assert pol["lowBandDbInvert"] > pol["lowBandDbAsis"] + 3.0             # the lows add when b is flipped back
    assert np.max(np.abs(y - (f32(a) + f32(b)))) < 1e-6                   # b's recorded -b is flipped back: a + b
    assert r["corrPolarity"] == 1 and r["corr"] > 0.7 and r["warnings"] == [] and r["lagMs"] == 0.0     # lag stays as recorded
    assert r["settingsKey"] == RS.settings_key(0.0, 0.0, "auto")


def test_asis_keeps_both_as_recorded(tmp_path):
    a, b = tracks()
    rc, out, r = run(tmp_path, a, -b, ["--polarity", "asis"])
    y, _ = sf.read(str(out), dtype="float32")
    assert rc == 0 and r["polarity"]["mode"] == "asis" and r["polarity"]["chosen"] == "asis"
    assert np.max(np.abs(y - (f32(a) - f32(b)))) < 1e-6
    assert r["corrPolarity"] == -1 and r["corr"] < -0.7 and any("opposite polarity" in w for w in r["warnings"])
    # both levels are still measured and recorded
    assert r["polarity"]["lowBandDbInvert"] > r["polarity"]["lowBandDbAsis"]


def test_auto_keeps_asis_for_same_polarity_tracks(tmp_path):
    a, b = tracks()
    rc, out, r = run(tmp_path, a, b)
    y, _ = sf.read(str(out), dtype="float32")
    assert r["polarity"]["chosen"] == "asis" and r["polarity"]["lowBandDbAsis"] > r["polarity"]["lowBandDbInvert"] + 3.0
    assert np.max(np.abs(y - (f32(a) + f32(b)))) < 1e-6


def test_forced_flips_and_ratio_unaffected(tmp_path):
    a, b = tracks()
    for d in ("ia", "ib", "asis"):
        (tmp_path / d).mkdir()
    _, out_a, ra = run(tmp_path / "ia", a, b, ["--polarity", "invert-a"])
    _, out_b, rb = run(tmp_path / "ib", a, b, ["--polarity", "invert-b"])
    ya, _ = sf.read(str(out_a), dtype="float32")
    yb, _ = sf.read(str(out_b), dtype="float32")
    assert np.max(np.abs(ya - (f32(b) - f32(a)))) < 1e-6 and ra["polarity"]["chosen"] == "invert-a"
    assert np.max(np.abs(yb - (f32(a) - f32(b)))) < 1e-6 and rb["polarity"]["chosen"] == "invert-b"
    assert np.max(np.abs(ya + yb)) < 1e-6                                 # the same sum up to overall sign
    _, _, rd = run(tmp_path / "asis", a, b, ["--polarity", "asis"])
    assert ra["refRatioDb"] == pytest.approx(rd["refRatioDb"], abs=1e-9)
    assert rb["refRatioDb"] == pytest.approx(rd["refRatioDb"], abs=1e-9)


def test_json_has_the_polarity_block(tmp_path):
    a, b = tracks(3.0)
    rc, out, r = run(tmp_path, a, b, ["--blend-db", "-3,0"])
    pol = r["polarity"]
    for k in ("mode", "chosen", "lowBandDbAsis", "lowBandDbInvert"):
        assert k in pol
    assert pol["lowBandHz"] == [60.0, 250.0] and r["settingsKey"] == RS.settings_key(-3.0, 0.0, "auto")
    for k in ("lagMs", "corr", "corrPolarity", "refRatioDb", "gainsDb"):
        assert k in r


def test_bad_polarity_and_settings_key(tmp_path):
    code = None
    try:
        RS.main(["--a", "a", "--b", "b", "--out", "o", "--polarity", "sideways"])       # argparse: invalid choice -> exit 2
    except SystemExit as e:
        code = e.code
    assert code == 2
    with pytest.raises(ValueError):
        RS.blend_refs(*tracks(1.0), FS, polarity="sideways")
    k = RS.settings_key(0.0, 0.0)
    assert k != RS.settings_key(0.0, 0.0, "asis") != RS.settings_key(0.0, 0.0, "invert-b") and k != RS.settings_key(-3.0, 0.0)
    assert RS.main(["--settings-key", "--blend-db", "-3,2.5", "--polarity", "asis"]) == 0
    assert RS.main(["--a", "x.wav"]) == 2                                 # without --settings-key all of a/b/out are required
