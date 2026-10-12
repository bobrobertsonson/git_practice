#!/usr/bin/env python3
"""Check that the C++ defaults/presets (dsp/rasp.h, dsp/presets.h) match the
playground's DEFAULTS/PRESETS, so the pedal, the renderer and the browser
preview all sound the same. Run via `make check`."""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
html = (ROOT / "playground" / "index.html").read_text()


def js_object(name):
    block = re.search(r"const " + name + r" = (\{.*?\n\});", html, re.S).group(1)
    block = re.sub(r"([{,]\s*)([A-Za-z_]\w*)\s*:", r'\1"\2":', block)
    block = re.sub(r",(\s*[}\]])", r"\1", block)
    return json.loads(block)


cpp = json.loads(subprocess.run([str(ROOT / "presets_dump")], capture_output=True, text=True, check=True).stdout)
js_defaults, js_presets = js_object("DEFAULTS"), js_object("PRESETS")
problems = []


def compare(label, c, j):
    for k in sorted(set(c) | set(j)):
        if k not in j:
            problems.append(f"{label}: {k} missing in playground")
        elif k not in c:
            problems.append(f"{label}: {k} missing in C++")
        elif isinstance(c[k], bool) or isinstance(j[k], bool):
            if bool(c[k]) != bool(j[k]):
                problems.append(f"{label}: {k} C++ {c[k]} vs playground {j[k]}")
        elif abs(float(c[k]) - float(j[k])) > 1e-4:
            problems.append(f"{label}: {k} C++ {c[k]} vs playground {j[k]}")


# Learn's reference tone must be the same in both engines.
js_target = json.loads(re.search(r"const MATCH_TARGET = (\[[^\]]*\])", html).group(1))
if [round(v, 3) for v in js_target] != [round(v, 3) for v in cpp.pop("matchTarget")]:
    problems.append("Learn target (kMatchTarget vs MATCH_TARGET) differs")
compare("defaults", cpp["defaults"], js_defaults)
for name in js_presets:
    if name not in cpp:
        problems.append(f"preset {name} missing in C++")
        continue
    compare(f"preset {name}", cpp[name], {**js_defaults, **js_presets[name]})
for name in cpp:
    if name != "defaults" and name not in js_presets:
        problems.append(f"preset {name} missing in playground")
print("\n".join(problems) if problems else f"OK: defaults and {len(js_presets)} presets match")
sys.exit(1 if problems else 0)
