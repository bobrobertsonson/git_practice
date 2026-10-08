"""Tests for the sawblade_core Python bindings (spec 3.1).

Needs the extension (see match/README.md; located by sawblade_match.core) and, for the bit-identity
tests, the tonerender binary ($SAWBLADE_TONERENDER, else <repo>/build*/cli/tonerender). Skipped
(with the reason) when the extension has not been built.
"""
from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

try:
    from sawblade_match import core
except ImportError as exc:  # pragma: no cover - depends on the local build
    pytest.skip(f"sawblade_core is not built: {exc}", allow_module_level=True)

REPO = Path(__file__).resolve().parents[2]
PRESETS = REPO / "tests" / "fixtures" / "presets"
DI = REPO / "tests" / "fixtures" / "di_riff.wav"
GOLDEN = ("golden_shared", "golden_perpath")


def _tonerender() -> Path | None:
    env = os.environ.get("SAWBLADE_TONERENDER")
    cands = [Path(env)] if env else []
    cands += [REPO / d / "cli" / "tonerender" for d in ("build", "build-py", "build-lead")]
    return next((c for c in cands if c.is_file()), None)


needs_cli = pytest.mark.skipif(_tonerender() is None, reason="tonerender binary not found")


@pytest.fixture(scope="module")
def di() -> tuple[np.ndarray, int]:
    x, sr = sf.read(DI, dtype="float32")
    return x, sr


def preset_text(name: str) -> str:
    return (PRESETS / f"{name}.json").read_text()


def render(name: str, x, sr, **kw):
    return core.render(preset_text(name), x, sr, base_dir=PRESETS, **kw)


def run_cli(name: str, wav: Path, out: Path, report: Path, *extra: str) -> np.ndarray:
    subprocess.run(
        [str(_tonerender()), "--preset", str(PRESETS / f"{name}.json"), "--in", str(wav),
         "--out", str(out), "--report", str(report), *extra],
        check=True, capture_output=True,
    )
    y, _ = sf.read(out, dtype="float32")
    return y


# ---- bit-identity with the CLI ------------------------------------------------------------------
@needs_cli
@pytest.mark.parametrize("name", GOLDEN)
def test_bit_identical_to_tonerender(name, di, tmp_path):
    x, sr = di
    y, rep = render(name, x, sr)
    ref = run_cli(name, DI, tmp_path / "o.wav", tmp_path / "r.json")
    assert y.dtype == np.float32 and y.shape == x.shape
    assert np.array_equal(y, ref)
    cli_rep = json.loads((tmp_path / "r.json").read_text())
    for key in ("latencySamples", "pathLatency", "renderRate", "inputRate", "alignDelay", "align", "liveCompatible", "frames"):
        assert rep[key] == cli_rep[key], key
    assert rep["latencySamples"] >= 0  # latency is always reported


@needs_cli
def test_44k_input(di, tmp_path):
    from scipy.signal import resample_poly

    x, _ = di
    x44 = resample_poly(x, 147, 160).astype(np.float32)
    wav = tmp_path / "di44.wav"
    sf.write(wav, x44, 44100, subtype="FLOAT")
    y, rep = render("golden_shared", x44, 44100.0)
    assert y.shape == x44.shape and rep["inputRate"] == 44100.0
    assert rep["renderRate"] != 44100.0  # NAM models pick the render rate; resampled back
    ref = run_cli("golden_shared", wav, tmp_path / "o.wav", tmp_path / "r.json")
    assert np.array_equal(y, ref)
    y_r, rep_r = render("golden_shared", x44, 44100, out_rate="render")
    assert rep_r["outputRate"] == rep_r["renderRate"] and len(y_r) != len(y)


def test_dict_preset_and_float64_and_block(di):
    x, sr = di
    x = x[:20000]
    y1, _ = render("golden_shared", x, sr)
    y2, _ = core.render(json.loads(preset_text("golden_shared")), x.astype(np.float64), sr, base_dir=str(PRESETS))
    assert np.array_equal(y1, y2)
    y3, rep3 = render("golden_shared", x, sr, block=37)
    assert rep3["blockSize"] == 37 and np.allclose(y1, y3, atol=1e-5)


