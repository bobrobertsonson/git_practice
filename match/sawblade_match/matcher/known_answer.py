"""Known-answer recovery (spec 3.2 acceptance a) with real pool captures.

A hidden preset is built from pool captures (seeded: combo + continuous values), the DI section is rendered through it
(real chain, 48 kHz-internal), and that render is the reference of a matched pair (offset 0). The matcher must find a
preset whose A-weighted LTAS error against the reference (tonecheck definition, whole section) is within 0.5 dB of the
hidden preset's own error (0 by construction). Run as ``python -m sawblade_match.matcher.known_answer --pool ... --di ...``.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from ..core import render
from .engine import Engine, RATE, to48
from .excerpt import select_excerpt
from .pool import default_cab, load_pool
from .reference import load_reference
from .run import Config, Log, Plan, caps_summary, gate_envelope_floor_db, run_match
from .space import Combo, Space, build_preset, gate_preset

TOLERANCE_DB = 0.5


def hidden_preset(pool, seed: int, gate: dict, engine: Engine, topology: str = "blend"):
    rng = np.random.default_rng(seed)
    pick = lambda lst: lst[int(rng.integers(len(lst)))]
    maybe = lambda: (pick(pool.pedals),) if rng.random() < 0.75 else ()
    if topology == "single":
        combo = Combo(maybe(), pick(pool.amps), None, None, pick(pool.cabs))
    elif topology == "single2":
        p1 = pick(pool.pedals)
        combo = Combo((p1, pick([p for p in pool.pedals if p.key != p1.key])), pick(pool.amps), None, None,
                      pick(pool.cabs))
    else:
        combo = Combo(maybe(), pick(pool.amps), maybe(), pick(pool.amps), pick(pool.cabs))
    sp = Space.for_combo(combo)
    u = np.clip(0.5 + 0.18 * rng.standard_normal(len(sp)), 0.05, 0.95)   # moderate, in-range values
    v = sp.decode(u)
    if "blend" in v:
        v["blend"] = float(rng.uniform(0.35, 0.65))
    align = engine.probe_align(combo, v)
    return combo, v, build_preset(combo, v, gate=gate, align=align, name="hidden"), align


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="sawblade-match-known-answer")
    p.add_argument("--pool", required=True)
    p.add_argument("--di", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--section-s", type=float, default=40.0, help="DI section length used for the whole test")
    p.add_argument("--budget", type=float, default=1.0)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--topology", choices=["single", "single2", "blend"], default="blend")
    a = p.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    log = Log()
    pool = load_pool(a.pool)
    x, fs = sf.read(a.di, dtype="float32")
    x = x if x.ndim == 1 else x[:, 0]
    s0, s1, _ = select_excerpt(x, fs, a.section_s, stride_s=1.0)
    di = out / "di_section.wav"
    sf.write(str(di), x[s0:s1], fs, subtype="FLOAT")
    gate = gate_preset(gate_envelope_floor_db(to48(x[s0:s1], fs), RATE))
    eng = Engine(gate, a.threads)
    combo, v, preset, align = hidden_preset(pool, a.seed, gate, eng, a.topology)
    log(f"hidden: {json.dumps(caps_summary(combo))[:600]}")
    y, _ = eng.render(preset, x[s0:s1], fs)
    eng.close()
    ref_path = out / "hidden_render.wav"
    sf.write(str(ref_path), y, fs, subtype="FLOAT")
    (out / "hidden.preset.resolved.json").write_text(json.dumps(preset, indent=2))
    ref = load_reference(ref_path, channel="mid", matched="mono", offset_ms=0.0)
    cfg = Config(di=di, ref=ref, pool=pool, out=out / "match", budget=a.budget, seed=a.seed, threads=a.threads,
                 write_audio=False, refine_offsets=False)
    res = run_match(cfg, log)
    err = res["after"][0]["aWeightedErrorDb"]
    found = res["best"]["captures"]
    same = {k: (found.get(k) and found[k]["modelId"]) == (c.model_id if c else None)
            for k, c in combo.captures().items()}
    report = {"hiddenTopology": combo.topology, "foundTopology": res["best"]["topology"], "hiddenCaptures": caps_summary(combo), "hiddenParams": v, "foundCaptures": found,
              "sameCaptureBySlot": same, "foundAWeightedErrorDb": err, "beforeStarterAWeightedErrorDb":
              res["before"][0]["aWeightedErrorDb"], "toleranceDb": TOLERANCE_DB, "pass": err <= TOLERANCE_DB,
              "seed": a.seed, "section": [s0 / fs, s1 / fs], "wallSeconds": res["wallSeconds"]}
    (out / "known_answer.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: report[k] for k in ("sameCaptureBySlot", "foundAWeightedErrorDb", "toleranceDb", "pass")}, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
