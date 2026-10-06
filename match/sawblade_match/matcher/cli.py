"""``sawblade-match`` command line (spec phase3 3.2)."""
from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Sequence

from .pool import load_pool
from .reference import load_reference
from .run import Config, Log, run_match


def _section(s: str) -> tuple[float, float]:
    a, b = s.split(":")
    return float(a), float(b)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-match", description="Match a DI to a reference with TONE3000 captures")
    p.add_argument("--di", required=True, help="DI WAV (left guitar / mono)")
    p.add_argument("--di-r", help="second DI (right double); rendered through the same preset for the stereo listening file")
    p.add_argument("--ref", required=True, help="reference audio (WAV/MP3)")
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
    p.add_argument("--offset-ms", type=float, help="coarse DI offset within the reference (default: unknown, searched within +-3 s and refined)")
    p.add_argument("--pool", required=True, help="pool_manifest.json from sawblade-t3k pull (captures must be downloaded)")
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
    p.add_argument("--targets", help="(deprecated alias) path of the base profile / tone-targets file")
    p.add_argument("--no-audio", action="store_true", help=argparse.SUPPRESS)      # deprecated no-op (listening is off unless --listen)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    try:
        if a.excerpt_s > 8.0:
            raise ValueError("--excerpt-s must be <= 8 (spec: short excerpts)")
        out = Path(a.out) if a.out else Path.home() / ".cache" / "sawblade" / "match_runs" / datetime.now().strftime("%Y%m%d-%H%M%S")
        t_load = time.time()
        pool = load_pool(a.pool)
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
                     timings_pre={"referenceLoad": time.time() - t_load})
        run_match(cfg, Log())
        return 0
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
