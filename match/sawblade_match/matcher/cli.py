"""``sawblade-match`` command line (spec phase3 3.2)."""
from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Sequence

from . import irlib, irscreen
from .calibration import CALIBRATION_MODES, DEFAULT_CALIBRATION, DI_CHANNEL_RULES, CalibrationOptions
from .pool import load_pool
from .reference import load_reference
from .run import ABLATIONS, BLEND_OCCAM_DB, Config, Log, TOPOLOGY_CHOICES, parse_ablate, run_match


def _section(s: str) -> tuple[float, float]:
    a, b = s.split(":")
    return float(a), float(b)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-match", description="Match a DI to a reference with TONE3000 captures")
    p.add_argument("--di", help="DI WAV (left guitar / mono)")
    p.add_argument("--di-channel", default="auto", choices=list(DI_CHANNEL_RULES),
                   help="stereo DI files (and --di-r): which channel is the guitar. auto = the louder of channels 1/2 by whole-file "
                        "RMS (the same rule as tonerender; a tie picks L), L, R, or mix = their mean. The rule used is in result.json")
    p.add_argument("--calibration", default=DEFAULT_CALIBRATION, choices=list(CALIBRATION_MODES),
                   help="input calibration: calibrated = every capture is driven at the level its dBu metadata implies for the "
                        "interface (see --device-dbu); legacy = no calibration (the pre-v0.8 render). Emitted presets (v5) carry "
                        f"this as calibration.mode (default: {DEFAULT_CALIBRATION})")
    p.add_argument("--device-dbu", type=float, metavar="DBU",
                   help="interface level, dBu at 0 dBFS (-60..60); no effect with --calibration legacy. Absent: the assumed "
                        "+12 dBu, recorded as assumed in result.json")
    p.add_argument("--di-r", help="second DI (right double); rendered through the same preset for the stereo listening file")
    p.add_argument("--ref", help="reference audio (WAV/MP3)")
    p.add_argument("--ref-section", action="append", type=_section, metavar="A:B",
                   help="seconds of the reference to use as the guitar-dominant LTAS target (repeatable; unmatched refs only)")
    p.add_argument("--ref-channel", default="auto", choices=["auto", "side", "left", "right", "mid"],
                   help="LTAS target signal: auto = cached htdemucs stem if present else side (L-R)/2 for stereo")
    p.add_argument("--ref-hf-limit", type=float, metavar="HZ",
                   help="full-mix reference only: LTAS above HZ is a one-sided ceiling (default 4500 for the automatic/side "
                        "fallback, off for explicit left/right/mid; 0 = off)")
    p.add_argument("--ref-clean", action=argparse.BooleanOptionalAction, default=None,
                   help="the reference is an isolated guitar track (e.g. the amp print of the same take), not a mix: "
                        "no stem lookup, no HF limit, its 5-12 kHz / low end / inter-note floor are targets. Implied by "
                        "--matched mono; --no-ref-clean (or --ref-mix) declares a mono matched file a full mix")
    p.add_argument("--ref-mix", dest="ref_clean", action="store_const", const=False, help=argparse.SUPPRESS)
    p.add_argument("--stems-dir", help="directory with sawblade-calibrate stems (default testdata/stems)")
    p.add_argument("--matched", choices=["left", "right", "mono"],
                   help="the reference is a matched pair with --di (time-aligned STFT term vs this channel)")
    p.add_argument("--offset-ms", type=float, help="coarse DI offset within the reference (default: unknown; a DI shorter than the reference is placed by a whole-song envelope search, otherwise searched within +-3 s; refined either way)")
    p.add_argument("--pool", help="pool_manifest.json from sawblade-t3k pull (captures must be downloaded)")
    p.add_argument("--out", help="output directory (default ~/.cache/sawblade/match_runs/<timestamp>)")
    p.add_argument("--budget", type=float, default=1.0, help="work scale (default 1.0: all pedal x amp pairs of the current pool)")
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--excerpt-s", type=float, default=6.0, help="screening/optimisation excerpt length (<= 8 s)")
    p.add_argument("--excerpt-window", type=_section, metavar="A:B", help="explicit excerpt window in DI seconds")
    p.add_argument("--top-k", type=int, default=3,
                   help="combos refined by CMA-ES: N blend, min(N,2) single, 1 two-pedal (default 3)")
    p.add_argument("--prescreen", type=int, metavar="N",
                   help="force the per-capture pre-screen, keeping the top N of every gear class (default: automatic, "
                        "only when the pair product exceeds the pair cap)")
    p.add_argument("--profile", default="derived",
                   help="guardrail profile: 'derived' (default, from the reference's guitars), a profile id in profiles/, or a path")
    p.add_argument("--base-profile", default="swedish_death_hm2", help="rule skeleton for --profile derived")
    p.add_argument("--threads", "--jobs", dest="threads", type=int, default=4,
                   help="parallel render workers (default 4). Threads, not processes: the C++ renderer releases the GIL "
                        "and scales to the core count, so a process pool only adds copies of the models and signals")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true",
                      help="fast preset (target <= 5 min on 4 cores): blend-aware pre-screen, coarse 2 s pair screen then "
                           "full pass on the top 10 %%, fewer re-scores/cab sweeps, smaller CMA-ES budgets with a plateau stop")
    mode.add_argument("--thorough", action="store_true", help="the full search (default): every pair of the capped pool on the "
                      "full excerpt, full CMA-ES budgets")
    p.add_argument("--progress-json", metavar="PATH",
                   help="write {stage, fraction, etaSeconds, bestErrorDb, message} to PATH (atomically, at least once per "
                        "second) for the plugin's progress bar")
    p.add_argument("--listen", action="store_true",
                   help="also render the listening files (full-length R render, stereo WAV/MP3); off by default")
    p.add_argument("--topology", choices=TOPOLOGY_CHOICES, default="auto",
                   help="restrict the search to the single-path topologies (single) or to the blend; auto (default) searches all "
                        "and applies the topology margin (a single beats a blend within %.2f dB)" % BLEND_OCCAM_DB)
    p.add_argument("--ablate", metavar="LIST", default="",
                   help="v0.4M on/off pairs: comma list of suspects to switch OFF (" + ", ".join(ABLATIONS) + "): feel = no feel "
                        "term in the loss; boost = no tight-boost variants; filters = no post-cab hp / low-pass slope; irsweep = only the "
                        "stage-1 cab sweep (the pre-v0.4M behaviour); irblend = no two-IR blend of the winner's cab; studio = detect "
                        "studio processing (always reported) but do not add a bus comp / wider post EQ; preeq = no pre-EQ grid before the amp")
    p.add_argument("--trace-tones", metavar="ID[,ID...]", default="",
                   help="TONE3000 tone ids to explain in result.json -> trace[id]: downloaded?, models, gear class, pre-screen "
                        "rank/score/survived, best pair, best candidate loss with it as the amp, and why it lost")
    p.add_argument("--ir-dir", action="append", metavar="DIR", default=[],
                   help="a directory of your own IRs (repeatable; recursive; .wav/.aif/.flac), added to the dirs in "
                        "~/.config/sawblade/ir_dirs.json. They are indexed once (~/.cache/sawblade/ir_index.json), screened "
                        "analytically together with the pool's cabs, and the top " + str(irscreen.TOP_N) + " per candidate get "
                        "the full-loss sweep. Never uploaded or committed; licence 'user-owned'")
    p.add_argument("--no-ir-dirs", action="store_true",
                   help="ignore ~/.config/sawblade/ir_dirs.json for this run (only --ir-dir directories are used)")
    p.add_argument("--ir-dirs-add", metavar="DIR", help="add DIR to ~/.config/sawblade/ir_dirs.json and exit")
    p.add_argument("--ir-dirs-list", action="store_true", help="print the persistent IR directories and exit")
    p.add_argument("--ir-screen-max", type=int, default=irscreen.SCREEN_MAX, metavar="N",
                   help="above N IRs the analytic screen is prefiltered (tags matching the candidate's cab, then k-means over "
                        "the spectral families), never randomly sampled (default %(default)s)")
    p.add_argument("--targets", help="(deprecated alias) path of the base profile / tone-targets file")
    p.add_argument("--no-audio", action="store_true", help=argparse.SUPPRESS)      # deprecated no-op (listening is off unless --listen)
    return p


