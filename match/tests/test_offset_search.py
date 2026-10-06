"""Whole-song DI placement (v0.2.1 Task B, matcher part): synthetic signals only, no network, no captures."""
from __future__ import annotations

import time

import numpy as np
import pytest

from sawblade_match.matcher import cli, offset
from sawblade_match.matcher.offset import PLACE_FAIL_MESSAGE, PlacementError, resolve_offset, whole_song_search

FS = 48000


def synth_di(seed: int, secs: float) -> np.ndarray:
    """A DI-like signal: harmonic notes with random pitch, length, decay and level (some palm-muted), never periodic."""
    rng = np.random.default_rng(seed)
    n = int(secs * FS)
    x = np.zeros(n + FS, np.float32)
    t = 0.0
    while t < secs:
        dur = float(rng.uniform(0.08, 0.6))
        f = 82.4 * 2 ** (int(rng.integers(0, 24)) / 12)
        tau = float(rng.uniform(0.03, 0.06)) if rng.random() < 0.3 else float(rng.uniform(0.1, 0.35))
        m = int(min(1.0, 5 * tau) * FS)
        tt = np.arange(m) / FS
        note = sum(np.sin(2 * np.pi * f * h * tt) / h for h in (1, 2, 3, 4)) * np.exp(-tt / tau)
        a = int(t * FS)
        x[a:a + m] += (float(rng.uniform(0.2, 1.0)) * note).astype(np.float32)
        t += dur
    x = x[:n]
    return x / np.max(np.abs(x))


def distort(di: np.ndarray, seed: int) -> np.ndarray:
    """What the reference holds: the same playing, hard-clipped, with a noise bed standing in for the rest of the band."""
    rng = np.random.default_rng(seed)
    return (0.5 * np.tanh(12.0 * di) + 0.02 * rng.standard_normal(len(di))).astype(np.float32)


@pytest.fixture(scope="module")
def song():
    di_full = synth_di(1, 60.0)
    return di_full, distort(di_full, 2)


@pytest.mark.parametrize("start_s", [23.417, 11.563, 47.985, 0.0])   # 47.985: 15 ms before ref_len - di_len
def test_found_within_2_5_ms(song, start_s):
    di_full, ref = song
    a = int(round(start_s * FS))
    di = di_full[a:a + 12 * FS]
    r = resolve_offset(di, ref, FS, offset_given=False)
    assert r["mode"] == "whole_song"
    assert abs(r["offset_ms"] - start_s * 1000) <= 2.5, r       # coarse + fine stage
    assert r["confidence"] >= offset.MIN_CONFIDENCE
    assert r["coarse_ms"] == pytest.approx(start_s * 1000, abs=10.0)


def test_clean_gain_does_not_matter(song):
    di_full, ref = song
    a = int(30.25 * FS)
    r = resolve_offset(0.1 * di_full[a:a + 10 * FS], ref, FS, offset_given=False)
    assert abs(r["offset_ms"] - 30250) <= 10.0


def test_unrelated_di_fails_with_message(song):
    _, ref = song
    other = synth_di(99, 12.0)
    with pytest.raises(PlacementError) as e:
        resolve_offset(other, ref, FS, offset_given=False)
    assert str(e.value) == "could not place the DI in the song: enter where it starts" == PLACE_FAIL_MESSAGE
    assert isinstance(e.value, ValueError)      # the CLI turns ValueError into "error: ..." + exit 3


def test_silent_di_fails(song):
    _, ref = song
    with pytest.raises(PlacementError):
        resolve_offset(np.zeros(8 * FS, np.float32), ref, FS, offset_given=False)


def test_ambiguous_loop_fails():
    """A riff looped three times fits three places equally well: ambiguous, so rejected."""
    loop = synth_di(5, 10.0)
    ref = distort(np.concatenate([loop, loop, loop]), 6)
    with pytest.raises(PlacementError):
        resolve_offset(loop[:8 * FS], ref, FS, offset_given=False)


