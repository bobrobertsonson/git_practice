"""Matcher pipeline: stage 1 screening, offset refinement, stage 2 CMA-ES, stage 3 verification + outputs."""
from __future__ import annotations

import copy
import json
import subprocess
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import soundfile as sf

from ..tonecheck.cli import check_audio, format_table
from ..tonecheck.analysis import analyze
from ..tonecheck.rules import evaluate_rules, load_targets
from . import loss as L
from .engine import RATE, Engine, to48
from .offset import refine_offset
from .pool import Capture, Pool, default_cab, starter_choice
from .profile import DEFAULT_BASE, derive_profile, load_profile, profile_path, save_profile
from .reference import Reference, build_target, make_excerpt
from .refine import refine_combo
from .screen import Scored, Screener, TOPOLOGIES
from .space import TOPOLOGY_RANK, Combo, Space, build_preset, gate_preset, manual_align

CLIP_PEAK = 1.0           # linear full scale; a candidate whose matched-level output exceeds it is "clipping"
CLIP_GUARD_DBFS = -1.0    # the final output gain is lowered until the full-length peak is below this
OCCAM_DB = 0.1            # prefer the simplest topology within this much total loss
SIZE_TIE_DB = 0.05        # prefer the lighter model set (size category) within this much total loss


@dataclass
class Plan:
    """Work plan scaled by ``--budget`` (1.0 = default). Counts, not wall-clock, so results are deterministic.
    The default renders every (pedal-or-none, amp) pair of the current pool (19 x 37 = 703) when it fits ``cap_pairs``;
    bigger pools are trimmed by the per-capture pre-screen first (prescreen.py), then by seeded random sampling."""
    cap_pairs: int
    n_rescore: int            # blend candidates re-scored with the full loss
    n_rescore_single: int     # single / single2 candidates re-scored
    n_cab: int                # blend candidates that get a cab sweep
    n_cab_single: int
    top_k: dict               # CMA-ES refined combos per topology
    gens_linear: int
    gens_gain: int
    gens_final: int
    pop_linear: int = 16
    pop_gain: int = 8
    prescreen_n: int | None = None     # None: automatic (only when the pair product exceeds cap_pairs)
    prescreen_n2: int = 4              # per-class pre-screen N used for the single2 pedal/amp subset
    n2_pedals: int = 6
    n2_amps: int = 8

    @staticmethod
    def from_budget(budget: float, top_k: int = 3, prescreen_n: int | None = None) -> "Plan":
        b = max(budget, 0.01)
        g = lambda n, lo: max(lo, int(round(n * min(b, 3.0))))
        return Plan(cap_pairs=g(800, 6), n_rescore=g(60, 3), n_rescore_single=g(40, 3), n_cab=g(15, 2),
                    n_cab_single=g(8, 2),
                    top_k={"blend": top_k, "single": min(top_k, 2), "single2": min(top_k, 1)},
                    gens_linear=g(40, 3), gens_gain=g(8, 2), gens_final=g(20, 2), prescreen_n=prescreen_n,
                    n2_pedals=g(6, 2), n2_amps=g(8, 2))


@dataclass
class Config:
    di: Path
    ref: Reference
    pool: Pool
    out: Path
    di_r: Path | None = None
    budget: float = 1.0
    seed: int = 0
    excerpt_s: float = 6.0
    top_k: int = 3
    prescreen_n: int | None = None
    profile: str = "derived"
    base_profile: str = DEFAULT_BASE
    threads: int = 4
    targets: Path | None = None
    window_s: tuple[float, float] | None = None
    write_audio: bool = True
    refine_offsets: bool = True
    plan: Plan | None = None


class Log:
    def __init__(self, path: Path | None = None):
        self.t0, self.path, self.lines = time.time(), path, []

    def __call__(self, msg: str):
        line = f"[{time.time() - self.t0:7.1f}s] {msg}"
        self.lines.append(line)
        print(line, flush=True)