def parse_tone_ids(spec: str) -> tuple[int, ...]:
    """``--trace-tones`` value -> tone ids (order kept, duplicates dropped)."""
    out: list[int] = []
    for x in (spec or "").split(","):
        x = x.strip()
        if not x:
            continue
        try:
            i = int(x)
        except ValueError:
            raise ValueError(f"--trace-tones: {x!r} is not a TONE3000 tone id (an integer)") from None
        if i not in out:
            out.append(i)
    return tuple(out)


def resolve_ir_dirs(cli_dirs: Sequence[str], no_config: bool = False) -> list[dict]:
    """The IR directories of a run with where each came from: ``cli`` (--ir-dir) first, then ``config``
    (~/.config/sawblade/ir_dirs.json, skipped by --no-ir-dirs). A directory given both ways is listed once, as cli."""
    out: list[dict] = []
    for d in cli_dirs or []:
        if all(x["path"] != d for x in out):
            out.append({"path": d, "source": "cli"})
    if not no_config:
        for d in irlib.load_dirs():
            if all(x["path"] != d for x in out):
                out.append({"path": d, "source": "config"})
    return out


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    a = parser.parse_args(argv)
    if a.ir_dirs_add or a.ir_dirs_list:
        try:
            dirs = irlib.add_dir(a.ir_dirs_add) if a.ir_dirs_add else irlib.load_dirs()
        except OSError as e:
            print("error: " + str(e), file=sys.stderr)
            return 3
        print("\n".join(dirs) if dirs else "(no IR directories configured)")
        return 0
    missing = [f for f, v in (("--di", a.di), ("--ref", a.ref), ("--pool", a.pool)) if not v]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    try:
        if a.excerpt_s > 8.0:
            raise ValueError("--excerpt-s must be <= 8 (spec: short excerpts)")
        ablate = parse_ablate(a.ablate)
        trace = parse_tone_ids(a.trace_tones)
        out = Path(a.out) if a.out else Path.home() / ".cache" / "sawblade" / "match_runs" / datetime.now().strftime("%Y%m%d-%H%M%S")
        t_load = time.time()
        pool = load_pool(a.pool)
        ir_dirs_info = resolve_ir_dirs(a.ir_dir, a.no_ir_dirs)
        ir_dirs = [d["path"] for d in ir_dirs_info]
        library = None
        if ir_dirs:
            library = irlib.scan(ir_dirs, progress=irlib.stderr_progress)
            r = library.report
            print(f"IR library: {r['unique']} unique IRs from {', '.join(ir_dirs)} ({r['accepted']} accepted, "
                  f"{r['exactDuplicates']} exact + {r['nearDuplicates']} near duplicates, {r['rejectedTotal']} rejected)")
            if not library.records:
                library = None
        ref = load_reference(a.ref, channel=a.ref_channel, stems_dir=Path(a.stems_dir) if a.stems_dir else None,
                             matched=a.matched, offset_ms=a.offset_ms, sections=a.ref_section,
                             hf_limit_hz="auto" if a.ref_hf_limit is None else (a.ref_hf_limit or None),
                             clean=a.ref_clean)
        cfg = Config(di=Path(a.di), ref=ref, pool=pool, out=out, di_r=Path(a.di_r) if a.di_r else None,
                     budget=a.budget, seed=a.seed, excerpt_s=a.excerpt_s, top_k=a.top_k, prescreen_n=a.prescreen, profile=a.profile,
                     base_profile=a.base_profile, threads=a.threads,
                     targets=Path(a.targets) if a.targets else None, window_s=a.excerpt_window,
                     write_audio=a.listen and not a.no_audio, quick=a.quick,
                     progress_json=Path(a.progress_json) if a.progress_json else None,
                     timings_pre={"referenceLoad": time.time() - t_load}, ablate=ablate, trace_tones=trace, topology=a.topology,
                     ir_library=library, ir_screen_max=a.ir_screen_max, ir_dirs=tuple(ir_dirs_info),
                     calibration=CalibrationOptions(a.calibration, a.device_dbu), di_channel=a.di_channel)
        run_match(cfg, Log())
        return 0
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
