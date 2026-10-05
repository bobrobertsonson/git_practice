"""``--progress-json``: atomic, throttled progress file for GUIs (the plugin's export panel)."""
from __future__ import annotations

import json
import time
from pathlib import Path

from .resume import atomic_write_text

# Absolute fraction range of each stage; the fraction is clamped into it and never decreases within a run.
STAGE_RANGE = {"plan": (0.0, 0.02), "signal": (0.02, 0.05), "render": (0.05, 0.10), "train": (0.10, 0.90),
               "validate": (0.90, 0.99), "done": (1.0, 1.0)}
STAGES = tuple(STAGE_RANGE) + ("cancelled", "error")
TERMINAL = ("done", "cancelled", "error")
MIN_INTERVAL_S = 1.0


def stage_fraction(stage: str, t: float) -> float:
    """Map ``t`` in [0, 1] (progress inside the stage) to the absolute fraction."""
    lo, hi = STAGE_RANGE[stage]
    return lo + (hi - lo) * min(1.0, max(0.0, t))


def train_progress(epochs_done: int, batch_frac: float, epochs: int, elapsed_s: float, cap_s: float) -> float:
    """Training progress in [0, 1]: epochs done + batch fraction, or elapsed / cap, whichever is larger."""
    by_epochs = (epochs_done + min(1.0, max(0.0, batch_frac))) / max(epochs, 1)
    by_time = elapsed_s / cap_s if cap_s > 0 else 0.0
    return min(1.0, max(by_epochs, by_time))


def eta_seconds(session_s: float, session_epochs: int, epochs_done: int, epochs: int, elapsed_s: float,
                cap_s: float) -> int:
    """Mean epoch time of this session x remaining epochs, capped by the time left; -1 until one epoch is done."""
    if session_epochs < 1:
        return -1
    eta = session_s / session_epochs * max(epochs - epochs_done, 0)
    return int(round(max(0.0, min(eta, cap_s - elapsed_s))))


class Progress:
    """Writes the progress JSON. ``path=None`` makes every call a no-op. Writes are immediate on stage changes,
    ``force=True`` and the terminal stages (done / cancelled / error); otherwise at most once per ``min_interval``."""

    def __init__(self, path=None, out_dir=None, min_interval: float = MIN_INTERVAL_S, clock=time.monotonic):
        self.path = Path(path).expanduser() if path else None
        self.min_interval = min_interval
        self.clock = clock
        self.t0 = clock()
        self.last_write: float | None = None
        self.last_stage: str | None = None
        self.state: dict = {"stage": "plan", "fraction": 0.0, "etaSeconds": -1, "epoch": 0, "epochs": 0,
                            "bestEsr": None, "message": "", "outDir": str(out_dir or ""), "resumable": False,
                            "elapsedSeconds": 0.0}

    def update(self, stage: str | None = None, fraction: float | None = None, *, eta: int | None = None,
               epoch: int | None = None, epochs: int | None = None, best_esr: float | None = None,
               message: str | None = None, out_dir=None, resumable: bool | None = None, force: bool = False) -> None:
        s = self.state
        if stage is not None:
            if stage not in STAGES:
                raise ValueError(f"unknown stage {stage!r}")
            s["stage"] = stage
        st = s["stage"]
        if st == "validate" and self.last_stage != "validate":
            s["etaSeconds"] = -1                         # training ETA no longer applies
        if st in STAGE_RANGE:
            lo, hi = STAGE_RANGE[st]
            f = lo if fraction is None else fraction
            s["fraction"] = max(s["fraction"], min(hi, max(lo, f)))    # monotonic, clamped to the stage
        for key, val in (("etaSeconds", eta), ("epoch", epoch), ("epochs", epochs), ("bestEsr", best_esr),
                         ("message", message), ("resumable", resumable)):
            if val is not None:
                s[key] = val
        if out_dir is not None:
            s["outDir"] = str(out_dir)
        if st == "done":
            s["etaSeconds"] = 0
        elif st in TERMINAL:
            s["etaSeconds"] = -1
        immediate = force or st != self.last_stage or st in TERMINAL
        self.last_stage = st
        if self.path is None:
            return
        now = self.clock()
        if immediate or self.last_write is None or now - self.last_write >= self.min_interval:
            s["elapsedSeconds"] = round(now - self.t0, 1)
            try:
                atomic_write_text(self.path, json.dumps(s, indent=2, default=float))
            except OSError:
                return                                  # a progress file must never kill the export
            self.last_write = now