def test_stereo_equal_channels_uses_left(di):
    x, sr = di
    x = x[:20000]
    y1, _ = render("golden_shared", x, sr)
    y2, rep = render("golden_shared", np.stack([x, -x], axis=1), sr)  # equal RMS: a tie picks L
    assert np.array_equal(y1, y2) and any("channels" in w for w in rep["warnings"])
    assert rep["diChannel"]["used"] == "L" and rep["diChannel"]["rule"] == "auto"


def test_stereo_di_channel_rule(di):
    """v0.8 I4a: the louder channel by whole-file RMS is the default; L / R / mix are explicit."""
    x, sr = di
    x = x[:20000]
    st = np.stack([0.1 * x, x], axis=1)
    mono, _ = render("golden_shared", x, sr)
    quiet, _ = render("golden_shared", 0.1 * x, sr)
    y, rep = render("golden_shared", st, sr)
    assert np.array_equal(y, mono) and rep["diChannel"]["used"] == "R" and rep["diChannel"]["fileChannels"] == 2
    y, rep = render("golden_shared", st, sr, di_channel="L")
    assert np.array_equal(y, quiet) and rep["diChannel"]["rule"] == "L"
    mix, rep = render("golden_shared", st, sr, di_channel="mix")
    assert rep["diChannel"]["used"] == "mix" and not np.array_equal(mix, mono) and not np.array_equal(mix, quiet)
    with pytest.raises(ValueError):
        render("golden_shared", st, sr, di_channel="left")


def test_calibration_options_and_report(di):
    """v0.8 I4a: calibration follows the preset (v<=4 = legacy, bit-identical), device_dbu and the report fields."""
    x, sr = di
    x = x[:20000]
    legacy, rep = render("golden_shared", x, sr)
    assert rep["calibration"]["mode"] == "legacy" and rep["calibration"]["modeSource"] == "preset"
    assert not rep["calibration"]["enabled"]
    forced_off, _ = render("golden_shared", x, sr, calibration="legacy")
    assert np.array_equal(legacy, forced_off)

    preset = json.loads(preset_text("golden_shared"))
    preset["version"] = 5
    preset["calibration"] = {"mode": "calibrated"}
    y, rep = core.render(preset, x, sr, base_dir=PRESETS)
    c = rep["calibration"]
    assert c["mode"] == "calibrated" and c["enabled"] and c["deviceDbu"] == 12.0 and c["deviceAssumed"]
    assert c["paths"]["a"] and "gainInDb" in c["paths"]["a"][0]
    y18, rep18 = core.render(preset, x, sr, base_dir=PRESETS, device_dbu=18.0)
    assert rep18["calibration"]["deviceDbu"] == 18.0 and not rep18["calibration"]["deviceAssumed"]
    g12 = [b["gainInDb"] for b in c["paths"]["a"]]
    g18 = [b["gainInDb"] for b in rep18["calibration"]["paths"]["a"]]
    assert g18[0] == pytest.approx(g12[0] + 6.0)
    # forcing it on a legacy preset is the same render as the calibrated preset
    forced_on, _ = render("golden_shared", x, sr, calibration="calibrated")
    assert np.array_equal(forced_on, y)

    for bad in ({"calibration": "maybe"}, {"device_dbu": 99.0}, {"device_dbu": True}):
        with pytest.raises(ValueError):
            render("golden_shared", x, sr, **bad)


# ---- capture cache ------------------------------------------------------------------------------
def test_cache_hits_and_no_reload_on_continuous_change(di):
    x, sr = di
    x = x[:48000]
    cache = core.CaptureCache()
    preset = json.loads(preset_text("golden_shared"))
    y0, _ = core.render(preset, x, sr, base_dir=PRESETS, cache=cache)
    assert (cache.misses, len(cache)) == (4, 4)  # wavenet, lstm, linear_identity, ir_a
    hits0 = cache.hits
    preset["blend"] = 0.2
    preset["paths"]["a"]["levelDb"] = -6.0
    preset["paths"]["b"]["blocks"][1]["inputGainDb"] = 3.0  # NAM gain is a continuous param too
    y1, _ = core.render(preset, x, sr, base_dir=PRESETS, cache=cache)
    assert cache.misses == 4 and cache.hits > hits0  # nothing reloaded
    y1_ref, _ = core.render(preset, x, sr, base_dir=PRESETS)  # uncached
    assert np.array_equal(y1, y1_ref) and not np.array_equal(y0, y1)
    assert cache.stats() == {"hits": cache.hits, "misses": 4, "files": 4}
    cache.clear()
    assert (cache.hits, cache.misses, len(cache)) == (0, 0, 0)


