"""Known-answer recovery (spec 3.2 acceptance a) with real pool captures.

A hidden preset is built from pool captures (seeded: combo + continuous values), the DI section is rendered through it
(real chain, 48 kHz-internal), and that render is the reference of a matched pair (offset 0). The matcher must find a
preset whose A-weighted LTAS error against the reference (tonecheck definition, whole section) is within 0.5 dB of the
hidden preset's own error (0 by construction). Run as ``python -m sawblade_match.matcher.known_answer --pool ... --di ...``.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from ..core import render
from . import feel as F
from . import loss as L
from .calibration import pick_di_channel
from .engine import Engine, RATE, to48
from .excerpt import select_excerpt
from .gatesweep import cell_gate
from .refine import DISCRETE_UP, HP_GRID
from .pool import default_cab, load_pool
from .reference import build_target, load_reference, make_excerpt
from .run import Config, Log, Plan, caps_summary, gate_envelope_floor_db, run_match
from .space import Combo, Space, build_preset, gate_preset

TOLERANCE_DB = 0.5
# D.1 feel pass criteria (whole section): A-weighted LTAS <= 0.5 dB; tightness median |dt12| <= 10 ms, |dsustain| <= 1.5 dB;
# fizz W1(hfRatioDb) <= 1.0 dB, W1(hfFlat) <= 0.02; flux W1 <= 0.3 dB; floor |d| <= 3 dB.
FEEL_TOLERANCES = {"aWeightedErrorDb": 0.5, "t12Ms": 10.0, "sustainDb": 1.5, "hfRatioDb": 1.0, "hfFlat": 0.02,
                   "fluxDb": 0.3, "floorDb": 3.0}
WEIGHT_SETS = {"current 0.25/0.25/0.125": (0.25, 0.25, 0.125), "double 0.5/0.5/0.25": (0.5, 0.5, 0.25),
               "half 0.125/0.125/0.0625": (0.125, 0.125, 0.0625)}


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
    sp = Space.for_combo(combo, filters=False, preeq=False)     # the hidden chain keeps its pre-v0.4M draws
    u = np.clip(0.5 + 0.18 * rng.standard_normal(len(sp)), 0.05, 0.95)   # moderate, in-range values
    v = sp.decode(u)
    if "blend" in v:
        v["blend"] = float(rng.uniform(0.35, 0.65))
    align = engine.probe_align(combo, v)
    return combo, v, build_preset(combo, v, gate=gate, align=align, name="hidden"), align


# ---- D.1 feel case: hidden chain with tight boost + 24 dB/oct post filters + gate, DI with gaps and a -70 dBFS floor -------------
def gap_di(seconds: float = 8.0, seed: int = 3, floor_db: float = -70.0) -> np.ndarray:
    """Synthetic DI (48 kHz float32, nothing committed): palm-muted chugs on low E with rests, one ring-out, and a white
    noise floor at ``floor_db`` dBFS RMS so the gaps are real (the gate and the feel ``floor`` term need them)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    x = np.zeros(n)
    i, bar = 0, 0
    while i < n - RATE:
        if bar % 4 < 3:
            for k in range(4):
                a, m = i + k * int(0.15 * RATE), int(0.14 * RATE)
                tt = np.arange(m) / RATE
                f0 = 82.4 * (1.0 if k % 2 == 0 else 1.122)
                note = sum(np.sin(2 * np.pi * f0 * h * tt + rng.uniform(0, 6)) / h for h in range(1, 12)) * np.exp(-tt / 0.04)
                x[a:a + m] += 0.15 * note
            i += int(0.8 * RATE)
        else:
            m = int(1.2 * RATE)
            tt = np.arange(m) / RATE
            x[i:i + m] += 0.05 * np.sin(2 * np.pi * 82.4 * tt) * np.exp(-tt / 0.35)
            i += int(1.4 * RATE)
        bar += 1
    x += rng.standard_normal(n) * 10 ** (floor_db / 20)
    return x.astype(np.float32)


def feel_hidden(pool, di48: np.ndarray, seed: int = 1):
    """Hidden chain of D.1, reachable by construction: every discrete parameter sits on the matcher's own grid (post.hp on
    ``refine.HP_GRID`` at 24 dB/oct, the post low-pass at 24 dB/oct, the gate on a cell of the gate sweep grid), continuous
    ones are inside their ranges. [tight boost] -> amp -> cab. Returns (combo, values, preset, gate)."""
    rng = np.random.default_rng(seed)
    combo = Combo((), pool.amps[int(rng.integers(len(pool.amps)))], None, None, pool.cabs[int(rng.integers(len(pool.cabs)))],
                  boost=True)
    sp = Space.for_combo(combo)
    v = sp.default()
    v.update({"boost.drive": 2.0, "boost.level": 8.0, "boost.tone": 5.5,
              "post.hp": HP_GRID[2], "post.hp_slope": DISCRETE_UP, "post.lp": 7500.0, "post.lp_slope": DISCRETE_UP})
    # gate cell: threshold = DI peak floor + 16 dB (the default is + 10), hold 10 ms, release 80 ms, range -50 dB (all on the sweep grids, not the default)
    gate = cell_gate(gate_envelope_floor_db(di48, RATE), 16.0, release_ms=80.0, hold_ms=10.0, range_db=-50.0)
    return combo, v, build_preset(combo, v, gate=gate, align=Engine(gate).probe_align(combo, v), name="hidden feel case"), gate