def gate_envelope_floor_db(x: np.ndarray, fs: int) -> float:
    """DI noise floor as the gate sees it: 5th percentile of the mean level (dB) of 20 ms frames of the gate's peak
    envelope detector (peak follower, 0.1 ms attack / 10 ms release; docs/PRESET_SCHEMA.md). Analysis only."""
    a_c = float(np.exp(-1.0 / (0.0001 * fs)))
    r_c = float(np.exp(-1.0 / (0.010 * fs)))
    ax = np.abs(np.asarray(x, dtype=np.float64))
    env = np.empty_like(ax)
    e = 0.0
    for i, v in enumerate(ax.tolist()):
        e = a_c * e + (1 - a_c) * v if v > e else r_c * e + (1 - r_c) * v
        env[i] = e
    n = int(round(0.020 * fs))
    nf = len(env) // n
    lvl = 20 * np.log10(np.maximum(env[: nf * n].reshape(nf, n).mean(axis=1), 1e-10))
    return float(np.percentile(lvl, 5))


def _sha(p) -> str:
    import hashlib
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def portable(preset: dict) -> dict:
    """Same preset with the machine-specific absolute capture paths replaced by cache-relative placeholders."""
    p = copy.deepcopy(preset)

    def walk(n):
        if isinstance(n, dict):
            src = n.get("source")
            if isinstance(src, dict) and "file" in n:
                ext = Path(n["file"]).suffix
                n["file"] = f"captures/{src['id']}_{src.get('modelId', 'x')}{ext}"
                n.pop("sha256", None)
            for v in n.values():
                walk(v)
        elif isinstance(n, list):
            for v in n:
                walk(v)
    walk(p)
    return p


def starter_preset(pool: Pool, gate: dict) -> tuple[dict, dict]:
    """Generic starter baseline: first amp + first cab, no pedals, no EQ, the matcher's fixed gate (the 'before' of the
    before/after numbers and the render used for the first offset refinement)."""
    ch = starter_choice(pool)
    combo = Combo((), ch["amp"], None, None, ch["cab"])
    p = build_preset(combo, Space.for_combo(combo).default(), gate=gate, align=manual_align(),
                     name="Generic starter baseline (first amp + first cab)")
    p["paths"]["a"]["eq"] = []
    p["postEq"] = []
    return p, {"kind": "generic starter baseline", **{k: (v.key, v.title, v.name) for k, v in ch.items()}}


def caps_summary(combo: Combo) -> dict:
    out = {}
    for slot, c in combo.captures().items():
        out[slot] = None if c is None else {"toneId": c.tone_id, "modelId": c.model_id, "title": c.title,
                                            "model": c.name, "class": c.kind, "license": c.license,
                                            "creator": c.creator, "sizeBytes": c.size_bytes,
                                            "sizeCategory": c.size_label or "standard", "architecture": c.arch}
    return out


def pick_output_gain(y_peak: float, offset_db: float, level_offset_db: float) -> tuple[float, bool]:
    """Output gain that puts the rendered guitar at the reference's per-guitar level; flags clipping at that level."""
    g = -offset_db + level_offset_db
    clipped = y_peak * 10 ** (g / 20) > CLIP_PEAK
    return float(g), bool(clipped)


def choose(cands: list[Scored]) -> Scored:
    """Selection (spec 3.3): lowest loss among finite, non-clipping candidates; within OCCAM_DB of it the simplest
    topology (single < single2 < blend); within that topology, within SIZE_TIE_DB of *that topology's best* the lighter
    model set by size category (manifest size / name label, then 10 % byte buckets); equal categories -> lower loss.
    The size window is relative to the best candidate of the chosen topology, not of the whole field, so the result can be
    at most OCCAM_DB + SIZE_TIE_DB = 0.15 dB above the global best."""
    cands = [c for c in cands if np.isfinite(c.loss)]
    if not cands:
        raise ValueError("no candidate with a finite loss")
    ok = [c for c in cands if not c.extra.get("clipped")] or cands
    best = min(ok, key=lambda c: c.loss)
    near = [c for c in ok if c.loss <= best.loss + OCCAM_DB]
    rank = min(TOPOLOGY_RANK[c.topology] for c in near)
    same = [c for c in near if TOPOLOGY_RANK[c.topology] == rank]
    top = min(c.loss for c in same)
    tie = [c for c in same if c.loss <= top + SIZE_TIE_DB]
    return min(tie, key=lambda c: (c.combo.size_rank()[0], c.combo.size_rank()[1], c.loss))


