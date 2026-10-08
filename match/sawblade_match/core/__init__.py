"""Loader for the ``sawblade_core`` extension module (C++ renderer, built with pybind11).

The extension is built by CMake (``-DSAWBLADE_BUILD_PYTHON=ON``, see match/README.md) into
``<build dir>/python``. It is found, in order:

1. already importable (``PYTHONPATH`` or installed);
2. ``$SAWBLADE_CORE_DIR`` (a directory holding the built module);
3. ``<repo>/build-py/python``, ``<repo>/build/python``, ``<repo>/build-lead/python``.

Usage::

    from sawblade_match.core import render, CaptureCache
"""
from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[3]
_SEARCH = ("build-py", "build", "build-lead")

_BUILD_HELP = (
    "sawblade_core is not built. From the repo root:\n"
    "  cmake -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PYTHON=ON \\\n"
    "        -DPython_EXECUTABLE=$PWD/match/.venv/bin/python\n"
    "  cmake --build build-py --target sawblade_py\n"
    "or point SAWBLADE_CORE_DIR (or PYTHONPATH) at the directory containing sawblade_core*.so."
)


def _candidate_dirs() -> list[Path]:
    dirs: list[Path] = []
    env = os.environ.get("SAWBLADE_CORE_DIR")
    if env:
        dirs.append(Path(env))
    dirs.extend(_REPO / d / "python" for d in _SEARCH)
    return dirs


def _import():
    try:
        return importlib.import_module("sawblade_core")
    except ImportError as first:
        for d in _candidate_dirs():
            if d.is_dir() and any(d.glob("sawblade_core*")):
                sys.path.insert(0, str(d))
                try:
                    return importlib.import_module("sawblade_core")
                except ImportError:
                    sys.path.remove(str(d))
        raise ImportError(f"{first}\n{_BUILD_HELP}") from first


_core = _import()

render = _core.render
CaptureCache = _core.CaptureCache
PresetError = _core.PresetError
RenderIOError = _core.RenderIOError
# Play-along backing (spec 5.1). getattr: tolerate a module built before these existed.
StemSet = getattr(_core, "StemSet", None)
StemPlayer = getattr(_core, "StemPlayer", None)
load_stems = getattr(_core, "load_stems", None)
stem_set_from_arrays = getattr(_core, "stem_set_from_arrays", None)
integrated_loudness_lufs = getattr(_core, "integrated_loudness_lufs", None)

__all__ = ["render", "CaptureCache", "PresetError", "RenderIOError", "StemSet", "StemPlayer", "load_stems", "stem_set_from_arrays", "integrated_loudness_lufs", "level_match", "resolve_dynamics", "peak_floor_db"]


def level_match(preset, sample_rate, base_dir=None, cache=None, device_dbu=None, calibration="preset") -> dict:
    """Phase 10.1 level-match probe (``trimADb, trimBDb, lufsA, lufsB, sumLufs, makeupDb[5], delaySamplesB,
    invertB``, ``calibrated``); no audio is rendered. Same argument conventions as ``render`` (``device_dbu``, ``calibration``)."""
    fn = getattr(_core, "level_match", None)
    if fn is None:
        raise ImportError("sawblade_core has no level_match (built before phase 10.1); rebuild the extension")
    return fn(preset, sample_rate, base_dir=base_dir, cache=cache, device_dbu=device_dbu, calibration=calibration)


def peak_floor_db(x, fs, key_hpf_hz: float = 0.0, mask=None):
    """The DI floor on the gate's own detector (Task H.1; core ``peakFloorDb``): the 92.5th percentile of the peak envelope (0.1 ms
    attack / 10 ms release, after a key high-pass when ``key_hpf_hz`` > 0) over the samples where ``mask`` is non-zero (all when
    None), in dBFS; None when the mask selects nothing."""
    fn = getattr(_core, "peak_floor_db", None)
    if fn is None:
        raise ImportError("sawblade_core has no peak_floor_db (built before Task H.1); rebuild the extension")
    return fn(x, fs, key_hpf_hz=key_hpf_hz, mask=mask)


def resolve_dynamics(preset, base_dir=None) -> dict:
    """The active dynamics set of a preset (Task G; core ``activeDynamics``): ``{"mode": "live" | "record", "gate": {...},
    "busComp": {...}}``. The one resolver the exporter and the matcher use."""
    fn = getattr(_core, "resolve_dynamics", None)
    if fn is None:
        raise ImportError("sawblade_core has no resolve_dynamics (built before Task G); rebuild the extension")
    return fn(preset, base_dir=base_dir)
