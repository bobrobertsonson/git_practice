"""Matcher pipeline: stage 1 screening, offset refinement, stage 2 CMA-ES, stage 3 verification + outputs."""
from __future__ import annotations

import copy
import dataclasses
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
from .offset import STRONG_MARGIN, STRONG_R1, PlacementError, refine_offset, resolve_offset
from .pool import Capture, Pool, default_cab, starter_choice
from .profile import DEFAULT_BASE, derive_profile, load_profile, profile_path, save_profile
from .excerpt import select_excerpt
from .reference import Reference, build_target, make_excerpt
from .progress import NullProgress, Progress
from .cabsweep import TOP_PER_TOPOLOGY, cab_sweep, sweep_summary
from .gatesweep import gate_sweep, reference_floor_db, render_gate
from .irblend import TOP_IRS, pair_search
from .refine import refine_combo, relinear
from .trace import trace_tones
from .screen import Scored, Screener, TOPOLOGIES
from .levelmatch import emit_gain_correction_db
from .loudness import choose_listen_section, match_gain_db, true_peak_db
from .space import TOPOLOGY_RANK, Combo, Space, build_preset, gate_preset, manual_align, post_eq, post_filters_from_eq

CLIP_PEAK = 1.0           # linear full scale; a candidate whose matched-level output exceeds it is "clipping"
CLIP_GUARD_DBFS = -1.0    # the final output gain is lowered until the full-length peak is below this
OCCAM_DB = 0.1            # prefer the simplest topology within this much total loss
SIZE_TIE_DB = 0.05        # prefer the lighter model set (size category) within this much total loss
ABLATIONS = ("feel", "boost", "filters", "irsweep", "irblend", "studio")      # --ablate names (v0.4M suspects)
CAB_SWITCH_DB = 0.01      # a different cab must lower the loss by at least this to replace the stage-2 cab
REFINE_SHARE = 0.95       # share of the refine stage's progress for stage 2; the cab / gate sweeps and the trace get the rest
NOOP_ABLATIONS = ("studio",)                                        # accepted and echoed; implemented in later tasks


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
    # ---- quick mode (spec 6b); the defaults below leave the thorough search exactly as before ----------------------------
    mode: str = "thorough"
    coarse_s: float = 0.0              # >0: two-pass screen, first pass on this many seconds of the excerpt
    coarse_keep: float = 0.10          # fraction of the pairs that get the full-length second pass
    coarse_min_keep: int = 32
    blend_aware: bool = False          # blend-aware pre-screen extras (measured: no recall gain, so off in quick)
    capped_prescreen: bool = False     # quick: the pair cap stays on (per-class quotas, pedals and amps sized separately)
    prescreen_n_ped: int = 4           # quick: per-class pedal quota (amps get the rest of the pair cap)
    patience: int | None = None        # CMA-ES plateau stop (linear blocks): generations without ``plateau_tol`` gain
    patience_gain: int | None = None   # same for the gain block (its generations are expensive)
    plateau_tol: float = 0.0
    blend_extra_ped: int = 0           # blend-aware pre-screen: extra pedals / amps per class ranked by their best blend
    blend_extra_amp: int = 0
    gain_s: float = 0.0                # >0: the gain block of CMA-ES runs on this many seconds of the excerpt
    short_linear: bool = False         # ... and so does the first linear block (the last one always uses the full excerpt)
    # ---- v0.4M suspects (always on; --ablate switches them off for the on/off pairs) -----------------------------------
    boost: bool = True                 # single-path candidates also compete with a tight boost (modeled pedal.ts) before the amp
    filters: bool = True               # post-cab high-pass / low-pass (post.hp, post.lp slope) in the search
    cab_sweep: bool = True             # after stage 2: every pool cab on the top candidates per topology (False: the old sweep only)

    @staticmethod
    def quick(budget: float = 1.0, top_k: int = 3, prescreen_n: int | None = None) -> "Plan":
        """Fast preset (``--quick``): blend-aware pre-screen -> coarse 2 s pair screen -> full pass on the top 10 % ->
        fewer re-scores / cab sweeps -> smaller CMA-ES budgets with a plateau stop."""
        b = max(budget, 0.01)
        g = lambda n, lo: max(lo, int(round(n * min(b, 3.0))))
        return Plan(cap_pairs=g(380, 6), n_rescore=g(16, 3), n_rescore_single=g(12, 3), n_cab=g(3, 2), n_cab_single=g(2, 2),
                    top_k={"blend": min(top_k, 2), "single": min(top_k, 2), "single2": 1 if top_k >= 4 else 0},
                    gens_linear=g(30, 3), gens_gain=g(5, 2), gens_final=g(10, 2), pop_linear=12, pop_gain=6,
                    prescreen_n=prescreen_n, n2_pedals=g(4, 2), n2_amps=g(4, 2), mode="quick", coarse_s=2.5,
                    coarse_keep=0.08, coarse_min_keep=32, blend_aware=False, capped_prescreen=True, patience=8, patience_gain=3, plateau_tol=0.005,
                    prescreen_n_ped=4, blend_extra_ped=0, blend_extra_amp=0, gain_s=2.5, short_linear=True)

    @staticmethod
    def from_budget(budget: float, top_k: int = 3, prescreen_n: int | None = None, quick: bool = False) -> "Plan":
        if quick:
            return Plan.quick(budget, top_k, prescreen_n)
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
    quick: bool = False
    progress_json: Path | None = None
    timings_pre: dict | None = None      # seconds spent before run_match (reference loading), from the CLI
    ablate: tuple = ()                   # v0.4M suspects switched off (ABLATIONS); echoed in result.json
    trace_tones: tuple = ()              # TONE3000 tone ids to explain in result.json -> trace


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
    # the tight boost costs like one extra block: it must beat the best plain candidate of its topology by > OCCAM_DB
    plain = [c for c in ok if not c.combo.boost]
    ok = [c for c in ok if not c.combo.boost
          or not any(p.topology == c.topology and p.loss <= c.loss + OCCAM_DB for p in plain)] or ok
    best = min(ok, key=lambda c: c.loss)
    near = [c for c in ok if c.loss <= best.loss + OCCAM_DB]
    rank = min(TOPOLOGY_RANK[c.topology] for c in near)
    same = [c for c in near if TOPOLOGY_RANK[c.topology] == rank]
    top = min(c.loss for c in same)
    tie = [c for c in same if c.loss <= top + SIZE_TIE_DB]
    return min(tie, key=lambda c: (c.combo.size_rank()[0], c.combo.size_rank()[1], c.loss))


