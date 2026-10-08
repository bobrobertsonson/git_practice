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
    p.add_argument("--arch", choices=("a2", "a1"), default="a2",
                   help="a2 (default): train the packed A2 net and write the standard NAM A2 file (<stem>.a2.nam, the "
                        "primary file) plus standalone A2 Full and A2 Lite extras; a1: one A1 WaveNet for older loaders")
    p.add_argument("--size", default=None, metavar="SIZE",
                   help="a2: full (default) | lite = which standalone extra is listed first and whose acceptance "
                        "--require-accept judges (the container and both extras are always written and "
                        "validated); a1: standard (default) | lite | feather, NAM's official A1 presets. Any other "
                        "arch/size pair is refused")
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
                   help="exit 2 when the acceptance status of the primary size is NOT MET (files are still written); "
                        "a1 sizes other than standard are not judged and exit 0")
    p.add_argument("--no-validate", action="store_true", help="skip validation + listening file")
    p.add_argument("--resume", metavar="DIR|auto", default=None,
                   help="continue an interrupted run: the run's output directory (refused if preset, signal, mode, size or "
                        "training settings differ), or 'auto' = the newest matching unfinished run in the exports dir "
                        "(else start fresh); --max-minutes counts training time across resumes")
    p.add_argument("--notes-preset", default=None, metavar="PATH",
                   help="no-cab 'drop' exports only: the ORIGINAL preset (same rig; it may differ from PRESET only in its bus "
                        "comp and non-tone keys) the export notes are written from, so a bus comp switched off in PRESET is "
                        "still listed with its settings. Repeat it on --resume")
    p.add_argument("--nam-input", default=None, metavar="NAM_INPUT.wav",
                   help="train on the NAM project's standard input file (as used by the NAM trainer; you supply it, Sawblade never "
                        "bundles or downloads it): checked and split by the pinned trainer's own data pipeline (version "
                        "recognition, blip latency calibration, validation split). Recorded as metadata.sawblade.trainingSignal "
                        "(nam-standard v<version>). One of --nam-input / --signal sawblade is required when training")
    p.add_argument("--signal", choices=("sawblade",), default=None,
                   help="sawblade = the labelled fallback: train on Sawblade's own test signal instead of the standard NAM "
                        "signal (recorded as trainingSignal sawblade-synthetic v<N>; the notes say so)")
    p.add_argument("--reamp-pair", default=None, metavar="NAM_INPUT.wav",
                   help="also write a reamp pair for training a model the standard NAM way: the NAM project's standard input "
                        "file (as used by the NAM trainer; you supply it, Sawblade never bundles it) is checked the way the "
                        "trainer does and copied as <stem>.reamp_input.wav, and rendered through the exportable chain as "
                        "<stem>.reamp_output.wav (48 kHz, 24-bit, latency-compensated). Personal use only: never upload or share")
    p.add_argument("--no-train", action="store_true",
                   help="with --reamp-pair: write only the pair, its notes and the report; no training or validation")
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
    msg = None
    if args.no_train and not args.reamp_pair:
        msg = "--no-train needs --reamp-pair NAM_INPUT.wav"
    elif args.no_train and args.resume:
        msg = "--no-train and --resume cannot be combined"
    elif args.no_train and args.require_accept:
        msg = "--require-accept needs validation (drop --no-train)"
    elif args.require_accept and args.no_validate:
        msg = "--require-accept needs validation (drop --no-validate)"
    elif args.nam_input and args.signal == "sawblade":
        msg = "--nam-input and --signal sawblade contradict each other: pick one"
    elif (not args.no_train and not args.nam_input and args.signal != "sawblade"
          and not (args.resume and args.resume != "auto")):      # a resumed run reuses the signal it started with
        msg = ("no training signal chosen: give --nam-input NAM_INPUT.wav (the NAM project's standard input file, as used by "
               "the NAM trainer; the default and recommended) or --signal sawblade (Sawblade's own test signal, a labelled "
               "fallback)")
    if msg:
        print(f"error: {msg}", file=sys.stderr)
        Progress(args.progress_json).update("error", message=msg)
        return EXIT_ERROR
    from . import train as T
    try:
        args.size = T.check_arch_size(args.arch, args.size)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        Progress(args.progress_json).update("error", message=str(e), arch=args.arch)
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
        rep = run_export(args.preset, mode=args.mode, size=args.size, arch=args.arch, out=args.out, name=args.name,
                         allow_inexact=args.allow_inexact, epochs=args.epochs, max_minutes=args.max_minutes,
                         seed=args.seed, threads=args.threads, di=args.di, validate=not args.no_validate,
                         signal_seed=args.signal_seed, target_esr=args.target_esr, lr_gamma=args.lr_gamma, batch_size=args.batch_size, device=args.device, keep_scratch=args.keep_scratch, resume=args.resume,
                         exports_root=args.exports_root, progress_json=args.progress_json, notes_preset=args.notes_preset,
                         reamp_pair=args.reamp_pair, no_train=args.no_train,
                         nam_input=args.nam_input, signal=args.signal,
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
        vals = ([(sz, rep["validation"][sz]) for sz in ("full", "lite")] if args.arch == "a2" else [(None, rep["validation"])])
        for sz, v in vals:
            print(f"{v['acceptance']['summary']}")
            print(f"{(sz + ': ') if sz else ''}held-out ESR {v['heldOut']['esr']:.4f}; DI LTAS error {v['diExcerpt']['ltas']['aWeightedErrorDb']:.2f} dB")
    for k, v in (rep.get("files") or {}).items():
        if k == "reampPair":
            print(f"reamp pair: {v['input']} + {v['output']} (notes: {v['notes']}); personal use only, never upload or share")
        elif k == "primary":
            print(f"file: {v}")
    if rep.get("trainingSignal"):
        print(f"training signal: {rep['trainingSignal']['label']}")
    print(rep["licenceNote"])
    if args.require_accept and (rep["validation"][args.size] if args.arch == "a2" else rep["validation"])["acceptance"]["status"] == "NOT MET":
        print("acceptance NOT MET (--require-accept)", file=sys.stderr)
        return EXIT_NOT_MET
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
