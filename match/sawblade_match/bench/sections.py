"""Fit / held-out sections (spec A.2). All times in seconds of the signal the section is defined on (the DI for tier 1, the
reference for tier 2). Deterministic.

Automatic rule: on the usable region (DI-reference overlap) find the active region, the first to the last 100 ms frame within
40 dB of the loudest frame; ``fit`` = its first half, ``heldOut`` = the densest ``heldOutMaxS`` seconds of its second half
(``excerpt.select_excerpt``, the matcher's own frame-energy rule). A pinned section is kept; the missing one is chosen outside it.
"""
from __future__ import annotations

import numpy as np

from ..matcher.excerpt import select_excerpt

FRAME_S = 0.100
WITHIN_DB = 40.0
MIN_SECTION_S = 2.0


def active_region(x: np.ndarray, fs: int, lo: int = 0, hi: int | None = None) -> tuple[int, int]:
    """Sample range [a, b) from the first to the last 100 ms frame within 40 dB of the loudest frame, inside [lo, hi)."""
    hi = len(x) if hi is None else min(hi, len(x))
    n = int(round(FRAME_S * fs))
    nf = (hi - lo) // n
    if nf < 1:
        raise ValueError("signal too short for sections")
    fr = np.asarray(x[lo:lo + nf * n], dtype=np.float64).reshape(nf, n)
    db = 10 * np.log10(np.maximum(np.mean(fr * fr, axis=1), 1e-20))
    act = np.nonzero(db >= db.max() - WITHIN_DB)[0]
    return lo + int(act[0]) * n, lo + (int(act[-1]) + 1) * n


def _densest(x: np.ndarray, fs: int, a: int, b: int, length_s: float) -> tuple[int, int]:
    if b - a <= int(round(length_s * fs)):
        return a, b
    s0, s1, _ = select_excerpt(x[a:b], fs, length_s)
    return a + s0, a + s1


def _sec(a: int, b: int, fs: int) -> list[float]:
    return [round(a / fs, 6), round(b / fs, 6)]


def resolve(x: np.ndarray, fs: int, fit: list | None, held: list | None, held_max_s: float,
            lo: int = 0, hi: int | None = None) -> dict:
    """``{"fit": [s, e], "heldOut": [s, e], "source": {"fit": "manifest"|"auto", "heldOut": ...}}`` (seconds). ``x`` is the signal
    the sections are defined on, [lo, hi) the usable region. Raises ValueError when a section would be shorter than 2 s or the two
    would overlap."""
    hi = len(x) if hi is None else min(hi, len(x))
    src = {"fit": "manifest" if fit else "auto", "heldOut": "manifest" if held else "auto"}
    if fit and held:
        f, h = list(fit), list(held)
    else:
        a, b = active_region(x, fs, lo, hi)
        if fit:                                   # held-out: the densest stretch after the pinned fit section
            f = list(fit)
            ha, hb = _densest(x, fs, max(a, int(round(f[1] * fs))), b, held_max_s)
            h = _sec(ha, hb, fs)
        elif held:                                # fit: the part of the active region before it (else after it)
            h = list(held)
            hs, he = int(round(h[0] * fs)), int(round(h[1] * fs))
            f = _sec(a, hs, fs) if hs - a >= MIN_SECTION_S * fs else _sec(he, b, fs)
        else:
            mid = a + (b - a) // 2
            f = _sec(a, mid, fs)
            ha, hb = _densest(x, fs, mid, b, held_max_s)
            h = _sec(ha, hb, fs)
    for name, s in (("fit", f), ("heldOut", h)):
        if s[1] - s[0] < MIN_SECTION_S:
            raise ValueError(f"{name} section {s} is shorter than {MIN_SECTION_S:g} s (the usable material is too short)")
    if not (f[1] <= h[0] or h[1] <= f[0]):
        raise ValueError(f"fit {f} and heldOut {h} overlap")
    return {"fit": f, "heldOut": h, "source": src}
