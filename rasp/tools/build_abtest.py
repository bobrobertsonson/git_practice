#!/usr/bin/env python3
"""Build abtest/index.html (the blind A/B listening page) from abtest/template.html
plus the playground's engine (class RaspEngine, DEFAULTS, PRESETS), so the
listening test always runs the same DSP as the playground and `make check`.

  python3 tools/build_abtest.py
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
html = (ROOT / "playground" / "index.html").read_text()
engine = html[html.index("class RaspEngine"):html.index("// Shared block processor")].rstrip() + "\n"
for name in ("DEFAULTS", "PRESETS"):
    engine += re.search(r"const " + name + r" = \{.*?\n\};", html, re.S).group(0) + "\n"
assert "</script" not in engine
tpl = (ROOT / "abtest" / "template.html").read_text()
out = tpl.replace("/*ENGINE*/", engine)
(ROOT / "abtest" / "index.html").write_text(out)
print(f"wrote abtest/index.html ({len(out) // 1024} KB)")