def parse_ablate(spec) -> tuple[str, ...]:
    """``--ablate`` value (comma list or iterable) -> validated tuple, order kept, duplicates dropped."""
    items = [x.strip() for x in spec.split(",")] if isinstance(spec, str) else [str(x).strip() for x in (spec or ())]
    out: list[str] = []
    for x in items:
        if not x:
            continue
        if x not in ABLATIONS:
            raise ValueError(f"--ablate: unknown suspect {x!r} (one of {', '.join(ABLATIONS)})")
        if x not in out:
            out.append(x)
    return tuple(out)


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


PLACED_WINDOW_MS = 250.0   # after a confident whole-song placement the refinement searches only +-250 ms
HINT_WINDOW_MS = 20.0  # an explicit --offset-ms is trusted: both refinements search only +-20 ms around it


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
    plan = cfg.plan or Plan.from_budget(cfg.budget, cfg.top_k, cfg.prescreen_n, cfg.quick)
    abl = parse_ablate(cfg.ablate)
    plan = dataclasses.replace(plan, boost=plan.boost and "boost" not in abl, filters=plan.filters and "filters" not in abl,
                               cab_sweep=plan.cab_sweep and "irsweep" not in abl)
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
    prog = Progress(cfg.progress_json, plan.mode).start() if cfg.progress_json else NullProgress()
    try:
        res = _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start, prog)
        prog.close("done", res["after"][0]["aWeightedErrorDb"] if res.get("after") else None)
        return res
    except BaseException as e:
        prog.close(f"error: {e}")
        raise
    finally:
        eng.close()


