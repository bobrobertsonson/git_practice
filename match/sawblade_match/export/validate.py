"""Validation of an export: ESR + A-weighted 1/3-octave LTAS error, original chain vs exported model (+ IR)."""
from __future__ import annotations

import copy
import json
from pathlib import Path

import numpy as np
import soundfile as sf

from ..core import CaptureCache
from ..tonecheck.analysis import analyze
from ..tonecheck.cli import compare_to_reference
from ..tonecheck.rules import load_targets
from .chain import RATE, render48

ACCEPT_ESR = 0.02          # standard size, held-out validation segment
ACCEPT_LTAS_DB = 0.5       # standard size, DI excerpt
PREROLL_S = 0.5            # rendered before the excerpt and dropped from the metrics (NAM / filter warm-up)


def esr(y: np.ndarray, t: np.ndarray) -> float:
    n = min(len(y), len(t))
    y = np.asarray(y[:n], np.float64)
    t = np.asarray(t[:n], np.float64)
    return float(np.sum((y - t) ** 2) / max(np.sum(t * t), 1e-30))


def targets_path() -> Path:
    return Path(__file__).resolve().parents[3] / "docs" / "tone_targets.json"


def ltas_error(y: np.ndarray, ref: np.ndarray, targets: dict | None = None) -> dict:
    """A-weighted RMS error (dB) of the 1/3-octave LTAS, bands 80 Hz-8 kHz, both spectra normalised to their 1 kHz
    band (tonecheck's ``--ref`` definition, so a pure level difference does not count)."""
    targets = targets or load_targets(targets_path())
    n = min(len(y), len(ref))
    a, b = analyze(y[:n], RATE, targets), analyze(ref[:n], RATE, targets)
    c = compare_to_reference(a, b)
    return {"aWeightedErrorDb": c["aWeightedErrorDb"], "unweightedRmsErrorDb": c["unweightedRmsErrorDb"],
            "meanDiffDb": c["meanDiffDb"]}


def impulse_ir(path: Path) -> Path:
    sf.write(str(path), np.array([1.0], np.float32), RATE, subtype="FLOAT")
    return path


def export_check_preset(nam_path, ir_path, scratch: Path, name: str = "export check") -> dict:
    """Preset whose single nam block is the exported model; ``ir_path`` (not normalised by the core) is the cab for
    ``nocab`` exports, ``None`` (cab disabled) for ``withcab``."""
    blk = {"id": "m1", "type": "nam", "slot": "amp", "model": {"file": str(Path(nam_path).resolve())}}
    if ir_path is None:
        cab = {"mode": "shared", "enabled": False, "ir": {"file": str(impulse_ir(Path(scratch) / "unit_impulse.wav"))}}
    else:
        cab = {"mode": "shared", "enabled": True, "normalize": False, "ir": {"file": str(Path(ir_path).resolve())}}
    return {"schema": "sawblade.preset", "version": 1, "name": name,
            "paths": {"a": {"role": "saw", "blocks": [blk]}, "b": {"role": "body", "enabled": False, "blocks": []}},
            "align": {"mode": "off"}, "blend": 0.0, "cab": cab, "postEq": [], "output": {"gainDb": 0.0}}


def compare_signals(name: str, x: np.ndarray, in_rate: int, ref_preset: dict, ref_base, check_preset: dict,
                    cache: CaptureCache, targets: dict, extra_refs: dict | None = None, drop: int = 0,
                    renders_dir: Path | None = None) -> tuple[dict, np.ndarray, np.ndarray]:
    """Render ``x`` through the reference chain and the exported-model chain; metrics skip the first ``drop`` samples."""
    ref, _ = render48(ref_preset, x, ref_base, cache, in_rate)
    out, rep = render48(check_preset, x, ".", cache, in_rate)
    res = {"seconds": (len(ref) - drop) / RATE,
           "esr": esr(out[drop:], ref[drop:]), "ltas": ltas_error(out[drop:], ref[drop:], targets),
           "refRmsDbfs": float(10 * np.log10(np.mean(ref[drop:].astype(np.float64) ** 2) + 1e-30)),
           "outRmsDbfs": float(10 * np.log10(np.mean(out[drop:].astype(np.float64) ** 2) + 1e-30)),
           "exportRealTimeFactor": rep.get("realTimeFactor")}
    for label, preset in (extra_refs or {}).items():
        r2, _ = render48(preset, x, ref_base, cache, in_rate)
        res[label] = {"esr": esr(out[drop:], r2[drop:]), "ltas": ltas_error(out[drop:], r2[drop:], targets)}
    if renders_dir is not None:
        renders_dir.mkdir(parents=True, exist_ok=True)
        sf.write(str(renders_dir / f"{name}_original.wav"), ref[drop:], RATE, subtype="PCM_24")
        sf.write(str(renders_dir / f"{name}_export.wav"), out[drop:], RATE, subtype="PCM_24")
    return res, ref[drop:], out[drop:]


def acceptance(size: str, held_out_esr: float, di_ltas_db: float) -> dict:
    """The spec's acceptance numbers; they apply to ``standard`` only (lite/feather are reported, not judged)."""
    return {"appliesTo": "standard", "evaluated": size == "standard", "esrLimit": ACCEPT_ESR, "ltasLimitDb": ACCEPT_LTAS_DB,
            "heldOutEsr": held_out_esr, "diLtasDb": di_ltas_db,
            "esrOk": held_out_esr <= ACCEPT_ESR, "ltasOk": di_ltas_db <= ACCEPT_LTAS_DB,
            "accepted": bool(size == "standard" and held_out_esr <= ACCEPT_ESR and di_ltas_db <= ACCEPT_LTAS_DB)}
