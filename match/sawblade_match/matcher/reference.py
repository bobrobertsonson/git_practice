"""Reference loading and per-excerpt loss targets."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf

from ..calibrate.channels import stem_guitar_signal
from ..tonecheck.analysis import activity_mask, detect_onsets
from . import loss as L
from .engine import RATE, to48
from .excerpt import select_excerpt

SIDE_POWER_TO_GUITAR_DB = 3.0103   # two uncorrelated hard-panned guitars: side = (gL-gR)/2 -> P_side = P_guitar / 2


@dataclass
class Reference:
    name: str
    path: str
    basis: str                      # "side" | "stem:<file>" | "left" | "right" | "mid"
    ltas_sig: np.ndarray            # 48 kHz mono signal whose LTAS is the target
    level_offset_db: float          # add to measured signal level to get the per-guitar level
    matched_sig: np.ndarray | None = None   # 48 kHz channel time-aligned (up to ``offset``) with the DI
    matched_channel: str | None = None
    offset_samples: int = 0         # coarse DI-within-ref offset (48 kHz samples)
    offset_given: bool = False      # False: unknown, searched in a wide window and refined
    sections: list[tuple[float, float]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    texture: bool = False           # stem basis: HF texture term is a target (loss.py)
    hf_limit_hz: float | None = None    # full-mix basis: LTAS above this is a one-sided ceiling (loss.py)
    matched_fmax: float = L.STFT_FMAX_MIX   # upper edge of the matched-pair STFT term (matched channel is a full mix)
    stem_channel: str | None = None


def _read(path: str | Path) -> tuple[np.ndarray, int]:
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    return x, int(fs)


def find_stem(ref_path: Path, stems_dir: Path | None) -> Path | None:
    """htdemucs 'other' stem cached by sawblade-calibrate (testdata/stems/<name>.<sha12>.htdemucs.other.wav)."""
    d = stems_dir if stems_dir is not None else Path("testdata/stems")
    if not d.is_dir():
        return None
    try:
        from ..calibrate.separation import stem_cache_path
        p = stem_cache_path(Path(ref_path), d)
        if p.exists():
            return p
    except Exception:
        pass
    hits = sorted(d.glob(f"{Path(ref_path).stem}.*.other.wav"))
    return hits[0] if hits else None


def load_reference(path: str | Path, *, channel: str = "auto", stems_dir: Path | None = None,
                   matched: str | None = None, offset_ms: float | None = None,
                   sections: list[tuple[float, float]] | None = None,
                   hf_limit_hz: float | None | str = "auto") -> Reference:
    """``channel`` for the LTAS target: auto (stem if cached, else side for stereo), side, left, right, mid.
    ``hf_limit_hz`` (full-mix bases only; stems never get it): "auto" = 4.5 kHz one-sided ceiling for the automatic
    fallback and for ``side`` (a mix's side channel still holds stereo cymbals), none for an explicitly chosen
    left/right/mid channel (taken as given; pass a number to impose a limit); None = off.
    ``matched`` ("left"/"right"/"mono"): the reference is a time-aligned pair with the DI; that channel is the STFT
    target (the LTAS target still follows ``channel``)."""
    path = Path(path)
    x, fs = _read(path)
    notes: list[str] = []
    stereo = x.shape[1] >= 2
    basis, off_db, sig, stem_kind, auto_fallback = None, 0.0, None, None, False
    if channel in ("auto",):
        stem = find_stem(path, stems_dir)
        if stem is not None:
            sx, sfs = _read(stem)
            mono, kind, off_db, info = stem_guitar_signal(sx)
            sig, basis, stem_kind = to48(mono, sfs), f"stem:{stem.name}", kind
            notes.append(f"htdemucs 'other' stem used as the guitar isolation ({kind} channel of the stem, "
                         f"side/mid {info['sideMidDb']:+.1f} dB)" if info["sideMidDb"] is not None else
                         "htdemucs 'other' stem used as the guitar isolation (mono)")
        else:
            channel = "side" if stereo else "mid"
            auto_fallback = True
    if sig is None:
        if channel == "side":
            if not stereo:
                raise ValueError("--ref-channel side needs a stereo reference")
            sig, basis, off_db = to48((x[:, 0] - x[:, 1]) / 2, fs), "side", SIDE_POWER_TO_GUITAR_DB
        else:
            col = {"left": 0, "right": 1}.get(channel)
            m = x.mean(axis=1) if col is None else x[:, min(col, x.shape[1] - 1)]
            sig, basis = to48(m, fs), channel
    ref = Reference(path.stem, str(path), basis, sig, off_db, notes=notes, stem_channel=stem_kind)
    if stem_kind is not None:
        ref.texture = True
    else:
        lim = (L.HF_LIMIT_HZ if (auto_fallback or channel == "side") else None) if hf_limit_hz == "auto" else hf_limit_hz
        ref.hf_limit_hz = None if lim is None else float(lim)
        ref.matched_fmax = ref.hf_limit_hz if ref.hf_limit_hz is not None else 8000.0
        if lim is not None:
            notes.append(f"full-mix reference ({basis}): LTAS above {lim:g} Hz is a one-sided ceiling (render may be "
                         "darker, never brighter); cymbals dominate there")
        elif channel in ("left", "right", "mid") and not auto_fallback:
            notes.append(f"explicit full-mix channel '{channel}' has no HF limit: cymbals/hats in the mix will pull the "
                         "5-10 kHz fit brighter; consider --ref-hf-limit 4500")
    if matched:
        col = {"left": 0, "right": 1, "mono": 0}[matched]
        ref.matched_sig = to48(x[:, min(col, x.shape[1] - 1)], fs)
        ref.matched_channel = matched
        ref.offset_given = offset_ms is not None
        ref.offset_samples = int(round((offset_ms or 0.0) * RATE / 1000))
    if sections:
        ref.sections = list(sections)
        if matched:
            ref.notes.append("--ref-section ignored for the LTAS of a matched pair (excerpt-matched segment used)")
        else:
            seg = [sig[int(a * RATE):int(b * RATE)] for a, b in sections]
            ref.ltas_sig = np.concatenate(seg)
    return ref


@dataclass
class Excerpt:
    start: int                 # 48 kHz samples into the DI (trim start)
    end: int
    lead: int                  # samples of lead-in rendered before ``start`` (warm-up), >= 0
    x: np.ndarray              # DI at 48 kHz from start-lead to end (float32)
    info: dict

    @property
    def n(self) -> int:
        return self.end - self.start

    def trim(self, y: np.ndarray) -> np.ndarray:
        return y[self.lead:self.lead + self.n]


def make_excerpt(di48: np.ndarray, length_s: float, lead_s: float = 0.5, window: tuple[int, int] | None = None,
                 ref: Reference | None = None) -> Excerpt:
    if window is None:
        a, b, info = select_excerpt(di48, RATE, length_s)
        if ref is not None and ref.matched_sig is not None:      # the matched segment must exist in the reference
            while b + ref.offset_samples + RATE > len(ref.matched_sig) and a >= RATE:
                a, b = a - RATE, b - RATE
            if b + ref.offset_samples > len(ref.matched_sig):
                raise ValueError("the reference is too short for the DI excerpt at this offset")
    else:
        (a, b), info = window, {"note": "explicit window"}
    lead = min(int(lead_s * RATE), a)
    return Excerpt(a, b, lead, np.ascontiguousarray(di48[a - lead:b], dtype=np.float32), info)


def build_target(ref: Reference, ex: Excerpt, offset_samples: int | None = None) -> L.Target:
    """Loss target for one DI excerpt. Unmatched: whole-reference LTAS (activity-gated) and the reference's own
    onsets. Matched: the reference segment at the DI->ref offset, same Welch segments and DI onsets as the output."""
    di = ex.x[ex.lead:]
    mask, _, _ = activity_mask(di.astype(np.float64), RATE)
    starts = L.segment_starts(len(di), mask)
    onsets = detect_onsets(di.astype(np.float64), RATE)
    matched = None
    if ref.matched_sig is not None:
        off = ref.offset_samples if offset_samples is None else offset_samples
        a = ex.start + off
        seg = ref.ltas_sig[a:a + ex.n]
        if len(seg) < ex.n:
            raise ValueError("matched reference segment runs past the end of the reference")
        rf = L.features(seg, starts, onsets if len(onsets) else None)
        matched = ref.matched_sig[a:a + ex.n]
    else:
        rmask, _, _ = activity_mask(ref.ltas_sig, RATE)
        rstarts = L.segment_starts(len(ref.ltas_sig), rmask)
        ron = detect_onsets(ref.ltas_sig, RATE)
        rf = L.features(ref.ltas_sig, rstarts, ron if len(ron) else None)
    return L.Target(starts, rf, onsets if len(onsets) else None, mask, matched, ref.texture, ref.hf_limit_hz,
                    ref.matched_fmax)
