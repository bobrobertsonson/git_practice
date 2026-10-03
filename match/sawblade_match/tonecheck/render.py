"""Run the tonerender CLI."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

DEFAULT_TONERENDER = ("build/cli/tonerender", "build-lead/cli/tonerender")
DEFAULT_TARGETS = "docs/tone_targets.json"


class RenderError(RuntimeError):
    pass


def find_tonerender(explicit: str | None = None) -> Path:
    """--tonerender, else $SAWBLADE_TONERENDER, else build/cli/tonerender or build-lead/cli/tonerender under
    the current directory (run from the repo root). There is no package-relative fallback."""
    import os
    path = explicit or os.environ.get("SAWBLADE_TONERENDER")
    if path:
        p = Path(path)
        if not p.exists():
            raise RenderError(f"tonerender not found at {path}")
        return p
    for rel in DEFAULT_TONERENDER:
        p = Path.cwd() / rel
        if p.exists():
            return p
    raise RenderError("tonerender not found (looked for " + ", ".join(DEFAULT_TONERENDER) +
                      f" under {Path.cwd()}); run from the repo root or pass --tonerender / set SAWBLADE_TONERENDER")


def find_targets(explicit: str | None = None) -> Path:
    """--targets, else $SAWBLADE_TARGETS, else docs/tone_targets.json under the current directory."""
    import os
    path = explicit or os.environ.get("SAWBLADE_TARGETS")
    if path:
        return Path(path)
    p = Path.cwd() / DEFAULT_TARGETS
    if p.exists():
        return p
    raise RenderError(f"{DEFAULT_TARGETS} not found under {Path.cwd()}; run from the repo root or pass "
                      "--targets / set SAWBLADE_TARGETS")


def render(tonerender: Path, preset: Path, di: Path, out_wav: Path, report_json: Path) -> dict:
    """Render with default settings (--render-rate auto). Returns the parsed tonerender report."""
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(tonerender), "--preset", str(preset), "--in", str(di), "--out", str(out_wav),
           "--report", str(report_json)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RenderError(f"tonerender exited {r.returncode}: {r.stderr.strip() or r.stdout.strip()}")
    return json.loads(report_json.read_text())
