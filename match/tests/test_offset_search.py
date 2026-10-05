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


@pytest.mark.parametrize("start_s", [23.417, 0.0, 47.9])
def test_found_within_10_ms(song, start_s):
    di_full, ref = song
    a = int(round(start_s * FS))
    di = di_full[a:a + 12 * FS]
    r = resolve_offset(di, ref, FS, offset_given=False)
    assert r["mode"] == "whole_song"
    assert abs(r["offset_ms"] - start_s * 1000) <= 10.0, r
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


def test_cli_prints_failure_message(monkeypatch, capsys, tmp_path):
    monkeypatch.setattr(cli, "load_pool", lambda *a, **k: object())
    monkeypatch.setattr(cli, "load_reference", lambda *a, **k: object())

    def fail(cfg, log=None):
        raise PlacementError(PLACE_FAIL_MESSAGE)
    monkeypatch.setattr(cli, "run_match", fail)
    rc = cli.main(["--di", "d.wav", "--ref", "r.wav", "--pool", "p.json", "--out", str(tmp_path)])
    assert rc != 0
    assert capsys.readouterr().err.strip() == "error: could not place the DI in the song: enter where it starts"
