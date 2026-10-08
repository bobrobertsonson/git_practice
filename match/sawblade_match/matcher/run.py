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
from .. import core
from ..tonecheck.analysis import analyze, gap_regions
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
from . import irscreen
from .gatesweep import gate_sweep, reference_floor_db, render_gate
from .irblend import TOP_IRS, pair_search
from .preeq import PRE_CONFIRM_DB, PRE_REFIT_L1, describe as describe_pre, di_spectrum_numbers, preeq_candidate, setting_params, widening
from .refine import SEED_FINAL, SEED_PREEQ, SEED_STUDIO, SEED_SWEEP, refine_combo, relinear
from .studio import detect as detect_studio, studio_stage
from .trace import trace_tones
from .screen import Scored, Screener, TOPOLOGIES
from .levelmatch import emit_gain_correction_db
from .loudness import choose_listen_section, match_gain_db, true_peak_db
from .space import TOPOLOGY_RANK, Combo, Space, build_preset, gate_preset, manual_align, post_eq, post_filters_from_eq

CLIP_PEAK = 1.0           # linear full scale; a candidate whose matched-level output exceeds it is "clipping"
CLIP_GUARD_DBFS = -1.0    # the final output gain is lowered until the full-length peak is below this
OCCAM_DB = 0.1            # prefer the simplest topology within this much total loss (single2 vs single / blend, boost vs plain)
BLEND_OCCAM_DB = 0.25     # a single-path candidate beats the best blend when within this much total loss (blend costs a second chain)
TOPOLOGY_DETERMINED_PCT = 10.0     # |best single - best blend| must be at least this % of the smaller loss to call the topology determined
TOPOLOGY_CHOICES = ("auto", "single", "blend")        # --topology; "single" = the single-path topologies (single and single2)
PEDAL_OCCAM_DB = 0.05     # a single-path combo with a pedal must beat the best pedal-less single (same amp if available) by this
OCCAM_CONFIRM_STARTS = 2  # ... and when it does, its pedal-less partner is fitted this many more times (other seeds) first: one
                          # stage-2 fit of a chain varies by ~0.2 dB of loss with the seed (measured, real scipy), more than the margin
PEDAL_CONFIRM_STARTS = OCCAM_CONFIRM_STARTS      # old name
OCCAM_NOISE_DB = 0.20     # a complex candidate that wins by the margin plus less than this is "contested": its simpler partner is refitted
                          # first. Stage-2 fits of one chain have sd ~0.11 dB across seeds (pedal known answer: 0.106 / 0.113, 8-9 seeds)
                          # and the best of three fits is on average 0.11 (at most 0.27) below a single fit: ~2 sd covers it.
# Seed layout, see refine.py (SEED_*): stage-2 refine 0..490 (REFINE_SEED_SLOTS slots of 10, picked by a hash of the candidate; two
# candidates may share a slot, which is harmless), confirmation fits CONFIRM_SEED_BASE + 10 j. A test checks that the ranges are disjoint.
REFINE_SEED_SLOTS = 50
CONFIRM_SEED_BASE = 600
SIZE_TIE_DB = 0.05        # prefer the lighter model set (size category) within this much total loss
ABLATIONS = ("feel", "boost", "filters", "irsweep", "irblend", "studio", "preeq")      # --ablate names (v0.4M suspects)
CAB_SWITCH_DB = 0.01      # a different cab must lower the loss by at least this to replace the stage-2 cab
REFINE_SHARE = 0.95       # share of the refine stage's progress for stage 2; the cab / gate sweeps and the trace get the rest


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
    topology: str = "auto"               # --topology: auto | single (single-path topologies) | blend
    ablate: tuple = ()                   # v0.4M suspects switched off (ABLATIONS); echoed in result.json
    trace_tones: tuple = ()              # TONE3000 tone ids to explain in result.json -> trace
    ir_library: object = None            # irlib.IrLibrary (the user's own IRs): screened with the pool cabs, top N swept
    ir_dirs: tuple = ()                  # [{"path", "source": "cli"|"config"}] the library was scanned from (for irPool.dirs)
    ir_screen_max: int = irscreen.SCREEN_MAX   # above this many IRs the screen prefilters (tags, then k-means)


class Log:
    def __init__(self, path: Path | None = None):
        self.t0, self.path, self.lines = time.time(), path, []

    def __call__(self, msg: str):
        line = f"[{time.time() - self.t0:7.1f}s] {msg}"
        self.lines.append(line)
        print(line, flush=True)


def _stationary_noise(x: np.ndarray, mask: np.ndarray, fs: int, frame_s: float = 0.010, within_db: float = 6.0) -> np.ndarray:
    """Within ``mask`` (the DI's gap regions) keep the 10 ms frames whose RMS is within ``within_db`` of the 10th percentile of the
    gap frames' RMS: the stationary noise floor. ``gap_regions`` only needs 10 ms RMS under -50 dBFS, so the tails of decaying
    notes (ring-out) land in the mask too, and their peaks would inflate the floor the gate is set from. The mask is returned
    unchanged when fewer than 100 ms of frames would remain."""
    n = int(round(frame_s * fs))
    nf = len(x) // n
    if nf < 5:
        return mask
    fm = mask[: nf * n].reshape(nf, n).all(axis=1)
    rms = np.sqrt(np.mean(x[: nf * n].reshape(nf, n) ** 2, axis=1))
    if not fm.any():
        return mask
    db = 20 * np.log10(np.maximum(rms, 1e-10))
    keep = fm & (db <= np.percentile(db[fm], 10) + within_db)
    if int(keep.sum()) * n < 0.1 * fs:
        return mask
    out = np.zeros(len(x), bool)
    out[: nf * n] = np.repeat(keep, n)
    return out


