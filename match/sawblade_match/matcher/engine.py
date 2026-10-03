"""Rendering engine: thin layer over ``sawblade_match.core.render`` with a shared CaptureCache.

Why path-wise rendering. After the NAM blocks everything in the graph is linear and time-invariant (path EQ, level,
alignment delay, blend, cab IR, post EQ; bus comp is off), so for a given combo and NAM input gains

    out = postEq(cab( (1-blend) * pathA + blend * delay(pathB) ))  ==  sum of the same linear chain applied per path.

The expensive part (NAMs) is rendered once per path ("core": gate -> [pre-EQ] -> blocks), and every continuous
parameter that is not a NAM input gain is evaluated with two cheap renders of the *same C++ renderer* (empty block
list: path EQ + level + cab + post EQ). Only the scalar blend and the integer alignment shift are done in Python.
No filtering/convolution is implemented in Python. ``tests/test_matcher.py`` checks emulation == full render.

All pipeline signals are at 48 kHz (the NAM rate) so the core never resamples between stages.
"""
from __future__ import annotations

import copy
from concurrent.futures import ThreadPoolExecutor
from fractions import Fraction

import numpy as np
from scipy import signal

from .. import core as _core
from .space import A_PREEQ, Combo, build_preset, manual_align, path_blocks, path_eq, post_eq

RATE = 48000


def to48(x: np.ndarray, fs: int) -> np.ndarray:
    x = np.asarray(x, dtype=np.float64)
    if fs == RATE:
        return x.astype(np.float32)
    f = Fraction(RATE, fs)
    return signal.resample_poly(x, f.numerator, f.denominator).astype(np.float32)


class Engine:
    def __init__(self, gate: dict | None, workers: int = 4, cache=None):
        self.cache = cache or _core.CaptureCache()
        self.gate = gate
        self.workers = workers
        self.pool = ThreadPoolExecutor(workers)
        self.n_renders = 0
        self.render_audio_s = 0.0

    def close(self):
        self.pool.shutdown(wait=True)

    def render(self, preset: dict, x: np.ndarray, fs: float = RATE) -> tuple[np.ndarray, dict]:
        x = np.ascontiguousarray(x, dtype=np.float32)
        y, rep = _core.render(preset, x, fs, cache=self.cache)
        self.n_renders += 1
        self.render_audio_s += len(x) / fs
        return y, rep

    def map(self, fn, items):
        return list(self.pool.map(fn, items))

    # ---- preset skeletons -------------------------------------------------------------------------------------
    @staticmethod
    def _base(cab, *, blend: float, cab_enabled: bool, gate, post=None, output_db=0.0) -> dict:
        return {"schema": "sawblade.preset", "version": 1, "name": "matcher-internal",
                "gate": gate if gate else {"enabled": False},
                "paths": {}, "align": {"mode": "off"}, "blend": blend,
                "cab": {"mode": "shared", "ir": cab.block_model(), "enabled": cab_enabled},
                "postEq": post or [], "output": {"gainDb": output_db}}

    @staticmethod
    def _disabled(role: str) -> dict:
        return {"role": role, "enabled": False, "blocks": []}

    def core_preset(self, combo: Combo, v: dict, path: str) -> dict:
        p = self._base(combo.cab, blend=0.0 if path == "a" else 1.0, cab_enabled=False, gate=self.gate)
        if path == "a":
            p["paths"] = {"a": {"role": "saw", "preEq": copy.deepcopy(A_PREEQ), "blocks": path_blocks(combo, v, "a")},
                          "b": self._disabled("body")}
        else:
            p["paths"] = {"a": self._disabled("saw"), "b": {"role": "body", "blocks": path_blocks(combo, v, "b")}}
        return p

    def linear_preset(self, cab, v: dict, path: str) -> dict:
        p = self._base(cab, blend=0.0 if path == "a" else 1.0, cab_enabled=True, gate=None, post=post_eq(v))
        live = {"role": "saw" if path == "a" else "body", "blocks": [], "eq": path_eq(v, path),
                "levelDb": float(v["levelA" if path == "a" else "levelB"])}
        p["paths"] = {"a": live, "b": self._disabled("body")} if path == "a" else \
            {"a": self._disabled("saw"), "b": live}
        return p

    # ---- stages -------------------------------------------------------------------------------------------------
    def core(self, combo: Combo, v: dict, path: str, x: np.ndarray) -> np.ndarray:
        """NAM core of one path: gate -> blocks (A also pre-EQ) at 48 kHz. Same length as ``x``."""
        y, rep = self.render(self.core_preset(combo, v, path), x)
        if rep.get("latencySamples", 0):
            raise RuntimeError("path latency != 0 is not supported by the matcher emulation yet")
        return y

    def linear(self, cab, v: dict, path: str, sig: np.ndarray) -> np.ndarray:
        y, _ = self.render(self.linear_preset(cab, v, path), sig)
        return y

    def probe_align(self, combo: Combo, v: dict) -> dict:
        """One-time auto-align probe for a discrete combo (tiny render; the probe signal is internal to the renderer).
        Returns the manual-align dict to use from then on (``align.resolved`` of the report)."""
        p = build_preset(combo, v, gate=None, align={"mode": "auto", "maxLagMs": 5.0})
        _, rep = self.render(p, np.zeros(2048, np.float32))
        r = rep["align"]["resolved"]
        return manual_align(r["delaySamplesB"], r["invertB"])

    @staticmethod
    def mix(a: np.ndarray, b: np.ndarray, blend: float, align: dict) -> np.ndarray:
        """(1-blend)*A + blend*B with the manual alignment (+n delays B, -n delays A; invertB flips B)."""
        n = int(align.get("delaySamplesB", 0))
        b = -b if align.get("invertB") else b
        if n > 0:
            b = np.concatenate([np.zeros(n, b.dtype), b[:-n]])
        elif n < 0:
            a = np.concatenate([np.zeros(-n, a.dtype), a[:n]])
        return ((1.0 - blend) * a + blend * b).astype(np.float32)

    def emulate(self, combo: Combo, v: dict, core_a: np.ndarray, core_b: np.ndarray, align: dict) -> np.ndarray:
        """Output of the full chain from the two cores (output gain not applied)."""
        a = self.linear(combo.cab, v, "a", core_a)
        b = self.linear(combo.cab, v, "b", core_b)
        return self.mix(a, b, v["blend"], align)
