"""`sawblade-calibrate`: propose calibrated tone targets from the reference audio."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

import numpy as np
import soundfile as sf

from ..tonecheck import analysis as A
from ..tonecheck.render import find_targets
from ..tonecheck.rules import evaluate_rules, load_targets
from . import report as R
from .channels import cover_di_onsets, mid_channel, side_channel, stem_guitar_signal
from .measure import Measured, measure
from .propose import propose
from .sections import (Selection, cover_guitar_frames, original_guitar_frames, parse_ranges)
from .separation import MODEL, SEED, Stem, Unavailable, separate_other

DEFAULTS = {
    "original": "testdata/reference/barbaric_pleasures_original.mp3",
    "cover_mix": "testdata/reference/barbaric_pleasures_cover_mix.mp3",
    "di_l": "testdata/gatecreeper_cover/Guitar_L.wav",
    "di_r": "testdata/gatecreeper_cover/Guitar_R.wav",
}


def _stereo48(path) -> tuple[np.ndarray, np.ndarray]:
    """Decode a file once; (left, right) at 48 kHz (a mono file gives the same signal twice)."""
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    l, r = x[:, 0], x[:, min(1, x.shape[1] - 1)]
    return A.to_analysis_rate(l, fs), A.to_analysis_rate(r, fs)


def _mono48(path) -> np.ndarray:
    x, fs = A.read_mono(path, "left")
    return A.to_analysis_rate(x, fs)


def _stem_mono48(s: Stem) -> tuple[np.ndarray, str, dict]:
    """Phase 3.4: the stem's side channel when the guitars are hard-panned, else its mid (channels.stem_guitar_signal)."""
    sig, kind, _, info = stem_guitar_signal(s.audio)
    return A.to_analysis_rate(sig, s.rate), kind, info


def _fmt_ranges(sel: Selection, min_s: float = 2.0, limit: int = 40) -> str:
    r = sel.ranges(min_s)
    txt = ", ".join(f"{a:.0f}-{b:.0f}" for a, b in r[:limit])
    return f"{len(r)} ranges >= {min_s:g} s (s): {txt}" + (" ..." if len(r) > limit else "")