def _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start, prog=None):
    prog = prog or NullProgress()
    cpu0 = time.process_time()
    prog.stage("prepare", "preparing the excerpt and the reference")
    T: dict = {"referenceLoad": (cfg.timings_pre or {}).get("referenceLoad"), "diPrep": time.time() - t_start}
    t_mark = time.time()

    def lap(name):
        nonlocal t_mark
        now = time.time()
        T[name] = T.get(name, 0.0) + now - t_mark
        t_mark = now
    cab0 = default_cab(pool.cabs)
    offset_search = None
    if ref.matched_sig is not None:
        # unknown offset + DI shorter than the song: whole-song coarse placement (raises PlacementError, a ValueError,
        # which the CLI prints as "error: could not place the DI in the song: enter where it starts")
        t_place = time.time()
        try:
            offset_search = resolve_offset(di48, ref.matched_sig, RATE, ref.offset_given, ref.offset_samples)
        except PlacementError as e:
            d = e.details       # one calibration line BEFORE the error line (the plugin shows the last line starting "error")
            if d.get("nonFinite"):
                log("non-finite audio in DI or reference")
            elif d:
                log(f"whole-song placement failed: r1 {d['r1']:.3f}, r2 {d['r2']:.3f}, sigma {d['sigma']:.4f}, "
                    f"confidence {d['confidence']:.2f} < MIN_CONFIDENCE {d['minConfidence']:.1f} "
                    f"(and not a strong placement: r1 >= {STRONG_R1}, r1 - r2 >= {STRONG_MARGIN})")
            raise
        if offset_search["mode"] == "whole_song":
            ref.offset_samples = offset_search["offset_samples"]
            offset_search["searchSeconds"] = time.time() - t_place
            log(f"whole-song placement: DI starts at {offset_search['offset_ms'] / 1000:.3f} s "
                f"(confidence {offset_search['confidence']:.1f}, accepted by {offset_search['acceptedBy']}, "
                f"{offset_search['searchSeconds']:.2f} s)")
    window = None if cfg.window_s is None else (int(cfg.window_s[0] * RATE), int(cfg.window_s[1] * RATE))
    ex = make_excerpt(di48, cfg.excerpt_s, window=window, ref=ref)
    lap("excerpt")
    log(f"excerpt {ex.start / RATE:.1f}-{ex.end / RATE:.1f} s ({ex.info})")
    for note in ref.notes:
        log(f"reference note: {note}")
    result: dict = {"schema": "sawblade.match_result", "version": 1, "seed": cfg.seed, "budget": cfg.budget,
                    "mode": plan.mode, "plan": plan.__dict__, "di": str(cfg.di), "diR": str(cfg.di_r) if cfg.di_r else None,
                    "reference": {"path": ref.path, "basis": ref.basis, "stemChannel": ref.stem_channel,
                                  "bandLimitHz": ref.hf_limit_hz, "textureTerm": ref.texture, "clean": ref.clean,
                                  "matchedStftFmaxHz": ref.matched_fmax if ref.matched_sig is not None else None,
                                  "matched": ref.matched_channel,
                                  "sections": ref.sections, "notes": ref.notes},
                    "excerpt": {"startS": ex.start / RATE, "endS": ex.end / RATE, **ex.info},
                    "gate": gate, "diNoiseFloorDb": floor, "poolCounts": pool.counts(),
                    "lossWeights": {"texFlat": L.W_FLAT, "texHf": L.W_HF, "ltas": L.W_LTAS, "buzz": L.W_BUZZ, "decay": L.W_DECAY, "stft": L.W_STFT,
                                    "reg": L.W_REG, "feelTight": L.W_TIGHT, "feelFizz": L.W_FIZZ, "feelPolish": L.W_POLISH},
                    "ablate": list(parse_ablate(cfg.ablate)),
                    "ablateNote": "studio is accepted and echoed; it is a no-op until its task (B2.3) lands",
                    "randomness": f"numpy default_rng(seed={cfg.seed}) for subset sampling and CMA-ES (seed + block)"}

    # ---- starter ("before") on the excerpt, which also gives the coarse offset refinement its render -----------------
    starter_p, starter_caps = starter_preset(pool, gate)
    result["starter"] = {"label": "generic starter baseline", "captures": starter_caps}
    if offset_search is not None:
        result["offset_search"] = offset_search
    y_st, rep_st = eng.render(starter_p, ex.x)

    hint_samples = ref.offset_samples if ref.offset_given else None

    def offset_from(y_full_excerpt, label):
        # unknown offset: +-3 s, then refined; given: only +-HINT_WINDOW_MS around the hint
        if ref.offset_given:
            search = int(HINT_WINDOW_MS * RATE / 1000)
        elif offset_search is not None and offset_search["mode"] == "whole_song":
            search = int(PLACED_WINDOW_MS * RATE / 1000)     # a confident placement: no jumping to a louder decoy
        else:
            search = int(3.0 * RATE)
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
        if offset_search is not None and offset_search["mode"] != "given":
            offset_search["offset_ms"] = 1000.0 * ref.offset_samples / RATE      # after the refinement
            offset_search["offset_samples"] = int(ref.offset_samples)
    lap("starterAndOffset")
    ablate = parse_ablate(cfg.ablate)
    tgt_full = build_target(ref, ex)        # always with the feel target: the gate sweep measures floor / tightness with it
    tgt = dataclasses.replace(tgt_full, feel=None) if "feel" in ablate else tgt_full

    def mk_target(e):
        t = build_target(ref, e)
        return dataclasses.replace(t, feel=None) if "feel" in ablate else t
    cex = ctgt = None
    if plan.coarse_s > 0 and ex.n > int(plan.coarse_s * 1.5 * RATE):
        # coarse excerpt: the densest `coarse_s` seconds inside the excerpt, with a short warm-up lead
        a0, b0, cinfo = select_excerpt(ex.x[ex.lead:], RATE, plan.coarse_s)
        cex = make_excerpt(di48, plan.coarse_s, lead_s=0.2, window=(ex.start + a0, ex.start + b0), ref=ref)
        ctgt = mk_target(cex)
        result["coarseExcerpt"] = {"startS": cex.start / RATE, "endS": cex.end / RATE, "leadS": cex.lead / RATE}
        log(f"coarse excerpt {cex.start / RATE:.1f}-{cex.end / RATE:.1f} s")
    gex = gtgt = None
    if plan.gain_s > 0 and ex.n > int(plan.gain_s * 1.2 * RATE):
        a1, b1, _ = select_excerpt(ex.x[ex.lead:], RATE, plan.gain_s)
        gex = make_excerpt(di48, plan.gain_s, lead_s=0.3, window=(ex.start + a1, ex.start + b1), ref=ref)
        gtgt = mk_target(gex)
        result["gainExcerpt"] = {"startS": gex.start / RATE, "endS": gex.end / RATE}
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
                                 "onsetsMeasured": tgt.ref.n_onsets, "feel": tgt_full.feel.summary() if tgt_full.feel else None}
    before_ex = L.evaluate(ex.trim(y_st), tgt, None)
    result["starter"]["excerptLoss"] = before_ex.as_dict()
    log(f"starter on excerpt: loss {before_ex.total:.3f} (ltas {before_ex.ltas:.2f} dB, feel {before_ex.feel:.3f})")
    if tgt_full.feel is not None:
        fs_ = tgt_full.feel.summary()
        log(f"feel term ({fs_['mode']}): fizz {'on' if fs_['fizzOn'] else 'OFF (' + str(fs_['fizzOffReason']) + ')'}; "
            f"notes {fs_['noteSet'] or 'dropped'} {fs_['notes']}; dropped: {fs_['dropped'] or 'none'}")
    lap("targetAndProfile")

    # ---- stage 1 -----------------------------------------------------------------------------------------------------
    scr = Screener(eng, pool, ex, tgt, cab0, rng, plan, log, cex, ctgt, prog)
    ranked = scr.run()
    result["stage1"] = {**scr.stats, "top": {t: [_scored_json(s) for s in lst[:30]] for t, lst in ranked.items()}}
    t1 = time.time() - t_start
    T["stage1"] = scr.stats.get("stage1Seconds")
    T["stage1Detail"] = scr.stats.get("timings")
    t_mark = time.time()
    prog.stage("refine", "refining the best candidates")

    # ---- stage 2 -----------------------------------------------------------------------------------------------------
    refined: list[Scored] = []
    unrefined: list[Scored] = []
    work: list[tuple] = []
    for topo in TOPOLOGIES:
        lst = ranked.get(topo, [])
        kk = plan.top_k.get(topo, 0)
        sel = list(lst[:kk])
        if topo == "single" and plan.boost and kk:     # the boost variant and a plain one are both always refined
            for want in (True, False):
                if not any(bool(c.combo.boost) == want for c in sel):
                    extra = next((c for c in lst[kk:] if bool(c.combo.boost) == want), None)
                    if extra is not None:
                        sel.append(extra)
        unrefined += [c for c in lst if not any(c is t for t in sel)][:2]
        work += [(topo, k, len(sel), c) for k, c in enumerate(sel)]
    n_refine = len(work)
    n_done = 0
    T["stage2PerCombo"] = []

    def finish_refined(combo: Combo, base: Scored, v: dict, r: L.LossResult, info: dict) -> Scored:
        """Stage-2 record of ``combo`` with parameters ``v`` (alignment / levels of the stage-1 candidate ``base``)."""
        ca = eng.core(combo, v, "a", ex.x)
        cb = eng.core(combo, v, "b", ex.x) if combo.topology == "blend" else None
        y = ex.trim(eng.emulate(combo, v, ca, cb, base.align, base.levels))
        g, clipped = pick_output_gain(float(np.max(np.abs(y))), r.offset_db, ref.level_offset_db)
        if base.levels is not None:      # emitted preset uses the constantLoudness law + make-up: keep the fitted level
            g -= emit_gain_correction_db(v["blend"], base.levels)
        guard = _guardrails(y, profile)
        return Scored(combo, r.total, v.get("blend", 0.0), base.align, r, "refined",
                      {"params": v, "outputGainDb": g, "clipped": clipped, "info": info, "guardrails": guard}, base.levels)

    for topo, k, kk, s in work:
        t_combo = time.time()

        def on_gen(block, g, n, _d=n_done):
            base = {"L1": 0.0, "G": 0.4, "L2": 0.85}[block]
            span = {"L1": 0.4, "G": 0.45, "L2": 0.15}[block]
            prog.update(REFINE_SHARE * (_d + base + span * g / max(n, 1)) / max(n_refine, 1))
        log(f"stage2 {topo} [{k + 1}/{kk}] {s.combo.describe()} (screen loss {s.loss:.3f})")
        sp = Space.for_combo(s.combo, plan.filters)
        v0 = sp.default()
        if "blend" in v0:
            v0["blend"] = s.blend
        v, r, info = refine_combo(eng, s.combo, sp, ex, tgt, s.align, v0, seed=cfg.seed * 1000 + len(refined) * 10,
                                  gens_linear=plan.gens_linear, pop_linear=plan.pop_linear,
                                  gens_gain=plan.gens_gain, pop_gain=plan.pop_gain, gens_final=plan.gens_final,
                                  patience=plan.patience, patience_gain=plan.patience_gain, tol=plan.plateau_tol,
                                  on_gen=on_gen, gex=gex, gtgt=gtgt, short_linear=plan.short_linear,
                                  levels=s.levels, log=log)
        refined.append(finish_refined(s.combo, s, v, r, info))
        n_done += 1
        prog.best(r.ltas)
        prog.update(REFINE_SHARE * n_done / max(n_refine, 1))
        T["stage2PerCombo"].append({"topology": topo, "seconds": round(time.time() - t_combo, 1)})
    unrefined = sorted([c for c in unrefined if np.isfinite(c.loss)], key=lambda c: c.loss)
    refined = [c for c in refined if np.isfinite(c.loss)]
    refined.sort(key=lambda c: c.loss)

    # ---- cab / IR breadth (v0.4M Task B): every pool cab on the top candidates per topology --------------------------
    cab_sweeps: list[dict] = []
    t_cab = time.time()
    if plan.cab_sweep:
        n_sw = max(1, sum(min(TOP_PER_TOPOLOGY, sum(1 for x in refined if x.topology == t)) for t in TOPOLOGIES))
        for topo in TOPOLOGIES:
            for c in sorted((x for x in refined if x.topology == topo), key=lambda x: x.loss)[:TOP_PER_TOPOLOGY]:
                sp = Space.for_combo(c.combo, plan.filters)
                rows = cab_sweep(eng, c, pool.cabs, sp, ex, tgt)
                summ = sweep_summary(c, rows)
                cur = next(x for x in rows if x["cab"].key == c.combo.cab.key)
                top = min(rows, key=lambda x: x["result"].total)
                if top["cab"].key != cur["cab"].key and top["result"].total < cur["result"].total - CAB_SWITCH_DB:
                    combo2 = c.combo.with_cab(top["cab"])
                    v2, r2 = relinear(eng, combo2, sp, ex, tgt, c.align, c.extra["params"], levels=c.levels,
                                      seed=cfg.seed * 1000 + 900 + len(cab_sweeps), gens=plan.gens_final, pop=plan.pop_linear,
                                      patience=plan.patience, tol=plan.plateau_tol, log=log)
                    new = finish_refined(combo2, c, v2, r2, c.extra["info"])
                    refined[next(i for i, x in enumerate(refined) if x is c)] = new
                    summ.update(changed=True, newCab=top["cab"].key, lossBeforeRelinear=top["result"].total,
                                lossAfterRelinear=r2.total)
                    log(f"cab sweep {topo}: {cur['cab'].title}:{cur['cab'].name} -> {top['cab'].title}:{top['cab'].name} "
                        f"(loss {cur['result'].total:.3f} -> {top['result'].total:.3f}, relinear {r2.total:.3f})")
                cab_sweeps.append(summ)
                prog.update(REFINE_SHARE + 0.03 * len(cab_sweeps) / n_sw)
        refined.sort(key=lambda c: c.loss)
        result["cabSweep"] = {"ablated": False, "topPerTopology": TOP_PER_TOPOLOGY, "poolCabs": len(pool.cabs),
                              "candidates": cab_sweeps, "seconds": round(time.time() - t_cab, 2)}
    else:
        result["cabSweep"] = {"ablated": True, "note": "--ablate irsweep: only the stage-1 cab sweep ran"}
    T["cabSweep"] = time.time() - t_cab
    best = choose(refined)
    # ---- two-IR blend (v0.4M B2.1): the winner's cab as one combined irMix IR of two of the top IRs --------------------
    t_ir = time.time()
    if "irblend" in ablate:
        result["irBlend"] = {"ablated": True, "tried": 0, "won": False}
    else:
        sw = next((x for x in cab_sweeps if x["topology"] == best.topology and x["boost"] == bool(best.combo.boost)), None)
        bykey = {c.key: c for c in pool.cabs}
        keys = [i["cab"] for i in sw["irs"]][:TOP_IRS] if sw else [c.key for c in pool.cabs][:TOP_IRS]
        irb = pair_search(eng, best, Space.for_combo(best.combo, plan.filters), ex, tgt, [bykey[k] for k in keys],
                          seed=cfg.seed * 1000 + 950, gens=plan.gens_final, pop=plan.pop_linear, patience=plan.patience,
                          tol=plan.plateau_tol, log=log)
        won_pair = irb.pop("_won", None)
        irb["ablated"] = False
        if won_pair is not None:
            combo2, v2, r2 = won_pair
            new = finish_refined(combo2, best, v2, r2, best.extra["info"])
            refined[next(i for i, x in enumerate(refined) if x is best)] = new
            log(f"two-IR blend: {irb['pair']['titleA']} + {irb['pair']['titleB']} (offset {irb['offset']}, invert {irb['invert']}, "
                f"mix {irb['mix']:.2f}) loss {best.loss:.3f} -> {new.loss:.3f}")
            best = new
        else:
            log(f"two-IR blend: no pair beats the single IR by {irb['minGain']} (best gain {irb['gainVsSingle']:+.3f})")
        result["irBlend"] = irb
    T["irBlend"] = time.time() - t_ir
    result["stage2Seconds"] = time.time() - t_start - t1
    T["stage2"] = time.time() - t_mark
    t_mark = time.time()
    prog.stage("finalize", "verifying on the full-length DI")
    result["topologies"] = {}
    for topo in TOPOLOGIES:
        c = [x for x in refined if x.topology == topo]
        if c:
            b = min(c, key=lambda x: x.loss)
            result["topologies"][topo] = {"loss": b.loss, "captures": caps_summary(b.combo), "blend": b.blend,
                                          "tightBoost": bool(b.combo.boost),
                                          "breakdown": b.result.as_dict(), "guardrails": b.extra.get("guardrails")}
    log("topology bests: " + ", ".join(f"{t} {d['loss']:.3f}" for t, d in result["topologies"].items()))
    log(f"selected: {best.combo.describe()} loss {best.loss:.3f}")

    # ---- gate matched to the reference (v0.4M Task B): sweep threshold x release on the final chain ----------------
    t_gate = time.time()
    prog.update(REFINE_SHARE + 0.03)
    gate_final = gate
    sp_best = Space.for_combo(best.combo, plan.filters)
    ref_floor = (reference_floor_db(ref.ltas_sig)
                 if (tgt_full.feel is not None and tgt_full.feel.mode == "soft" and ref.clean) else None)
    gs = gate_sweep(eng, best, sp_best, ex, tgt_full, floor, ref_floor, ref_clean=bool(ref.clean))
    result["gateSweep"] = gs
    result["gateDefault"] = gate
    if gs.get("changed"):
        gate_final = gs["gate"]
        y = render_gate(eng, best, ex, gate_final)
        r = L.evaluate(y, tgt, sp_best.eq_gains(best.extra["params"]))
        g, clipped = pick_output_gain(float(np.max(np.abs(y))), r.offset_db, ref.level_offset_db)
        if best.levels is not None:
            g -= emit_gain_correction_db(best.extra["params"]["blend"], best.levels)
        best.loss, best.result = r.total, r
        best.extra.update(outputGainDb=g, clipped=clipped, guardrails=_guardrails(y, profile), gate=gate_final)
        base_row, pick = gs["baseline"], gs["picked"]
        log(f"gate sweep: threshold {base_row['thresholdDb']:+.1f} -> {pick['thresholdDb']:+.1f} dBFS, release "
            f"{base_row['releaseMs']:.0f} -> {pick['releaseMs']:.0f} ms (floor term {base_row['floorTerm']:.3f} -> "
            f"{pick['floorTerm']:.3f}, ltas {base_row['ltas']:.2f} -> {pick['ltas']:.2f} dB)")
    else:
        log("gate sweep: " + (f"skipped ({gs['skipped']})" if gs.get("skipped") else "the default gate (floor + 4 dB, 150 ms) stays"))
    result["gateFinal"] = gate_final
    prog.update(REFINE_SHARE + 0.04)
    T["gateSweep"] = time.time() - t_gate

    # ---- reporting of the v0.4M suspects ---------------------------------------------------------------------------------
    st_boost = scr.stats.get("tightBoost") or {}
    won = bool(best.combo.boost)
    bp = best.extra["params"]
    result["tightBoost"] = {
        "tried": int(st_boost.get("tried", 0)), "refined": sum(1 for c in refined if c.combo.boost), "won": won,
        "params": {k.split(".", 1)[1]: bp[k] for k in bp if k.startswith("boost.")} if won else None,
        "bestBoostLoss": min((c.loss for c in refined if c.combo.boost), default=None),
        "bestPlainSingleLoss": min((c.loss for c in refined if c.topology == "single" and not c.combo.boost), default=None),
        "occamDb": OCCAM_DB, "ablated": not plan.boost}
    result["postFilters"] = {"searched": plan.filters, **post_filters_from_eq(post_eq(bp))}
    if cfg.trace_tones:
        t_tr = time.time()
        result["trace"] = trace_tones(cfg.trace_tones, eng=eng, pool=pool, scr=scr, ranked=ranked, refined=refined, best=best,
                                      ex=ex, tgt=tgt, plan=plan, cab_sweeps=cab_sweeps, seed=cfg.seed, filters=plan.filters,
                                      gate=gate_final, on_tone=lambda n, k: prog.update(REFINE_SHARE + 0.04 + 0.01 * n / max(k, 1)),
                                      log=log)
        T["trace"] = time.time() - t_tr

    # ---- stage 3: full-length verification ----------------------------------------------------------------------
    v = best.extra["params"]
    gain_db = best.extra["outputGainDb"]
    final = build_preset(best.combo, v, gate=gate_final, align=best.align, output_db=gain_db,
                         name="Sawblade match", notes=_notes(cfg, ref, best), levels=best.levels)
    full_jobs = {"best_L": (final, cfg.di)}
    if plan.mode != "quick" or cfg.write_audio:     # quick: no full-length "before" render (the excerpt loss has it)
        full_jobs["starter_L"] = (starter_p, cfg.di)
    if cfg.di_r is not None:       # always: the clip guard (peak = max of L/R) and the R offset refinement need it
        full_jobs["best_R"] = (final, cfg.di_r)
    log(f"stage3: full-length renders {list(full_jobs)}")

    def full_render(item):
        name, (preset, path) = item
        x, fs = sf.read(str(path), dtype="float32")
        x = x if x.ndim == 1 else x[:, 0]
        y, rep = eng.render(preset, x, fs)
        return name, y, fs, rep

    renders = {n: (y, fs, rep) for n, y, fs, rep in eng.map(full_render, list(full_jobs.items()))}
    lap("fullRenders")
    prog.update(0.5, "measuring the result")
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
    for name in [n for n in ("best_L", "starter_L") if n in renders]:
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
    lap("tonecheck")
    result["before"] = _err_summary(reports["starter_L"]) if "starter_L" in reports else None
    result["after"] = _err_summary(reports["best_L"])
    log(f"A-weighted error vs {Path(refs[0][0]).name}: "
        + (f"before {result['before'][0]['aWeightedErrorDb']:.2f} dB, " if result["before"] else "")
        + f"after {result['after'][0]['aWeightedErrorDb']:.2f} dB")

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

    lap("finalOffsets")
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
            if s.levels is not None:
                gdb -= emit_gain_correction_db(s.blend, s.levels)
        preset = build_preset(s.combo, v_alt, gate=gate_final, align=s.align, output_db=gdb, name=f"Sawblade match alt {i}",
                              levels=s.levels)
        (out / f"alt{i}.preset.resolved.json").write_text(json.dumps(preset, indent=2) + "\n")
        alts.append({**_scored_json(s), "file": f"alt{i}.preset.resolved.json"})
    result["alternatives"] = alts
    result["best"] = {**_scored_json(best), "params": v, "outputGainDb": gain_db, "preset": "best.preset.resolved.json"}
    result["best"]["captures"] = caps_summary(best.combo)
    result["best"]["fullLengthPeakDbfs"] = best.extra["fullLengthPeakDbfs"]
    result["best"]["fullLengthPeakAfterGuardDbfs"] = best.extra["fullLengthPeakAfterGuardDbfs"]
    result["best"]["clipGuardDb"] = best.extra.get("clipGuardDb", 0.0)
    result["candidatesStage2"] = [_scored_json(c) for c in refined]
    lap("outputs")
    result["listening"] = {}
    if cfg.write_audio:
        fin_l = (result.get("offsetRefinement", {}).get("final") or {}).get("L") or {}
        off48 = (int(round(fin_l["offsetSamples"] * RATE / fin_l["rate"])) if "offsetSamples" in fin_l
                 else ref.offset_samples)
        result["listening"] = _listening(out, renders, cfg, log, ref=ref, di48=di48, offset=off48)
        lap("listening")
    T["coreCache"] = {"hits": eng.core_hits, "misses": eng.core_misses}
    T["cpuSeconds"] = time.process_time() - cpu0
    result["timings"] = {k: (round(v, 2) if isinstance(v, float) else v) for k, v in T.items()}
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
         "levelMatch": s.levels and s.levels.preset_block(),
         "tightBoost": bool(s.combo.boost), "irMix": None if s.combo.cab_b is None else
         {"irB": s.combo.cab_b.key, "offsetSamplesB": s.combo.cab_offset, "invertB": s.combo.cab_invert},
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


