"""Tone profiles (docs: profiles/README.md): guardrail rules + tolerances + provenance.

A profile is a tone-targets document (same ``analysis`` / ``rules`` / ``metrics`` as docs/tone_targets.json) with
``schema: "sawblade.profile"``, an ``id`` and ``provenance``. The reference LTAS is always the matching target; the
rules are guardrails reported by tonecheck (and counted per candidate in result.json).

* ``load_profile(id | path)``: ``profiles/<id>.json`` next to the repo root, or an explicit path.
* ``derive_profile(base, reference_signal, ...)``: the reference-derived default. The base profile contributes the
  rule skeleton (relations between band groups); every rule the reference's isolated guitars pass is kept as it is,
  every rule they fail or pass only marginally is loosened (never tightened) so that the reference passes with about
  its tolerance as margin, using sawblade_match.calibrate.propose.calibrated_offset (0.5 dB steps).
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

import numpy as np

from ..calibrate.propose import calibrated_offset, format_expr
from ..tonecheck.analysis import analyze
from ..tonecheck.rules import evaluate_rules, load_targets, parse_expr

RATE = 48000
REPO = Path(__file__).resolve().parents[3]
PROFILES_DIR = REPO / "profiles"
DEFAULT_BASE = "swedish_death_hm2"


def profile_path(spec: str) -> Path:
    p = Path(spec)
    if p.suffix == ".json" or p.exists():
        return p
    return PROFILES_DIR / f"{spec}.json"


def load_profile(spec: str) -> dict:
    return load_targets(profile_path(spec))


def derive_profile(base: dict, ref_sig: np.ndarray, basis: str, ref_name: str, rate: int = RATE) -> tuple[dict, list[dict]]:
    """Return (derived profile, change table). ``ref_sig``: mono guitar signal of the reference (stem / side / sections)."""
    groups = analyze(ref_sig.astype(np.float64), rate, base).groups
    new = copy.deepcopy(base)
    table = []
    for i, rule in enumerate(base["rules"]):
        e = parse_expr(rule["expr"])
        cur = evaluate_rules(groups, [rule])[0]
        row = {"id": rule["id"], "baseExpr": rule["expr"], "statusOnReference": cur["status"],
               "measuredDiffDb": round(groups[e.lhs] - groups[e.rhs], 2)}
        if cur["status"] == "pass":
            row["derivedExpr"] = rule["expr"]
        else:
            c = calibrated_offset(rule, groups)
            row["derivedExpr"] = format_expr(e.lhs, e.op, e.rhs, c)
            new["rules"][i]["expr"] = row["derivedExpr"]
            new["rules"][i]["derivedFrom"] = {"was": rule["expr"], "statusOnReference": cur["status"]}
        table.append(row)
    base_id = base.get("id", "docs/tone_targets.json")
    new["schema"] = "sawblade.profile"
    new["version"] = 1
    new["id"] = f"derived:{ref_name}"
    new["name"] = f"derived:{ref_name}"
    new["calibrated"] = "reference-derived"          # explicit: independent of the base profile's own flag
    new["notes"] = [f"Rule offsets are measured from the reference's isolated guitars ({basis}); rules the reference "
                    "passes are unchanged, the others loosened (never tightened).",
                    f"Rule 'why' texts and the report-only 'metrics' definitions are inherited from {base_id}."]
    new["status"] = (f"reference-derived (loosen-only) from {ref_name} [{basis}], rule skeleton "
                     f"{base.get('id', base.get('source', 'base'))}")
    new["provenance"] = {"kind": "reference-derived", "reference": ref_name, "basis": basis,
                         "baseProfile": base_id, "policy": "loosen-only",
                         "rulesOffsets": "measured from the reference's guitars",
                         "metrics": f"inherited priors from {base_id} (report-only, not measured here)",
                         "referenceGroupsDbRe1k": {k: round(float(v), 2) for k, v in groups.items()}}
    return new, table


def save_profile(p: dict, path: Path) -> None:
    Path(path).write_text(json.dumps(p, indent=2) + "\n")