def encode_mp3(wav: Path, mp3: Path, log) -> bool:
    try:
        import lameenc
        x, fs = sf.read(str(wav), dtype="float32", always_2d=True)
        enc = lameenc.Encoder()
        enc.set_bit_rate(192); enc.set_in_sample_rate(int(fs)); enc.set_channels(x.shape[1]); enc.set_quality(2)
        pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
        mp3.write_bytes(bytes(enc.encode(pcm.tobytes()) + enc.flush()))
        return True
    except ImportError:
        pass
    try:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-b:a", "192k", str(mp3)], check=True)
        return True
    except (OSError, subprocess.CalledProcessError) as e:
        log(f"warning: MP3 encoding unavailable ({e}); WAV only")
        return False


HINT_WINDOW_MS = 20.0   # an explicit --offset-ms is trusted: both refinements search only +-20 ms around it


def _refine_native(render: np.ndarray, fs: int, ref_path: str, col: int, coarse_ms: float, search_ms: float = 250.0):
    x, rfs = sf.read(ref_path, dtype="float64", always_2d=True)
    if rfs != fs:
        raise ValueError("render and reference rates differ")
    ch = x[:, min(col, x.shape[1] - 1)]
    r = refine_offset(render.astype(np.float64), ch, fs, int(round(coarse_ms * fs / 1000)), start=0,
                      search=int(search_ms * fs / 1000))
    r["offsetMs"] = 1000.0 * r["offset"] / fs
    r["offsetSamples"] = r["offset"]
    r["rate"] = fs
    return r


def run_match(cfg: Config, log=None) -> dict:
    log = log or Log()
    t_start = time.time()
    out = Path(cfg.out)
    out.mkdir(parents=True, exist_ok=True)
    plan = cfg.plan or Plan.from_budget(cfg.budget, cfg.top_k, cfg.prescreen_n)
    rng = np.random.default_rng(cfg.seed)
    ref, pool = cfg.ref, cfg.pool
    if not (pool.amps and pool.cabs):
        raise ValueError(f"pool needs amps and cabs, got {pool.counts()}")
    log(f"pool {pool.counts()} seed {cfg.seed} budget {cfg.budget} plan {plan}")

    di_x, di_fs = sf.read(str(cfg.di), dtype="float32")
    if di_x.ndim > 1:
        di_x = di_x[:, 0]
    di48 = to48(di_x, di_fs)
    floor = gate_envelope_floor_db(di48, RATE)
    gate = gate_preset(floor)
    log(f"DI floor on the gate's peak envelope {floor:.1f} dBFS -> gate {gate}")
    eng = Engine(gate, cfg.threads)
    try:
        return _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start)
    finally:
        eng.close()