def gate_floor(x: np.ndarray, fs: int) -> dict:
    """The DI floor for the gate (Task H.1), recorded in result.json as ``gateFloor``.

    ``peakDb``: the core's ``peak_floor_db`` (92.5th percentile of the gate's own peak envelope, 0.1 ms attack / 10 ms release)
    over the DI's gap regions (reduced to their stationary-noise frames: those within 6 dB of the gaps' 10th-percentile 10 ms RMS, so
    ring-out tails do not inflate it): this is what the gate compares its threshold with, and what ``space.gate_preset`` /
    ``gatesweep.cell_gate`` are relative to. ``rmsDb``: the plain RMS level of the same samples (the number the old "floor + 4 dB"
    rule used, ~10 dB below the peak floor for noise). The samples are the DI's real-silence gaps (``gap_regions``); a DI without
    any (a noise bed under the playing) falls back to its quietest 20 % of 20 ms frames, and a steady signal (frame levels within
    3 dB) to the whole signal (``source`` says which)."""
    x64 = np.asarray(x, dtype=np.float64)
    mask = np.zeros(len(x64), bool)
    for a, b in gap_regions(x64, fs):
        mask[a:b] = True
    source = "gaps"
    if int(mask.sum()) >= 0.1 * fs:
        mask = _stationary_noise(x64, mask, fs)          # decaying note tails also sit below -50 dBFS: keep the stationary floor only
    if int(mask.sum()) < 0.1 * fs:             # no real silence (a steady noise bed): the quietest 20 % of the 20 ms frames
        n = int(round(0.020 * fs))
        nf = len(x64) // n
        if nf >= 5:
            rms = np.sqrt(np.mean(x64[: nf * n].reshape(nf, n) ** 2, axis=1))
            db = 20 * np.log10(np.maximum(rms, 1e-10))
            if np.percentile(db, 95) - np.percentile(db, 5) >= 3.0:        # something plays over the bed
                quiet = rms <= np.percentile(rms, 20)
                mask = np.zeros(len(x64), bool)
                mask[: nf * n] = np.repeat(quiet, n)
                source = "quietest 20 % of frames"
    if int(mask.sum()) < 0.1 * fs:
        mask = np.ones(len(x64), bool)
        source = "whole DI"
    peak = core.peak_floor_db(np.asarray(x, dtype=np.float32), float(fs), 0.0, mask.astype(np.uint8))
    rms = 10.0 * np.log10(max(float(np.mean(x64[mask] ** 2)), 1e-20))
    return {"peakDb": None if peak is None else float(peak), "rmsDb": float(rms), "source": source, "samples": int(mask.sum())}


def gate_envelope_floor_db(x: np.ndarray, fs: int) -> float:
    """The DI floor the gate is set from: ``gate_floor(...)["peakDb"]`` (dBFS)."""
    f = gate_floor(x, fs)["peakDb"]
    return float(f) if f is not None else -90.0


def _sha(p) -> str:
    import hashlib
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def portable(preset: dict) -> dict:
    """Same preset with the machine-specific absolute capture paths replaced by cache-relative placeholders."""
    p = copy.deepcopy(preset)

    def walk(n):
        if isinstance(n, dict):
            src = n.get("source")
            if isinstance(src, dict) and "file" in n and src.get("provider") == "local":
                # file stem + the sha256 stay. Two IRs with the same stem collide on this placeholder name only; the sha256
                # (and source.id) disambiguate. For converted IRs (aif/flac) the sha256 is the converted WAV's, while
                # source.id is the original file's sha256[:16].
                n["file"] = f"local-irs/{Path(src.get('title') or 'ir').name}.wav"
            elif isinstance(src, dict) and "file" in n:
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
                                            "sizeCategory": c.size_label or "standard", "architecture": c.arch,
                                            **({"source": "local", "path": c.orig_path, "sha256": c.orig_sha}
                                               if c.provider == "local" else {})}
    return out


def pick_output_gain(y_peak: float, offset_db: float, level_offset_db: float) -> tuple[float, bool]:
    """Output gain that puts the rendered guitar at the reference's per-guitar level; flags clipping at that level."""
    g = -offset_db + level_offset_db
    clipped = y_peak * 10 ** (g / 20) > CLIP_PEAK
    return float(g), bool(clipped)


def allowed_topologies(mode: str) -> tuple[str, ...]:
    """Topologies a ``--topology`` mode lets through: auto = all, single = the single-path ones, blend = blend."""
    if mode not in TOPOLOGY_CHOICES:
        raise ValueError(f"topology must be one of {', '.join(TOPOLOGY_CHOICES)}, got {mode!r}")
    return {"auto": TOPOLOGIES, "single": ("single", "single2"), "blend": ("blend",)}[mode]


def topology_margin(cands: list[Scored]) -> dict:
    """Result record ``topology``: the best single-path and the best blend loss among ``cands`` (refined candidates), their
    difference as a percentage of the smaller loss (positive: the blend is better) and ``determined`` = the gap is at least
    ``TOPOLOGY_DETERMINED_PCT`` %. With only one side present (forced ``--topology``) nothing is determined."""
    fin = [c for c in cands if np.isfinite(c.loss)]
    single = [c for c in fin if c.topology != "blend"]
    blend = [c for c in fin if c.topology == "blend"]
    bs = min(single, key=lambda c: c.loss) if single else None
    bb = min(blend, key=lambda c: c.loss) if blend else None
    rec: dict = {"bestSingle": None if bs is None else float(bs.loss), "bestSingleTopology": None if bs is None else bs.topology,
                 "bestBlend": None if bb is None else float(bb.loss), "deltaPct": None, "determined": False,
                 "determinedAtPct": TOPOLOGY_DETERMINED_PCT}
    if bs is None or bb is None:
        rec["note"] = "only one topology was evaluated (--topology forced it)"
        return rec
    small = max(min(abs(bs.loss), abs(bb.loss)), 1e-9)
    rec["deltaPct"] = float(100.0 * (bs.loss - bb.loss) / small)
    rec["determined"] = bool(abs(rec["deltaPct"]) >= TOPOLOGY_DETERMINED_PCT)
    return rec


