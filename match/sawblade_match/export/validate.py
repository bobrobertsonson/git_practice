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
                    renders_dir: Path | None = None, ref_cache: dict | None = None
                    ) -> tuple[dict, np.ndarray, np.ndarray]:
    """Render ``x`` through the reference chain and the exported-model chain; metrics skip the first ``drop`` samples.
    ``ref_cache`` (a dict the caller keeps across calls) reuses the reference renders when several exported models
    (A2 Full and Lite) are compared on the same signal."""
    if ref_cache is not None and name in ref_cache:
        ref, extra = ref_cache[name]
    else:
        ref, _ = render48(ref_preset, x, ref_base, cache, in_rate)
        extra = {label: render48(preset, x, ref_base, cache, in_rate)[0] for label, preset in (extra_refs or {}).items()}
        if ref_cache is not None:
            ref_cache[name] = (ref, extra)
    out, rep = render48(check_preset, x, ".", cache, in_rate)
    res = {"seconds": (len(ref) - drop) / RATE,
           "esr": esr(out[drop:], ref[drop:]), "ltas": ltas_error(out[drop:], ref[drop:], targets),
           "refRmsDbfs": float(10 * np.log10(np.mean(ref[drop:].astype(np.float64) ** 2) + 1e-30)),
           "outRmsDbfs": float(10 * np.log10(np.mean(out[drop:].astype(np.float64) ** 2) + 1e-30)),
           "exportRealTimeFactor": rep.get("realTimeFactor")}
    for label, r2 in extra.items():
        res[label] = {"esr": esr(out[drop:], r2[drop:]), "ltas": ltas_error(out[drop:], r2[drop:], targets)}
    if renders_dir is not None:
        renders_dir.mkdir(parents=True, exist_ok=True)
        sf.write(str(renders_dir / f"{name}_original.wav"), ref[drop:], RATE, subtype="PCM_24")
        sf.write(str(renders_dir / f"{name}_export.wav"), out[drop:], RATE, subtype="PCM_24")
    return res, ref[drop:], out[drop:]


# A2 (v0.6, proposed thresholds): Full is judged like A1 standard (8 channels, 12 145 parameters: the same capacity class);
# Lite (3 channels, 1 870 parameters) gets a looser rule so that it reports "met" when it is a usable small model, not
# only when it is as exact as Full.  Both are reported with their numbers whatever the status.
A2_ACCEPT = {"full": (ACCEPT_ESR, ACCEPT_LTAS_DB), "lite": (0.05, 1.0)}


def acceptance(size: str, held_out_esr: float, di_ltas_db: float, arch: str = "a1") -> dict:
    """The acceptance rule.  A1: ESR <= 0.02 and LTAS <= 0.5 dB, judged for ``standard`` only (lite/feather are reported,
    not judged).  A2: Full and Lite are both judged, with their own limits (``A2_ACCEPT``)."""
    if arch == "a2":
        esr_lim, ltas_lim = A2_ACCEPT[size]
        judged, applies = True, f"a2 {size}"
    else:
        esr_lim, ltas_lim = ACCEPT_ESR, ACCEPT_LTAS_DB
        judged, applies = size == "standard", "standard"
    esr_ok, ltas_ok = held_out_esr <= esr_lim, di_ltas_db <= ltas_lim
    accepted = bool(judged and esr_ok and ltas_ok)
    return {"appliesTo": applies, "evaluated": judged, "esrLimit": esr_lim, "ltasLimitDb": ltas_lim,
            "heldOutEsr": held_out_esr, "diLtasDb": di_ltas_db, "esrOk": esr_ok, "ltasOk": ltas_ok,
            "accepted": accepted, **status_fields(size, held_out_esr, di_ltas_db, accepted, arch)}


def status_fields(size: str, held_out_esr: float, di_ltas_db: float, accepted: bool, arch: str = "a1") -> dict:
    esr_lim, ltas_lim = A2_ACCEPT[size] if arch == "a2" else (ACCEPT_ESR, ACCEPT_LTAS_DB)
    if arch == "a2" or size == "standard":
        status = "met" if accepted else "NOT MET"
    else:
        status = "not judged (non-standard size)"
    label = f"a2 {size}: " if arch == "a2" else ""
    summary = (f"{label}acceptance {status}: held-out ESR {held_out_esr:.4f} (limit {esr_lim}), "
               f"DI-excerpt LTAS error {di_ltas_db:.2f} dB (limit {ltas_lim})")
    return {"status": status, "summary": summary}