@pytest.mark.parametrize("name", GOLDEN)
def test_cached_render_is_bit_identical(name, di):
    x, sr = di
    x = x[:48000]
    cache = core.CaptureCache()
    ref, _ = render(name, x, sr)
    first, _ = render(name, x, sr, cache=cache)
    second, _ = render(name, x, sr, cache=cache)
    assert np.array_equal(ref, first) and np.array_equal(ref, second)


# ---- errors -------------------------------------------------------------------------------------
def test_preset_error_is_value_error_with_json_path(di):
    x, sr = di
    p = json.loads(preset_text("golden_shared"))
    p["blend"] = 7.0
    with pytest.raises(core.PresetError) as ei:
        core.render(p, x[:1000], sr, base_dir=PRESETS)
    assert isinstance(ei.value, ValueError) and ei.value.json_path == "blend"
    assert "blend" in str(ei.value)

    p = json.loads(preset_text("golden_shared"))
    p["paths"]["a"]["blocks"][0]["bogus"] = 1
    with pytest.raises(ValueError) as ei:
        core.render(p, x[:1000], sr, base_dir=PRESETS)
    assert ei.value.json_path.startswith("paths.a.blocks[0]")

    with pytest.raises(core.PresetError):  # invalid JSON text
        core.render("{nope", x[:1000], sr)
    p = json.loads(preset_text("golden_shared"))
    p["postEq"][0]["freq"] = 40000.0  # fails only at load time for the render rate
    with pytest.raises(core.PresetError) as ei:
        core.render(p, x[:1000], sr, base_dir=PRESETS)
    assert ei.value.json_path == "postEq"


@pytest.mark.parametrize("cached", [False, True])
def test_io_errors_are_os_errors_with_json_path(di, cached):
    x, sr = di
    x = x[:1000]
    cache = core.CaptureCache() if cached else None
    p = json.loads(preset_text("golden_shared"))
    p["paths"]["b"]["blocks"][1]["model"]["file"] = "../nam/absent.nam"
    with pytest.raises(core.RenderIOError) as ei:
        core.render(p, x, sr, base_dir=PRESETS, cache=cache)
    assert isinstance(ei.value, OSError) and ei.value.json_path == "paths.b.blocks[1].model.file"

    p = json.loads(preset_text("golden_shared"))
    p["cab"]["ir"]["sha256"] = "0" * 64
    with pytest.raises(OSError, match="sha256 mismatch") as ei:
        core.render(p, x, sr, base_dir=PRESETS, cache=cache)
    assert ei.value.json_path == "cab.ir.file"


def test_argument_errors(di):
    x, sr = di
    t = preset_text("golden_shared")
    for kw in ({"block": 0}, {"render_rate": "fast"}, {"render_rate": 5.0}, {"out_rate": "x"}):
        with pytest.raises(ValueError):
            core.render(t, x[:100], sr, base_dir=PRESETS, **kw)
    with pytest.raises(ValueError):
        core.render(t, x[:0], sr, base_dir=PRESETS)
    with pytest.raises(ValueError):
        core.render(t, x[:100], 0.0, base_dir=PRESETS)
    with pytest.raises(TypeError):
        core.render(42, x[:100], sr)


# ---- threads ------------------------------------------------------------------------------------
def test_parallel_renders_match_serial(di):
    x, sr = di
    x = x[:72000]
    jobs = [("golden_shared", 0.55), ("golden_perpath", None), ("golden_shared", 0.2), ("golden_shared", 0.9)]

    def make(name, blend):
        p = json.loads(preset_text(name))
        if blend is not None:
            p["blend"] = blend
        return p

    serial = [core.render(make(*j), x, sr, base_dir=PRESETS)[0] for j in jobs]
    cache = core.CaptureCache()
    with ThreadPoolExecutor(max_workers=4) as pool:
        futs = [pool.submit(lambda j=j: core.render(make(*j), x, sr, base_dir=PRESETS, cache=cache)[0]) for j in jobs]
        par = [f.result() for f in futs]
    for s, p in zip(serial, par):
        assert np.array_equal(s, p)
    assert cache.misses == 5  # each distinct capture loaded exactly once despite the race
    with ThreadPoolExecutor(max_workers=4) as pool:  # and again with no cache at all
        par2 = list(pool.map(lambda j: core.render(make(*j), x, sr, base_dir=PRESETS)[0], jobs))
    for s, p in zip(serial, par2):
        assert np.array_equal(s, p)


