"""Machine-readable targets (docs/tone_targets.json) and rule evaluation."""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

_EXPR = re.compile(r"^\s*(\w+)\s*(<=|>=)\s*(\w+)\s*(?:([+-])\s*([0-9.]+))?\s*$")


@dataclass(frozen=True)
class Expr:
    lhs: str
    op: str          # "<=" or ">="
    rhs: str
    offset: float    # added to the rhs group level (dB)


def parse_expr(text: str) -> Expr:
    """Parse ``group (<=|>=) group [+|- number]`` (dB offsets)."""
    m = _EXPR.match(text)
    if not m:
        raise ValueError(f"unsupported rule expression: {text!r}")
    lhs, op, rhs, sign, num = m.groups()
    off = 0.0 if num is None else (float(num) if sign == "+" else -float(num))
    return Expr(lhs, op, rhs, off)


def load_targets(path: str | Path) -> dict:
    t = json.loads(Path(path).read_text())
    if t.get("schema") != "sawblade.tone_targets":
        raise ValueError(f"{path}: not a sawblade.tone_targets file")
    return t


def classify(margin: float, tolerance_db: float) -> str:
    """margin >= 0 pass; violated by at most the tolerance -> marginal; else fail."""
    if margin >= 0:
        return "pass"
    if margin >= -tolerance_db:
        return "marginal"
    return "fail"


def evaluate_rules(groups_db: dict[str, float], rules: list[dict]) -> list[dict]:
    """Evaluate each rule. margin is positive when satisfied (dB of headroom)."""
    out = []
    for r in rules:
        e = parse_expr(r["expr"])
        value = groups_db[e.lhs]
        threshold = groups_db[e.rhs] + e.offset
        margin = threshold - value if e.op == "<=" else value - threshold
        tol = float(r.get("toleranceDb", 0.0))
        out.append({
            "id": r["id"], "expr": r["expr"], "group": e.lhs, "op": e.op,
            "value": round(value, 3), "threshold": round(threshold, 3), "margin": round(margin, 3),
            "toleranceDb": tol, "status": classify(margin, tol), "why": r.get("why", ""),
        })
    return out


def summarize(results: list[dict]) -> dict:
    counts = {"pass": 0, "marginal": 0, "fail": 0, "n/a": 0}
    for r in results:
        counts[r["status"]] += 1
    overall = "fail" if counts["fail"] else ("marginal" if counts["marginal"] else "pass")
    return {**counts, "overall": overall}
