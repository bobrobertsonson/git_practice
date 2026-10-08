"""Reamp-pair export (v0.6 decision 20): render the NAM project's standard input file through the exportable chain.

The input is recognised by ``standard_input.recognise``.  The rendered output is written as 24-bit PCM mono at 48 kHz, the
same length, latency-compensated, because the trainer reads WAV files through ``wavio`` (integer PCM only,
``nam/data.py:118``) and the standard workflow records 24-bit.
"""
from __future__ import annotations

import shutil
from pathlib import Path

import numpy as np
import soundfile as sf

from .standard_input import CLIP_CEILING, RATE, read_standard_samples, recognise as validate_input  # noqa: F401


def render_output(preset: dict, base_dir, cache, in_path, render) -> tuple[np.ndarray, dict]:
    """The standard input rendered through ``preset`` (the training chain): float64, exactly the input's length,
    latency-compensated by the core.  ``render(preset, x32, base_dir, cache)`` is ``chain.render48``."""
    x, _ = read_standard_samples(Path(in_path).expanduser())
    y, rep = render(preset, x.astype(np.float32), base_dir, cache)
    y = np.asarray(y, np.float64)
    if len(y) < len(x):
        y = np.concatenate([y, np.zeros(len(x) - len(y))])
    return y[: len(x)], rep


def write_output(y: np.ndarray, path: Path) -> dict:
    """Write ``y`` as 24-bit PCM mono 48 kHz (the trainer reads integer PCM only); a render that would clip is scaled down
    to ``CLIP_CEILING``.  Returns ``{"peakDbfs", "levelReducedDb"}``."""
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    gain = min(1.0, CLIP_CEILING / peak) if peak > 0 else 1.0
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), y * gain, RATE, subtype="PCM_24")
    return {"outputPeakDbfs": float(20 * np.log10(max(peak * gain, 1e-12))),
            "levelReducedDb": float(-20 * np.log10(gain)) if gain < 1.0 else 0.0}


def render_pair(preset: dict, base_dir, cache, in_path, info: dict, out_dir: Path, stem: str, render, y=None,
                rep: dict | None = None) -> dict:
    """Write ``<stem>.reamp_input.wav`` (the input, copied unchanged) and ``<stem>.reamp_output.wav`` (``y``, or the input
    rendered through ``preset``).  Returns the report block."""
    if y is None:
        y, rep = render_output(preset, base_dir, cache, in_path, render)
    rep = rep or {}
    out_dir.mkdir(parents=True, exist_ok=True)
    fin = out_dir / f"{stem}.reamp_input.wav"
    fout = out_dir / f"{stem}.reamp_output.wav"
    shutil.copyfile(Path(in_path).expanduser(), fin)
    lv = write_output(y, fout)
    return {"input": fin.name, "output": fout.name, "inputVersion": info["version"], "inputMatch": info["match"],
            "inputMd5": info["md5"], "samples": int(len(y)), "rate": RATE, "format": "mono WAV, 48 kHz, 24-bit PCM",
            **lv, "latencySamples": rep.get("latencySamples"), "latencyCompensated": True}


def format_notes(preset_name: str | None, mode: str, pair: dict, notes: dict, licence_note: str, nonc: list[str],
                 ir_name: str | None) -> str:
    """Text of ``<stem>.reamp_notes.txt``."""
    L = [f"Sawblade reamp pair{f' - {preset_name}' if preset_name else ''} ({mode} export)", "",
         f"Input : {pair['input']}  (the NAM standard input file you supplied, copied unchanged)",
         f"Output: {pair['output']}  (the same signal rendered through the chain, {pair['format']}, same length, "
         "latency-compensated)", "",
         "Use: train a NAM model the standard way (the NAM trainer, GUI or command line) with this input and output file.",
         "The render is the chain a trained model would learn: no gate, no reverb, delay or modulation, no bus compressor "
         + ("(" + ("no cab and no post EQ: the exported IR holds them" if mode == "nocab" else "the cab is inside the output") + ")."),
         ]
    if pair.get("levelReducedDb"):
        L += ["", f"The output was too loud for 24-bit and was reduced by {pair['levelReducedDb']:.2f} dB; "
                  "raise the trained model's output level by that amount."]
    if mode == "nocab" and ir_name:
        L += ["", f"Cab IR + post EQ for the no-cab model: {ir_name} (load it WITHOUT loudness normalisation)."]
    L += ["", "Stages of the preset that are NOT in the pair, in signal order:", ""]
    if not notes["stages"]:
        L += [notes.get("message", "Nothing: the whole chain is in the pair."), ""]
    for i, st in enumerate(notes["stages"], 1):
        L += [f"{i}. {st['stage']} [{st['position']}]", f"   {st['hardware']}", ""]
    L += ["PERSONAL USE ONLY. This pair (and any model trained from it) is derived from TONE3000 captures: use it for "
          "yourself, never upload or share it (sharing needs permission from the capture creators and TONE3000)."]
    if nonc:
        L += ["NON-COMMERCIAL: it contains non-commercially licensed captures: " + "; ".join(nonc) + "."]
    L += ["", licence_note]
    return "\n".join(L) + "\n"