@contextlib.contextmanager
def feel_weights(w: tuple[float, float, float] | None):
    """Temporarily set the feel weights (tight, fizz, polish); None keeps them."""
    if w is None:
        yield
        return
    old = (F.W_TIGHT, F.W_FIZZ, F.W_POLISH)
    F.W_TIGHT, F.W_FIZZ, F.W_POLISH = w
    try:
        yield
    finally:
        F.W_TIGHT, F.W_FIZZ, F.W_POLISH = old


def feel_deltas(found: np.ndarray, ft: "F.FeelTarget") -> dict:
    """Physical differences between a found render and the reference (inside ``ft``, paired mode), whole section:
    median |dt12| (ms) and |dsustain| (dB) over the DI-selected notes, W1 of hfRatioDb (dB) and hfFlat, flux W1 (dB), floor
    difference (dB). None where a feature could not be measured."""
    out = {"t12Ms": None, "sustainDb": None, "hfRatioDb": None, "hfFlat": None, "fluxDb": None, "floorDb": None}
    m = F.measure(np.asarray(found, np.float64), ft.plan, notes=ft.sel is not None, fizz=ft.fizz_on, floor=ft.plan.gap_ok)
    r = ft.ref
    if m.notes is not None and r.notes is not None and ft.sel is not None:
        idx = ft.sel[m.notes.valid[ft.sel] & r.notes.valid[ft.sel]]
        if len(idx):
            out["t12Ms"] = float(np.median(np.abs(m.notes.t12[idx] - r.notes.t12[idx])))
            out["sustainDb"] = float(np.median(np.abs(m.notes.sus[idx] - r.notes.sus[idx])))
    if m.fizz is not None and r.fizz is not None:
        out["hfRatioDb"] = F.w1(m.fizz["hfRatioDb"], r.fizz["hfRatioDb"])
        out["hfFlat"] = F.w1(m.fizz["hfFlat"], r.fizz["hfFlat"])
    if m.flux is not None and r.flux is not None:
        out["fluxDb"] = F.w1(m.flux, r.flux)
    if m.floor is not None and r.floor is not None:
        out["floorDb"] = float(abs(m.floor - r.floor))
    return out


