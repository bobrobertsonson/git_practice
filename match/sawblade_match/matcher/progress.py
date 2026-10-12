"""Progress reporting for the plugin's progress bar (spec 6b): ``--progress-json PATH``.

The file holds ``{"stage", "fraction", "etaSeconds", "bestErrorDb", "message"}`` (plus ``elapsedSeconds`` and ``done``)
and is rewritten atomically (temp file in the same directory + ``os.replace``) on every state change and at least every
``interval`` seconds by a heartbeat thread, so a reader never sees a torn file and never sees a stale one for long.

``fraction`` is the weighted completion (monotonic, 0..1); the stage weights come from the measured time profile of the
mode (``WEIGHTS``). ``etaSeconds`` is extrapolated from the elapsed time and the fraction (null before 3 %).
``bestErrorDb`` is the best LTAS error (A-weighted band error, dB) found so far, then the final A-weighted error vs the
reference once the verification render is measured; null before the first score.
"""
from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path

STAGES = ("prepare", "prescreen", "screen", "refine", "finalize")
WEIGHTS = {
    "quick": {"prepare": 0.04, "prescreen": 0.10, "screen": 0.30, "refine": 0.40, "finalize": 0.16},
    "thorough": {"prepare": 0.01, "prescreen": 0.04, "screen": 0.62, "refine": 0.30, "finalize": 0.03},
}


class NullProgress:
    """No-op sink with the same interface."""
    path = None

    def start(self): return self
    def stage(self, name, message=""): pass
    def update(self, frac, message=None): pass
    def best(self, db): pass
    def callback(self, lo=0.0, hi=1.0, message=None): return lambda d, t: None
    def close(self, message="done", error_db=None): pass


class Progress(NullProgress):
    def __init__(self, path, mode: str = "thorough", interval: float = 0.5, clock=time.time):
        self.path = Path(path)
        self.w = WEIGHTS.get(mode, WEIGHTS["thorough"])
        tot = sum(self.w.values())
        self.w = {k: v / tot for k, v in self.w.items()}
        self.interval, self.clock = interval, clock
        self.t0 = clock()
        self._lock = threading.RLock()
        self._stage, self._within, self._msg, self._best = "prepare", 0.0, "", None
        self._frac, self._done = 0.0, False
        self._stop = threading.Event()
        self._thread = None
        self._seq = 0

    # ---- state -----------------------------------------------------------------------------------------------------
    def stage(self, name, message=""):
        with self._lock:
            self._stage, self._within, self._msg = name, 0.0, message or name
        self._write()

    def update(self, frac, message=None):
        with self._lock:
            self._within = min(max(float(frac), 0.0), 1.0)
            if message is not None:
                self._msg = message
        self._write()

    def best(self, db):
        with self._lock:
            if db is not None and (self._best is None or db < self._best):
                self._best = float(db)

    def callback(self, lo=0.0, hi=1.0, message=None):
        """``(done, total)`` callback mapping onto the [lo, hi] part of the current stage (for Engine.map)."""
        def cb(d, t):
            self.update(lo + (hi - lo) * d / max(t, 1), message)
        return cb

    # ---- output ----------------------------------------------------------------------------------------------------
    def snapshot(self) -> dict:
        with self._lock:
            before = 0.0
            for s in STAGES:
                if s == self._stage:
                    break
                before += self.w.get(s, 0.0)
            f = before + self.w.get(self._stage, 0.0) * self._within
            self._frac = 1.0 if self._done else max(self._frac, min(f, 0.999))
            el = self.clock() - self.t0
            eta = 0.0 if self._done else (el * (1 - self._frac) / self._frac if self._frac >= 0.03 else None)
            return {"stage": self._stage, "fraction": round(self._frac, 4),
                    "etaSeconds": None if eta is None else round(eta, 1),
                    "bestErrorDb": None if self._best is None else round(self._best, 3),
                    "message": self._msg, "elapsedSeconds": round(el, 1), "done": self._done}

    def _write(self):
        snap = self.snapshot()
        with self._lock:
            self._seq += 1
            tmp = self.path.with_name(f".{self.path.name}.{os.getpid()}.{self._seq}.tmp")
            try:
                tmp.write_text(json.dumps(snap))
                os.replace(tmp, self.path)
            except OSError:
                try:
                    tmp.unlink()
                except OSError:
                    pass

    def _beat(self):
        while not self._stop.wait(self.interval):
            self._write()

    def start(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._write()
        self._thread = threading.Thread(target=self._beat, daemon=True)
        self._thread.start()
        return self

    def close(self, message="done", error_db=None):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
        with self._lock:
            self._done, self._msg, self._stage = True, message, "finalize"
            if error_db is not None:
                self._best = float(error_db)
        self._write()