def _json_safe(o):
    if isinstance(o, dict):
        return {k: _json_safe(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_json_safe(v) for v in o]
    if isinstance(o, np.ndarray):
        return [_json_safe(v) for v in o.tolist()]
    if isinstance(o, (np.floating, float)):
        return None if not np.isfinite(o) else round(float(o), 4)
    if isinstance(o, np.integer):
        return int(o)
    return o


def _measured_json(m: Measured) -> dict:
    return {"centresHz": m.centres, "ltasDbRel1k": [round(float(v), 2) for v in m.rel_db],
            "groupsDb": {k: round(v, 2) for k, v in m.groups.items()}, "rulesCurrent": m.rules,
            "metrics": _json_safe(m.metrics), "welchSegments": m.n_segments,
            "selectedFraction": round(m.selected_fraction, 4), "selectedSeconds": round(m.selected_seconds, 1),
            "chunkSpreadDb": m.chunk_spread_db, "warnings": m.warnings}


def run(a: argparse.Namespace) -> int:
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    targets_path = find_targets(a.targets)
    targets = load_targets(targets_path)
    P = {k: Path(getattr(a, k)) for k in DEFAULTS}
    for k, p in P.items():
        if not p.exists():
            raise ValueError(f"missing input {k}: {p}")
    ranges = parse_ranges(a.sections) if a.sections else None

    orig_l, orig_r = _stereo48(P["original"])
    orig = mid_channel(orig_l, orig_r)
    mix_l, mix_r = _stereo48(P["cover_mix"])
    cover_mid = mid_channel(mix_l, mix_r)
    di_l, di_r = _mono48(P["di_l"]), _mono48(P["di_r"])

    results: dict[str, Measured] = {}
    availability: dict[str, str] = {}
    selection_notes: dict[str, str] = {}
    notes: list[str] = []

    # method 1: stems
    for ref, src in (("original", P["original"]), ("cover", P["cover_mix"])):
        if a.no_separation:
            availability[f"{ref} / stems (method 1)"] = "unavailable: disabled with --no-separation"
            continue
        s = separate_other(src, Path(a.stems_dir), MODEL)
        if isinstance(s, Unavailable):
            availability[f"{ref} / stems (method 1)"] = str(s)
            continue
        sig, kind, sinfo = _stem_mono48(s)
        availability[f"{ref} / stems (method 1)"] = (f"ran ({MODEL} 'other' stem, seed {SEED}, {'cached' if s.cached else 'fresh'}: "
                                                     f"{s.path}; analysed channel: stem {kind}, side/mid {sinfo.get('sideMidDb')} dB)")
        results[f"{ref}/stems"] = measure(f"{ref}/stems", sig, targets, source=f"{s.path} ({kind})", spread=True)

    csel = cover_guitar_frames(mix_l, mix_r, di_l, di_r)
    di_onsets = cover_di_onsets(di_l, di_r, csel.info['offsetsS'])

    # method 3: side channel (L-R)/2 - hard-panned double-tracked guitars survive, centred bass/kick/snare/vocal cancel
    for ref, src in (("original", P["original"]), ("cover", P["cover_mix"])):
        l, r = (orig_l, orig_r) if ref == "original" else (mix_l, mix_r)
        side = side_channel(l, r)
        mid_rms = float(np.sqrt(np.mean(mid_channel(l, r) ** 2)))
        ratio = 20 * np.log10(max(float(np.sqrt(np.mean(side ** 2))), 1e-12) / max(mid_rms, 1e-12))
        try:
            # low-end metrics need note onsets: cover = both DIs' onsets shifted by the measured offsets;
            # original has no DI -> reported n/a
            results[f"{ref}/side"] = measure(f"{ref}/side", side, targets, source="(L-R)/2", spread=True,
                                             onsets=di_onsets if ref == "cover" else None, no_onsets=ref != "cover")
            availability[f"{ref} / side (method 3)"] = f"ran ((L-R)/2, activity gate; side/mid RMS {ratio:+.1f} dB)"
        except ValueError as e:
            availability[f"{ref} / side (method 3)"] = f"unavailable: side channel unusable (mono file?): {e}"

    # method 2: sections
    osel = original_guitar_frames(orig, ranges=ranges)
    results["original/sections"] = measure("original/sections", orig, targets, mask=osel.sample_mask(len(orig)),
                                           source="mix mid, selected frames", spread=True)
    if ranges is not None:
        selection_notes["original / sections"] = (f"user ranges --sections {a.sections} ({osel.fraction * 100:.0f} % of frames "
                                                  "after the activity gate); heuristic not used")
    else:
        selection_notes["original / sections"] = (
            f"vocal/drum heuristic: kept {osel.fraction * 100:.0f} % of frames; excluded: "
            + ", ".join(f"{k} {v.mean() * 100:.0f} %" for k, v in osel.reasons.items())
            + f"; drum hits {osel.info['drumHits']}. {_fmt_ranges(osel)}. "
            "UNVALIDATED: growled vocals are aperiodic, so the voice detector is expected to miss them; "
            "treat the original/sections numbers as a hypothesis or pass --sections with hand-picked ranges.")
        notes.append("original/sections used the automatic vocal-exclusion heuristic, which is not reliable "
                     "(see selection details); prefer --sections or the stems method.")
    n = min(len(mix_l), len(mix_r))
    results["cover/sections"] = measure("cover/sections", cover_mid[:n], targets, mask=csel.sample_mask(n),
                                        source="mix mid, selected frames", spread=True)
    selection_notes["cover / sections"] = (
        f"DI-active & 2-5 kHz-correlated, drums excluded: kept {csel.fraction * 100:.0f} % of frames; DI offsets in mix "
        f"{ {k: round(v * 1000, 1) for k, v in csel.info['offsetsS'].items()} } ms; drum hits {csel.info['drumHits']}; "
        f"per DI {csel.info['perDi']}. {_fmt_ranges(csel)}")

    # proposal from the original
    basis = next(k for k in ("original/stems", "original/side", "original/sections") if k in results)
    basis_m = results[basis]
    others = {k: v.groups for k, v in results.items() if k.startswith("original/") and k != basis}
    if basis != "original/stems":
        notes.append("method 1 (stems) unavailable for the original: the proposal is based on "
                     + ("method 3 (side channel)." if basis == "original/side" else "method 2 only."))
    notes.append(f"proposal policy: {a.policy}")
    prov = {"inputs": {k: str(v) for k, v in P.items()}, "targetsFile": str(targets_path),
            "methods": {k: availability.get(f"{k.split('/')[0]} / stems (method 1)") for k in results if k.endswith("/stems")},
            "sectionsArg": a.sections, "policy": a.policy, "randomness": f"none (demucs: seed {SEED}, shifts=0)",
            "referenceMetrics": {k: _json_safe({m: v.metrics[m] for m in ("buzz", "lowTightnessMs", "lowDecayDbPerMs", "fizzTexture")})
                                 for k, v in results.items()}}
    proposed, table, contradicted = propose(targets, basis_m.groups, basis, others, prov, a.policy)
    (out / "tone_targets.proposed.json").write_text(json.dumps(proposed, indent=2))

    # smoke render(s) under current vs proposed
    scores = {}
    extra = {}
    for sp in a.score:
        sx, sfs = A.read_mono(sp, "mid")
        an = A.analyze(sx, sfs, targets)
        scores[Path(sp).name] = {"current": evaluate_rules(an.groups, targets["rules"]),
                                 "proposed": evaluate_rules(an.groups, proposed["rules"])}
        extra[f"render: {Path(sp).name}"] = Measured(Path(sp).name, an.abs_db, an.rel_db, an.centres, an.groups, [], {}, an.n_segments,
                                                     an.active_fraction, 0.0)

    R.ltas_png(out / "ltas_original_vs_cover.png", results, targets["analysis"]["bandGroups"], extra)
    R.thresholds_png(out / "thresholds_current_vs_proposed.png", table, results, targets)
    md = R.markdown(inputs={**{k: str(v) for k, v in P.items()}, "targets": str(targets_path)}, availability=availability,
                    results=results, targets=targets, proposed=proposed, table=table, contradicted=contradicted,
                    basis=basis, scores=scores, notes=notes, selection_notes=selection_notes,
                    pngs=["ltas_original_vs_cover.png", "thresholds_current_vs_proposed.png"])
    (out / "calibration_report.md").write_text(md)
    (out / "calibration.json").write_text(json.dumps(_json_safe({
        "schema": "sawblade.calibration", "basis": basis, "availability": availability,
        "results": {k: _measured_json(v) for k, v in results.items()},
        "rules": table, "contradicted": contradicted, "scores": scores,
        "coverDrumHitTimesS": csel.info.get("drumHitTimesS")}), indent=2))
    print(md.split("## Figures")[0])
    print(f"wrote {out}/tone_targets.proposed.json, calibration_report.md, calibration.json, *.png")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-calibrate",
                                description="Measure guitar-dominant audio of the reference songs and propose calibrated tone targets "
                                            "(never modifies docs/tone_targets.json)")
    p.add_argument("--original", default=DEFAULTS["original"])
    p.add_argument("--cover-mix", dest="cover_mix", default=DEFAULTS["cover_mix"])
    p.add_argument("--di-l", dest="di_l", default=DEFAULTS["di_l"])
    p.add_argument("--di-r", dest="di_r", default=DEFAULTS["di_r"])
    p.add_argument("--sections", help="ORIGINAL: use only these time ranges (s), e.g. 0:12,95:110, instead of the "
                                      "vocal/drum heuristic")
    p.add_argument("--stems-dir", default="testdata/stems", help="stem cache (default testdata/stems, git-ignored)")
    p.add_argument("--no-separation", action="store_true", help="skip method 1 (demucs)")
    p.add_argument("--score", action="append", default=[], metavar="WAV",
                   help="rendered audio to score under current and proposed targets (repeatable)")
    p.add_argument("--policy", choices=["loosen-only", "tighten"], default="loosen-only",
                   help="loosen-only (default): change only rules the basis original fails/marginally passes; "
                        "tighten: set every threshold to the original +- tolerance")
    p.add_argument("--targets", help="current targets JSON (default docs/tone_targets.json)")
    p.add_argument("--out", default=str(Path.home() / ".cache" / "sawblade" / "calibration"),
                   help="output directory (default ~/.cache/sawblade/calibration, outside the repo)")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args and args[0] == "pedal-fit":   # phase 7.1: fit pedal.hm to captures of real HM-2 pedals
        from . import pedal_fit
        return pedal_fit.main(args[1:])
    if args and args[0] == "pedal-accuracy":   # v0.4a: accuracy report from the per-pedal fits JSON files
        from . import pedal_accuracy
        return pedal_accuracy.main(args[1:])
    if args and args[0] == "device-null":   # v0.6 Task D: hardware loader (Anagram) vs the plugin render
        from . import device_null
        return device_null.main(args[1:])
    a = build_parser().parse_args(argv)
    try:
        return run(a)
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