def _finite_or_none(o):
    """JSON-safe: non-finite floats (a silent section gives -inf LUFS) become None, recursively."""
    if isinstance(o, dict):
        return {k: _finite_or_none(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_finite_or_none(v) for v in o]
    if isinstance(o, (float, np.floating)) and not np.isfinite(o):
        return None
    return o


def _louder_quieter(gain_db: float) -> str:
    """The raw render needed ``gain_db``: negative -> it was louder than the reference."""
    return f"{abs(gain_db):.1f} dB {'louder' if gain_db < 0 else 'quieter'}"


def _listening(out: Path, renders: dict, cfg, log, ref: Reference | None = None, di48: np.ndarray | None = None,
               offset: int = 0) -> dict:
    """Listening files, all loudness-matched to the reference (BS.1770 integrated LUFS over one 30 s guitar-dominant
    section, time-aligned with ``offset`` = reference index of DI sample 0, 48 kHz samples); never peak-normalised,
    float WAV. ``listen/ref.wav`` + ``render.wav`` (+ ``before.wav``) are that section; the full-length stereo file gets
    the same gain as the render. Without a reference (or DI) nothing can be matched: no section files, gain 0 dB."""
    d = out / "listen"
    d.mkdir(exist_ok=True)
    yl, fs, _ = renders["best_L"]
    info: dict = {"loudnessMatched": False}
    gain_db = 0.0
    if ref is not None and di48 is not None:
        try:
            matched = ref.matched_sig is not None
            r48 = to48(yl, fs)
            st48 = to48(renders["starter_L"][0], renders["starter_L"][1]) if "starter_L" in renders else None
            n_av = min(len(di48), len(r48))
            if matched:
                a, b, _ = choose_listen_section(di48[:n_av], len(ref.matched_sig), offset, fs=RATE)
                ref_seg = ref.matched_sig[a + offset:b + offset]
                ref_sig = "matched reference channel"
            else:        # no time alignment exists: the reference's own guitar-dominant section (its guitar isolation)
                a, b, _ = choose_listen_section(di48[:n_av], None, 0, fs=RATE)
                ra, rb, _ = select_excerpt(ref.ltas_sig, RATE, (b - a) / RATE)
                ref_seg = ref.ltas_sig[ra:rb]
                ref_sig = f"guitar isolation ({ref.basis}), its own guitar-dominant section; not time-aligned"
            sec = r48[a:b]
            gain_db, l_ref, l_raw = match_gain_db(ref_seg, sec, RATE)
            sf.write(str(d / "ref.wav"), ref_seg.astype(np.float32), RATE, subtype="FLOAT")
            sf.write(str(d / "render.wav"), (sec * 10 ** (gain_db / 20)).astype(np.float32), RATE, subtype="FLOAT")
            tp = {"ref": true_peak_db(ref_seg), "render": true_peak_db(sec * 10 ** (gain_db / 20))}
            info.update({"loudnessMatched": True, "section": [a / RATE, b / RATE], "lufsRef": l_ref, "lufsRenderRaw": l_raw,
                         "gainDb": gain_db, "offsetMs": 1000.0 * offset / RATE if matched else None,
                         "referenceSignal": ref_sig, "truePeakDb": tp,
                         "files": {"ref": str(d / "ref.wav"), "render": str(d / "render.wav")}})
            if not matched:
                info["refSection"] = [ra / RATE, rb / RATE]
            if st48 is not None:
                gb, _, l_bef = match_gain_db(ref_seg, st48[a:b], RATE)
                bef = st48[a:b] * 10 ** (gb / 20)
                sf.write(str(d / "before.wav"), bef.astype(np.float32), RATE, subtype="FLOAT")
                info.update({"lufsBefore": l_bef, "gainBeforeDb": gb})
                tp["before"] = true_peak_db(bef)
                info["files"]["before"] = str(d / "before.wav")
            if np.isfinite(l_ref) and np.isfinite(l_raw):
                log(f"render was {_louder_quieter(gain_db)} than the reference before matching "
                    f"(integrated {l_raw:.1f} vs {l_ref:.1f} LUFS over {a / RATE:.1f}-{b / RATE:.1f} s)")
            else:
                log("loudness matching skipped: the listening section is silent or shorter than 400 ms; gain 0 dB")
        except Exception as e:      # listening must never abort the run
            gain_db = 0.0
            for f in ("ref.wav", "render.wav", "before.wav"):
                (d / f).unlink(missing_ok=True)
            info = {"loudnessMatched": False, "listenError": f"{type(e).__name__}: {e}"}
            log(f"warning: listening section files skipped ({info['listenError']}); full-length files at 0 dB")
        else:
            info = _finite_or_none(info)
    if "best_R" in renders:
        yr = renders["best_R"][0]
        n = min(len(yl), len(yr))
        st = np.stack([yl[:n], yr[:n]], axis=1)
        name = "cover_guitars_L-R"
    else:
        st = np.stack([yl, yl], axis=1)
        name = "guitar_L_mono"
    st = (st * 10 ** (gain_db / 20)).astype(np.float32)
    pk = float(np.max(np.abs(st)))
    info["fullLengthGainDb"] = float(gain_db)
    info["fullLengthSamplePeakDb"] = float(20 * np.log10(max(pk, 1e-12)))
    sf.write(str(d / f"{name}.wav"), st, fs, subtype="FLOAT")
    info["wav"] = str(d / f"{name}.wav")
    # the MP3 is integer PCM: encode an attenuated copy when the (float, unclipped) WAV would clip; the WAV is untouched
    mp3_src, tmp = d / f"{name}.wav", None
    if pk > 0.89:         # -1 dBFS
        tmp = d / f"{name}.mp3src.wav"
        info["mp3GainDb"] = float(20 * np.log10(0.89 / pk))
        sf.write(str(tmp), (st * (0.89 / pk)).astype(np.float32), fs, subtype="FLOAT")
        mp3_src = tmp
    try:
        if encode_mp3(mp3_src, d / f"{name}.mp3", log):
            info["mp3"] = str(d / f"{name}.mp3")
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)
    return _finite_or_none(info)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _targets_path() -> Path:
    return _repo_root() / "docs" / "tone_targets.json"
