"""Summary printer of ``scripts/run_v04m_validation.sh`` (paste-back text for the lead).

    python -m sawblade_match.matcher.validation_summary <out dir>

One block per run folder holding a ``result.json`` (blend runs first, then single-amp runs, then ablations): the run's
numbers, the matched gate and bus comp in full (read from the winning preset), the per-path check
(``pathcheck.json``) and the dynamics sweep table (``dynsweep.json``, matched vs bypassed). Then every top-level
``*.pathcheck.json`` (the held-out transfer). Missing pieces are skipped, never fatal.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

GATE_KEYS = ("enabled", "mode", "thresholdDb", "hysteresisDb", "attackMs", "holdMs", "releaseMs", "rangeDb", "ratio",
             "keyHighPassHz", "releaseCurve")
COMP_KEYS = ("enabled", "thresholdDb", "ratio", "kneeDb", "attackMs", "releaseMs")


def _fmt(v) -> str:
    return "default" if v is None else (f"{v:g}" if isinstance(v, float) else str(v))


def gate_line(preset: dict | None, di_floor_db=None) -> str:
    """The matched gate in full, one line; ``ratio`` only for an expander."""
    g = (preset or {}).get("gate")
    if not g:
        return "gate: off (not in the preset)"
    keys = [k for k in GATE_KEYS if k != "ratio" or g.get("mode") == "expander"]
    s = "gate: " + " ".join(f"{k}={_fmt(g.get(k))}" for k in keys)
    if di_floor_db is not None:
        s += f" | threshold set from the DI noise floor {di_floor_db:.1f} dBFS"
    return s


def comp_line(preset: dict | None) -> str:
    c = (preset or {}).get("busComp")
    if not c or not c.get("enabled"):
        return "busComp: off"
    return "busComp: " + " ".join(f"{k}={_fmt(c.get(k))}" for k in COMP_KEYS)


def _load(p: Path):
    try:
        return json.loads(p.read_text())
    except Exception:
        return None


def pathcheck_lines(pc: dict) -> list[str]:
    f = lambda v: "n/a" if v is None else f"{v:.2f}"
    if pc.get("singlePath"):
        fu = pc.get("full") or {}
        return [f"  per-path: singlePath true (path {str(pc.get('path')).upper()}); full vs ref-{fu.get('ref')} "
                f"{f(fu.get('aWeightedErrorDb'))} dB"]
    a, b, s, q = pc.get("a") or {}, pc.get("b") or {}, pc.get("swapped") or {}, pc.get("ratio") or {}
    out = [f"  per-path LTAS (A-weighted dB): A vs ref-a {f(a.get('aWeightedErrorDb'))}, B vs ref-b {f(b.get('aWeightedErrorDb'))}"
           + (f", full vs blend {f(pc['full'].get('aWeightedErrorDb'))}" if pc.get("full") else "") + " | singlePath false",
           f"  swapped: A vs ref-b {f(s.get('aVsRefB'))}, B vs ref-a {f(s.get('bVsRefA'))}",
           f"  blend ratio A-B: chosen {f(q.get('chosenDb'))} dB vs reference {f(q.get('refDb'))} dB (diff {f(q.get('diffDb'))})"]
    return out


def run_block(d: Path) -> list[str]:
    r = _load(d / "result.json")
    if r is None:
        return []

    def g(*ks):
        v = r
        for k in ks:
            v = v.get(k) if isinstance(v, dict) else None
        return v
    b = r.get("best", {}); bd = b.get("breakdown", {}) or {}; ft = bd.get("feelTerms") or {}
    L = [f"== {d.name}  wall {r.get('wallSeconds', 0)/60:.1f} min"]
    L.append(f"  A-weighted dB: {(r.get('after') or [{}])[0].get('aWeightedErrorDb')} | loss {bd.get('total')} "
             f"ltas {bd.get('ltas')} feel {bd.get('feel')}")
    L.append(f"  feel: tight {ft.get('tight')} fizz {ft.get('fizz')} polish {ft.get('polish')} dropped {ft.get('dropped')}")
    L.append(f"  chain: {b.get('topology')} {({k: (v or {}).get('title') for k, v in (b.get('captures') or {}).items()})}")
    L.append(f"  boost won: {g('tightBoost', 'won')} | IR pair won: {g('irBlend', 'won')} | IR winner: {g('irPool', 'winner')}")
    L.append(f"  pre-EQ: {g('preEq', 'chosen')} | studio: {({k: g('studio', k) for k in ('compressed', 'eqd', 'busCompUsed')})}")
    preset = _load(d / (b.get("preset") or "best.preset.resolved.json"))
    L.append("  " + gate_line(preset if preset is not None else {"gate": r.get("gateFinal")}, r.get("diNoiseFloorDb")))
    L.append("  " + comp_line(preset))
    L.append(f"  post filters: {r.get('postFilters')}")
    L.append(f"  listening gain dB: {g('listening', 'gainDb')} | IR pool: "
             f"{({k: g('irPool', k) for k in ('total', 'screened', 'prefiltered')})}")
    tc = (r.get("tonecheck") or {}).get("best_L", {})
    L.append(f"  guardrails: {[(x.get('id'), x.get('status')) for x in tc.get('rules', []) if x.get('status') != 'pass']}")
    if r.get("trace"):
        L.append(f"  trace: {({k: (v or {}).get('why') for k, v in r['trace'].items()})}")
    pc = _load(d / "pathcheck.json")
    if pc:
        L += pathcheck_lines(pc)
    ds = _load(d / "dynsweep.json")
    if ds:
        try:
            from .dynformat import format_table
            L += ["  " + ln for ln in format_table(ds).splitlines()]
        except Exception as e:                                 # pragma: no cover - reporting only
            L.append(f"  dynsweep: could not format ({e})")
    return L


def order_key(d: Path):
    n = d.name
    return (0 if "_blend" in n else (2 if "_no_" in n else 1), n)


def ref_lines(out: Path) -> list[str]:
    """One block per blend reference (``refs/<side>_blend.json``): the polarity choice and both 60-250 Hz levels, the lag
    (reported, never shifted), the correlation and the reference ratio."""
    lines: list[str] = []
    for f in sorted((out / "refs").glob("*_blend.json")) if (out / "refs").is_dir() else []:
        r = _load(f)
        if not r:
            continue
        pol = r.get("polarity")
        g = lambda v, fmt="{:.2f}": "n/a" if v is None else fmt.format(v)
        if isinstance(pol, dict):
            p = (f"polarity {pol.get('mode')} -> {pol.get('chosen')} (60-250 Hz: as recorded {g(pol.get('lowBandDbAsis'), '{:.1f}')} dB, "
                 f"b inverted {g(pol.get('lowBandDbInvert'), '{:.1f}')} dB)")
        else:
            p = "polarity: not recorded (built before the polarity option)"
        lines.append(f"== refs/{f.stem}: {p}; lag {g(r.get('lagMs'))} ms (not shifted), corr {g(r.get('corr'), '{:+.3f}')}, "
                     f"reference ratio A-B {g(r.get('refRatioDb'))} dB, faders {r.get('gainsDb')}")
    return lines


def summary(out: Path) -> str:
    lines: list[str] = ref_lines(out)
    for d in sorted((p for p in out.iterdir() if p.is_dir() and p.name != "refs"), key=order_key):
        lines += run_block(d)
    for f in sorted(out.glob("*.pathcheck.json")):
        pc = _load(f)
        if pc:
            lines.append(f"== {f.name} (held-out transfer: the left preset on the right side, not re-fitted)")
            lines += pathcheck_lines(pc)
    return "\n".join(lines)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: validation_summary <out dir>", file=sys.stderr)
        return 2
    print(summary(Path(argv[0])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