def choose(cands: list[Scored], topology: str = "auto") -> Scored:
    """Selection (spec 3.3): lowest loss among finite, non-clipping candidates; within OCCAM_DB of it the simplest
    topology (single < single2 < blend); within that topology, within SIZE_TIE_DB of *that topology's best* the lighter
    model set by size category (manifest size / name label, then 10 % byte buckets); equal categories -> lower loss.
    The size window is relative to the best candidate of the chosen topology, not of the whole field, so the result can be
    at most OCCAM_DB + SIZE_TIE_DB = 0.15 dB above the global best (BLEND_OCCAM_DB + SIZE_TIE_DB = 0.30 dB when a plain
    single beats a blend: the blend must be better by more than BLEND_OCCAM_DB to win). ``topology`` (--topology) restricts
    the field to that topology."""
    allowed = allowed_topologies(topology)
    cands = [c for c in cands if np.isfinite(c.loss) and c.topology in allowed]
    if not cands:
        raise ValueError("no candidate with a finite loss" + ("" if topology == "auto" else f" in the {topology} topology"))
    ok = [c for c in cands if not c.extra.get("clipped")] or cands
    # the tight boost costs like one extra block: it must beat the best plain candidate of its topology by > OCCAM_DB
    plain = [c for c in ok if not c.combo.boost]
    ok = [c for c in ok if not c.combo.boost
          or not any(p.topology == c.topology and p.loss <= c.loss + OCCAM_DB for p in plain)] or ok
    # a pedal costs like one extra block: a single-path combo with a pedal must beat the best pedal-less single (with the same
    # amp when one is among the candidates) by PEDAL_OCCAM_DB, else the pedal is spurious (a drive in front can mimic EQ noise)
    bare = [p for p in ok if p.topology == "single" and not p.combo.a_pedals]

    def pedal_justified(c):
        if c.topology != "single" or not c.combo.a_pedals or not bare:
            return True
        same = [p for p in bare if p.combo.a_amp.key == c.combo.a_amp.key and bool(p.combo.boost) == bool(c.combo.boost)]
        ref = same or [p for p in bare if p.combo.a_amp.key == c.combo.a_amp.key] or bare
        return c.loss < min(p.loss for p in ref) - PEDAL_OCCAM_DB
    dropped = [c for c in ok if not pedal_justified(c)]
    ok = [c for c in ok if pedal_justified(c)] or ok
    best = min(ok, key=lambda c: c.loss)
    # a single-path candidate beats the best BLEND when within BLEND_OCCAM_DB (the blend's second chain must earn more than the
    # 0.1 dB that single2 / boost / pedals have to); every other comparison keeps OCCAM_DB
    near = [c for c in ok if c.loss <= best.loss + (BLEND_OCCAM_DB if best.topology == "blend" and c.topology == "single"
                                                    else OCCAM_DB)]
    rank = min(TOPOLOGY_RANK[c.topology] for c in near)
    same = [c for c in near if TOPOLOGY_RANK[c.topology] == rank]
    top = min(c.loss for c in same)
    tie = [c for c in same if c.loss <= top + SIZE_TIE_DB]
    win = min(tie, key=lambda c: (c.combo.size_rank()[0], c.combo.size_rank()[1], c.loss))
    win.extra["pedalOccamDropped"] = [c.combo.key() for c in dropped]       # recorded in result.json -> pedalOccam
    return win


def refine_seed(seed: int, combo: Combo) -> int:
    """Stage-2 seed of a candidate: ``seed`` * 1000 plus a stable hash of its identity (pair key) in one of REFINE_SEED_SLOTS slots of
    10, so it stays below SEED_TRACE and the other stages' seeds (checked by a test); two candidates can still share a slot. It used to be the position in the refine list, so
    candidates that tie at stage 1 (a pedal that is almost a gain stage ties its pedal-less partner exactly) swapped seeds whenever
    floating-point noise swapped their order, and the pedal-Occam comparison then moved by the CMA-ES noise across platforms."""
    import zlib
    return seed * 1000 + (zlib.crc32("|".join(combo.pair_key()).encode()) % REFINE_SEED_SLOTS) * 10


def confirm_seed(seed: int, j: int) -> int:
    """Seed of the j-th Occam confirmation fit (0 <= j < PEDAL_CONFIRM_STARTS)."""
    return seed * 1000 + CONFIRM_SEED_BASE + 10 * j


def pedal_contested(c: Scored, b: Scored) -> bool:
    """The pedal variant ``c`` passes the pedal-Occam rule against its pedal-less partner ``b`` (beats it by PEDAL_OCCAM_DB)."""
    return bool(c.loss < b.loss - PEDAL_OCCAM_DB)


def occam_decision(c: Scored, b: Scored) -> str:
    """Which simpler-vs-complex decision ``c`` (complex) against ``b`` (simpler) is: pedal / single2 / blend."""
    return "blend" if c.topology == "blend" else "single2" if c.topology == "single2" else "pedal"


def occam_margin(c: Scored, b: Scored) -> float:
    """The loss ``c`` has to win by to be preferred over its simpler partner ``b`` (the margins ``choose`` applies)."""
    if c.topology == "single":
        return PEDAL_OCCAM_DB
    return BLEND_OCCAM_DB if (c.topology == "blend" and b.topology == "single") else OCCAM_DB


def occam_contested(c: Scored, b: Scored, noise: float | None = None) -> bool:
    """``c`` would beat ``b`` by its Occam margin but by less than margin + the noise allowance: the win could be a seed flip, so ``b``
    is refitted before ``c`` is accepted. A bigger win is robust, a smaller one already loses (a refit can only help ``b``)."""
    gain, m = b.loss - c.loss, occam_margin(c, b)
    return bool(m < gain < m + (OCCAM_NOISE_DB if noise is None else noise))


def bare_partner(refined: list, c: Scored) -> Scored | None:
    """The pedal-less single candidate with the same amp as ``c``, preferring the same tight-boost flag (``choose`` falls back to the
    same amp with the other flag, so a boosted pedal variant against a plain bare candidate is confirmed too): the comparator of the
    pedal-Occam rule in ``choose``. None when ``c`` has no capture pedal or no pedal-less candidate of that amp was refined."""
    if c.topology != "single" or not c.combo.a_pedals:
        return None
    same_amp = [p for p in refined if p.topology == "single" and not p.combo.a_pedals and p.combo.a_amp.key == c.combo.a_amp.key]
    return next((p for p in same_amp if bool(p.combo.boost) == bool(c.combo.boost)), None) or min(same_amp, key=lambda p: p.loss, default=None)


