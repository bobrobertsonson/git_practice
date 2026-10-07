"""v0.4M Task G.4: the matcher scores with the RECORD dynamics set and emits presets that play LIVE
(origin "match", dynamicsMode "live", no explicit liveDynamics). Needs the sawblade_core built after Task G."""
from __future__ import annotations

import json

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher.space import build_preset, gate_preset, manual_align

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
if not hasattr(core, "resolve_dynamics") or getattr(core, "_core", None) is None or not hasattr(core._core, "resolve_dynamics"):
    pytest.skip("sawblade_core built before Task G (no resolve_dynamics)", allow_module_level=True)
from sawblade_match.matcher.engine import Engine                      # noqa: E402
from sawblade_match.matcher.run import Config, Log, run_match         # noqa: E402
from test_matcher import _setup_known, fixture_pool, hidden, mkplan   # noqa: E402

COMP = {"enabled": True, "thresholdDb": -40.0, "ratio": 4.0, "kneeDb": 6.0, "attackMs": 10.0, "releaseMs": 100.0,
        "makeupDb": 0.0}


def test_emitted_preset_has_origin_live_mode_and_rounded_threshold():
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    gate = {**gate_preset(-50.0), "thresholdDb": -46.123456789}
    p = build_preset(combo, v, gate=gate, align=manual_align(0, False), emit=True)
    assert p["version"] == 4 and p["origin"] == "match" and p["dynamicsMode"] == "live" and "liveDynamics" not in p
    assert p["gate"]["thresholdDb"] == -46.1235
    r = core.resolve_dynamics(p)                                  # the core derives the live set
    assert r["mode"] == "live" and r["gate"]["mode"] == "expander" and r["gate"]["thresholdMode"] == "floorRelative"
    assert r["busComp"]["enabled"] is False
    q = build_preset(combo, v, gate=gate, align=manual_align(0, False))      # internal presets are unchanged
    assert "origin" not in q and "dynamicsMode" not in q and q["version"] == 1 and q["gate"]["thresholdDb"] == -46.123456789


def test_engine_render_forces_record_and_live_is_explicit(tmp_path):
    pool = fixture_pool()
    combo, sp, v = hidden(pool)
    gate = gate_preset(-50.0)
    p = build_preset(combo, v, gate=gate, align=manual_align(0, False), bus_comp=COMP, emit=True)
    x = (np.random.default_rng(1).normal(size=48000) * 0.1).astype(np.float32)
    old = {k: val for k, val in p.items() if k not in ("origin", "dynamicsMode")}      # the pre-G form (record by default)
    eng = Engine(None, 1)
    try:
        y_emitted, _ = eng.render(p, x, 48000)
        y_old, _ = core.render(old, x, 48000.0)
        y_live, _ = eng.render(p, x, 48000, dynamics="live")
        with pytest.raises(ValueError):
            eng.render(p, x, 48000, dynamics="bogus")
    finally:
        eng.close()
    assert np.array_equal(np.asarray(y_emitted), np.asarray(y_old))          # scoring renders never see the live set
    assert not np.allclose(np.asarray(y_emitted), np.asarray(y_live))        # the live set (comp off) really differs


def test_run_emits_live_presets_scores_in_record_and_writes_render_live(tmp_path):
    pool, combo, di, ref = _setup_known(tmp_path, "blend")
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp_path / "out", seed=1, excerpt_s=2.0, threads=2, plan=mkplan(),
                 write_audio=True, refine_offsets=False)
    res = run_match(cfg, Log())
    out = tmp_path / "out"
    assert res["dynamics"] == {"scoredWith": "record", "emittedMode": "live", "origin": "match"}
    files = [out / res["best"]["preset"]] + sorted(out.glob("alt*.preset.resolved.json"))
    assert len(files) >= 2
    for f in files:
        p = json.loads(f.read_text())
        assert p["origin"] == "match" and p["dynamicsMode"] == "live" and "liveDynamics" not in p and p["version"] == 4
        thr = p["gate"].get("thresholdDb")
        assert thr is None or thr == round(thr, 4)
    port = json.loads((out / "best.preset.json").read_text())
    assert port["origin"] == "match" and port["dynamicsMode"] == "live"
    # scoring unchanged: the emitted preset renders (record forced) bit-identically to the same preset without the G keys
    best = json.loads(files[0].read_text())
    x, fs = sf.read(str(di), dtype="float32")
    eng = Engine(None, 1)
    try:
        y1, _ = eng.render(best, x, fs)
    finally:
        eng.close()
    y0, _ = core.render({k: v for k, v in best.items() if k not in ("origin", "dynamicsMode")}, x, float(fs))
    assert np.array_equal(np.asarray(y1), np.asarray(y0))
    # listening: render_live.wav next to render.wav, same section, same gain
    lst = res["listening"]
    d = out / "listen"
    assert (d / "render_live.wav").exists() and lst["files"]["renderLive"].endswith("render_live.wav")
    a, ra = sf.read(str(d / "render.wav"), dtype="float32")
    b, rb = sf.read(str(d / "render_live.wav"), dtype="float32")
    assert ra == rb and len(a) == len(b)
    assert set(res["best"]["fullLengthPeakDbfs"]) == {"best_L", "live_L"}          # the live render feeds the clip guard too
    assert "lufsRenderLive" in lst and "renderLive" in lst["truePeakDb"]
