"""`sawblade-export` command line."""
from __future__ import annotations

import argparse
import signal
import sys
from pathlib import Path
from typing import Sequence

from . import stop as STOP
from .plan import ExportRefused
from .progress import Progress
from .stop import ExportInterrupted

EXIT_OK, EXIT_NOT_MET, EXIT_ERROR, EXIT_INTERRUPTED = 0, 2, 1, 130


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-export", description="Train a NAM model of a resolved Sawblade preset.")
    p.add_argument("preset", help="resolved preset JSON (e.g. the matcher's best.preset.resolved.json)")
    p.add_argument("--mode", choices=("nocab", "withcab"), default="nocab",
                   help="nocab (default): model before the cab + IR (x) post EQ wav; withcab: the whole chain")
    p.add_argument("--size", choices=("feather", "lite", "standard"), default="standard", help="A1 WaveNet size (Sawblade's own approximations of the community sizes, recalled "
                   "from memory; not NAM's official presets)")
    p.add_argument("--epochs", type=int, default=None, help="max epochs (default per size: see README)")
    p.add_argument("--max-minutes", type=float, default=None, help="training wall-time cap (default per size)")
    p.add_argument("--lr-gamma", type=float, default=None,
                   help="ExponentialLR gamma per epoch (default: decay to 5%% of the learning rate over --epochs)")
    p.add_argument("--batch-size", type=int, default=16, help="training batch size (datums of 8192 samples)")
    p.add_argument("--seed", type=int, default=0, help="training seed (model init + batch order)")
    p.add_argument("--signal-seed", type=int, default=1, help="seed of the training-signal generator")
    p.add_argument("--threads", type=int, default=4, help="torch CPU threads")
    p.add_argument("--allow-inexact", action="store_true",
                   help="nocab with the bus comp on: drop the comp and report the error it introduces")
    p.add_argument("--target-esr", type=float, default=None, help="stop early when the validation ESR reaches this")
    p.add_argument("--out", default=None, help="output directory; overrides --exports-root (default <exports root>/<name>-<mode>-<size>-<ts>)")
    p.add_argument("--exports-root", default=None, metavar="DIR",
                   help="parent of the output directory <name>-<mode>-<size>-<ts> (default ~/.cache/sawblade/exports); "
                        "'--resume auto' searches it")
    p.add_argument("--progress-json", default=None, metavar="PATH",
                   help="write stage / fraction / ETA as JSON (atomically, at most once a second while training)")
    p.add_argument("--name", default=None, help="file name stem (default: slug of the preset name)")
    p.add_argument("--di", default=None, help="DI wav for the excerpt validation, or 'builtin' = an excerpt of the built-in "
                   "held-out signal (default testdata/gatecreeper_cover/Guitar_L.wav; builtin when that file is missing)")
    p.add_argument("--device", choices=("auto", "cpu", "cuda", "mps"), default="auto",
                   help="training device (auto: cuda > mps > cpu)")
    p.add_argument("--require-accept", action="store_true",
                   help="exit 2 when the acceptance status is NOT MET (files are still written); other sizes are not judged "
                        "and exit 0")
    p.add_argument("--no-validate", action="store_true", help="skip validation + listening file")
    p.add_argument("--resume", metavar="DIR|auto", default=None,
                   help="continue an interrupted run: the run's output directory (refused if preset, signal, mode, size or "
                        "training settings differ), or 'auto' = the newest matching unfinished run in the exports dir "
                        "(else start fresh); --max-minutes counts training time across resumes")
    p.add_argument("--notes-preset", default=None, metavar="PATH",
                   help="no-cab 'drop' exports only: the ORIGINAL preset (same rig; it may differ from PRESET only in its bus "
                        "comp and non-tone keys) the export notes are written from, so a bus comp switched off in PRESET is "
                        "still listed with its settings. Repeat it on --resume")
    p.add_argument("--keep-scratch", action="store_true", help="keep the scratch + checkpoint directories (the run is marked complete, never auto-resumed)")
    return p


def _install_sigint():
    """First SIGINT = cancel (cooperative stop flag); a second one falls back to the default KeyboardInterrupt."""
    def handler(signum, frame):
        STOP.request_stop()
        signal.signal(signal.SIGINT, signal.default_int_handler)
    try:
        return signal.signal(signal.SIGINT, handler)
    except ValueError:                       # not the main thread (tests, embedding)
        return None


def main(argv: Sequence[str] | None = None) -> int:
    """Exit codes: 0 finished, 2 trained but NOT MET (--require-accept), 1 refused / error, 130 interrupted."""
    args = build_parser().parse_args(argv)
    if args.require_accept and args.no_validate:
        msg = "--require-accept needs validation (drop --no-validate)"
        print(f"error: {msg}", file=sys.stderr)
        Progress(args.progress_json).update("error", message=msg)
        return EXIT_ERROR
    if args.notes_preset:
        err = None
        if args.mode != "nocab":
            err = "--notes-preset is only for no-cab exports (use --mode nocab)"
        elif not Path(args.notes_preset).expanduser().is_file():
            err = f"--notes-preset {args.notes_preset}: no such file"
        if err:
            print(f"error: {err}", file=sys.stderr)
            Progress(args.progress_json).update("error", message=err)
            return EXIT_ERROR
    from .run import run_export
    STOP.clear()
    old = _install_sigint()
    try:
        rep = run_export(args.preset, mode=args.mode, size=args.size, out=args.out, name=args.name,
                         allow_inexact=args.allow_inexact, epochs=args.epochs, max_minutes=args.max_minutes,
                         seed=args.seed, threads=args.threads, di=args.di, validate=not args.no_validate,
                         signal_seed=args.signal_seed, target_esr=args.target_esr, lr_gamma=args.lr_gamma, batch_size=args.batch_size, device=args.device, keep_scratch=args.keep_scratch, resume=args.resume,
                         exports_root=args.exports_root, progress_json=args.progress_json, notes_preset=args.notes_preset,
                         log=lambda m: print(m, flush=True))
    except ExportInterrupted:
        print("interrupted: the last complete epoch's checkpoint is kept (resume with --resume)", file=sys.stderr)
        return EXIT_INTERRUPTED
    except ExportRefused as e:
        print(f"refused: {e}", file=sys.stderr)
        return EXIT_ERROR
    except (OSError, ValueError, RuntimeError, ImportError) as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR
    finally:
        if old is not None:
            signal.signal(signal.SIGINT, old)
        STOP.clear()
    if rep.get("validation"):
        v = rep["validation"]
        print(f"{v['acceptance']['summary']}")
        print(f"held-out ESR {v['heldOut']['esr']:.4f}; DI LTAS error {v['diExcerpt']['ltas']['aWeightedErrorDb']:.2f} dB")
    print(rep["licenceNote"])
    if args.require_accept and rep["validation"]["acceptance"]["status"] == "NOT MET":
        print("acceptance NOT MET (--require-accept)", file=sys.stderr)
        return EXIT_NOT_MET
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