def feel_case(pool, out: Path, *, di48: np.ndarray | None = None, seed: int = 1, plan: Plan | None = None, ablate=(),
              weights=None, threads: int = 2, excerpt_s: float = 4.0, log=None) -> dict:
    """Run the D.1 synthetic known answer once and return the row: A-weighted error, the feel deltas of the found render vs the
    hidden render on the whole section, the tolerances and which of them pass, the chosen chain.
    TODO(D.1 later): hidden irMix pair (offsetSamplesB / invertB, B2.1), fast bus comp (studio detector, B2.3), pre-EQ (B4)."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    di48 = gap_di() if di48 is None else di48
    di = out / "di.wav"
    sf.write(str(di), di48, RATE, subtype="FLOAT")
    combo, v, preset, gate = feel_hidden(pool, di48, seed)
    hidden, _ = render(preset, di48, float(RATE))
    ref_wav = out / "hidden_render.wav"
    sf.write(str(ref_wav), hidden, RATE, subtype="FLOAT")
    (out / "hidden.preset.resolved.json").write_text(json.dumps(preset, indent=2))
    ref = load_reference(ref_wav, channel="mid", matched="mono", offset_ms=0.0)
    cfg = Config(di=di, ref=ref, pool=pool, out=out / "match", seed=seed, excerpt_s=excerpt_s, threads=threads, plan=plan,
                 write_audio=False, refine_offsets=False, ablate=tuple(ablate))
    with feel_weights(weights):
        res = run_match(cfg, log or Log())
    found, _ = render(json.loads((out / "match" / "best.preset.resolved.json").read_text()), di48, float(RATE))
    ex = make_excerpt(di48, len(di48) / RATE, window=(0, len(di48)))
    tgt = build_target(ref, ex)
    ft = tgt.feel
    fb = L.features(np.asarray(found, np.float64), tgt.starts, None).band_db
    d = fb - tgt.ref.band_db
    d = d - np.sum(L.A_POWER_W * d) / np.sum(L.A_POWER_W)
    row = {"ltasResidualDbByBand": {str(c): round(float(x), 3) for c, x in zip(L.BAND_CENTRES, d)},
           "hiddenCaptures": {k: (c.key if c else None) for k, c in combo.captures().items()},
           "foundCaptures": {k: (c and f"{c['toneId']}/{c['modelId']}") for k, c in res["best"]["captures"].items()},
           "foundParams": res["best"]["params"], "hiddenParams": v, "gateSweep": {k: res["gateSweep"].get(k) for k in ("changed", "picked", "skipped")},
           "aWeightedErrorDb": res["after"][0]["aWeightedErrorDb"],
           "beforeStarterDb": (res["before"] or [{}])[0].get("aWeightedErrorDb") if res.get("before") else None,
           **(feel_deltas(found, ft) if ft is not None else {}),
           "hiddenFloorDb": None if ft is None else ft.ref.floor, "ablate": list(ablate),
           "weights": list(weights) if weights else [F.W_TIGHT, F.W_FIZZ, F.W_POLISH],
           "topology": res["best"]["topology"], "tightBoost": res["tightBoost"]["won"], "postFilters": res["postFilters"],
           "gate": res.get("gateFinal"), "hiddenGate": gate, "seed": seed}
    row["tolerances"] = FEEL_TOLERANCES
    row["passes"] = {k: (row.get(k) is not None and row[k] <= tol) for k, tol in FEEL_TOLERANCES.items()}
    row["pass"] = all(row["passes"].values())
    return row


def feel_report(pool, out: Path, *, seed: int = 1, plan: Plan | None = None, threads: int = 2, di48=None,
                weight_sets: dict | None = None, log=None) -> dict:
    """Report helper (not a test): the same case with each suspect off (--ablate feel / boost / filters / irsweep), with feel
    weights 0, and with each of ``weight_sets``; writes ``feel_report.json`` (one row per variant) and returns it."""
    out = Path(out)
    di48 = gap_di() if di48 is None else di48
    variants = {"full (as shipped)": dict(), "ablate feel": dict(ablate=("feel",)), "ablate boost": dict(ablate=("boost",)),
                "ablate filters": dict(ablate=("filters",)), "ablate irsweep": dict(ablate=("irsweep",)),
                "feel weights 0": dict(weights=(0.0, 0.0, 0.0))}
    for name, w in (weight_sets or WEIGHT_SETS).items():
        variants[f"weights {name}"] = dict(weights=w)
    table = {}
    for i, (name, kw) in enumerate(variants.items()):
        table[name] = feel_case(pool, out / f"v{i}", di48=di48, seed=seed, plan=plan, threads=threads, log=log or (lambda m: None), **kw)
    out.mkdir(parents=True, exist_ok=True)
    (out / "feel_report.json").write_text(json.dumps(table, indent=2, default=float))
    return table


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="sawblade-match-known-answer")
    p.add_argument("--pool", required=True)
    p.add_argument("--di", help="DI WAV (required unless --feel-case)")
    p.add_argument("--out", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--section-s", type=float, default=40.0, help="DI section length used for the whole test")
    p.add_argument("--budget", type=float, default=1.0)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--topology", choices=["single", "single2", "blend"], default="blend")
    p.add_argument("--feel-case", action="store_true",
                   help="D.1: hidden chain with tight boost + 24 dB/oct post filters + gate on a synthetic gappy DI "
                        "(--di is not used); prints the feel deltas against the tolerances")
    p.add_argument("--feel-report", action="store_true", help="with --feel-case: also the ablation / weight-set table")
    a = p.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    log = Log()
    pool = load_pool(a.pool)
    if a.feel_case:
        if a.feel_report:
            table = feel_report(pool, out, seed=a.seed, threads=a.threads, log=log)
            print(json.dumps({k: {m: r.get(m) for m in ("aWeightedErrorDb", "pass")} for k, r in table.items()}, indent=2))
            return 0
        row = feel_case(pool, out, seed=a.seed, threads=a.threads, log=log)
        (out / "feel_case.json").write_text(json.dumps(row, indent=2, default=float))
        print(json.dumps({k: row[k] for k in ("aWeightedErrorDb", "t12Ms", "sustainDb", "hfRatioDb", "hfFlat", "fluxDb",
                                              "floorDb", "passes", "pass")}, indent=2))
        return 0 if row["pass"] else 1
    if not a.di:
        p.error("--di is required")
    x, fs = sf.read(a.di, dtype="float32")
    x, _ = pick_di_channel(x, "auto")
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