def _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start):
    cab0 = default_cab(pool.cabs)
    window = None if cfg.window_s is None else (int(cfg.window_s[0] * RATE), int(cfg.window_s[1] * RATE))
    ex = make_excerpt(di48, cfg.excerpt_s, window=window, ref=ref)
    log(f"excerpt {ex.start / RATE:.1f}-{ex.end / RATE:.1f} s ({ex.info})")
    for note in ref.notes:
        log(f"reference note: {note}")
    result: dict = {"schema": "sawblade.match_result", "version": 1, "seed": cfg.seed, "budget": cfg.budget,
                    "plan": plan.__dict__, "di": str(cfg.di), "diR": str(cfg.di_r) if cfg.di_r else None,
                    "reference": {"path": ref.path, "basis": ref.basis, "stemChannel": ref.stem_channel,
                                  "bandLimitHz": ref.hf_limit_hz, "textureTerm": ref.texture,
                                  "matchedStftFmaxHz": ref.matched_fmax if ref.matched_sig is not None else None,
                                  "matched": ref.matched_channel,
                                  "sections": ref.sections, "notes": ref.notes},
                    "excerpt": {"startS": ex.start / RATE, "endS": ex.end / RATE, **ex.info},
                    "gate": gate, "diNoiseFloorDb": floor, "poolCounts": pool.counts(),
                    "lossWeights": {"texFlat": L.W_FLAT, "texHf": L.W_HF, "ltas": L.W_LTAS, "buzz": L.W_BUZZ, "decay": L.W_DECAY, "stft": L.W_STFT,
                                    "reg": L.W_REG},
                    "randomness": f"numpy default_rng(seed={cfg.seed}) for subset sampling and CMA-ES (seed + block)"}

    # ---- starter ("before") on the excerpt, which also gives the coarse offset refinement its render -----------------
    starter_p, starter_caps = starter_preset(pool, gate)
    result["starter"] = {"label": "generic starter baseline", "captures": starter_caps}
    y_st, rep_st = eng.render(starter_p, ex.x)

    hint_samples = ref.offset_samples if ref.offset_given else None

    def offset_from(y_full_excerpt, label):
        # unknown offset: +-3 s, then refined; given: only +-HINT_WINDOW_MS around the hint
        search = int(HINT_WINDOW_MS * RATE / 1000) if ref.offset_given else int(3.0 * RATE)
        return refine_offset(ex.trim(y_full_excerpt), ref.matched_sig.astype(np.float64), RATE, ref.offset_samples,
                             start=ex.start, search=search)

    if ref.matched_sig is not None and cfg.refine_offsets:
        r = offset_from(y_st, "starter")
        result["offsetRefinement"] = {"coarseMs": 1000 * ref.offset_samples / RATE,
                                      "excerptStarterRender": {**r, "offsetMs": 1000 * r["offset"] / RATE}}
        log(f"offset refinement (starter render, excerpt): {r['offset']} samples @48k = {1000 * r['offset'] / RATE:.2f} ms "
            f"[{r['method']}, env ratio {r['envPeakRatio']:.1f}, fine ratio {r.get('peakRatio', 0):.1f}]")
        if r["envPeakRatio"] >= 2.0:
            ref.offset_samples = r["offset"]
    tgt = build_target(ref, ex)
    # ---- profile: guardrail rules (the reference LTAS stays the target) ----------------------------------------------
    base = load_profile(cfg.targets and str(cfg.targets) or cfg.base_profile)
    if cfg.profile == "derived":
        profile, ptable = derive_profile(base, ref.ltas_sig, ref.basis, ref.name)
        save_profile(profile, out / "profile.derived.json")
        result["profile"] = {"id": profile["id"], "kind": "derived", "base": base.get("id", "docs/tone_targets.json"),
                             "changes": ptable, "file": "profile.derived.json"}
        log("profile: derived from the reference; rules changed vs base: " +
            (", ".join(f"{r['id']} ({r['baseExpr']} -> {r['derivedExpr']})" for r in ptable
                       if r["derivedExpr"] != r["baseExpr"]) or "none"))
    else:
        profile = load_profile(cfg.profile)
        result["profile"] = {"id": profile.get("id", cfg.profile), "kind": "fixed"}
    result["profile"]["baseRulesStatusOnReference"] = {r["id"]: r["statusOnReference"] for r in ptable} \
        if cfg.profile == "derived" else None
    result["referenceTarget"] = {"buzzDb": tgt.ref.buzz_db, "lowDecayDbPerMs": tgt.ref.decay,
                                 "onsetsMeasured": tgt.ref.n_onsets}
    before_ex = L.evaluate(ex.trim(y_st), tgt, None)
    result["starter"]["excerptLoss"] = before_ex.as_dict()
    log(f"starter on excerpt: loss {before_ex.total:.3f} (ltas {before_ex.ltas:.2f} dB)")

    # ---- stage 1 -----------------------------------------------------------------------------------------------------
    scr = Screener(eng, pool, ex, tgt, cab0, rng, plan, log)
    ranked = scr.run()
    result["stage1"] = {**scr.stats, "top": {t: [_scored_json(s) for s in lst[:30]] for t, lst in ranked.items()}}
    t1 = time.time() - t_start

    # ---- stage 2 -----------------------------------------------------------------------------------------------------
    refined: list[Scored] = []
    unrefined: list[Scored] = []
    for topo in TOPOLOGIES:
        lst = ranked.get(topo, [])
        kk = plan.top_k.get(topo, 0)
        unrefined += lst[kk:kk + 2]
        for k, s in enumerate(lst[:kk]):
            log(f"stage2 {topo} [{k + 1}/{kk}] {s.combo.describe()} (screen loss {s.loss:.3f})")
            sp = Space.for_combo(s.combo)
            v0 = sp.default()
            if "blend" in v0:
                v0["blend"] = s.blend
            v, r, info = refine_combo(eng, s.combo, sp, ex, tgt, s.align, v0, seed=cfg.seed * 1000 + len(refined) * 10,
                                      gens_linear=plan.gens_linear, pop_linear=plan.pop_linear,
                                      gens_gain=plan.gens_gain, pop_gain=plan.pop_gain, gens_final=plan.gens_final,
                                      log=log)
            ca = eng.core(s.combo, v, "a", ex.x)
            cb = eng.core(s.combo, v, "b", ex.x) if s.combo.topology == "blend" else None
            y = ex.trim(eng.emulate(s.combo, v, ca, cb, s.align))
            g, clipped = pick_output_gain(float(np.max(np.abs(y))), r.offset_db, ref.level_offset_db)
            guard = _guardrails(y, profile)
            refined.append(Scored(s.combo, r.total, v.get("blend", 0.0), s.align, r, "refined",
                                  {"params": v, "outputGainDb": g, "clipped": clipped, "info": info,
                                   "guardrails": guard}))
    unrefined = sorted([c for c in unrefined if np.isfinite(c.loss)], key=lambda c: c.loss)
    refined = [c for c in refined if np.isfinite(c.loss)]
    refined.sort(key=lambda c: c.loss)
    best = choose(refined)
    result["stage2Seconds"] = time.time() - t_start - t1
    result["topologies"] = {}
    for topo in TOPOLOGIES:
        c = [x for x in refined if x.topology == topo]
        if c:
            b = min(c, key=lambda x: x.loss)
            result["topologies"][topo] = {"loss": b.loss, "captures": caps_summary(b.combo), "blend": b.blend,
                                          "breakdown": b.result.as_dict(), "guardrails": b.extra.get("guardrails")}
    log("topology bests: " + ", ".join(f"{t} {d['loss']:.3f}" for t, d in result["topologies"].items()))
    log(f"selected: {best.combo.describe()} loss {best.loss:.3f}")

    # ---- stage 3: full-length verification ----------------------------------------------------------------------
    v = best.extra["params"]
    gain_db = best.extra["outputGainDb"]
    final = build_preset(best.combo, v, gate=gate, align=best.align, output_db=gain_db,
                         name="Sawblade match", notes=_notes(cfg, ref, best))
    full_jobs = {"best_L": (final, cfg.di), "starter_L": (starter_p, cfg.di)}
    if cfg.di_r is not None:
        full_jobs["best_R"] = (final, cfg.di_r)
    log(f"stage3: full-length renders {list(full_jobs)}")

    def full_render(item):
        name, (preset, path) = item
        x, fs = sf.read(str(path), dtype="float32")
        x = x if x.ndim == 1 else x[:, 0]
        y, rep = eng.render(preset, x, fs)
        return name, y, fs, rep

    renders = {n: (y, fs, rep) for n, y, fs, rep in eng.map(full_render, list(full_jobs.items()))}
    peaks = {n: float(np.max(np.abs(renders[n][0]))) for n in renders if n.startswith("best")}
    peak = max(peaks.values())
    best.extra["fullLengthPeakDbfs"] = {n: float(20 * np.log10(max(p, 1e-12))) for n, p in peaks.items()}
    if peak >= 10 ** (CLIP_GUARD_DBFS / 20):
        cut = 20 * np.log10(10 ** (CLIP_GUARD_DBFS / 20) / peak)
        gain_db += cut
        final["output"]["gainDb"] = float(gain_db)
        for n in peaks:
            y, fs, rep = renders[n]
            renders[n] = ((y * 10 ** (cut / 20)).astype(np.float32), fs, rep)
        log(f"clip guard: full-length peak (max of L/R) {20 * np.log10(peak):.1f} dBFS -> output gain lowered by {-cut:.1f} dB")
        best.extra["clipGuardDb"] = float(cut)
    best.extra["fullLengthPeakAfterGuardDbfs"] = {n: float(20 * np.log10(max(float(np.max(np.abs(renders[n][0]))), 1e-12)))
                                                  for n in peaks}
    result["clipped"] = bool(max(float(np.max(np.abs(renders[n][0]))) for n in peaks) >= 1.0)

    targets = profile
    targets_path = out / "profile.derived.json" if cfg.profile == "derived" else profile_path(cfg.profile)
    v2_rules = base["rules"]
    tc_dir = out / "tonecheck"
    refs = _tonecheck_refs(ref, out, cfg)
    reports = {}
    for name in ("best_L", "starter_L"):
        y, fs, rep = renders[name]
        wav = out / f"render_{name}.wav"
        sf.write(str(wav), y, fs, subtype="FLOAT")
        preset_used = final if name == "best_L" else starter_p
        reports[name] = check_audio(name, wav, cfg.di, refs, targets, tc_dir / name, rep, None, targets_path)
        (tc_dir / f"{name}.txt").write_text(format_table(reports[name]) + "\n")
    for name, rep in reports.items():
        print(format_table(rep) + "\n")
    result["tonecheck"] = {n: {"summary": r["summary"], "rules": r["rules"],
                               "aWeightedErrorVsRefs": [{"ref": x["path"], "channel": x["channel"],
                                                         "aWeightedErrorDb": x["aWeightedErrorDb"],
                                                         "meanDiffDb": x["meanDiffDb"]} for x in r["references"]],
                               "metrics": r["metrics"],
                               "baseProfileRules": [{"id": q["id"], "status": q["status"], "margin": q["margin"]}
                                                    for q in evaluate_rules(r["groupsDb"], v2_rules)]}
                           for n, r in reports.items()}
    result["before"] = _err_summary(reports["starter_L"])
    result["after"] = _err_summary(reports["best_L"])
    log(f"A-weighted error vs {Path(refs[0][0]).name}: before {result['before'][0]['aWeightedErrorDb']:.2f} dB, "
        f"after {result['after'][0]['aWeightedErrorDb']:.2f} dB")

    # final offsets (+-1 ms) on the full-length renders
    if ref.matched_sig is not None:
        fin = {}
        try:
            y, fs, _ = renders["best_L"]
            col = 0 if ref.matched_channel in ("left", "mono") else 1
            if hint_samples is not None:      # the user's hint bounds the final search too (+-HINT_WINDOW_MS)
                hint_ms, win = 1000.0 * hint_samples / RATE, HINT_WINDOW_MS
            else:
                hint_ms, win = 1000.0 * ref.offset_samples / RATE, 250.0
            fin["L"] = _refine_native(y, fs, ref.path, col, hint_ms, win)
            if "best_R" in renders:
                fin["R"] = _refine_native(renders["best_R"][0], renders["best_R"][1], ref.path, 1,
                                          hint_ms if hint_samples is not None else fin["L"]["offsetMs"], win)
            log("final offsets (full-length renders vs mix): " +
                ", ".join(f"{k} {v['offsetSamples']} smp = {v['offsetMs']:.2f} ms ({v['method']}, fine ratio "
                          f"{v.get('peakRatio', 0):.1f})" for k, v in fin.items()))
        except Exception as e:  # reporting only
            fin["error"] = str(e)
        result.setdefault("offsetRefinement", {})["final"] = fin

    # ---- outputs -------------------------------------------------------------------------------------------------------
    resolved = out / "best.preset.resolved.json"
    resolved.write_text(json.dumps(final, indent=2) + "\n")
    (out / "best.preset.json").write_text(json.dumps(portable(final), indent=2) + "\n")
    alts = []
    for i, s in enumerate([c for c in refined if c is not best] + unrefined, 1):
        if i > 5:
            break
        if s.stage == "refined":
            v_alt, gdb = s.extra["params"], s.extra["outputGainDb"]
        else:
            v_alt, gdb = Space.for_combo(s.combo).default(), 0.0
            if "blend" in v_alt:
                v_alt["blend"] = s.blend
        preset = build_preset(s.combo, v_alt, gate=gate, align=s.align, output_db=gdb, name=f"Sawblade match alt {i}")
        (out / f"alt{i}.preset.resolved.json").write_text(json.dumps(preset, indent=2) + "\n")
        alts.append({**_scored_json(s), "file": f"alt{i}.preset.resolved.json"})
    result["alternatives"] = alts
    result["best"] = {**_scored_json(best), "params": v, "outputGainDb": gain_db, "preset": "best.preset.resolved.json"}
    result["best"]["captures"] = caps_summary(best.combo)
    result["best"]["fullLengthPeakDbfs"] = best.extra["fullLengthPeakDbfs"]
    result["best"]["fullLengthPeakAfterGuardDbfs"] = best.extra["fullLengthPeakAfterGuardDbfs"]
    result["best"]["clipGuardDb"] = best.extra.get("clipGuardDb", 0.0)
    result["candidatesStage2"] = [_scored_json(c) for c in refined]
    result["listening"] = {}
    if cfg.write_audio:
        result["listening"] = _listening(out, renders, cfg, log)
    result["engine"] = {"renders": eng.n_renders, "renderedAudioSeconds": eng.render_audio_s,
                        "threads": cfg.threads}
    result["wallSeconds"] = time.time() - t_start
    result["createdUtc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    (out / "result.json").write_text(json.dumps(result, indent=2, default=_json_default) + "\n")
    log(f"done in {result['wallSeconds'] / 60:.1f} min; results in {out}")
    return result


def _json_default(o):
    if isinstance(o, (np.floating, np.integer)):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    return str(o)


def _names(c: Combo) -> str:
    return c.describe()


def _guardrails(y: np.ndarray, profile: dict) -> dict:
    """Profile rules evaluated on an excerpt render (excerpt-level analysis, reported per candidate)."""
    try:
        g = analyze(y.astype(np.float64), RATE, profile).groups
        rows = evaluate_rules(g, profile["rules"])
        return {"fail": [r["id"] for r in rows if r["status"] == "fail"],
                "marginal": [r["id"] for r in rows if r["status"] == "marginal"]}
    except ValueError:
        return {"fail": None, "marginal": None}


def _scored_json(s: Scored) -> dict:
    d = {"stage": s.stage, "topology": s.topology, "loss": s.loss, "blend": s.blend, "align": s.align,
         "captures": caps_summary(s.combo), "modelBytes": s.combo.model_bytes(),
         "sizeRank": {"category": s.combo.size_rank()[0], "byteBucket": s.combo.size_rank()[1]}}
    if "guardrails" in s.extra:
        d["guardrails"] = s.extra["guardrails"]
    if s.result is not None:
        d["breakdown"] = s.result.as_dict()
    if "clipped" in s.extra:
        d["clippedAtMatchedLevel"] = s.extra["clipped"]
    return d


def _notes(cfg, ref, best) -> str:
    return (f"Matched by sawblade-match seed {cfg.seed} against {Path(ref.path).name} ({ref.basis}). "
            "Captures are referenced by TONE3000 ids; exported/derived models are for the user's own use only.")


def _err_summary(rep) -> list[dict]:
    return [{"ref": r["path"], "channel": r["channel"], "aWeightedErrorDb": r["aWeightedErrorDb"],
             "unweightedRmsErrorDb": r["unweightedRmsErrorDb"], "meanDiffDb": r["meanDiffDb"]}
            for r in rep["references"]]


def _tonecheck_refs(ref: Reference, out: Path, cfg) -> list[tuple[Path, str]]:
    basis = out / "ref_basis.wav"
    sf.write(str(basis), ref.ltas_sig.astype(np.float32), RATE, subtype="FLOAT")
    refs = [(basis, "left")]
    if ref.matched_sig is not None and ref.matched_channel in ("left", "right"):
        refs.append((Path(ref.path), ref.matched_channel))
    return refs


def _listening(out: Path, renders: dict, cfg, log) -> dict:
    d = out / "listen"
    d.mkdir(exist_ok=True)
    yl, fs, _ = renders["best_L"]
    info = {}
    if "best_R" in renders:
        yr = renders["best_R"][0]
        n = min(len(yl), len(yr))
        st = np.stack([yl[:n], yr[:n]], axis=1)
        name = "cover_guitars_L-R"
    else:
        st = np.stack([yl, yl], axis=1)
        name = "guitar_L_mono"
    peak = float(np.max(np.abs(st)))
    gain = 10 ** (-1.0 / 20) / max(peak, 1e-9)
    info["normalisationGainDb"] = float(20 * np.log10(gain))
    st = (st * gain).astype(np.float32)
    sf.write(str(d / f"{name}.wav"), st, fs, subtype="PCM_24")
    info["wav"] = str(d / f"{name}.wav")
    if encode_mp3(d / f"{name}.wav", d / f"{name}.mp3", log):
        info["mp3"] = str(d / f"{name}.mp3")
    return info


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _targets_path() -> Path:
    return _repo_root() / "docs" / "tone_targets.json"
