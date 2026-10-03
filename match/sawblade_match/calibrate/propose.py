"""Threshold proposal: calibrate the rule thresholds to the original's guitar levels.

For a rule ``lhs OP rhs + c`` the calibrated offset ``c`` makes the original's guitars *pass with ~ the rule's
tolerance as margin*:

    <=  rules:  c = ceil2((lhs - rhs) + tol)      (lhs sits ``tol`` dB below the threshold)
    >=  rules:  c = floor2((lhs - rhs) - tol)     (lhs sits ``tol`` dB above the threshold)

``ceil2``/``floor2`` round outwards to 0.5 dB, so the margin is >= tol and < tol + 0.5.

Policies (``policy`` argument / ``--policy``):
* ``loosen-only`` (default): only a rule the basis original passes *marginally* is recalibrated with the formula
  above; rules it passes keep their current expression.
* ``tighten`` (the spec's literal behaviour): every non-contradicted rule is recalibrated, which also
  tightens rules the original passes with a large margin.

A rule is *contradicted* when the original **fails** the current rule on the basis measurement (violates it by
more than its tolerance): that is evidence against the rule itself, not a calibration, so it is NOT changed in
the proposal's ``rules``; it is listed under ``calibration.contradictedRules`` with the measured values and the
calibrated replacement the lead may adopt.
"""
from __future__ import annotations

import copy
import math

from ..tonecheck.rules import classify, evaluate_rules, parse_expr

STEP_DB = 0.5
POLICIES = ("loosen-only", "tighten")


def measured_diff(rule: dict, groups: dict[str, float]) -> float:
    """lhs level minus rhs level (dB) for a rule."""
    e = parse_expr(rule["expr"])
    return groups[e.lhs] - groups[e.rhs]


def format_expr(lhs: str, op: str, rhs: str, offset: float) -> str:
    if offset == 0:
        return f"{lhs} {op} {rhs}"
    return f"{lhs} {op} {rhs} {'+' if offset > 0 else '-'} {abs(offset):g}"


def calibrated_offset(rule: dict, groups: dict[str, float]) -> float:
    e = parse_expr(rule["expr"])
    tol = float(rule.get("toleranceDb", 0.0))
    d = measured_diff(rule, groups)
    if e.op == "<=":
        c = math.ceil((d + tol) / STEP_DB - 1e-9) * STEP_DB
    else:
        c = math.floor((d - tol) / STEP_DB + 1e-9) * STEP_DB
    return float(c) + 0.0          # normalise -0.0


def propose(targets: dict, basis_groups: dict[str, float], basis_name: str,
            other_groups: dict[str, dict[str, float]] | None = None,
            provenance: dict | None = None, policy: str = "loosen-only") -> tuple[dict, list[dict], list[dict]]:
    """Return (proposed targets JSON, rule table, contradicted rules).

    ``basis_groups``: the original's group levels (method ``basis_name``) that the thresholds are set from;
    ``other_groups``: other methods' group levels, shown as evidence (status of the *current* rule on each)."""
    if policy not in POLICIES:
        raise ValueError(f"unknown policy {policy!r}")
    other_groups = other_groups or {}
    new = copy.deepcopy(targets)
    table, contradicted = [], []
    for i, rule in enumerate(targets["rules"]):
        e = parse_expr(rule["expr"])
        tol = float(rule.get("toleranceDb", 0.0))
        d = measured_diff(rule, basis_groups)
        cur = evaluate_rules(basis_groups, [rule])[0]
        c_new = calibrated_offset(rule, basis_groups)
        if policy == "loosen-only" and cur["status"] == "pass":
            c_new = e.offset
        new_expr = format_expr(e.lhs, e.op, e.rhs, c_new)
        row = {"id": rule["id"], "currentExpr": rule["expr"], "proposedExpr": new_expr,
               "measuredDiffDb": round(d, 2), "currentStatusOnBasis": cur["status"],
               "currentMarginDb": cur["margin"], "toleranceDb": tol,
               "thresholdChangeDb": round(c_new - e.offset, 2),
               "otherMethods": {k: evaluate_rules(g, [rule])[0]["status"] for k, g in other_groups.items()}}
        if cur["status"] == "fail":
            row["decision"] = "contradicted"
            contradicted.append({**row, "why": rule.get("why", ""),
                                 "evidence": (f"original ({basis_name}): {e.lhs} - {e.rhs} = {d:+.1f} dB; rule needs "
                                              f"{'<=' if e.op == '<=' else '>='} {e.offset:+.1f} (tolerance {tol:g}) -> "
                                              f"violated by {-cur['margin']:.1f} dB")})
            new["rules"][i]["calibration"] = {"decision": "contradicted-unchanged", "measuredDiffDb": round(d, 2),
                                              "proposedExprIfAccepted": new_expr}
        else:
            row["decision"] = "calibrated" if new_expr != rule["expr"] else "unchanged"
            new["rules"][i]["expr"] = new_expr
            new["rules"][i]["calibration"] = {"was": rule["expr"], "measuredDiffDb": round(d, 2), "marginDb": tol}
        table.append(row)
    new["version"] = int(targets.get("version", 1)) + 1
    new["status"] = (f"PROPOSAL (not adopted) - {policy} thresholds calibrated on the original's guitars, basis: {basis_name}; "
                     f"{len(contradicted)} rule(s) contradicted by the data are left unchanged")
    pol = ("only rules the basis original fails/marginally passes are changed; the original then passes with ~toleranceDb margin"
           if policy == "loosen-only" else "every non-contradicted rule is set so the original passes with ~toleranceDb margin")
    new["calibration"] = {"basis": basis_name, "policy": policy, "policyDescription": pol,
                          "contradictedRules": contradicted, **(provenance or {})}
    return new, table, contradicted


def consistency_check(proposed: dict, groups: dict[str, float]) -> dict[str, str]:
    """Status of every rule in ``proposed`` for ``groups`` (used to assert the original passes)."""
    return {r["id"]: r["status"] for r in evaluate_rules(groups, proposed["rules"])}


__all__ = ["propose", "calibrated_offset", "format_expr", "measured_diff", "consistency_check", "classify"]