def test_gil_is_released_while_rendering(di):
    x, sr = di
    stop = threading.Event()
    ticks = [0]

    def spin():
        while not stop.is_set():
            ticks[0] += 1
            time.sleep(0)

    t = threading.Thread(target=spin)
    t.start()
    try:
        before = ticks[0]
        t0 = time.perf_counter()
        render("golden_perpath", x, sr)
        dt = time.perf_counter() - t0
        progressed = ticks[0] - before
    finally:
        stop.set()
        t.join()
    # A render that held the GIL would let the spinner advance by ~0 for the whole call.
    assert progressed > 100 * dt


# ---- input validation / robustness (follow-ups) ---------------------------------------------------
def test_rejects_channels_first_and_integer_audio(di):
    x, sr = di
    t = preset_text("golden_shared")
    with pytest.raises(ValueError, match=r"channels-first.*\.T"):
        core.render(t, np.stack([x[:100], x[:100]]).reshape(1, -1), sr, base_dir=PRESETS)
    for dt in (np.int16, np.int32, np.uint8):
        with pytest.raises(ValueError, match="float audio"):
            core.render(t, (x[:100] * 1000).astype(dt), sr, base_dir=PRESETS)
    # (frames, 1) and (frames, 2) stay valid
    core.render(t, x[:2000, None], sr, base_dir=PRESETS)


def test_rejects_bool_block_and_low_sample_rate(di):
    x, sr = di
    t = preset_text("golden_shared")
    for bad in (True, False, 256.0, "256"):
        with pytest.raises(ValueError):
            core.render(t, x[:100], sr, base_dir=PRESETS, block=bad)
    for bad in (True, 999.0, 0, -48000, float("nan"), "48000"):
        with pytest.raises(ValueError):
            core.render(t, x[:100], bad, base_dir=PRESETS)


def test_failed_load_then_fixed_file_retries(di, tmp_path):
    import shutil

    x, sr = di
    x = x[:5000]
    nam = tmp_path / "m.nam"
    p = json.loads(preset_text("golden_shared"))
    p["paths"]["a"]["blocks"][0]["model"]["file"] = str(nam)
    cache = core.CaptureCache()
    nam.write_text("not a nam file")
    with pytest.raises(core.RenderIOError) as ei:
        core.render(p, x, sr, base_dir=PRESETS, cache=cache)
    assert ei.value.json_path == "paths.a.blocks[0].model.file"
    shutil.copy(REPO / "tests/fixtures/nam/wavenet.nam", nam)  # fixed
    y, _ = core.render(p, x, sr, base_dir=PRESETS, cache=cache)
    ref, _ = core.render(p, x, sr, base_dir=PRESETS)
    assert np.array_equal(y, ref)


def test_clear_during_inflight_renders(di):
    x, sr = di
    x = x[:96000]
    p = json.loads(preset_text("golden_shared"))
    ref, _ = core.render(p, x, sr, base_dir=PRESETS)
    cache = core.CaptureCache()
    stop = threading.Event()

    def clearer():
        while not stop.is_set():
            cache.clear()
            time.sleep(0.01)

    t = threading.Thread(target=clearer)
    t.start()
    try:
        with ThreadPoolExecutor(max_workers=4) as pool:
            outs = list(pool.map(lambda _: core.render(p, x, sr, base_dir=PRESETS, cache=cache)[0], range(8)))
    finally:
        stop.set()
        t.join()
    assert all(np.array_equal(o, ref) for o in outs)


def test_numpy_scalars_accepted(di):
    x, sr = di
    core.render(preset_text("golden_shared"), x[:2000], np.int64(sr), base_dir=PRESETS, block=np.int64(64))
