"""``sawblade-bench`` command line: ``run`` and ``compare`` (docs/benchmark/BENCHMARK.md)."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

from . import compare as CMP
from . import manifest as MF
from . import runner as RN
from . import score as SC
from .runner import run_bench


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-bench", description="Sawblade genre benchmark")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run the matcher on the benchmark cases and score the held-out sections")
    r.add_argument("--root", required=True, help="the folder the manifest's relative paths start from (e.g. your NailTheMix folder)")
    r.add_argument("--manifest", default=str(MF.DEFAULT_MANIFEST), help="default: docs/benchmark/cases.json of the repo")
    r.add_argument("--cases", default="", help="comma list of case ids (default: all of the tier)")
    r.add_argument("--tier", default="1", choices=["1", "2", "all"], help="default 1 (the known-answer pairs)")
    mode = r.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true", help="the matcher's quick preset (<= 5 min per case on 4 cores)")
    mode.add_argument("--thorough", action="store_true", help="the full search (default)")
    r.add_argument("--seed", type=int, default=0)
    r.add_argument("--threads", "--jobs", dest="threads", type=int, default=4)
    r.add_argument("--pool", help="pool_manifest.json from sawblade-t3k pull (required unless --check-only)")
    r.add_argument("--ir-dir", action="append", default=[], metavar="DIR", help="a directory of your own IRs (repeatable), as sawblade-match")
    r.add_argument("--no-ir-dirs", action="store_true", help="ignore ~/.config/sawblade/ir_dirs.json")
    r.add_argument("--check-only", action="store_true", help="preflight: list every file (ok/missing, rate, length) and exit 0, or 2 if any is missing")
    r.add_argument("--out", required=True, help="output directory (scores.json, scores.md, listen/, the matcher run directories)")
    c = sub.add_parser("compare", help="compare two scores.json files (A = baseline, B = candidate)")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--json", metavar="PATH", help="write the full comparison here")
    c.add_argument("--threshold", action="append", default=[], metavar="KEY=V", help="override a regression threshold (repeatable)")
    return p


def _err(msg: str) -> int:
    print(f"sawblade-bench: {msg}", file=sys.stderr)
    return 2


def _select(m: dict, a) -> list[dict]:
    ids = [x.strip() for x in a.cases.split(",") if x.strip()]
    known = {c["id"] for c in m["cases"]}
    bad = [i for i in ids if i not in known]
    if bad:
        raise MF.ManifestError(f"unknown case id(s): {', '.join(bad)}")
    if ids:                     # explicit ids win over --tier; a path check without its parent is marked skipped by the runner
        return [c for c in m["cases"] if c["id"] in ids]
    return [c for c in m["cases"] if a.tier == "all" or str(c["tier"]) == a.tier]


def cmd_run(a, run=None) -> int:
    run = run or run_bench
    try:
        m = MF.load_manifest(a.manifest)
        sel = _select(m, a)
    except MF.ManifestError as e:
        return _err(str(e))
    if not sel:
        return _err("no cases selected")
    root = Path(a.root)
    pre = RN.preflight(sel, root)
    for c in sel:
        if not c["confirmed"]:
            print(f"note: {c['id']}: file names are inferred, not confirmed against your listing")
    if a.check_only:
        print(RN.format_preflight(sel, pre))
        bad = [c["id"] for c in sel if pre[c["id"]]["status"] != "ok"]
        for cid in bad:
            print(f"{cid}: {pre[cid]['error']}")
        return 2 if bad else 0
    if not a.pool:
        return _err("--pool is required (unless --check-only)")
    from ..matcher import irlib
    from ..matcher.cli import resolve_ir_dirs
    from ..matcher.pool import load_pool
    try:
        pool = load_pool(a.pool)
        dirs = resolve_ir_dirs(a.ir_dir, a.no_ir_dirs)
        lib = None
        if dirs:
            lib = irlib.scan([d["path"] for d in dirs], progress=irlib.stderr_progress)
            lib = lib if lib.records else None
    except (OSError, ValueError, RuntimeError) as e:
        return _err(f"cannot load the pool / IR library: {' '.join(str(e).split())}")
    args = {"cases": [c["id"] for c in sel], "tier": a.tier, "quick": bool(a.quick), "seed": a.seed, "threads": a.threads,
            "pool": str(a.pool), "irDirs": [d["path"] for d in dirs]}
    s = run(sel, root, a.out, pool=pool, quick=a.quick, seed=a.seed, threads=a.threads, ir_library=lib, ir_dirs=dirs,
            log=lambda msg: print(msg, flush=True), manifest_path=a.manifest, args=args)
    print((Path(a.out) / "scores.md").read_text().split("\n\n")[1])
    print(SC.total_line(s["total"]))
    return 0


def cmd_compare(a) -> int:
    try:
        over = CMP.parse_threshold_overrides(a.threshold)
        r = CMP.compare(CMP.load_scores(a.a), CMP.load_scores(a.b), over)
    except CMP.CompareError as e:
        return _err(str(e))
    print(CMP.format_markdown(r))
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps(r, indent=2) + "\n")
    return 1 if r["counts"]["worse"] else 0


def main(argv: Sequence[str] | None = None) -> int:
    try:
        a = build_parser().parse_args(argv)
    except SystemExit as e:                       # argparse: bad arguments -> 2
        return int(e.code) if isinstance(e.code, int) else 2
    return cmd_run(a) if a.cmd == "run" else cmd_compare(a)


if __name__ == "__main__":
    sys.exit(main())
