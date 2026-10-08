"""``sawblade-bench compare A.json B.json`` (spec B.4): per-case deltas B - A of the held-out distances, with regression flags.
All metrics are distances to the reference (lower is better)."""
from __future__ import annotations

import json
from pathlib import Path

from .runner import SCHEMA

# Regression thresholds (lead, stated): B - A above these flags a metric. The feel ones are the D.1 known-answer tolerances
# (known_answer.FEEL_TOLERANCES); the A-weighted one is the lead's provisional noise bar (the benchmark fixes the seed).
THRESHOLDS = {"aWeightedErrorDb": 0.30, "t12Ms": 10.0, "sustainDb": 1.5, "hfRatioDb": 1.0, "hfFlat": 0.02, "fluxDb": 0.3, "floorDb": 3.0}
BETTER_AWT_DB = 0.30
EPS = 1e-9


class CompareError(ValueError):
    """Unreadable input or schema mismatch (exit 2)."""


def load_scores(path) -> dict:
    try:
        s = json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        raise CompareError(f"cannot read {path}: {e}") from e
    if not isinstance(s, dict) or s.get("schema") != SCHEMA or not isinstance(s.get("cases"), dict):
        raise CompareError(f"{path} is not a {SCHEMA} file")
    return s


def parse_threshold_overrides(items) -> dict:
    out = {}
    for it in items or []:
        k, _, v = str(it).partition("=")
        if k not in THRESHOLDS:
            raise CompareError(f"--threshold: unknown metric {k!r} (one of {', '.join(THRESHOLDS)})")
        try:
            out[k] = float(v)
        except ValueError:
            raise CompareError(f"--threshold {it!r}: not KEY=NUMBER") from None
    return out


def metrics(held: dict | None) -> dict:
    """The comparable numbers of a held-out block: A-weighted error and the raw feel deltas (None where absent)."""
    held = held or {}
    d = ((held.get("feel") or {}).get("deltas")) or {}
    return {"aWeightedErrorDb": held.get("aWeightedErrorDb"), **{k: d.get(k) for k in THRESHOLDS if k != "aWeightedErrorDb"}}


def _row(label: str, a_held, b_held, thr: dict) -> dict:
    ma, mb = metrics(a_held), metrics(b_held)
    delta = {k: (mb[k] - ma[k]) if ma[k] is not None and mb[k] is not None else None for k in THRESHOLDS}
    regress = [k for k, v in delta.items() if v is not None and v > thr[k] + EPS]
    return {"label": label, "a": ma, "b": mb, "delta": delta, "regressed": regress,
            "improved": delta["aWeightedErrorDb"] is not None and delta["aWeightedErrorDb"] < -BETTER_AWT_DB - EPS}


def _guard(c) -> set:
    return set(((c.get("heldOut") or {}).get("guardrails") or {}).get("fail") or [])


def compare(a: dict, b: dict, overrides: dict | None = None) -> dict:
    thr = {**THRESHOLDS, **(overrides or {})}
    cases, counts = {}, {"better": 0, "worse": 0, "same": 0, "notCompared": 0}
    for cid in sorted(set(a["cases"]) | set(b["cases"])):
        ca, cb = a["cases"].get(cid), b["cases"].get(cid)
        if not (ca and cb and ca.get("status") == "ok" and cb.get("status") == "ok"):
            why = "missing in one file" if not (ca and cb) else f"status A {ca.get('status')}, B {cb.get('status')}"
            cases[cid] = {"verdict": "not compared", "reason": why, "rows": []}
            counts["notCompared"] += 1
            continue
        rows = [_row(cid, ca.get("heldOut"), cb.get("heldOut"), thr)]
        pa, pb = ca.get("pathchecks") or {}, cb.get("pathchecks") or {}
        for k in ("a", "b"):          # inner path checks; a manifest pathcheck case carries them as its own case
            if k in pa and k in pb and not pb[k].get("case"):
                rows.append(_row(f"{cid}/path {k}", pa[k], pb[k], thr))
        for i, (ta, tb) in enumerate(zip(ca.get("transfer") or [], cb.get("transfer") or [])):
            if ta.get("status") == "ok" and tb.get("status") == "ok":
                rows.append(_row(f"{cid}/transfer {i}", ta["heldOut"], tb["heldOut"], thr))
        ra, rb = (pa.get("ratio") or {}).get("diffDb"), (pb.get("ratio") or {}).get("diffDb")
        newly = sorted(_guard(cb) - _guard(ca))
        if ca.get("tier") == 2:
            verdict = "shown"
        elif any(r["regressed"] for r in rows):
            verdict = "worse"
        elif rows[0]["improved"]:
            verdict = "better"
        else:
            verdict = "same"
        if verdict in counts:
            counts[verdict] += 1
        cases[cid] = {"verdict": verdict, "rows": rows, "newGuardrailFails": newly,
                      "ratioDiffDb": {"a": ra, "b": rb}}
    return {"counts": counts, "thresholds": thr, "overrides": overrides or {}, "cases": cases,
            "baseline": a.get("gitSha"), "candidate": b.get("gitSha")}


def summary_line(r: dict) -> str:
    c = r["counts"]
    t = ", ".join(f"{k} {v:g}" for k, v in r["thresholds"].items())
    return f"compare: better on {c['better']}, worse on {c['worse']}, same on {c['same']}, not compared on {c['notCompared']} (thresholds: {t})"


def format_markdown(r: dict) -> str:
    f = lambda v, p="{:+.2f}": "-" if v is None else p.format(v)
    keys = list(THRESHOLDS)
    lines = ["| case | verdict | " + " | ".join(keys) + " |", "|---|---|" + "---|" * len(keys)]
    for cid, c in r["cases"].items():
        if not c["rows"]:
            lines.append(f"| {cid} | not compared ({c['reason']}) |" + " |" * len(keys))
            continue
        for i, row in enumerate(c["rows"]):
            cells = [f(row["delta"][k], "{:+.3f}") + ("!" if k in row["regressed"] else "") for k in keys]
            lines.append(f"| {row['label']} | {c['verdict'] if i == 0 else ''} | " + " | ".join(cells) + " |")
    lines += ["", "`!` = regression beyond the threshold (B - A, lower is better).", ""]
    for cid, c in r["cases"].items():
        if c.get("newGuardrailFails"):
            lines.append(f"newly failing guardrails in {cid}: {', '.join(c['newGuardrailFails'])}")
    if r["overrides"]:
        lines.append("threshold overrides: " + ", ".join(f"{k}={v:g}" for k, v in r["overrides"].items()))
    lines.append(summary_line(r))
    return "\n".join(lines)