def test_offset_given_skips_search(song, monkeypatch):
    di_full, ref = song

    def boom(*a, **k):
        raise AssertionError("whole-song search must not run when --offset-ms is given")
    monkeypatch.setattr(offset, "whole_song_search", boom)
    r = resolve_offset(di_full[:10 * FS], ref, FS, offset_given=True, offset_samples=int(1.5 * FS))
    assert r["mode"] == "given" and r["offset_ms"] == pytest.approx(1500.0) and r["offset_samples"] == int(1.5 * FS)
    assert r["confidence"] is None


def test_di_not_shorter_uses_window(song, monkeypatch):
    di_full, ref = song

    def boom(*a, **k):
        raise AssertionError("whole-song search must not run for a full-length DI")
    monkeypatch.setattr(offset, "whole_song_search", boom)
    for di in (di_full, np.concatenate([di_full, di_full[:FS]]), di_full[:len(di_full) - 2 * FS]):   # equal, longer, 2 s short
        r = resolve_offset(di, ref, FS, offset_given=False)
        assert r["mode"] == "window" and r["offset_samples"] == 0 and r["confidence"] is None


def test_deterministic(song):
    di_full, ref = song
    di = di_full[int(10.1 * FS):int(10.1 * FS) + 9 * FS]
    a, b = resolve_offset(di, ref, FS, False), resolve_offset(di.copy(), ref.copy(), FS, False)
    assert a["offset_samples"] == b["offset_samples"] and a["confidence"] == b["confidence"]


def test_five_minute_song_is_fast():
    secs = 300.0
    di_full = synth_di(7, secs)
    ref = distort(di_full, 8)
    a = int(151.234 * FS)
    di = di_full[a:a + 30 * FS]
    t0 = time.perf_counter()
    r = resolve_offset(di, ref, FS, False)
    dt = time.perf_counter() - t0
    print(f"whole-song search over {secs:.0f} s reference, 30 s DI: {dt:.2f} s")
    assert abs(r["offset_ms"] - 151234) <= 10.0
    assert dt < 20.0


def test_log_env_is_finite_after_loud_to_silent():
    """Regression: the running-sum smoothing used to return tiny negative energies after a loud frame, so sqrt gave NaN."""
    import warnings
    x = np.zeros(4 * FS, np.float32)
    x[FS:FS + 4800] = np.random.default_rng(3).standard_normal(4800).astype(np.float32) * 30.0
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        for hop, sm in ((48, 5), (240, 3), (480, 1)):
            e = offset._log_env(x, hop, 200, sm)
            assert np.all(np.isfinite(e))


def test_ncc_is_finite_and_rejects_nonfinite_input():
    rng = np.random.default_rng(4)
    a = np.concatenate([rng.standard_normal(500) * 50, np.zeros(2000), rng.standard_normal(500)])
    b = rng.standard_normal(100)
    c = offset._ncc_valid(a, b)
    assert np.all(np.isfinite(c)) and np.max(np.abs(c)) <= 1.0 + 1e-6
    bad = a.copy()
    bad[10] = np.nan
    with pytest.raises(FloatingPointError):
        offset._ncc_valid(bad, b)


def test_fine_stage_recentres_when_peak_is_on_the_window_edge(song):
    di_full, _ = song
    a = int(20.0 * FS)
    di = di_full[a:a + 10 * FS]
    w, fh = int(0.020 * FS), int(0.001 * FS)
    off = a + int(0.030 * FS)                      # 30 ms off: outside the first +-20 ms window
    for _ in range(2):
        off, edge = offset._fine_place(di, di_full, off, w, fh)
        if not edge:
            break
    assert abs(1000.0 * (off - a) / FS) <= 2.5


