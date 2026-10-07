"""Recognition of the NAM project's standard input file, the way the pinned trainer does (reusable: the reamp pair
uses it now, built-in training later).

The user supplies the standard input file ("the NAM project's standard input file, as used by the NAM trainer"); it is
never bundled, downloaded or committed by Sawblade.  It is recognised the way the pinned trainer
(``neural-amp-modeler`` 0.13.0, ``nam/train/core.py``) recognises it, see ``recognise``.  The trainer ships no input file (its GUI links Google Drive downloads, ``nam/train/gui/__init__.py:267-269``;
``nam/train/_names.py:21`` maps version 3.0.0 to ``input.wav`` / ``v3_0_0.wav``).
"""
from __future__ import annotations

import hashlib
from pathlib import Path

import numpy as np
import soundfile as sf

from .plan import ExportRefused

RATE = 48000

# ---- the trainer's known standard inputs (neural-amp-modeler 0.13.0, nam/train/core.py) -------------------------------
# Strong match: MD5 of the file's bytes (core.py:91-96).  Only V3 is current; V1/V2 and the 44.1 kHz Proteus file are
# deprecated by the trainer (core.py:802-806, data checks fail for any major version other than 3), so they are not accepted.
# Weak match: MD5 of the float64 sample array (int sample / 2^(8*sampwidth-1)) of the first 17 s and of the last 9 s
# (core.py:146-158 (17 s / 9 s: 154-155), table core.py:204-211).  Tests monkeypatch these tables with a synthetic stand-in's signature.
STANDARD_INPUT_STRONG_MD5 = {"4d54a958861bf720ec4637f43d44a7ef": "1.0.0", "7c3b6119c74465f79d96c761a0e27370": "1.1.1",
                             "ede3b9d82135ce10c7ace3bb27469422": "2.0.0", "36cd1af62985c2fac3e654333e36431e": "3.0.0"}
# weak: version -> {(start hash, end hash)} (core.py:204-233); the slices per version are in ``_weak_slices`` (core.py:112-158).
# Checked newest first, like the trainer.  The 44.1 kHz Proteus file (version 4, core.py:236) is recognised by the trainer but
# Sawblade renders at 48 kHz, so it is refused by the rate check below.
STANDARD_INPUT_WEAK = {
    "3.0.0": {("dadb5d62f6c3973a59bf01439799809b", "8458126969a3f9d8e19a53554eb1fd52")},
    "2.0.0": {("1c4d94fbcb47e4d820bef611c1d4ae65", "28694e7bf9ab3f8ae6ef86e9545d4663")},
    "1.0.0": {("bb4e140c9299bae67560d280917eb52b", "9b2468fcb6e9460a399fc5f64389d353")},
    "1.1.1": {("9f20c6b5f7fef68dd88307625a573a14", "8458126969a3f9d8e19a53554eb1fd52")},
}
T_VALIDATE = 432_000       # core.py:270-: validation segment length of every version (9 s)
EXPECTED_SIGNATURE = ("Expected the NAM project's standard input file as used by the NAM trainer (current version 3, "
                      "v3_0_0 / input.wav): mono WAV, 48 kHz, 24-bit PCM.")
CLIP_CEILING = 0.999       # the 24-bit output must not clip; louder renders are scaled down (reported)
MIN_SECONDS = 26           # shortest span the weak hashes read (v3: first 17 s + last 9 s)


def _weak_slices(x: np.ndarray) -> dict:
    """The sample ranges the trainer hashes, per version (48 kHz): v1 ``core.py:112-127``, v2 ``129-144``, v3 ``146-158``."""
    r = RATE
    return {"3.0.0": (x[: 17 * r], x[-9 * r:]),
            "2.0.0": (x[: 96_000 + 6 * r], x[-(2 * T_VALIDATE + 96_000):]),
            "1.0.0": (x[: 48_000 + 6 * r], x[-T_VALIDATE:]), "1.1.1": (x[: 48_000 + 6 * r], x[-T_VALIDATE:])}


