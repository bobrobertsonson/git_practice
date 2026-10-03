"""``sawblade-match`` command line (spec phase3 3.2)."""
from __future__ import annotations

import argparse
import sys
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
    p.add_argument("--stems-dir", help="directory with sawblade-calibrate stems (default testdata/stems)")
    p.add_argument("--matched", choices=["left", "right", "mono"],
                   help="the reference is a matched pair with --di (time-aligned STFT term vs this channel)")
    p.add_argument("--offset-ms", type=float, help="coarse DI offset within the reference (default 190 left / 175 right)")
    p.add_argument("--pool", required=True, help="pool_manifest.json from sawblade-t3k pull (captures must be downloaded)")
    p.add_argument("--out", help="output directory (default ~/.cache/sawblade/match_runs/<timestamp>)")
    p.add_argument("--budget", type=float, default=1.0, help="work scale (default 1.0 = about 45 min on 4 cores)")
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--excerpt-s", type=float, default=6.0, help="screening/optimisation excerpt length (<= 8 s)")
    p.add_argument("--excerpt-window", type=_section, metavar="A:B", help="explicit excerpt window in DI seconds")
    p.add_argument("--top-k", type=int, default=3, help="combos refined by CMA-ES")
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--targets", help="docs/tone_targets.json")
    p.add_argument("--no-audio", action="store_true", help="skip the listening WAV/MP3")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    try:
        if a.excerpt_s > 8.0:
            raise ValueError("--excerpt-s must be <= 8 (spec: short excerpts)")
        out = Path(a.out) if a.out else Path.home() / ".cache" / "sawblade" / "match_runs" / datetime.now().strftime("%Y%m%d-%H%M%S")
        pool = load_pool(a.pool)
        ref = load_reference(a.ref, channel=a.ref_channel, stems_dir=Path(a.stems_dir) if a.stems_dir else None,
                             matched=a.matched, offset_ms=a.offset_ms, sections=a.ref_section)
        cfg = Config(di=Path(a.di), ref=ref, pool=pool, out=out, di_r=Path(a.di_r) if a.di_r else None,
                     budget=a.budget, seed=a.seed, excerpt_s=a.excerpt_s, top_k=a.top_k, threads=a.threads,
                     targets=Path(a.targets) if a.targets else None, window_s=a.excerpt_window,
                     write_audio=not a.no_audio)
        run_match(cfg, Log())
        return 0
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
