"""Turn timing_51a.log (run_timing.sh) into the RESULTS.md speed/memory table (medians over reps).

usage: summarize_timing.py [~/sawblade-sep-data/timing_51a.log]
"""
import json, re, statistics, sys
from collections import OrderedDict
from pathlib import Path

log = Path(sys.argv[1] if len(sys.argv) > 1 else Path.home() / "sawblade-sep-data/timing_51a.log")
runs = OrderedDict(); label = None
for line in log.read_text().splitlines():
    m = re.match(r"== (\S+) m=(\S+) threads=(\d+) rep=(\d+)", line)
    if m:
        label = (m.group(1), m.group(2), int(m.group(3))); continue
    if line.startswith("RESULT") and label:
        body = line[len("RESULT "):].strip()
        if body.startswith("{"):
            d = json.loads(body)
            r = dict(load=d["load_s"], sep=d["sep_s"], audio=d["audio_s"], rss=d["peak_rss_mb"])
        else:
            kv = dict(p.split("=") for p in body.split() if "=" in p)
            r = dict(load=float(kv["load_s"]), sep=float(kv["sep_s"]), audio=float(kv["audio_s"]), rss=float(kv["peak_rss_mb"]))
        runs.setdefault(label, []).append(r)
NAMES = {"eigen-v3": "(a) Eigen GEMM, FTZ", "blas-v3": "(a) OpenBLAS, FTZ", "onnx": "(b) ORT CPU EP", "python": "Python torch (stock, shifts=0)"}
BUILD = {"eigen-v3": "x86-64-v3", "blas-v3": "x86-64-v3", "onnx": "ORT dispatch", "python": "torch 2.5.1"}
order = ["eigen-v3", "blas-v3", "onnx", "python"]
print("| engine | build | model | threads | runs | load s | separate s (70 s audio) | s per min of audio | RTF | peak RSS MB | individual s/min |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for e in order:
    for m in ("4s", "6s"):
        for t in (1, 4):
            rs = runs.get((e, m, t))
            if not rs: continue
            med = lambda k: statistics.median(r[k] for r in rs)
            spm = [r["sep"] / r["audio"] * 60 for r in rs]
            print(f"| {NAMES[e]} | {BUILD[e]} | {m} | {t} | {len(rs)} | {med('load'):.2f} | {med('sep'):.1f} | {statistics.median(spm):.1f} | "
                  f"{statistics.median(spm)/60:.2f} | {med('rss'):.0f} | {', '.join(f'{x:.1f}' for x in spm)} |")