def simpler_partner(refined: list, c: Scored) -> Scored | None:
    """The simpler candidate ``c`` is compared with: pedal -> its pedal-less partner (``bare_partner``); single2 -> the best single
    with the same amp, preferring one that keeps one of its two pedals (removing one block) and the same boost flag; blend -> the
    best single-path candidate (single or single2). None when there is none."""
    ok = [x for x in refined if np.isfinite(x.loss)]
    if c.topology == "single":
        return bare_partner(ok, c)
    if c.topology == "single2":
        pk = {p.key for p in c.combo.a_pedals}
        same = [x for x in ok if x.topology == "single" and x.combo.a_amp.key == c.combo.a_amp.key]
        pref = [x for x in same if x.combo.a_pedals and x.combo.a_pedals[0].key in pk and bool(x.combo.boost) == bool(c.combo.boost)]
        return min(pref or same, key=lambda x: x.loss, default=None)
    return min([x for x in ok if x.topology in ("single", "single2")], key=lambda x: x.loss, default=None)


def confirm_partner(c: Scored, b: Scored, refit, starts: int | None = None) -> tuple[Scored, dict]:
    """Occam confirmation, shared by the pedal / single2 / blend decisions: if the complex candidate ``c`` is contested against the
    simpler ``b`` (``occam_contested``), refit ``b`` ``starts`` (OCCAM_CONFIRM_STARTS) more times (``refit(j)`` -> the refitted
    partner, a Scored) and keep the best fit. Returns (partner to use, record for result.json)."""
    starts = OCCAM_CONFIRM_STARTS if starts is None else starts
    rec = {"decision": occam_decision(c, b), "complex": list(c.combo.key()), "partner": list(b.combo.key()), "complexLoss": c.loss,
           "partnerLoss": b.loss, "marginDb": occam_margin(c, b), "extraFits": []}
    if occam_contested(c, b):
        for j in range(starts):
            nb = refit(j)
            rec["extraFits"].append(nb.loss)
            if nb.loss < b.loss - 1e-9:
                b = nb
        rec["partnerLossAfter"] = b.loss
        rec["stillJustified"] = bool(b.loss - c.loss > occam_margin(c, b))
    return b, rec


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
    allowed = allowed_topologies(cfg.topology)           # raises ValueError for a bad mode
    plan = dataclasses.replace(plan, top_k={t: (k if t in allowed else 0) for t, k in plan.top_k.items()}, boost=plan.boost and "boost" not in abl, filters=plan.filters and "filters" not in abl,
                               cab_sweep=plan.cab_sweep and "irsweep" not in abl)
    rng = np.random.default_rng(cfg.seed)
    ref, pool = cfg.ref, cfg.pool
    lib = cfg.ir_library if "irsweep" not in abl else None
    if lib is not None and not pool.cabs and len(lib.records):
        pool.cabs.append(default_cab(lib.captures()))      # no TONE3000 cab in the pool: the library supplies the stage-1 cab
    if not (pool.amps and pool.cabs):
        raise ValueError(f"pool needs amps and cabs, got {pool.counts()}")
    log(f"pool {pool.counts()} seed {cfg.seed} budget {cfg.budget} plan {plan}")

    di_x, di_fs = sf.read(str(cfg.di), dtype="float32")
    if di_x.ndim > 1:
        di_x = di_x[:, 0]
    di48 = to48(di_x, di_fs)
    gfloor = gate_floor(di48, RATE)
    floor = gfloor["peakDb"] if gfloor["peakDb"] is not None else -90.0
    gate = gate_preset(floor)
    log(f"DI floor on the gate's peak envelope {floor:.1f} dBFS (RMS {gfloor['rmsDb']:.1f} dBFS, from the DI's {gfloor['source']}) "
        f"-> gate {gate}")
    eng = Engine(gate, cfg.threads)
    prog = Progress(cfg.progress_json, plan.mode).start() if cfg.progress_json else NullProgress()
    try:
        res = _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start, prog, gfloor)
        prog.close("done", res["after"][0]["aWeightedErrorDb"] if res.get("after") else None)
        return res
    except BaseException as e:
        prog.close(f"error: {e}")
        raise
    finally:
        eng.close()


