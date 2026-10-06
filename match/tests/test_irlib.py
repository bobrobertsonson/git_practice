"""IR library (v0.4M Task B3): robustness of the scan, index cache, dedupe, tags, report. Needs the sawblade_core extension
(the IR as the core applies it comes from a render)."""
from __future__ import annotations

import json
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

pytest.importorskip("sawblade_core")
from sawblade_match.matcher import irlib                       # noqa: E402
from sawblade_match.matcher.pool import Capture                # noqa: E402


def synth_ir(fs: int, seconds: float = 0.08, seed: int = 0, freqs=(1800.0, 3500.0, 5200.0), taus=(0.004, 0.003, 0.002)) -> np.ndarray:
    """Band-limited decaying resonances sampled directly at ``fs`` (so the 44.1 and 48 kHz versions are the same IR)."""
    t = np.arange(int(seconds * fs)) / fs
    rng = np.random.default_rng(seed)
    x = np.zeros_like(t)
    for f, tau in zip(freqs, taus):
        x += rng.uniform(0.5, 1.0) * np.exp(-t / tau) * np.sin(2 * np.pi * f * t + rng.uniform(0, 6))
    x *= 1.0 - np.exp(-(t / 2e-4) ** 2)
    return (x / np.max(np.abs(x)) * 0.5).astype(np.float32)


def _home(tmp_path, monkeypatch) -> Path:
    """Isolated ~/.cache/sawblade and ~/.config/sawblade (setattr only, so it also runs on the local pytest shim)."""
    h = tmp_path / "home"
    h.mkdir(exist_ok=True)
    monkeypatch.setattr(irlib, "cache_dir", lambda: h / ".cache" / "sawblade")
    monkeypatch.setattr(irlib, "config_path", lambda: h / ".config" / "sawblade" / "ir_dirs.json")
    return h


def make_tree(root: Path) -> dict:
    a = root / "Pack One" / "Marshall 1960A V30" / "SM57 Cap"
    b = root / "Pack One" / "nested" / "deeper still"
    c = root / "ünïcode pack"
    for d in (a, b, c):
        d.mkdir(parents=True)
    sf.write(str(a / "my ir 01.wav"), synth_ir(48000, seed=1), 48000, subtype="PCM_24")
    sf.write(str(b / "stereo.wav"), np.stack([synth_ir(48000, seed=2), synth_ir(48000, seed=3)], axis=1), 48000, subtype="PCM_16")
    sf.write(str(b / "long 3s.wav"), synth_ir(48000, seconds=3.0, seed=4, taus=(0.5, 0.4, 0.3)), 48000, subtype="FLOAT")
    sf.write(str(c / "silent.wav"), np.zeros(4800, np.float32), 48000, subtype="FLOAT")
    (c / "notes.txt").write_text("not audio")
    (c / "corrupt.wav").write_bytes(b"RIFF\x00\x00garbage that is not a wav file at all")
    sf.write(str(c / "same 48k.wav"), synth_ir(48000, seed=5), 48000, subtype="FLOAT")
    sf.write(str(c / "same 44k.wav"), synth_ir(44100, seed=5), 44100, subtype="FLOAT")
    sf.write(str(c / "tiny.wav"), synth_ir(48000, seed=6)[:48], 48000, subtype="FLOAT")      # 1 ms
    return {"a": a, "b": b, "c": c}


