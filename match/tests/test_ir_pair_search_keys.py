"""Regression: the two-IR pair search resolved the sweep's top-6 IR keys through the capture dict of ANOTHER candidate
(run.py: ``sw`` = the first swept candidate of the winner's topology / boost, ``sw_caps`` = the last one's captures), so with a
large local IR library (each candidate screens its own top 24) a key was missing: ``KeyError: 'local/<sha>'``."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from sawblade_match.matcher import irlib

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
from scipy import signal                                                # noqa: E402

FS = 48000


def diverse_irs(n: int, seed: int) -> list[np.ndarray]:
    """n spectrally distinct cab-like IRs (random peaking cascade, band limits, short noise tail): none is a near-duplicate
    of another (the library keeps all of them)."""
    rng = np.random.default_rng(seed)
    out, m = [], 2048
    t = np.arange(m) / FS
    for _ in range(n):
        h = np.zeros(m)
        h[8] = 1.0
        for _ in range(3):
            f, q, g = rng.uniform(80, 9000), rng.uniform(0.7, 4), rng.uniform(-9, 9)
            w0, A = 2 * np.pi * f / FS, 10 ** (g / 40)
            al = np.sin(w0) / (2 * q)
            h = signal.lfilter([1 + al * A, -2 * np.cos(w0), 1 - al * A], [1 + al / A, -2 * np.cos(w0), 1 - al / A], h)
        h = signal.sosfilt(signal.butter(2, [rng.uniform(40, 200), rng.uniform(2500, 10000)], btype="band", fs=FS, output="sos"), h)
        tail = rng.normal(size=m) * np.exp(-t / rng.uniform(0.002, 0.01)) * rng.uniform(0.02, 0.2)
        out.append((h + tail * (np.arange(m) > 20)).astype(np.float32))
    return out


def _run(tmp, monkeypatch, n_local=110, n_dup=12, top_k_single=3):
    from test_matcher import mkplan
    from test_matcher_v04m import _known, distinct_tone_pool
    from sawblade_match.matcher.run import Config, Log, run_match
    from sawblade_match.matcher.space import Combo, Space
    h = tmp / "home"
    h.mkdir()
    monkeypatch.setattr(irlib, "cache_dir", lambda: h / ".cache" / "sawblade")
    root = tmp / "my irs" / "Pack A"
    root.mkdir(parents=True)
    irs = diverse_irs(n_local, 7)
    rng = np.random.default_rng(5)
    for i, ir in enumerate(irs):
        sf.write(str(root / f"V30 SM57 cap {i}.wav"), ir, FS, subtype="FLOAT")
    for i in range(n_dup):                 # near-duplicates: the same IR plus a whisper of noise (dropped by the library)
        d = irs[i] + rng.normal(size=len(irs[i])).astype(np.float32) * 1e-5
        sf.write(str(root / f"V30 SM57 cap {i} copy.wav"), d, FS, subtype="FLOAT")
    lib = irlib.scan([tmp / "my irs"], workers=2)
    pool = distinct_tone_pool()
    combo = Combo((pool.pedals[0],), pool.amps[1], None, None, lib.captures()[7])     # the hidden tone's cab is a LOCAL IR
    v = Space.for_combo(combo).default()
    v.update({"post.g1": 1.5})
    di, ref = _known(tmp, pool, combo, v)
    plan = mkplan(top_k={"blend": 0, "single": top_k_single, "single2": 0}, gens_linear=4, gens_gain=2, gens_final=3,
                  n_rescore_single=8, n_cab_single=3)
    cfg = Config(di=di, ref=ref, pool=pool, out=tmp / "out", seed=3, excerpt_s=2.0, threads=2, plan=plan, write_audio=False,
                 refine_offsets=False, ir_library=lib, ir_dirs=({"path": str(tmp / "my irs"), "source": "cli"},))
    return run_match(cfg, Log()), lib


def test_pair_search_keys_resolve_with_a_large_local_library(tmp_path, monkeypatch):
    res, lib = _run(tmp_path, monkeypatch)
    sweeps = res["cabSweep"]["candidates"]
    # --- fixture preconditions: a failure here means the fixture NO LONGER EXERCISES the bug (it says nothing about the bug) ---
    assert lib.report["nearDuplicates"] >= 1, "FIXTURE: the library produced no near-duplicates (dedupe path not exercised)"
    assert len(lib.records) >= 60, f"FIXTURE: only {len(lib.records)} unique local IRs; the per-candidate top-24 screens need many more"
    assert len(sweeps) >= 2, "FIXTURE: fewer than two candidates of one topology were swept (top_k / plan changed?)"
    assert len({tuple(sorted(i["cab"] for i in s["irs"])) for s in sweeps}) >= 2, \
        "FIXTURE: every swept candidate screened the same IRs, so a topology-keyed lookup could not go wrong"
    # --- the regression itself: the run did not die at the pair search, and the IRs came from the winner's own sweep ---
    assert res["irBlend"]["ablated"] is False
    keys = res["irBlend"]["irs"]
    swept = {i["cab"] for s in sweeps for i in s["irs"]}
    assert keys and set(keys) <= swept
    mine = next(s for s in sweeps if s["pairKey"] == res["irBlend"]["candidatePairKey"])
    assert set(keys) <= {i["cab"] for i in mine["irs"]}
