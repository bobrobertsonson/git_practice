"""Held-out scoring (spec B.3). Everything here reuses the matcher's own measures: the A-weighted error is
``pathcheck._Analyses.error`` (``tonecheck.cli.compare_to_reference`` of two ``analyze`` results), the LTAS loss and feel terms are
``pathcheck.feel_and_ltas``, the raw feel deltas are ``known_answer.feel_deltas``, the guardrails are ``run._guardrails`` and the
levels are ``loudness.integrated_lufs``."""
from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from ..matcher.engine import RATE
from ..matcher.known_answer import feel_deltas
from ..matcher.loudness import integrated_lufs
from ..matcher.pathcheck import _Analyses, feel_and_ltas
from ..matcher.profile import DEFAULT_BASE, load_profile
from ..matcher.reference import Excerpt, build_target, load_reference
from ..matcher.run import _guardrails

FEEL_KEYS = ("t12Ms", "sustainDb", "hfRatioDb", "hfFlat", "fluxDb", "floorDb")
WITHIN_TOL_DB = 0.5          # known_answer.TOLERANCE_DB


def lufs(x) -> float:
    return float(integrated_lufs(np.asarray(x, dtype=np.float64), RATE))


class Scorer:
    """One per run: memoises the tonecheck analyses (the A-weighted error of a render against a reference)."""

    def __init__(self):
        self.an = _Analyses(load_profile(DEFAULT_BASE))
        self._n = 0

    def _name(self) -> str:
        self._n += 1
        return f"s{self._n}"

    def aweighted(self, y: np.ndarray, ref) -> float:
        n = self._name()
        return self.an.error(n, y, RATE, n, ref)

    def measure(self, y_h: np.ndarray, di_h: np.ndarray, ref_path: str | Path, profile: dict | None) -> dict:
        """Held-out numbers of one aligned render section (``y_h``, ``di_h`` 48 kHz, same length; the reference file is the
        same section of the isolated track, time-aligned): A-weighted error, LTAS loss, feel terms + raw deltas, guardrails."""
        ref = load_reference(ref_path, channel="auto", matched="mono", offset_ms=0.0)
        out: dict = {"aWeightedErrorDb": self.aweighted(y_h, ref), "seconds": len(y_h) / RATE, "ltasLossDb": None, "feel": None,
                     "guardrails": None if profile is None else _guardrails(np.asarray(y_h, np.float64), profile)}
        try:
            fl = feel_and_ltas(y_h, ref, 0, di_h)
        except ValueError as e:
            out["note"] = str(e)
            return out
        out["ltasLossDb"] = fl["ltasLossDb"]
        fe = fl.get("feel")
        ex = Excerpt(0, len(di_h), 0, np.ascontiguousarray(di_h, dtype=np.float32), {"note": "held-out section"})
        ft = build_target(ref, ex).feel
        deltas = feel_deltas(np.asarray(y_h, np.float64), ft) if ft is not None else {k: None for k in FEEL_KEYS}
        if fe is None:
            out["feel"] = {"unavailable": "no feel target for this reference", "deltas": deltas}
        else:
            out["feel"] = {"tight": fe.get("tight"), "fizz": fe.get("fizz"), "polish": fe.get("polish"), "total": fe.get("weighted"),
                           "dropped": fe.get("dropped"), "deltas": deltas,
                           "unavailable": {k: "not measurable on this reference" for k, v in deltas.items() if v is None}}
        return out

    def measure_unmatched(self, y: np.ndarray, ref_path: str | Path) -> dict:
        """Tier 2: A-weighted LTAS error only (the reference is not time-aligned with the DI)."""
        ref = load_reference(ref_path, channel="auto")
        return {"aWeightedErrorDb": self.aweighted(y, ref), "ltasLossDb": None, "guardrails": None,
                "feel": None, "feelReason": "unmatched reference"}


def write_listen(out_dir: Path, ref_h: np.ndarray, y_h: np.ndarray) -> dict:
    """``ref.wav`` and ``render.wav`` (float32, 48 kHz): the render gained to the reference's BS.1770 loudness."""
    out_dir.mkdir(parents=True, exist_ok=True)
    lr, ly = lufs(ref_h), lufs(y_h)
    gain = lr - ly if np.isfinite(lr) and np.isfinite(ly) else 0.0
    sf.write(str(out_dir / "ref.wav"), np.asarray(ref_h, np.float32), RATE, subtype="FLOAT")
    sf.write(str(out_dir / "render.wav"), (np.asarray(y_h, np.float64) * 10 ** (gain / 20)).astype(np.float32), RATE, subtype="FLOAT")
    return {"refLufs": lr if np.isfinite(lr) else None, "renderLufs": ly if np.isfinite(ly) else None, "renderGainDb": float(gain)}


def total(cases: dict, manifest_cases: list[dict]) -> dict:
    """Over the counted cases of the selection: n ran (status ok) of m, mean / median held-out A-weighted error, how many are
    within the known-answer tolerance, how many have a failing guardrail."""
    counted = [c["id"] for c in manifest_cases if c["counts"]]
    ok = [cid for cid in counted if cases.get(cid, {}).get("status") == "ok"]
    vals = [cases[c]["heldOut"]["aWeightedErrorDb"] for c in ok]
    gfail = sum(1 for c in ok if (cases[c]["heldOut"].get("guardrails") or {}).get("fail"))
    return {"n": len(ok), "m": len(counted), "meanAWeightedDb": float(np.mean(vals)) if vals else None,
            "medianAWeightedDb": float(np.median(vals)) if vals else None,
            "within05": int(sum(v <= WITHIN_TOL_DB for v in vals)), "guardrailFails": int(gfail)}


def total_line(t: dict) -> str:
    f = lambda v: "n/a" if v is None else f"{v:.2f}"
    return (f"bench total: {t['n']} of {t['m']} counted cases ran, mean held-out A-wt {f(t['meanAWeightedDb'])} dB, "
            f"median {f(t['medianAWeightedDb'])} dB, within 0.5 dB: {t['within05']}, guardrail fails {t['guardrailFails']}")