def test_robust_scan_exact_counts(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    root = tmp_path / "irs"
    make_tree(root)
    lib = irlib.scan([root], workers=2)
    r = lib.report
    assert r["filesSeen"] == 9 and r["notAudio"] == {".txt": 1} and r["audioFiles"] == 8
    assert r["rejected"]["silent"]["count"] == 1 and r["rejected"]["unreadable"]["count"] == 1
    assert r["rejected"]["too-short"]["count"] == 1 and r["rejectedTotal"] == 3
    assert any("corrupt.wav" in p for p in r["rejected"]["unreadable"]["examples"])
    assert r["accepted"] == 5                        # my ir 01, stereo, long 3s, same 48k, same 44k
    assert r["exactDuplicates"] == 0 and r["nearDuplicates"] == 1 and r["unique"] == 4
    assert r["truncated"] == 1 and r["channels"] == {"1": 4, "2": 1} and r["rates"] == {"44100": 1, "48000": 4}
    names = {Path(x.path).name for x in lib.records}
    assert "same 48k.wav" in names and "same 44k.wav" not in names          # the 48 kHz copy is kept
    kept = next(x for x in lib.records if x.path.endswith("same 48k.wav"))
    assert len(kept.aliases) == 1 and "near-duplicate" in kept.aliases[0]["reason"] and "same 44k.wav" in kept.aliases[0]["path"]
    lt = next(x for x in lib.records if x.path.endswith("long 3s.wav"))
    assert lt.truncated and lt.orig_seconds == pytest.approx(3.0)
    assert lib.h.shape == (4, irlib.H_BINS) and np.all(np.isfinite(lib.h))
    # tags from the folder names
    t = next(x for x in lib.records if x.path.endswith("my ir 01.wav")).tags
    assert {"1960", "marshall", "v30"} <= set(t["cab"]) and t["mic"] == ["sm57"] and "cap" in t["position"]
    assert r["tagCoverage"]["mic"]["top"] == [["sm57", 1]]
    assert "unique 4" in irlib.format_report(r)


def test_exact_duplicates_and_aliases(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    root = tmp_path / "irs"
    (root / "x").mkdir(parents=True)
    (root / "y").mkdir()
    ir = synth_ir(48000, seed=9)
    for p in (root / "x" / "a.wav", root / "y" / "copy of a.wav"):
        sf.write(str(p), ir, 48000, subtype="FLOAT")
    lib = irlib.scan([root], workers=1)
    assert lib.report["accepted"] == 2 and lib.report["exactDuplicates"] == 1 and lib.report["unique"] == 1
    assert lib.records[0].path.endswith("a.wav") and "exact duplicate" in lib.records[0].aliases[0]["reason"]


def test_symlink_loop_and_multiple_roots_are_fine(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    import os
    root = tmp_path / "irs"
    (root / "sub").mkdir(parents=True)
    sf.write(str(root / "sub" / "a.wav"), synth_ir(48000, seed=1), 48000, subtype="FLOAT")
    try:
        os.symlink(str(root), str(root / "sub" / "loop"))
    except OSError:
        pytest.skip("no symlinks")
    lib = irlib.scan([root, root], workers=1)
    assert lib.report["filesSeen"] == 1 and lib.report["unique"] == 1


def test_index_is_cached_and_rescans_only_changes(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    root = tmp_path / "irs"
    make_tree(root)
    calls = []

    def counting(path, cdir=None):
        calls.append(path)
        return irlib.analyze_file(path, cdir)
    irlib.scan([root], workers=2, analyze=counting)
    n = len(calls)
    assert n == 8
    lib = irlib.scan([root], workers=2, analyze=counting)
    assert len(calls) == n and lib.report["indexReused"] == 8 and lib.report["unique"] == 4      # nothing re-read
    sf.write(str(root / "ünïcode pack" / "new.wav"), synth_ir(48000, seed=11), 48000, subtype="FLOAT")
    irlib.scan([root], workers=2, analyze=counting)
    assert len(calls) == n + 1
    idx = json.loads((home / ".cache" / "sawblade" / "ir_index.json").read_text())
    assert idx["version"] == 1 and len(idx["files"]) == 9


def test_progress_every_two_seconds_and_resumable(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    root = tmp_path / "irs"
    root.mkdir()
    for i in range(10):
        sf.write(str(root / f"ir{i}.wav"), synth_ir(48000, seed=i), 48000, subtype="FLOAT")
    calls = []

    def slow(path, cdir=None):
        calls.append(path)
        time.sleep(0.3)
        return irlib.analyze_file(path, cdir)
    stamps = []

    class Stop(Exception):
        pass

    def prog(done, total, eta):
        stamps.append((time.monotonic(), done))
        if done >= 4:
            raise Stop

    with pytest.raises(Stop):
        irlib.scan([root], workers=1, analyze=slow, progress=prog)
    idx = json.loads((home / ".cache" / "sawblade" / "ir_index.json").read_text())
    assert len(idx["files"]) >= 4                             # the interrupted scan kept what it had
    first = len(calls)
    irlib.scan([root], workers=1, analyze=slow, progress=lambda *a: stamps.append((time.monotonic(), a[0])))
    assert len(calls) < first + 10                             # resumed, not restarted
    gaps = np.diff([s[0] for s in stamps if s[1] >= 0])
    assert len(stamps) >= 3 and gaps.max() <= 2.0


def test_tags():
    t = irlib.tags_for("Cabs/Mesa 4x12 OS/G12T75 MD421 edge 2in/ir_cap_edge.wav")
    assert {"mesa", "4x12", "os", "g12t75"} <= set(t["cab"]) and "md421" in t["mic"]
    assert "capedge" in t["position"] and "dist:2in" in t["position"]
    t = irlib.tags_for("Greenback_R121_offaxis_1960 standard")
    assert {"greenback", "1960", "standard"} <= set(t["cab"]) and t["mic"] == ["r121"] and t["position"] == ["offaxis"]
    assert irlib.tags_for("random name 12")["cab"] == [] and irlib.tags_for("SM58 cone, 414")["mic"] == ["sm58", "414"]


def test_capture_is_local_user_owned_and_the_core_parser_accepts_it(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    from sawblade_match import core
    root = tmp_path / "irs"
    root.mkdir()
    sf.write(str(root / "my cab.wav"), synth_ir(48000, seed=1), 48000, subtype="FLOAT")
    lib = irlib.scan([root], workers=1)
    cap = lib.captures()[0]
    assert isinstance(cap, Capture) and cap.provider == "local" and cap.license == "user-owned" and cap.key.startswith("local/")
    bm = cap.block_model()
    assert bm["source"] == {"provider": "local", "id": cap.local_id, "title": "my cab", "license": "user-owned"}
    assert bm["sha256"] == irlib.file_sha256(root / "my cab.wav") and Path(bm["file"]).is_absolute()
    assert cap.source_info()["source"] == "local"
    preset = {"schema": "sawblade.preset", "version": 1, "name": "t", "gate": {"enabled": False},
              "paths": {"a": {"role": "saw", "blocks": []}, "b": {"role": "body", "enabled": False, "blocks": []}},
              "align": {"mode": "off"}, "blend": 0.0, "cab": {"mode": "shared", "ir": bm, "enabled": True},
              "postEq": [], "output": {"gainDb": 0.0}}
    y, rep = core.render(preset, np.random.default_rng(0).standard_normal(4800).astype(np.float32) * 0.1, 48000.0)
    assert len(y) == 4800 and np.all(np.isfinite(y))


def test_flac_is_converted_for_the_core_or_skipped_cleanly(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    root = tmp_path / "irs"
    root.mkdir()
    try:
        sf.write(str(root / "cab.flac"), synth_ir(44100, seed=2), 44100, subtype="PCM_16")
    except Exception:
        pytest.skip("this soundfile build cannot write FLAC")
    lib = irlib.scan([root], workers=1)
    assert lib.report["unique"] == 1
    cap = lib.captures()[0]
    assert cap.path.endswith(".wav") and cap.orig_path.endswith("cab.flac")


def test_response_matches_the_core_ir(tmp_path, monkeypatch):
    home = _home(tmp_path, monkeypatch)
    """|H|^2 on the Welch grid is the expected Welch response: white noise through the IR shows it."""
    ir = irlib._render_ir  # noqa: F841 (the render path is exercised by every scan)
    p = tmp_path / "a.wav"
    sf.write(str(p), synth_ir(48000, seed=1), 48000, subtype="FLOAT")
    y = irlib._render_ir(str(p))
    assert np.sqrt(np.sum(y.astype(np.float64) ** 2)) == pytest.approx(1.0, abs=1e-4)      # L2 = 1 like the core's IR
    h = irlib.response_from_ir(y)
    f = np.fft.rfftfreq(irlib.NFFT, 1 / irlib.RATE)
    assert h.shape == (irlib.H_BINS,) and h[f < 20000].max() > 0
    rng = np.random.default_rng(0)
    x = rng.standard_normal(irlib.NFFT * 64)
    from scipy import signal
    z = np.fft.irfft(np.fft.rfft(x, len(x) + len(y)) * np.fft.rfft(y, len(x) + len(y)))[:len(x)]
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(irlib.NFFT) / irlib.NFFT)
    segs = z[: (len(z) // irlib.NFFT) * irlib.NFFT].reshape(-1, irlib.NFFT) * w
    P = np.mean(np.abs(np.fft.rfft(segs, axis=1)) ** 2, axis=0)
    X = np.mean(np.abs(np.fft.rfft(x[: len(segs) * irlib.NFFT].reshape(-1, irlib.NFFT) * w, axis=1)) ** 2, axis=0)
    ratio_db = 10 * np.log10(np.maximum(P / X, 1e-30))
    pred_db = 20 * np.log10(np.maximum(h, 1e-15))
    sel = (f > 200) & (f < 8000)
    # band-averaged over 1/3 octave the two agree closely
    def band(v, lo, hi):
        m = (f >= lo) & (f < hi)
        return 10 * np.log10(np.mean(10 ** (v[m] / 10)))
    for lo in (250, 500, 1000, 2000, 4000):
        assert abs(band(ratio_db, lo, lo * 1.26) - band(pred_db, lo, lo * 1.26)) < 0.6
    assert sel.any()