def validation_slice(version: str, n: int) -> slice:
    """Validation split of the trainer for a file of ``n`` samples (``_get_data_config``, core.py:810-): v1 / v3 the last 9 s,
    v2 the 9 s before the closing blips."""
    return slice(n - 960_000, n - 960_000 + T_VALIDATE) if version.startswith("2.") else slice(n - T_VALIDATE, n)


_WIDTH = {"PCM_16": 2, "PCM_24": 3, "PCM_32": 4}


def read_standard_samples(path: Path) -> tuple[np.ndarray, int]:
    """Samples the trainer's ``wav_to_np`` would see: ``(float64 samples, rate)`` of a mono integer-PCM WAV
    (int / 2^(8*sampwidth-1), bit-exact for 16/24/32-bit).  Anything else raises ``ExportRefused``."""
    try:
        info = sf.info(str(path))
    except (RuntimeError, OSError) as e:
        raise ExportRefused(f"NAM input file {path}: cannot read the file as a WAV ({e}). {EXPECTED_SIGNATURE}") from e
    if info.format != "WAV" or info.subtype not in _WIDTH:
        raise ExportRefused(f"NAM input file {path}: the trainer reads integer PCM WAV files only (this is "
                            f"{info.format} {info.subtype}). {EXPECTED_SIGNATURE}")
    if info.channels != 1:
        raise ExportRefused(f"NAM input file {path}: {info.channels} channels; the trainer needs a mono file. "
                            f"{EXPECTED_SIGNATURE}")
    w = _WIDTH[info.subtype]
    raw, rate = sf.read(str(path), dtype="int32", always_2d=False)      # libsndfile left-aligns narrower PCM in int32
    x = (raw.astype(np.int64) >> (32 - 8 * w)).astype(np.float64) / float(2 ** (8 * w - 1))
    return x, int(rate)


def _md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(65536), b""):
            h.update(blk)
    return h.hexdigest()


def _md5_array(x: np.ndarray) -> str:
    return hashlib.md5(np.ascontiguousarray(x)).hexdigest()


def recognise(path) -> dict:
    """Recognise ``path`` the way the pinned trainer does (``_detect_input_version``, core.py:66-240) and return
    ``{"version": "3.0.0", "match": "strong"|"weak", "samples", "rate", "md5", "sampwidthBytes"}``.  Every version the trainer
    knows at 48 kHz is accepted (older ones are deprecated by the trainer).  Raises ``ExportRefused`` (a plain message naming
    what was expected) for a missing file, a wrong format / sample rate / length, or a file that is not a known standard
    input.  Reusable: the reamp pair and the built-in training both gate on it."""
    p = Path(path).expanduser()
    if not p.is_file():
        raise ExportRefused(f"NAM input file {path}: no such file. {EXPECTED_SIGNATURE}")
    x, rate = read_standard_samples(p)
    if rate != RATE:
        raise ExportRefused(f"NAM input file {path}: sample rate {rate} Hz; Sawblade needs the {RATE} Hz standard input. "
                            f"{EXPECTED_SIGNATURE}")
    need = MIN_SECONDS * RATE
    if len(x) < need:
        raise ExportRefused(f"NAM input file {path}: {len(x) / RATE:.1f} s long; the standard input is several minutes "
                            f"(at least {need / RATE:.0f} s). {EXPECTED_SIGNATURE}")
    md5 = _md5_file(p)
    version = STANDARD_INPUT_STRONG_MD5.get(md5)
    match = "strong"
    if version is None:
        match = "weak"
        sl = _weak_slices(x)
        hashes = {v: (_md5_array(a), _md5_array(b)) for v, (a, b) in sl.items()}
        version = next((v for v in ("3.0.0", "2.0.0", "1.0.0", "1.1.1") if hashes[v] in STANDARD_INPUT_WEAK.get(v, ())), None)
    if version is None:
        raise ExportRefused(f"NAM input file {path}: this is not a known NAM standard input file (the trainer would not "
                            f"recognise it). {EXPECTED_SIGNATURE}")
    return {"version": version, "match": match, "samples": int(len(x)), "rate": rate, "md5": md5,
            "sampwidthBytes": _WIDTH[sf.info(str(p)).subtype]}


validate_input = recognise
