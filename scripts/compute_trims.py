#!/usr/bin/env python3
"""Fills `output.autoTrimDb` / `output.autoTrimHash` of the committed presets and writes the loudness table.

    scripts/compute_trims.py [--tonerender build/cli/tonerender] [--presets presets]
                             [--table docs/reports/v0_3/loudness_table.md] [--check]

For every preset under --presets (recursive) it runs `tonerender --trim-report`: the built-in reference DI
(core/include/sawblade/reference_di.h) through the whole preset, BS.1770 integrated loudness, trim = -18 LUFS - measured (limited to +12 dB; 0 for a rig with no active non-linear block),
and the staleness hash of the level-affecting parts (docs/PRESET_SCHEMA.md "Level matching"). The result is patched into the
preset file as text (only `version` -> 3 and the `output` object change, the rest of the file keeps its formatting) and the
table `loudness_table.md` is regenerated (before = no trim, after = with the trim).

A preset whose TONE3000 captures are not on this machine (not in $SAWBLADE_CACHE_DIR / ~/.cache/sawblade/captures) is reported
as "skipped: capture not cached" and left alone: its trim is computed in the plugin at load (background thread), or run this
script again on a machine that has the captures (`sawblade-t3k resolve <preset>` fetches them).

Rerunnable: a second run changes nothing. --check changes nothing at all and exits 1 when a computable preset has no trim, a stale
hash or a trim more than 0.01 dB off (for CI / before a commit). Needs only the Python standard library.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRIM_DECIMALS = 3  # the file stores the trim to 0.001 dB; the hash, not the number, decides staleness


def skip_ws(text: str, i: int) -> int:
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    return i


def top_level(text: str):
    """{key: (value_start, value_end, value)} of the root object, and the index of its closing brace."""
    dec = json.JSONDecoder()
    i = skip_ws(text, 0)
    if text[i] != "{":
        raise ValueError("not a JSON object")
    i += 1
    spans: dict = {}
    while True:
        i = skip_ws(text, i)
        if text[i] == "}":
            return spans, i
        if text[i] == ",":
            i += 1
            continue
        key, j = dec.raw_decode(text, i)
        j = skip_ws(text, j)
        if text[j] != ":":
            raise ValueError("malformed object")
        j = skip_ws(text, j + 1)
        val, k = dec.raw_decode(text, j)
        spans[key] = (j, k, val)
        i = k


def first_key_indent(text: str) -> str:
    i = text.index("{") + 1
    while i < len(text) and text[i] in "\r\n":
        i += 1
    j = i
    while j < len(text) and text[j] in " \t":
        j += 1
    return text[i:j] if text[j] == '"' and "\n" in text[: j + 1] else "  "


def render_output(obj: dict, old_text: str | None, indent: str) -> str:
    if old_text is not None and "\n" not in old_text:  # inline style: { "gainDb": -3.0 } or {"gainDb": -3.0}
        spaced = old_text.startswith("{ ")
        body = ", ".join(f"{json.dumps(k)}: {json.dumps(v)}" for k, v in obj.items())
        return "{ " + body + " }" if spaced else "{" + body + "}"
    inner = indent * 2
    lines = [f"{inner}{json.dumps(k)}: {json.dumps(v)}" for k, v in obj.items()]
    return "{\n" + ",\n".join(lines) + "\n" + indent + "}"


def patch_preset(text: str, trim_db: float, digest: str) -> str:
    spans, close = top_level(text)
    indent = first_key_indent(text)
    old_out = spans.get("output")
    out = dict(old_out[2]) if old_out and isinstance(old_out[2], dict) else {}
    out.pop("autoTrimDb", None)
    out.pop("autoTrimHash", None)
    out["autoTrimDb"] = round(trim_db, TRIM_DECIMALS)
    out["autoTrimHash"] = digest
    edits = []  # (start, end, replacement)
    if "version" in spans and spans["version"][2] not in (3, 4):  # never downgrade a v4 file
        edits.append((spans["version"][0], spans["version"][1], "3"))
    if old_out:
        edits.append((old_out[0], old_out[1], render_output(out, text[old_out[0]:old_out[1]], indent)))
    else:
        last = max(v[1] for v in spans.values())
        edits.append((last, last, ",\n" + indent + '"output": ' + render_output(out, None, indent)))
    for s, e, r in sorted(edits, reverse=True):
        text = text[:s] + r + text[e:]
    json.loads(text)  # still valid JSON
    return text


def fmt(x, nd=2):
    return "—" if x is None else f"{x:.{nd}f}"


def stored_trim(path: Path):
    try:
        out = json.loads(path.read_text()).get("output", {})
        return out.get("autoTrimDb"), out.get("autoTrimHash")
    except (OSError, ValueError):
        return None, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tonerender", default=str(ROOT / "build/cli/tonerender"))
    ap.add_argument("--presets", default=str(ROOT / "presets"))
    ap.add_argument("--table", default=str(ROOT / "docs/reports/v0_3/loudness_table.md"))
    ap.add_argument("--check", action="store_true", help="change nothing; exit 1 when a computable preset is missing / stale")
    a = ap.parse_args()

    files = sorted(Path(a.presets).rglob("*.json"))
    if not files:
        print(f"compute_trims: no presets under {a.presets}", file=sys.stderr)
        return 2
    proc = subprocess.run([a.tonerender, "--trim-report", *map(str, files)], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr)
        return proc.returncode
    report = json.loads(proc.stdout)

    rows, bad, computed, skipped, errors = [], 0, 0, 0, 0
    for e in report["presets"]:
        path = Path(e["file"])
        rel = path.resolve().relative_to(ROOT) if path.resolve().is_relative_to(ROOT) else path
        if e["status"] == "ok":
            computed += 1
            trim = round(e["trimDb"], TRIM_DECIMALS)
            old, oldHash = stored_trim(path)
            fresh = oldHash == e["hash"] and old is not None and abs(old - trim) <= 0.01
            if not fresh:
                bad += 1
                if not a.check:
                    path.write_text(patch_preset(path.read_text(), e["trimDb"], e["hash"]))
                    print(f"compute_trims: {rel}: trim {trim:+.3f} dB written")
                else:
                    print(f"compute_trims: {rel}: trim missing or stale", file=sys.stderr)
            rows.append(f"| `{rel}` | {fmt(e['lufsBefore'])} | {trim:+.2f} | {fmt(e['lufsAfter'])} | {e.get('outputGainDb', 0.0):+.2f} |")
        elif e["status"] == "skipped":
            skipped += 1
            old, oldHash = stored_trim(path)
            note = f"skipped: {e['reason']}" + (f" (stored trim {old:+.2f} dB, not re-measured here)" if old is not None else "")
            rows.append(f"| `{rel}` | {note} | | | {e.get('outputGainDb', 0.0):+.2f} |")
            print(f"compute_trims: {rel}: skipped: {e['reason']}")
        else:
            errors += 1
            rows.append(f"| `{rel}` | error: {e.get('reason', '?')} | | | |")
            print(f"compute_trims: {rel}: error: {e.get('reason', '?')}", file=sys.stderr)

    if not a.check:
        table = Path(a.table)
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_text(
            "# Loudness table (v0.3 Task B)\n\n"
            "Generated by `scripts/compute_trims.py` (do not edit by hand). Reference signal: the built-in reference DI "
            "(`core/include/sawblade/reference_di.h`, version 1, 10 s, 48 kHz, peak -10 dBFS) through the whole preset "
            "(cab, post EQ, bus comp, the preset's own output gain), BS.1770-4 integrated loudness of the mono output "
            f"(right channel silent). Target {report['target']:.0f} LUFS. *Before* = no trim, *after* = with "
            "`output.autoTrimDb` applied; both at OUTPUT 0 dB. The preset as stored plays at -18 LUFS + its *output offset* "
            "(its stored `output.gainDb`, a persistent user offset).\n\n"
            f"{computed} computed, {skipped} skipped, {errors} errors. Skipped presets need TONE3000 captures that are not on the "
            "machine that ran the script; their trim is computed in the plugin at load, or run the script again where the "
            "captures are cached.\n\n"
            "| Preset | LUFS before | Trim (dB) | LUFS after | Output offset (dB) |\n|---|---:|---:|---:|---:|\n" + "\n".join(rows) + "\n"
        )
        print(f"compute_trims: table written to {table}")
    print(f"compute_trims: {computed} computed, {skipped} skipped, {errors} errors")
    if a.check:
        return 1 if bad or errors else 0
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