def test_cli_prints_failure_message(monkeypatch, capsys, tmp_path):
    monkeypatch.setattr(cli, "load_pool", lambda *a, **k: object())
    monkeypatch.setattr(cli, "load_reference", lambda *a, **k: object())

    def fail(cfg, log=None):
        raise PlacementError(PLACE_FAIL_MESSAGE)
    monkeypatch.setattr(cli, "run_match", fail)
    rc = cli.main(["--di", "d.wav", "--ref", "r.wav", "--pool", "p.json", "--out", str(tmp_path)])
    assert rc != 0
    assert capsys.readouterr().err.strip() == "error: could not place the DI in the song: enter where it starts"


def test_window_slack_boundary(song):
    """2 s short -> window (the +-3 s search covers it); 3.5 s short -> whole-song search."""
    di_full, ref = song
    assert resolve_offset(di_full[:len(di_full) - 2 * FS], ref, FS, False)["mode"] == "window"
    r = resolve_offset(di_full[:len(di_full) - int(3.5 * FS)], ref, FS, False)
    assert r["mode"] == "whole_song" and abs(r["offset_ms"]) <= 2.5


def test_result_never_exceeds_ref_minus_di(song):
    di_full, ref = song
    di = di_full[:len(di_full) - int(3.5 * FS)]
    r = whole_song_search(di, ref, FS)
    assert 0 <= r["offset"] <= len(ref) - len(di)
    tail = di_full[len(di_full) - 20 * FS:]         # placed at the very end of the song
    r = whole_song_search(tail, ref, FS)
    assert r["offset"] <= len(ref) - len(tail)


# ---- end to end through run_match (the fixture pool is the one test_matcher.py uses) ---------------------------------
def _e2e_cfg(tmp_path, di_sig, ref_sig, name):
    import soundfile as sf
    from sawblade_match.matcher.reference import load_reference
    from sawblade_match.matcher.run import Config
    from test_matcher import fixture_pool, mkplan
    d = tmp_path / name
    d.mkdir()
    sf.write(str(d / "di.wav"), di_sig, FS, subtype="FLOAT")
    sf.write(str(d / "ref.wav"), ref_sig, FS, subtype="FLOAT")
    ref = load_reference(d / "ref.wav", channel="mid", matched="mono", offset_ms=None)
    plan = mkplan(top_k={"blend": 0, "single": 1, "single2": 0})
    return Config(di=d / "di.wav", ref=ref, pool=fixture_pool(), out=d / "out", seed=1, excerpt_s=2.0, threads=2,
                  plan=plan, write_audio=False, refine_offsets=True)


def test_run_match_fails_for_an_unplaceable_di(tmp_path):
    from sawblade_match.matcher.run import run_match
    cfg = _e2e_cfg(tmp_path, synth_di(99, 10.0), distort(synth_di(1, 20.0), 2), "unrelated")   # 10 s short > 3 s
    lines: list[str] = []
    with pytest.raises(PlacementError) as e:
        run_match(cfg, lines.append)
    assert str(e.value) == PLACE_FAIL_MESSAGE
    assert any(l.startswith("whole-song placement failed: r1 ") and "MIN_CONFIDENCE" in l for l in lines), lines


def test_run_match_after_placement_refines_only_within_250_ms(tmp_path, monkeypatch):
    """A confident whole-song placement must not be overridden by a louder decoy: the starter-render refinement gets a
    +-250 ms window, not +-3 s."""
    from sawblade_match.matcher import run as R
    full = synth_di(1, 20.0)
    a = int(7.3 * FS)
    cfg = _e2e_cfg(tmp_path, full[a:a + 8 * FS], distort(full, 2), "placed")
    calls = []

    class Stop(RuntimeError):
        pass

    def spy(render, refsig, fs, coarse, start=0, search=None, **kw):
        calls.append((coarse, search))
        raise Stop()
    monkeypatch.setattr(R, "refine_offset", spy)
    with pytest.raises(Stop):
        R.run_match(cfg, R.Log())
    assert calls and calls[0][1] == int(R.PLACED_WINDOW_MS * FS / 1000) == int(0.25 * FS)
    assert abs(1000.0 * calls[0][0] / FS - 7300) <= 2.5          # handed the coarse placement, not 0
