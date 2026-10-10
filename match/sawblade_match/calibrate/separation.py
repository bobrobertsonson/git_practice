"""Method 1: stem separation with Demucs (``htdemucs``, MIT). Optional: ``pip install 'sawblade-match[separation]'``.

The "other" stem approximates the guitars for this instrumentation (vocals, drums, bass are separate stems).
Stems are cached under ``testdata/stems/`` (git-ignored) keyed by the input file's sha256 and the model name.
Deterministic: ``shifts=0`` (no random shift averaging), fixed torch seed (``SEED``), CPU, ``split=True``.
If demucs / torch are not installed or the model weights cannot be fetched (for instance the network policy
blocks dl.fbaipublicfiles.com) :func:`separate_other` returns an :class:`Unavailable` - never raises - and
the calibration run continues with method 2 only.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf

MODEL = "htdemucs"
SEED = 0


@dataclass(frozen=True)
class Unavailable:
    reason: str

    def __str__(self) -> str:
        return f"unavailable: {self.reason}"


@dataclass(frozen=True)
class Stem:
    audio: np.ndarray        # (samples, channels) float32
    rate: int
    path: Path
    cached: bool


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stem_cache_path(src: Path, stems_dir: Path, stem: str = "other", model: str = MODEL) -> Path:
    return stems_dir / f"{src.stem}.{_sha256(src)[:12]}.{model}.{stem}.wav"


def _short(e: BaseException) -> str:
    return " ".join(f"{type(e).__name__}: {e}".split())[:300]


def separate_other(src: Path, stems_dir: Path, model: str = MODEL) -> Stem | Unavailable:
    """Return the 'other' stem of ``src`` (cached), or Unavailable(reason)."""
    src, stems_dir = Path(src), Path(stems_dir)
    try:
        cache = stem_cache_path(src, stems_dir, "other", model)
        if cache.exists():
            a, fs = sf.read(str(cache), dtype="float32", always_2d=True)
            return Stem(a, int(fs), cache, True)
    except OSError as e:
        return Unavailable(f"cannot read input/cache: {_short(e)}")
    try:
        import torch
        from demucs.apply import apply_model
        from demucs.pretrained import get_model
    except Exception as e:  # ImportError, or a torch/torchaudio ABI problem
        return Unavailable("demucs/torch not importable (pip install 'sawblade-match[separation]'): " + _short(e))
    try:
        mdl = get_model(model)            # downloads the weights on first use
    except Exception as e:
        return Unavailable("model weights could not be loaded/downloaded: " + _short(e))
    try:
        torch.manual_seed(SEED)
        torch.set_num_threads(max(1, torch.get_num_threads()))
        x, fs = sf.read(str(src), dtype="float32", always_2d=True)
        if x.shape[1] == 1:
            x = np.repeat(x, 2, axis=1)
        x = x[:, :2]
        if fs != mdl.samplerate:
            from fractions import Fraction
            from scipy import signal
            f = Fraction(mdl.samplerate, fs)
            x = signal.resample_poly(x, f.numerator, f.denominator, axis=0).astype(np.float32)
        wav = torch.from_numpy(np.ascontiguousarray(x.T))[None]
        ref = wav.mean(0)
        wav = (wav - ref.mean()) / (ref.std() + 1e-8)
        with torch.no_grad():
            out = apply_model(mdl, wav, shifts=0, split=True, overlap=0.25, device="cpu", progress=False)[0]
        out = out * (ref.std() + 1e-8) + ref.mean()
        idx = mdl.sources.index("other")
        other = out[idx].numpy().T.astype(np.float32)
        stems_dir.mkdir(parents=True, exist_ok=True)
        sf.write(str(cache), other, int(mdl.samplerate), subtype="FLOAT")
        return Stem(other, int(mdl.samplerate), cache, False)
    except Exception as e:
        return Unavailable("separation failed: " + _short(e))
