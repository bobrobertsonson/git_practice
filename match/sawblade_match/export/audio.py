"""Listening files: level-matched A/B (original then export) as WAV + MP3."""
from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np
import soundfile as sf

from .chain import RATE


def encode_mp3(wav: Path, mp3: Path, kbps: int = 192) -> bool:
    try:
        import lameenc
        x, fs = sf.read(str(wav), dtype="float32", always_2d=True)
        enc = lameenc.Encoder()
        enc.set_bit_rate(kbps)
        enc.set_in_sample_rate(int(fs))
        enc.set_channels(x.shape[1])
        enc.set_quality(2)
        pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
        mp3.write_bytes(bytes(enc.encode(pcm.tobytes()) + enc.flush()))
        return True
    except ImportError:
        pass
    try:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-b:a", f"{kbps}k", str(mp3)], check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def listening_ab(original: np.ndarray, export: np.ndarray, out_dir: Path, name: str = "ab_original_then_export",
                 gap_s: float = 0.8) -> dict:
    """original, gap, export (export gain-matched to the original's RMS; the pair is peak-limited to -1 dBFS)."""
    n = min(len(original), len(export))
    o, e = original[:n].astype(np.float64), export[:n].astype(np.float64)
    match_db = 20 * np.log10(np.sqrt(np.mean(o * o)) / max(np.sqrt(np.mean(e * e)), 1e-12))
    e = e * 10 ** (match_db / 20)
    peak = max(np.max(np.abs(o)), np.max(np.abs(e)), 1e-9)
    g = min(1.0, 10 ** (-1.0 / 20) / peak)
    y = np.concatenate([o, np.zeros(int(gap_s * RATE)), e]) * g
    out_dir.mkdir(parents=True, exist_ok=True)
    wav = out_dir / f"{name}.wav"
    sf.write(str(wav), y.astype(np.float32), RATE, subtype="PCM_24")
    mp3 = out_dir / f"{name}.mp3"
    ok = encode_mp3(wav, mp3)
    return {"wav": str(wav), "mp3": str(mp3) if ok else None, "exportGainMatchDb": float(match_db),
            "limiterGainDb": float(20 * np.log10(g)), "layout": f"original {n / RATE:.1f} s, {gap_s} s gap, export"}
