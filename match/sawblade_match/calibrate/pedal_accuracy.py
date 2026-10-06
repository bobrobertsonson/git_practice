"""v0.4a: capture-to-capture harmonic spread and the ``sawblade-calibrate pedal-accuracy`` report generator.

No audio, no scipy: this module only reads the per-pedal ``fits_<pedal>.json`` files written by ``pedal-fit`` (and,
optionally, the known-answer JSON of ``pedal-fit --known-answers``) and writes ``docs/reports/v0_4/accuracy.md``.
Capture numbers are never invented: with no fits file for a pedal the report says so and is marked PENDING USER RUN.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

import numpy as np

REPO = Path(__file__).resolve().parents[3]
DEFAULT_OUT = REPO / "docs" / "reports" / "v0_4" / "accuracy.md"
PEDAL_ORDER = ("hm", "hmx", "eye", "muff", "ts")
TARGET_LTAS_DB = 2.0          # v0.4 target: constrained LTAS shape error on labelled captures
TARGET_LTAS_FRACTION = 0.75   # ... on at least this fraction of them
TARGET_HARM_SPREAD_X = 2.0    # ... and constrained harmonic error within this multiple of the capture spread


# ---------------------------------------------------------------------------------------------------------
# capture-to-capture spread
# ---------------------------------------------------------------------------------------------------------
def harm_spread(profiles: Sequence) -> dict:
    """RMS distance (dB) of each fixed harmonic profile to the group mean profile. ``profiles`` are
    (slots x harmonics) arrays from the same probe layout, already clamped at the fixed floor. A group of one has no
    spread (``None``); note that for two captures each distance is half their mutual distance."""
    n = len(profiles)
    if n < 2:
        return {"n": n, "mean_rms_db": None, "max_rms_db": None, "per_capture_rms_db": [None] * n}
    P = np.stack([np.asarray(p, float) for p in profiles])
    mean = P.mean(axis=0)
    d = np.sqrt(np.mean((P - mean[None]) ** 2, axis=tuple(range(1, P.ndim))))
    return {"n": n, "mean_rms_db": float(d.mean()), "max_rms_db": float(d.max()),
            "per_capture_rms_db": [float(v) for v in d]}


def _setting_key(labels: dict) -> tuple:
    return tuple(sorted((k, float(v)) for k, v in labels.items()))


def spread_report(models: Sequence[dict]) -> dict:
    """Capture spread of one pedal family from fits-JSON model records (``ref.harm_fixed`` and ``labels``):
    ``by_setting`` = groups of captures with the same labelled setting (two or more members), ``family`` = every
    capture of the family (the fallback when there are no labels)."""
    have = [m for m in models if m.get("ref", {}).get("harm_fixed") is not None]
    groups: dict[tuple, list[dict]] = {}
    for m in have:
        if m.get("labels"):
            groups.setdefault(_setting_key(m["labels"]), []).append(m)
    by_setting = []
    for key, ms in sorted(groups.items()):
        if len(ms) < 2:
            continue
        sp = harm_spread([m["ref"]["harm_fixed"] for m in ms])
        by_setting.append({"setting": dict(key), "model_ids": [m["model_id"] for m in ms], **sp})
    return {"family": harm_spread([m["ref"]["harm_fixed"] for m in have]), "by_setting": by_setting}


def reference_spread_db(spread: dict) -> float | None:
    """The spread the harmonic target is judged against: the mean of the same-setting group spreads when any group
    has two or more captures, else the whole-family spread."""
    vals = [g["mean_rms_db"] for g in spread.get("by_setting", []) if g.get("mean_rms_db") is not None]
    if vals:
        return float(np.mean(vals))
    return spread.get("family", {}).get("mean_rms_db")


# ---------------------------------------------------------------------------------------------------------
# targets and verdict
# ---------------------------------------------------------------------------------------------------------
def target_check(models: Sequence[dict], spread: dict) -> dict:
    """The v0.4 target on labelled captures (labels read from the capture's name; assumed pins do not count, and neither do labels that did not cover every
    searched knob, whose missing knobs were pinned at the block default: ``n_partially_labelled``)."""
    full = [m for m in models if m.get("labels") and "constrained" in m]
    lab = [m for m in full if not m["constrained"].get("pins_filled_with_default")]
    out: dict = {"n_labelled": len(lab), "n_partially_labelled": len(full) - len(lab), "ltas_ok": None, "harm_ok": None}
    ref_sp = reference_spread_db(spread)
    out["reference_spread_db"] = ref_sp
    if lab:
        ok = [m["constrained"]["ltas_rms_db"] <= TARGET_LTAS_DB for m in lab]
        out["ltas_within"] = int(sum(ok))
        out["ltas_fraction"] = float(np.mean(ok))
        out["ltas_ok"] = bool(np.mean(ok) >= TARGET_LTAS_FRACTION)
        h = float(np.mean([m["constrained"]["harm_rms_db"] for m in lab]))
        out["mean_constrained_harm_db"] = h
        if ref_sp is not None:
            out["harm_ok"] = bool(h <= TARGET_HARM_SPREAD_X * ref_sp)
    return out


def verdict(name: str, models: Sequence[dict], chk: dict) -> str:
    if not models:
        return f"{name}: PENDING USER RUN (no capture fits)."
    if not chk["n_labelled"]:
        free = float(np.mean([m["free"]["ltas_rms_db"] for m in models]))
        part = (f" ({chk['n_partially_labelled']} partially labelled, unlabelled knobs pinned at the block default, "
                "not counted)") if chk.get("n_partially_labelled") else ""
        return (f"{name}: no fully labelled captures{part}, so the v0.4 target cannot be judged; mean free-fit LTAS "
                f"{free:.2f} dB over {len(models)} captures.")
    parts = [f"constrained LTAS <= {TARGET_LTAS_DB:g} dB on {chk['ltas_within']}/{chk['n_labelled']} labelled "
             f"({'PASS' if chk['ltas_ok'] else 'MISS'}, need {TARGET_LTAS_FRACTION:.0%})"]
    if chk["harm_ok"] is None:
        parts.append("harmonic target n/a (no capture spread)")
    else:
        parts.append(f"constrained harm {chk['mean_constrained_harm_db']:.1f} dB vs {TARGET_HARM_SPREAD_X:g} x spread "
                     f"{chk['reference_spread_db']:.1f} dB ({'PASS' if chk['harm_ok'] else 'MISS'})")
    ok = chk["ltas_ok"] and chk["harm_ok"] is not False
    if chk.get("n_partially_labelled"):
        parts.append(f"{chk['n_partially_labelled']} partially labelled capture(s) (unlabelled knobs pinned at the block "
                     "default) not counted")
    return f"{name}: {'MEETS' if ok else 'MISSES'} the v0.4 target; " + "; ".join(parts) + "."


# ---------------------------------------------------------------------------------------------------------
# markdown
# ---------------------------------------------------------------------------------------------------------
def _f(x, nd: int = 2) -> str:
    return "-" if x is None else f"{x:.{nd}f}"


def _fit_cells(r: dict | None) -> str:
    if not r:
        return "- | - | -"
    return (f"{r['ltas_rms_db']:.2f} | {r['harm_rms_db']:.1f} ({r['harm_even_rms_db']:.1f}/{r['harm_odd_rms_db']:.1f})"
            f" | {r['dyn_db']:.2f}")


def known_answer_section(ka: dict | None) -> list[str]:
    L = ["## Known-answer results (A.1)", ""]
    if not ka:
        return L + ["_No known-answer file given (`sawblade-calibrate pedal-fit --known-answers`)._", ""]
    L += [f"Probe `{ka.get('probe', '?')}`, harmonic floor {ka.get('harm_floor_db')} dB, free-fit budget "
          f"{ka.get('search', {})}. The reference is the pedal itself rendered at the true params, so the error "
          "of a perfect fit is 0; metrics are LTAS shape (dB) / `harm_rms_db` (even/odd) / dynamics (dB).", "",
          *([f"_{ka['note']}_", ""] if ka.get("note") else []),
          "| pedal | version | true knobs | constrained (true params) LTAS / harm / dyn | free fit LTAS / harm / dyn "
          "| max knob error | unrecovered knobs |", "|---|---|---|---|---|---|---|"]
    for name in PEDAL_ORDER:
        r = ka.get("pedals", {}).get(name)
        if not r:
            continue
        c, f = r["constrained"], r["free"]
        errs = r.get("knob_errors", {})
        bad = ", ".join(f"{k} ({e:.2f})" for k, e in errs.items() if e > 0.5) or "none"
        L.append(f"| {name} | {r.get('model_version')} | {', '.join(f'{k} {v:g}' for k, v in r['truth'].items())} | "
                 f"{c['ltas_rms_db']:.3f} / {c['harm_rms_db']:.3f} / {c['dyn_db']:.3f} | "
                 f"{f['ltas_rms_db']:.3f} / {f['harm_rms_db']:.3f} / {f['dyn_db']:.3f} | "
                 f"{max(errs.values()) if errs else 0:.2f} | {bad} |")
    L.append("")
    return L


def generate(fits_by_pedal: dict[str, dict], known: dict | None = None) -> str:
    pending = [p for p in PEDAL_ORDER if p not in fits_by_pedal]
    L = ["# v0.4a pedal accuracy baseline", ""]
    if pending:
        L += [f"**Status: PENDING USER RUN** for {', '.join(pending)}: no capture fits exist for them yet "
              "(TONE3000 needs the user's OAuth login and the DI is not in the repo). No capture numbers appear "
              "below for those pedals; follow `docs/runbooks/v0_4a_pedal_accuracy.md`.", ""]
    else:
        L += ["Status: all five pedals fitted.", ""]
    L += ["Metrics: LTAS = 1/3-octave shape error (60 Hz-12 kHz, offset removed), dB RMS. `harm` = `harm_rms_db` "
          "(H2..H7 re fundamental, 16 stepped sines, clamped at the fixed floor), with even/odd parts in brackets. "
          "`dyn` = crest + envelope-spread error, dB. Free = knobs searched; constrained = knobs pinned to the "
          "capture's label (`labelled` = read from its name, `assumed` = from the targets manifest).", ""]
    L += known_answer_section(known)
    L += ["## Captures", ""]
    verdicts = []
    for name in PEDAL_ORDER:
        doc = fits_by_pedal.get(name)
        L += [f"### {name}", ""]
        if not doc:
            L += ["PENDING USER RUN: no fits file.", ""]
            verdicts.append(verdict(name, [], {}))
            continue
        models = doc["models"]
        L += [f"Model version {doc.get('model_version')}, harmonic floor {doc.get('cost', {}).get('harm_floor_db')} dB, "
              f"seed base {doc.get('seed')}.", "",
              "| tone | model | licence / creator | pins | free LTAS | free harm (even/odd) | free dyn "
              "| constrained LTAS | constrained harm (even/odd) | constrained dyn |",
              "|---|---|---|---|---|---|---|---|---|---|"]
        for m in models:
            pins = "none" if not m.get("pinned_knobs") else ("assumed" if m.get("pinned_is_assumed") else "labelled")
            if pins == "labelled" and m.get("constrained", {}).get("pins_filled_with_default"):
                pins = "partial (" + ", ".join(m["constrained"]["pins_filled_with_default"]) + " default)"
            lic = f"{m.get('license')} / {m.get('creator')}" + (" (non-commercial)" if m.get("non_commercial") else "")
            fc, cc = _fit_cells(m["free"]).split(" | "), _fit_cells(m.get("constrained")).split(" | ")
            L.append(f"| {m['tone_id']} | {m['name']} ({m['model_id']}) | {lic} | {pins} | "
                     f"{fc[0]} | {fc[1]} | {fc[2]} | {cc[0]} | {cc[1]} | {cc[2]} |")
        sp = doc.get("spread") or spread_report(models)
        chk = target_check(models, sp)
        L += ["", f"Capture spread (RMS distance of each fixed harmonic profile to its group mean, dB): "
                  f"family {_f(sp['family']['mean_rms_db'])} (n={sp['family']['n']})"
              + "".join(f"; setting {g['setting']} {_f(g['mean_rms_db'])} (n={g['n']})" for g in sp["by_setting"]) + ".",
              ""]
        v = verdict(name, models, chk)
        verdicts.append(v)
        L += [f"**Verdict.** {v}", ""]
    L += ["## Verdicts", ""] + [f"* {v}" for v in verdicts] + [""]
    return "\n".join(L)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-calibrate pedal-accuracy",
                                description="Write docs/reports/v0_4/accuracy.md from per-pedal fits JSON files.")
    p.add_argument("--fits", action="append", default=[], metavar="PATH",
                   help="a fits_<pedal>.json written by pedal-fit (repeatable); the pedal is read from the file")
    p.add_argument("--known-answers", metavar="PATH", help="known_answers.json written by pedal-fit --known-answers")
    p.add_argument("--out", default=str(DEFAULT_OUT))
    return p


def main(argv: Sequence[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    fits: dict[str, dict] = {}
    try:
        for f in a.fits:
            doc = json.loads(Path(f).read_text())
            if doc.get("schema") != "sawblade.pedal_fit" or int(doc.get("version", 0)) < 2:
                raise ValueError(f"{f}: not a pedal_fit schema-2 file (re-run pedal-fit; 7.1 files have no even/odd "
                                 "harmonic terms or capture profiles)")
            fits[doc["pedal"]] = doc
        known = json.loads(Path(a.known_answers).read_text()) if a.known_answers else None
        text = generate(fits, known)
    except (ValueError, OSError, KeyError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
