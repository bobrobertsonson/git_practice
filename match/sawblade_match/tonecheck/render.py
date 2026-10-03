"""Run the tonerender CLI."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_TONERENDER = ("build/cli/tonerender", "build-lead/cli/tonerender")
DEFAULT_TARGETS = "docs/tone_targets.json"


class RenderError(RuntimeError):
    pass


def _find(rel: str) -> Path | None:
    for base in (Path.cwd(), REPO_ROOT):
        p = base / rel
        if p.exists():
            return p
    return None


def find_tonerender(explicit: str | None = None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise RenderError(f"tonerender not found at {explicit}")
        return p
    for rel in DEFAULT_TONERENDER:
        p = _find(rel)
        if p:
            return p
    raise RenderError("tonerender not found (tried " + ", ".join(DEFAULT_TONERENDER) + "); use --tonerender")


def find_targets(explicit: str | None = None) -> Path:
    if explicit:
        return Path(explicit)
    p = _find(DEFAULT_TARGETS)
    if not p:
        raise RenderError(f"{DEFAULT_TARGETS} not found; use --targets")
    return p


def render(tonerender: Path, preset: Path, di: Path, out_wav: Path, report_json: Path) -> dict:
    """Render with default settings (--render-rate auto). Returns the parsed tonerender report."""
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(tonerender), "--preset", str(preset), "--in", str(di), "--out", str(out_wav),
           "--report", str(report_json)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RenderError(f"tonerender exited {r.returncode}: {r.stderr.strip() or r.stdout.strip()}")
    return json.loads(report_json.read_text())
