"""`sawblade-tonecheck` command line."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

import numpy as np

from .analysis import NOMINAL_CENTRES, Analysis, analyze, read_mono
from .plot import make_plot
from .render import RenderError, find_targets, find_tonerender, render
from .rules import evaluate_rules, load_targets, summarize

GATE_NOISE_TARGET_DB = -60.0


def a_weight_db(f: np.ndarray) -> np.ndarray:
    f2 = np.asarray(f, dtype=float) ** 2
    ra = (12194.0 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2))
    return 20 * np.log10(ra) + 2.0


def compare_to_reference(out: Analysis, ref: Analysis) -> dict:
    """Per-band LTAS difference (both normalised to their 1 kHz band) and A-weighted RMS error 80 Hz-8 kHz.

    The A-weighted error is sqrt(sum(w d^2) / sum(w)) with w = 10^(A(fc)/10) over the 80 Hz..8 kHz bands
    (A-weighting as a power weight on each band's difference; adding A to both levels would cancel)."""
    d = out.rel_db - ref.rel_db
    c = np.array(out.centres)
    sel = (c >= 80) & (c <= 8000)
    w = 10 ** (a_weight_db(c[sel]) / 10)
    return {
        "centres": list(out.centres),
        "diff_db": d,
        "aWeightedErrorDb": float(np.sqrt(np.sum(w * d[sel] ** 2) / np.sum(w))),
        "unweightedRmsErrorDb": float(np.sqrt(np.mean(d[sel] ** 2))),
        "meanDiffDb": float(np.mean(d[sel])),
        "rel_db": ref.rel_db,
    }


def _r(v, n=3):
    return None if v is None else round(float(v), n)


def _metrics_json(m: dict) -> dict:
    out = {}
    for k, v in m.items():
        out[k] = {kk: (_r(vv, 4) if isinstance(vv, float) else vv) for kk, vv in v.items()}
    return out


def _analysis_json(a: Analysis) -> dict:
    return {
        "bandCentresHz": a.centres,
        "ltasDbRel1k": [_r(v, 2) for v in a.rel_db],
        "ltasDbAbs": [_r(v, 2) for v in a.abs_db],
        "groupsDb": {k: _r(v, 2) for k, v in a.groups.items()},
        "metrics": _metrics_json(a.metrics),
        "welchSegments": a.n_segments,
        "activeFrameFraction": _r(a.active_fraction, 4),
        "warnings": a.warnings,
    }


def check_audio(name: str, wav: Path, di_path: Path | None, ref_path: Path | None, targets: dict,
                out_dir: Path, tonerender_report: dict | None = None, preset: Path | None = None,
                targets_path: Path | None = None, ref_channel: str = "mid") -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    x, fs = read_mono(wav, "mid")
    di = read_mono(di_path) if di_path else None
    a = analyze(x, fs, targets, di=di)
    results = evaluate_rules(a.groups, targets["rules"])
    gap = a.metrics["gapNoiseDb"]["value"]
    gm = gap - GATE_NOISE_TARGET_DB  # <= -60 required: margin = -60 - gap
    results.append({"id": "gap_noise", "expr": "gapNoiseDb <= -60", "group": "gapNoiseDb", "op": "<=",
                    "value": round(gap, 3), "threshold": GATE_NOISE_TARGET_DB, "margin": round(-gm, 3),
                    "toleranceDb": 0.0, "status": "pass" if gap <= GATE_NOISE_TARGET_DB else "fail",
                    "why": "metric target from tone_targets.json (no tolerance given)"})
    report = {
        "schema": "sawblade.tonecheck_report", "version": 1, "name": name,
        "randomness": "none (analysis is deterministic; no seeds)",
        "inputs": {"preset": str(preset) if preset else None, "audio": str(wav),
                   "di": str(di_path) if di_path else None, "ref": str(ref_path) if ref_path else None, "refChannel": ref_channel if ref_path else None,
                   "targets": str(targets_path) if targets_path else None,
                   "targetsStatus": targets.get("status")},
        "rates": {"audioInputHz": fs, "analysisHz": 48000,
                  "diHz": di[1] if di else None,
                  "tonerender": ({k: tonerender_report.get(k) for k in ("inputRate", "renderRate", "outputRate")}
                                 if tonerender_report else None)},
        "summary": summarize(results),
        "rules": results,
        **_analysis_json(a),
    }
    refcmp = None
    if ref_path:
        rx, rfs = read_mono(ref_path, ref_channel)
        ra = analyze(rx, rfs, targets)
        refcmp = compare_to_reference(a, ra)
        report["reference"] = {
            "path": str(ref_path), "analysis": _analysis_json(ra),
            "ltasDiffDb": [_r(v, 2) for v in refcmp["diff_db"]],
            "aWeightedErrorDb": _r(refcmp["aWeightedErrorDb"], 3),
            "unweightedRmsErrorDb": _r(refcmp["unweightedRmsErrorDb"], 3),
            "meanDiffDb": _r(refcmp["meanDiffDb"], 3),
            "rulesOnReference": evaluate_rules(ra.groups, targets["rules"]),
        }
    if tonerender_report is not None:
        report["tonerender"] = tonerender_report
    (out_dir / "report.json").write_text(json.dumps(report, indent=2))
    make_plot(out_dir / "report.png", f"{name}  [{report['summary']['overall']}]", a.centres, a.rel_db,
              a.groups, targets["analysis"]["bandGroups"], results, ref=refcmp)
    return report


def format_table(report: dict) -> str:
    lines = [f"{report['name']}: {report['summary']['overall'].upper()} "
             f"(pass {report['summary']['pass']}, marginal {report['summary']['marginal']}, "
             f"fail {report['summary']['fail']})",
             f"{'rule':<20}{'expr':<26}{'value':>8}{'thresh':>8}{'margin':>8}{'tol':>5}  status"]
    for r in report["rules"]:
        lines.append(f"{r['id']:<20}{r['expr']:<26}{r['value']:>8.1f}{r['threshold']:>8.1f}"
                     f"{r['margin']:>+8.1f}{r['toleranceDb']:>5.1f}  {r['status']}")
    m = report["metrics"]
    lt = m["lowTightnessMs"].get("valueMs")
    lines.append(f"buzz {m['buzz']['value']:.3f} | lowTightnessMs {'n/a' if lt is None else f'{lt:.0f}'} | "
                 f"crest {m['crestFactorDb']['value']:.1f} dB | LRA "
                 f"{'n/a' if m['loudnessRangeLU']['value'] is None else format(m['loudnessRangeLU']['value'], '.1f')} LU | "
                 f"gap {m['gapNoiseDb']['value']:.1f} dB")
    if "reference" in report:
        lines.append(f"vs reference: A-weighted error {report['reference']['aWeightedErrorDb']:.2f} dB "
                     f"(unweighted {report['reference']['unweightedRmsErrorDb']:.2f}, "
                     f"mean diff {report['reference']['meanDiffDb']:+.2f})")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-tonecheck", description="Objective tone check of a rendered preset")
    p.add_argument("preset", nargs="?", help="preset JSON (single mode)")
    p.add_argument("--presets", nargs="+", help="batch mode: several preset JSONs")
    p.add_argument("--audio", help="analyse this already-rendered WAV instead of rendering")
    p.add_argument("--di", help="DI WAV (render input; onset source for lowTightnessMs)")
    p.add_argument("--ref", help="reference WAV (need not be time-aligned)")
    p.add_argument("--ref-channel", choices=["left", "right", "mid"], default="mid", help="reference channel (default mid)")
    p.add_argument("--out", default="tonecheck_out", help="output directory (default ./tonecheck_out)")
    p.add_argument("--tonerender", help="tonerender binary (default build/cli/tonerender, then build-lead/cli/tonerender)")
    p.add_argument("--targets", help="targets JSON (default docs/tone_targets.json)")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    p = build_parser()
    a = p.parse_args(argv)
    out = Path(a.out)
    try:
        targets_path = find_targets(a.targets)
        targets = load_targets(targets_path)
        di = Path(a.di) if a.di else None
        ref = Path(a.ref) if a.ref else None
        if a.audio:
            if a.preset or a.presets:
                p.error("--audio analyses a rendered file; do not pass presets")
            rep = check_audio(Path(a.audio).stem, Path(a.audio), di, ref, targets, out, targets_path=targets_path, ref_channel=a.ref_channel)
            print(format_table(rep))
            return 0
        presets = [Path(s) for s in (a.presets or ([a.preset] if a.preset else []))]
        if not presets:
            p.error("give PRESET.json, --presets ..., or --audio OUT.wav")
        if not di:
            p.error("--di is required when rendering")
        tr = find_tonerender(a.tonerender)
        batch = len(presets) > 1 or bool(a.presets)
        rows = []
        for pr in presets:
            odir = out / pr.stem if batch else out
            odir.mkdir(parents=True, exist_ok=True)
            wav = odir / "render.wav"
            trep = render(tr, pr, di, wav, odir / "tonerender_report.json")
            rep = check_audio(pr.stem, wav, di, ref, targets, odir, trep, pr, targets_path, a.ref_channel)
            print(format_table(rep) + "\n")
            row = {"preset": str(pr), "overall": rep["summary"]["overall"],
                   "pass": rep["summary"]["pass"], "marginal": rep["summary"]["marginal"],
                   "fail": rep["summary"]["fail"], "failedRules": [r["id"] for r in rep["rules"] if r["status"] == "fail"],
                   "marginalRules": [r["id"] for r in rep["rules"] if r["status"] == "marginal"],
                   "buzz": rep["metrics"]["buzz"]["value"],
                   "lowTightnessMs": rep["metrics"]["lowTightnessMs"].get("valueMs"),
                   "gapNoiseDb": rep["metrics"]["gapNoiseDb"]["value"],
                   "aWeightedErrorDb": rep.get("reference", {}).get("aWeightedErrorDb"),
                   "report": str(odir / "report.json")}
            rows.append(row)
        if batch:
            (out / "summary.json").write_text(json.dumps({"presets": rows}, indent=2))
            print(f"{'preset':<28}{'overall':<10}{'P':>3}{'M':>3}{'F':>3}{'buzz':>8}{'tight ms':>10}{'gap dB':>8}{'A-err':>8}")
            for r in rows:
                t = "n/a" if r["lowTightnessMs"] is None else f"{r['lowTightnessMs']:.0f}"
                e = "-" if r["aWeightedErrorDb"] is None else f"{r['aWeightedErrorDb']:.2f}"
                print(f"{Path(r['preset']).stem:<28}{r['overall']:<10}{r['pass']:>3}{r['marginal']:>3}{r['fail']:>3}"
                      f"{r['buzz']:>8.3f}{t:>10}{r['gapNoiseDb']:>8.1f}{e:>8}")
        return 0
    except (RenderError, ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
