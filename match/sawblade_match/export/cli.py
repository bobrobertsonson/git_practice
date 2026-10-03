"""`sawblade-export` command line."""
from __future__ import annotations

import argparse
import sys
from typing import Sequence

from .plan import ExportRefused, LICENCE_NOTE


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-export", description="Train a NAM model of a resolved Sawblade preset.")
    p.add_argument("preset", help="resolved preset JSON (e.g. the matcher's best.preset.resolved.json)")
    p.add_argument("--mode", choices=("nocab", "withcab"), default="nocab",
                   help="nocab (default): model before the cab + IR (x) post EQ wav; withcab: the whole chain")
    p.add_argument("--size", choices=("feather", "lite", "standard"), default="standard", help="A1 WaveNet size")
    p.add_argument("--epochs", type=int, default=None, help="max epochs (default per size: see README)")
    p.add_argument("--max-minutes", type=float, default=None, help="training wall-time cap (default per size)")
    p.add_argument("--lr-gamma", type=float, default=None,
                   help="ExponentialLR gamma per epoch (default: decay to 5%% of the learning rate over --epochs)")
    p.add_argument("--seed", type=int, default=0, help="training seed (model init + batch order)")
    p.add_argument("--signal-seed", type=int, default=1, help="seed of the training-signal generator")
    p.add_argument("--threads", type=int, default=4, help="torch CPU threads")
    p.add_argument("--allow-inexact", action="store_true",
                   help="nocab with the bus comp on: drop the comp and report the error it introduces")
    p.add_argument("--target-esr", type=float, default=None, help="stop early when the validation ESR reaches this")
    p.add_argument("--out", default=None, help="output directory (default ~/.cache/sawblade/exports/<name>-<mode>-<size>-<ts>)")
    p.add_argument("--name", default=None, help="file name stem (default: slug of the preset name)")
    p.add_argument("--di", default=None, help="DI wav for the excerpt validation (default testdata/gatecreeper_cover/Guitar_L.wav)")
    p.add_argument("--no-validate", action="store_true", help="skip validation + listening file")
    p.add_argument("--keep-scratch", action="store_true", help="keep the checkpoint scratch directory")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    from .run import run_export
    try:
        rep = run_export(args.preset, mode=args.mode, size=args.size, out=args.out, name=args.name,
                         allow_inexact=args.allow_inexact, epochs=args.epochs, max_minutes=args.max_minutes,
                         seed=args.seed, threads=args.threads, di=args.di, validate=not args.no_validate,
                         signal_seed=args.signal_seed, target_esr=args.target_esr, lr_gamma=args.lr_gamma, keep_scratch=args.keep_scratch,
                         log=lambda m: print(m, flush=True))
    except ExportRefused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError, ImportError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    print(LICENCE_NOTE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
