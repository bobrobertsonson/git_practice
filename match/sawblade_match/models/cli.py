"""`sawblade-models` command line: fetch / status."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

from .paths import DEFAULT_MODEL, MODEL_IDS, models_dir
from .store import format_status


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-models", description="Fetch and verify the stem-separation models.")
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download the official checkpoint, export the ONNX core, verify, write the sha256")
    f.add_argument("--model", choices=(*MODEL_IDS, "all"), default=DEFAULT_MODEL, help=f"default {DEFAULT_MODEL}")
    f.add_argument("--dir", default=None, help="models dir (default: $SAWBLADE_MODELS_DIR, else the per-user data dir)")
    f.add_argument("--threads", type=int, default=4, help="torch / ORT threads for the export and the check")
    f.add_argument("--force", action="store_true", help="re-export even if a verified ONNX is already present")
    s = sub.add_parser("status", help="list each model: present / sha ok / pinned match")
    s.add_argument("--dir", default=None)
    return p


def _dir(arg: str | None) -> Path:
    return Path(arg).expanduser() if arg else models_dir()


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    d = _dir(args.dir)
    if args.cmd == "status":
        print(format_status(d))
        return 0
    from .download import ensure_checkpoint
    from .store import model_status
    ids = MODEL_IDS if args.model == "all" else (args.model,)
    rc = 0
    for mid in ids:
        st = model_status(mid, d)
        if st.sha_ok and not args.force:
            print(f"{mid}: already present and verified ({st.actual})" +
                  ("" if st.pinned_match else "  [differs from the pinned export hash]"))
            continue
        try:
            ensure_checkpoint(mid, d, log=lambda m: print(m, flush=True))
            from .export_onnx import export_and_verify
            export_and_verify(mid, d, threads=args.threads, log=lambda m: print(m, flush=True))
        except ImportError as e:
            print(f"error: {e}\nInstall the extra first: see match/README.md (\"Separation models\").", file=sys.stderr)
            return 3
        except Exception as e:       # one model failing must not stop `--model all`
            print(f"error: {mid}: {type(e).__name__}: {e}", file=sys.stderr)
            rc = 4
    return rc


if __name__ == "__main__":
    sys.exit(main())