def _run(cfg, plan, rng, ref, pool, di48, di_x, di_fs, gate, floor, eng, log, out, t_start, prog=None, gfloor=None):
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

    t_mark = time.time()
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

    def finish_refined(combo: Combo, base: Scored, v: dict, r: L.LossResult, info: dict, comp: dict | None = None) -> Scored:
        """Stage-2 record of ``combo`` with parameters ``v`` (alignment / levels of the stage-1 candidate ``base``)."""
        ca = eng.core(combo, v, "a", ex.x)
        cb = eng.core(combo, v, "b", ex.x) if combo.topology == "blend" else None
        y = eng.apply_comp(ex.trim(eng.emulate(combo, v, ca, cb, base.align, base.levels)), comp, combo.cab)
        g, clipped = pick_output_gain(float(np.max(np.abs(y))), r.offset_db, ref.level_offset_db)
        if base.levels is not None:      # emitted preset uses the constantLoudness law + make-up: keep the fitted level
            g -= emit_gain_correction_db(v["blend"], base.levels)
        guard = _guardrails(y, profile)
        extra = {"params": v, "outputGainDb": g, "clipped": clipped, "info": info, "guardrails": guard}
        if comp:
            extra["busComp"] = comp
        return Scored(combo, r.total, v.get("blend", 0.0), base.align, r, "refined", extra, base.levels)

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
        v, r, info = refine_combo(eng, s.combo, sp, ex, tgt, s.align, v0, seed=refine_seed(cfg.seed, s.combo),
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
    # ---- Occam confirmation (pedal vs none, single2 vs single, blend vs single) --------------------------------------------------
    # One stage-2 fit of a chain varies by ~0.1 dB (sd; up to ~0.3 range) of loss with the seed, about the size of the Occam margins, and
    # a simpler chain with the complex one's tie at stage 1 loses a coin flip (CI run 280: a pedal; single2 beating single by 0.12 dB).
    # A complex candidate that wins by its margin but by less than margin + OCCAM_NOISE_DB is only accepted once its simpler partner,
    # fitted OCCAM_CONFIRM_STARTS more times with other seeds, still loses; the best fit of the partner is kept. Robust wins (a pedal the
    # chain needs: 5.1 -> 0.5) cost nothing.
    partners: list[dict] = []
    snapshot = list(refined)        # every decision is judged on the stage-2 fits as they were, so the records do not depend on order
    order = sorted([x for x in snapshot if np.isfinite(x.loss) and (x.topology != "single" or x.combo.a_pedals)],
                   key=lambda x: (TOPOLOGY_RANK[x.topology], x.loss))
    pairs = [(c, simpler_partner(snapshot, c)) for c in order]
    fits: dict = {}                 # partner key -> its confirmation fits (Scored), shared by every decision on that partner
    n_extra = OCCAM_CONFIRM_STARTS * len({b.combo.key() for c, b in pairs if b is not None and occam_contested(c, b)})
    n_refine += n_extra             # the confirmation fits count in the progress bar and in T["stage2PerCombo"]
    for c, b in pairs:
        if b is None:
            partners.append({"decision": occam_decision(c, c), "complex": list(c.combo.key()), "partner": None,
                             "complexLoss": c.loss, "note": "no simpler partner was refined"})
            continue
        sp_b = Space.for_combo(b.combo, plan.filters)
        v0b = sp_b.default()

        def refit(j, b=b, sp_b=sp_b, v0b=v0b):
            nonlocal n_done
            have = fits.setdefault(b.combo.key(), [])
            while len(have) <= j:
                t_fit = time.time()
                v2, r2, info2 = refine_combo(eng, b.combo, sp_b, ex, tgt, b.align, v0b, seed=confirm_seed(cfg.seed, len(have)),
                                             gens_linear=plan.gens_linear, pop_linear=plan.pop_linear, gens_gain=plan.gens_gain,
                                             pop_gain=plan.pop_gain, gens_final=plan.gens_final, patience=plan.patience,
                                             patience_gain=plan.patience_gain, tol=plan.plateau_tol, gex=gex, gtgt=gtgt,
                                             short_linear=plan.short_linear, levels=b.levels, log=log)
                have.append(finish_refined(b.combo, b, v2, r2, info2))
                n_done += 1
                prog.update(REFINE_SHARE * n_done / max(n_refine, 1))
                T["stage2PerCombo"].append({"topology": b.topology, "seconds": round(time.time() - t_fit, 1), "confirm": True})
            return have[j]

        _, rec = confirm_partner(c, b, refit)
        partners.append(rec)
    for key, have in fits.items():                 # the partner's best fit replaces its stage-2 record
        i = next(i for i, x in enumerate(refined) if x.combo.key() == key)
        best_fit = min(have, key=lambda x: x.loss)
        if best_fit.loss < refined[i].loss - 1e-9:
            log(f"occam: partner {refined[i].combo.describe()} refitted {refined[i].loss:.3f} -> {best_fit.loss:.3f}")
            refined[i] = best_fit
    unrefined = sorted([c for c in unrefined if np.isfinite(c.loss)], key=lambda c: c.loss)
    refined = [c for c in refined if np.isfinite(c.loss)]
    refined.sort(key=lambda c: c.loss)

    # ---- pre-EQ before the drive (v0.4M B4): after stage 2, on the refined winner(s), confirmed by a short re-fit -------------
    t_pre = time.time()
    nums = di_spectrum_numbers(ex.x[ex.lead:], tgt.starts)
    wide = widening(nums)
    log(f"pre-EQ: DI tilt {nums['diTilt']:+.2f} dB/oct, low excess {nums['diLowExcess']:+.2f} dB; widened: "
        f"{', '.join(wide['widened']) or 'none'}")
    pre_res: dict = {**nums, "widened": wide["widened"], "ablated": "preeq" in ablate, "candidates": [], "chosen": {},
                     "gainVsOff": 0.0, "keepDb": PRE_CONFIRM_DB, "grid": {}}
    pre_gain: dict = {}
    if "preeq" not in ablate:
        n_per = 1 if plan.mode == "quick" else 2            # quick: the winner only; thorough: also the runner-up
        for topo in TOPOLOGIES:
            for c in sorted((x for x in refined if x.topology == topo), key=lambda x: x.loss)[:n_per]:
                sp = Space.for_combo(c.combo, plan.filters)
                rec = preeq_candidate(eng, c, sp, ex, tgt, wide, v0=c.extra["params"])
                entry = {k: rec[k] for k in ("topology", "captures", "renders", "paths")}
                entry.update(offLoss=c.loss, gridBest=rec["loss"], kept=False, refitLoss=None, gain=0.0, pairKey=list(c.combo.pair_key()))
                if rec["gainVsOff"] > 0:
                    v0 = {**c.extra["params"], **rec["params"]}
                    v_new, r_new, info_new = refine_combo(
                        eng, c.combo, sp, ex, tgt, c.align, v0, seed=cfg.seed * 1000 + SEED_PREEQ + len(pre_res["candidates"]),
                        gens_linear=PRE_REFIT_L1, pop_linear=plan.pop_linear, gens_gain=max(2, plan.gens_gain // 2),
                        pop_gain=plan.pop_gain, gens_final=max(3, plan.gens_final // 2), patience=plan.patience,
                        patience_gain=plan.patience_gain, tol=plan.plateau_tol, gex=gex, gtgt=gtgt,
                        short_linear=plan.short_linear, levels=c.levels, staged=False, log=lambda *_: None)
                    entry.update(refitLoss=r_new.total, gain=c.loss - r_new.total)
                    if c.loss - r_new.total >= PRE_CONFIRM_DB:
                        new = finish_refined(c.combo, c, v_new, r_new, info_new)
                        refined[next(i for i, x in enumerate(refined) if x is c)] = new
                        entry["kept"] = True
                        pre_gain[c.combo.pair_key()] = c.loss - r_new.total
                        log(f"pre-EQ {topo}: " + "; ".join(f"path {p} {x['chosen']}" for p, x in rec["paths"].items())
                            + f" kept after the re-fit (refined loss {c.loss:.3f} -> {r_new.total:.3f})")
                    else:
                        log(f"pre-EQ {topo}: grid pick ({'; '.join(x['chosen'] for x in rec['paths'].values())}) not confirmed "
                            f"(refined loss {c.loss:.3f} vs {r_new.total:.3f} with it): pre-EQ stays off")
                pre_res["candidates"].append(entry)
        refined.sort(key=lambda c: c.loss)
    T["preEq"] = time.time() - t_pre

    # ---- cab / IR breadth (v0.4M Task B): every pool cab on the top candidates per topology --------------------------
    cab_sweeps: list[dict] = []
    sw_caps: dict = {}
    t_cab = time.time()
    lib = cfg.ir_library if plan.cab_sweep else None
    bank, ir_pool = None, {"ablated": not plan.cab_sweep, "local": 0, "tone3000": len(pool.cabs), "total": len(pool.cabs),
                           "skipped": 0, "dirs": [dict(d) for d in cfg.ir_dirs], "screened": False, "screenSeconds": 0.0, "prefiltered": False}
    if plan.cab_sweep and (lib is not None or len(pool.cabs) > irscreen.TOP_N):
        t_bank = time.time()
        bank = irscreen.Bank.build(lib, pool.cabs)
        ir_pool.update(screened=True, local=bank.n_local, tone3000=bank.n_pool, total=len(bank), skipped=len(bank.skipped),
                       skippedReasons=bank.skipped[:20], screenMax=cfg.ir_screen_max, topN=irscreen.TOP_N,
                       bankSeconds=round(time.time() - t_bank, 2),
                       libraryReport={k: lib.report[k] for k in ("accepted", "exactDuplicates", "nearDuplicates", "rejectedTotal")}
                       if lib is not None else None)
        log(f"IR pool: {bank.n_local} local + {bank.n_pool} TONE3000 = {len(bank)} IRs ({len(bank.skipped)} skipped), "
            f"analytic screen -> top {irscreen.TOP_N} per candidate")
    if plan.cab_sweep:
        n_sw = max(1, sum(min(TOP_PER_TOPOLOGY, sum(1 for x in refined if x.topology == t)) for t in TOPOLOGIES))
        for topo in TOPOLOGIES:
            for c in sorted((x for x in refined if x.topology == topo), key=lambda x: x.loss)[:TOP_PER_TOPOLOGY]:
                sp = Space.for_combo(c.combo, plan.filters)
                screen_rec = None
                if bank is not None:
                    caps, screen_rec = irscreen.candidate_irs(bank, eng, c, ex, tgt, top_n=irscreen.TOP_N,
                                                              screen_max=cfg.ir_screen_max, seed=cfg.seed)
                    ir_pool["screenSeconds"] += screen_rec["seconds"]
                    ir_pool["prefiltered"] = ir_pool["prefiltered"] or screen_rec["prefilter"]["prefiltered"]
                else:
                    caps = pool.cabs
                rows = cab_sweep(eng, c, caps, sp, ex, tgt)
                summ = sweep_summary(c, rows)
                summ["pairKey"] = list(c.combo.pair_key())      # identity of the swept candidate (the cab is what the sweep changes)
                if screen_rec is not None:
                    full = sorted(rows, key=lambda r: r["result"].total)
                    summ["screen"] = {**screen_rec, "fullTop6": [r["cab"].key for r in full[:irscreen.TOP_PAIR]]}
                cur = next(x for x in rows if x["cab"].key == c.combo.cab.key)
                top = min(rows, key=lambda x: x["result"].total)
                if top["cab"].key != cur["cab"].key and top["result"].total < cur["result"].total - CAB_SWITCH_DB:
                    combo2 = c.combo.with_cab(top["cab"])
                    v2, r2 = relinear(eng, combo2, sp, ex, tgt, c.align, c.extra["params"], levels=c.levels,
                                      seed=cfg.seed * 1000 + SEED_SWEEP + len(cab_sweeps), gens=plan.gens_final, pop=plan.pop_linear,
                                      patience=plan.patience, tol=plan.plateau_tol, log=log)
                    new = finish_refined(combo2, c, v2, r2, c.extra["info"])
                    refined[next(i for i, x in enumerate(refined) if x is c)] = new
                    summ.update(changed=True, newCab=top["cab"].key, lossBeforeRelinear=top["result"].total,
                                lossAfterRelinear=r2.total)
                    log(f"cab sweep {topo}: {cur['cab'].title}:{cur['cab'].name} -> {top['cab'].title}:{top['cab'].name} "
                        f"(loss {cur['result'].total:.3f} -> {top['result'].total:.3f}, relinear {r2.total:.3f})")
                sw_caps[c.combo.pair_key()] = {x.key: x for x in caps}      # per CANDIDATE: each one screens its own top IRs
                cab_sweeps.append(summ)
                prog.update(REFINE_SHARE + 0.03 * len(cab_sweeps) / n_sw)
        refined.sort(key=lambda c: c.loss)
        result["cabSweep"] = {"ablated": False, "topPerTopology": TOP_PER_TOPOLOGY, "poolCabs": len(pool.cabs),
                              "candidates": cab_sweeps, "seconds": round(time.time() - t_cab, 2)}
        ir_pool["screenSeconds"] = round(ir_pool["screenSeconds"], 2)
    else:
        result["cabSweep"] = {"ablated": True, "note": "--ablate irsweep: only the stage-1 cab sweep ran"}
    T["cabSweep"] = time.time() - t_cab
    best = choose(refined, cfg.topology)
    result["pedalOccam"] = {"minGainDb": PEDAL_OCCAM_DB, "dropped": best.extra.get("pedalOccamDropped", []),
                            "partners": partners}
    # ---- two-IR blend (v0.4M B2.1): the winner's cab as one combined irMix IR of two of the top IRs --------------------
    t_ir = time.time()
    if "irblend" in ablate:
        result["irBlend"] = {"ablated": True, "tried": 0, "won": False}
    else:
        # the winner's OWN sweep and capture set: matched by candidate identity (pair key), never by topology / boost (several
        # candidates of one topology are swept and each screens a different top-N of the IR bank)
        pk = best.combo.pair_key()
        sw = next((x for x in cab_sweeps if tuple(x["pairKey"]) == pk), None)
        bykey = {**{c.key: c for c in pool.cabs}, **(sw_caps.get(pk) or {})}
        if sw and sw.get("screen"):            # B3's analytic screen: its full-render top 6 feed the pair search
            keys = list(sw["screen"]["fullTop6"])
        else:
            keys = [i["cab"] for i in sw["irs"]][:TOP_IRS] if sw else [c.key for c in pool.cabs][:TOP_IRS]
        missing = [k for k in keys if k not in bykey]
        if missing:                            # internal invariant: the keys come from this candidate's own sweep
            raise RuntimeError(f"two-IR blend: IRs {missing} are not among the captures swept for the winning candidate {pk}")
        irb = pair_search(eng, best, Space.for_combo(best.combo, plan.filters), ex, tgt, [bykey[k] for k in keys],
                          seed=cfg.seed * 1000 + SEED_FINAL, gens=plan.gens_final, pop=plan.pop_linear, patience=plan.patience,
                          tol=plan.plateau_tol, log=log)
        won_pair = irb.pop("_won", None)
        irb["ablated"] = False
        irb["candidatePairKey"] = list(pk)
        irb["sweepFound"] = sw is not None
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
    ir_pool["winner"] = {"key": best.combo.cab.key, "title": best.combo.cab.title, **best.combo.cab.source_info()}
    if best.combo.cab_b is not None:           # a winning two-IR blend: both IRs are recorded
        ir_pool["winner"]["irB"] = {"key": best.combo.cab_b.key, "title": best.combo.cab_b.title,
                                    **best.combo.cab_b.source_info(), "offsetSamplesB": best.combo.cab_offset,
                                    "invertB": best.combo.cab_invert, "mix": best.extra["params"].get("cab.mix")}
    result["irPool"] = ir_pool
    # ---- studio processing in the reference (v0.4M B2.3): detect, then bus comp / wider post EQ ----------------------------
    t_st = time.time()
    sp_st = Space.for_combo(best.combo, plan.filters)
    paths_st = ("a", "b") if best.combo.topology == "blend" else ("a",)
    cores_st = [eng.core(best.combo, best.extra["params"], p, ex.x) for p in paths_st]
    y_st0 = ex.trim(eng.emulate(best.combo, best.extra["params"], cores_st[0], cores_st[1] if len(cores_st) > 1 else None,
                                best.align, best.levels))
    ref_sig = tgt_full.matched if tgt_full.matched is not None else (ref.ltas_sig if ref.clean else None)
    det = detect_studio(bool(ref.clean), tgt_full, tgt, y_st0, best.extra["params"], ref_sig)
    studio = {**det, "ablated": "studio" in ablate, "busCompUsed": False}
    if (det["compressed"] or det["eqd"]) and "studio" not in ablate:
        srec = studio_stage(eng, best, sp_st, ex, tgt, det, seed=cfg.seed * 1000 + SEED_STUDIO, gens=plan.gens_final, pop=plan.pop_linear,
                            patience=plan.patience, tol=plan.plateau_tol, log=log)
        won_st = srec.pop("_won", None)
        studio.update(srec)
        if won_st is not None:
            v_st, r_st, comp_st = won_st
            new = finish_refined(best.combo, best, v_st, r_st, best.extra["info"], comp_st)
            log(f"studio processing: loss {best.loss:.3f} -> {new.loss:.3f} (bus comp {'yes' if comp_st else 'no'}, "
                f"post EQ widened {studio.get('widenedPostEq')})")
            refined[next(i for i, x in enumerate(refined) if x is best)] = new
            best = new
    result["studio"] = studio
    T["studio"] = time.time() - t_st
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
    result["topology"] = {**topology_margin(refined), "mode": cfg.topology, "blendOccamDb": BLEND_OCCAM_DB}
    tm = result["topology"]
    log("topology margin: " + (f"best single {tm['bestSingle']:.3f}, best blend {tm['bestBlend']:.3f}, delta {tm['deltaPct']:+.1f} % -> "
                               + ("determined" if tm["determined"] else "topology not determined")
                               if tm["deltaPct"] is not None else f"{tm.get('note')}"))
    log("topology bests: " + ", ".join(f"{t} {d['loss']:.3f}" for t, d in result["topologies"].items()))
    log(f"selected: {best.combo.describe()} loss {best.loss:.3f}")

    # ---- gate matched to the reference (v0.4M Task B): sweep threshold x release on the final chain ----------------
    t_gate = time.time()
    prog.update(REFINE_SHARE + 0.03)
    gate_final = gate
    sp_best = Space.for_combo(best.combo, plan.filters)
    ref_floor = (reference_floor_db(ref.ltas_sig)
                 if (tgt_full.feel is not None and tgt_full.feel.mode == "soft" and ref.clean) else None)
    gs = gate_sweep(eng, best, sp_best, ex, tgt_full, floor, ref_floor, ref_clean=bool(ref.clean),
                    full={"di": di48, "ref": ref.matched_sig, "offset": int(ref.offset_samples)})     # H.2: gaps of the full DI
    if gs.get("gapSource") == "fullDi":
        log(f"gate sweep: the excerpt has no DI gaps; using {len(gs.get('gapWindows', []))} gap window(s) of the full-length DI")
    result["gateSweep"] = gs
    result["gateDefault"] = gate
    result["gateFloor"] = {"rmsDb": None if gfloor is None else gfloor["rmsDb"], "peakDb": floor,
                           **({} if gfloor is None else {"source": gfloor["source"]}),
                           "definition": "peakDb = 92.5th percentile of the gate's own peak envelope over the DI gaps (core peak_floor_db); "
                                         "default gate opens at peakDb + 10 dB, closes at + 4"}
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
        log("gate sweep: " + (f"skipped ({gs['skipped']})" if gs.get("skipped") else "the default gate (peak floor + 10 dB, 150 ms) stays"))
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
    pre_res["chosen"] = {p: describe_pre(tuple(float(bp.get(k, d)) for k, d in (
        (f"pre.{p}.hpf", 0.0), (f"pre.{p}.mid_db", 0.0), (f"pre.{p}.mid_hz", 800.0), (f"pre.{p}.shelf_db", 0.0))))
        for p in (("a", "b") if best.combo.topology == "blend" else ("a",))}
    pre_res["gainVsOff"] = pre_gain.get(best.combo.pair_key(), 0.0)
    pre_res["grid"] = next((c["paths"] for c in pre_res["candidates"] if c["topology"] == best.topology), {})   # the winner topology's grid, per path
    result["preEq"] = pre_res
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
                         name="Sawblade match", notes=_notes(cfg, ref, best), levels=best.levels,
                         bus_comp=best.extra.get("busComp"), emit=True)     # emitted: origin match, dynamicsMode live (Task G)
    result["dynamics"] = {"scoredWith": "record", "emittedMode": "live", "origin": "match"}
    full_jobs = {"best_L": (final, cfg.di)}
    if plan.mode != "quick" or cfg.write_audio:     # quick: no full-length "before" render (the excerpt loss has it)
        full_jobs["starter_L"] = (starter_p, cfg.di)
    if cfg.write_audio:             # listening: the live dynamics set (what a rig plays) next to render.wav (record set)
        full_jobs["live_L"] = (final, cfg.di)
    if cfg.di_r is not None:       # always: the clip guard (peak = max of L/R) and the R offset refinement need it
        full_jobs["best_R"] = (final, cfg.di_r)
    log(f"stage3: full-length renders {list(full_jobs)}")

    def full_render(item):
        name, (preset, path) = item
        x, fs = sf.read(str(path), dtype="float32")
        x = x if x.ndim == 1 else x[:, 0]
        y, rep = eng.render(preset, x, fs, dynamics="live" if name == "live_L" else "record")   # all scoring: record set
        return name, y, fs, rep

    renders = {n: (y, fs, rep) for n, y, fs, rep in eng.map(full_render, list(full_jobs.items()))}
    lap("fullRenders")
    prog.update(0.5, "measuring the result")
    # The guard acts on the RECORD renders only, exactly as before G.4: the live render exists only with --listen, and the emitted
    # gainDb, the cut and the level-dependent tonecheck numbers must not depend on that. The live peak is reported and warned about.
    peaks = {n: float(np.max(np.abs(renders[n][0]))) for n in renders if n.startswith("best")}
    peak = max(peaks.values())
    live_peak = float(np.max(np.abs(renders["live_L"][0]))) if "live_L" in renders else None
    db = lambda p_: float(20 * np.log10(max(p_, 1e-12)))
    best.extra["fullLengthPeakDbfs"] = {**{n: db(p_) for n, p_ in peaks.items()}, **({} if live_peak is None else {"live_L": db(live_peak)})}
    cut = 0.0
    if peak >= 10 ** (CLIP_GUARD_DBFS / 20):
        cut = 20 * np.log10(10 ** (CLIP_GUARD_DBFS / 20) / peak)
        gain_db += cut
        final["output"]["gainDb"] = float(gain_db)
        for n in peaks:
            y, fs, rep = renders[n]
            renders[n] = ((y * 10 ** (cut / 20)).astype(np.float32), fs, rep)
        log(f"clip guard: full-length peak (max of L/R) {20 * np.log10(peak):.1f} dBFS -> output gain lowered by {-cut:.1f} dB")
        best.extra["clipGuardDb"] = float(cut)
    if live_peak is not None:                  # same gain as the record render (the preset's gain), reported, never steering it
        y, fs, rep = renders["live_L"]
        renders["live_L"] = ((y * 10 ** (cut / 20)).astype(np.float32), fs, rep)
        live_after = live_peak * 10 ** (cut / 20)
        if live_after >= 10 ** (CLIP_GUARD_DBFS / 20):
            log(f"warning: the live-dynamics render peaks at {db(live_after):.1f} dBFS (above the {CLIP_GUARD_DBFS:g} dBFS guard ceiling; "
                "the record set's compression holds the scored renders lower). The output gain is NOT changed for it.")
            best.extra["liveClipWarning"] = True
    best.extra["fullLengthPeakAfterGuardDbfs"] = {n: float(20 * np.log10(max(float(np.max(np.abs(renders[n][0]))), 1e-12)))
                                                  for n in list(peaks) + (["live_L"] if live_peak is not None else [])}
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
                              levels=s.levels, emit=True)
        (out / f"alt{i}.preset.resolved.json").write_text(json.dumps(preset, indent=2) + "\n")
        alts.append({**_scored_json(s), "file": f"alt{i}.preset.resolved.json"})
    result["alternatives"] = alts
    result["best"] = {**_scored_json(best), "params": v, "outputGainDb": gain_db, "preset": "best.preset.resolved.json"}
    result["best"]["captures"] = caps_summary(best.combo)
    result["best"]["fullLengthPeakDbfs"] = best.extra["fullLengthPeakDbfs"]
    result["best"]["fullLengthPeakAfterGuardDbfs"] = best.extra["fullLengthPeakAfterGuardDbfs"]
    result["best"]["clipGuardDb"] = best.extra.get("clipGuardDb", 0.0)
    result["best"]["liveClipWarning"] = bool(best.extra.get("liveClipWarning"))      # the live render peaks above the guard ceiling
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
         "pairKey": list(s.combo.pair_key()), "tightBoost": bool(s.combo.boost), "irMix": None if s.combo.cab_b is None else
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
            "Captures are referenced by TONE3000 ids; exported/derived models are for the user's own use only."
            + (" bus comp added by the matcher (studio processing); dropped from no-cab exports"
               if best.extra.get("busComp") else ""))


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
            live48 = to48(renders["live_L"][0], renders["live_L"][1]) if "live_L" in renders else None
            if live48 is not None:      # the live dynamics set: same section, the SAME gain as render.wav (not re-matched)
                live_sec = live48[a:b] * 10 ** (gain_db / 20)
                sf.write(str(d / "render_live.wav"), live_sec.astype(np.float32), RATE, subtype="FLOAT")
            tp = {"ref": true_peak_db(ref_seg), "render": true_peak_db(sec * 10 ** (gain_db / 20))}
            info.update({"loudnessMatched": True, "section": [a / RATE, b / RATE], "lufsRef": l_ref, "lufsRenderRaw": l_raw,
                         "gainDb": gain_db, "offsetMs": 1000.0 * offset / RATE if matched else None,
                         "referenceSignal": ref_sig, "truePeakDb": tp,
                         "files": {"ref": str(d / "ref.wav"), "render": str(d / "render.wav")}})
            if not matched:
                info["refSection"] = [ra / RATE, rb / RATE]
            if live48 is not None:
                info["files"]["renderLive"] = str(d / "render_live.wav")
                info["truePeakDb"]["renderLive"] = true_peak_db(live_sec)
                info["lufsRenderLive"] = match_gain_db(ref_seg, live48[a:b], RATE)[2]
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
            for f in ("ref.wav", "render.wav", "render_live.wav", "before.wav"):
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
