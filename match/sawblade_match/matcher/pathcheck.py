"""Per-path check of a matched blend (v0.4M Task F.2).

    python -m sawblade_match.matcher.pathcheck --result <run>/result.json --di <di.wav> --ref-a <hm2.wav> --ref-b <body.wav>
        [--ref-blend <blend.wav>] [--blend-db A_DB,B_DB] [--offset-ms MS] [--json out.json]

Takes the winning preset of a matcher run and renders the DI over its full length three ways with the core engine: the full
preset, path B disabled (A alone) and path A disabled (B alone); everything else unchanged (cab, post EQ, bus comp, alignment,
the gate as stored in the preset). Each render is scored against its reference (A alone vs ``--ref-a``, B alone vs ``--ref-b``,
the full preset vs ``--ref-blend``) with the numbers the run itself reports:

* ``aWeightedErrorDb``  exactly the run's ``after[0].aWeightedErrorDb`` (tonecheck LTAS comparison, both signals normalised to
  their 1 kHz band; the run writes the render and the reference as float32 WAV, so this does too);
* ``ltasLossDb``        ``loss.ltas_error`` of the matcher loss (level offset removed) on the DI-aligned overlap, with its offset;
* ``feel``              ``feel.evaluate`` on the same overlap (tight / fizz / polish values and the raw measures).

The "swapped" pairing (A vs ref-b, B vs ref-a) shows a role swap; the primary pairing is A <-> ref-a, B <-> ref-b. The blend
ratio compares ``LUFS(A alone) - LUFS(B alone)`` of the renders with ``refRatioDb`` of the references (``refsum``).

Trims: the chain measures the level-match trims only while both paths are enabled, so each single-path render gets the full
preset's effective trim folded into that path's ``levelDb`` (and level matching off): a path alone is then exactly its own
contribution to the blend, and the sum of the two single renders equals the full render (linear chain, no bus comp).

DI offset: the run's stored offset (``offsetRefinement.final.L``, else the starter refinement, else ``offset_search``) when
``--di`` is the DI of that run; otherwise (e.g. the held-out transfer on another DI) the matcher's own search
(``offset.resolve_offset`` + ``refine_offset``), or ``--offset-ms`` to force one. The LTAS number does not depend on it; the
feel terms do. Single-path winners (one path disabled, or level <= -60 dB) report ``singlePath`` and only the full render.
Deterministic. Exit 0 unless the inputs are unreadable (2).
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

from ..tonecheck.analysis import analyze
from ..tonecheck.cli import compare_to_reference
from . import feel as FEEL
from . import loss as L
from . import refsum as RS
from .engine import RATE, Engine, to48
from .loudness import integrated_lufs
from .offset import refine_offset, resolve_offset
from .profile import DEFAULT_BASE, load_profile
from .reference import Excerpt, Reference, build_target, load_reference
from .run import PLACED_WINDOW_MS, _finite_or_none

OFF_LEVEL_DB = -60.0      # a path at or below this level counts as off


class PathcheckError(Exception):
    """Unreadable / unusable inputs (exit 2)."""


def load_run(result_path: Path, di_path) -> tuple[dict, dict, str, np.ndarray, int]:
    """(result.json, winning preset, its file name, DI samples as read, DI rate); PathcheckError when unreadable."""
    try:
        res = json.loads(Path(result_path).read_text())
        pname = (res.get("best") or {}).get("preset") or "best.preset.resolved.json"
        preset = json.loads((Path(result_path).parent / pname).read_text())
        x, fs = sf.read(str(di_path), dtype="float32")
    except Exception as e:
        raise PathcheckError(f"cannot read the result / preset / DI: {e}") from e
    return res, preset, pname, x, int(fs)


# ---- presets ------------------------------------------------------------------------------------------------------
def path_state(preset: dict) -> dict:
    """{"a": live?, "b": live?} (enabled and level above OFF_LEVEL_DB)."""
    st = {}
    for k in ("a", "b"):
        p = (preset.get("paths") or {}).get(k) or {}
        st[k] = bool(p.get("enabled", True)) and float(p.get("levelDb", 0.0)) > OFF_LEVEL_DB
    return st


def single_preset(preset: dict, keep: str, trims_db: dict) -> dict:
    """``preset`` with the other path disabled, ``keep``'s trim folded into its levelDb and level matching off."""
    p = copy.deepcopy(preset)
    other = "b" if keep == "a" else "a"
    p["paths"][other]["enabled"] = False
    live = p["paths"][keep]
    live["enabled"] = True
    live["levelDb"] = float(live.get("levelDb", 0.0)) + float(trims_db.get(keep, 0.0))
    p["levelMatch"] = {"mode": "off"}
    return p


# ---- offset -------------------------------------------------------------------------------------------------------
def stored_offset(res: dict) -> tuple[int, str] | None:
    """The run's DI offset in 48 kHz samples and where it came from, or None."""
    orf = res.get("offsetRefinement") or {}
    fin = (orf.get("final") or {}).get("L") or {}
    if "offsetSamples" in fin:
        return int(round(fin["offsetSamples"] * RATE / fin.get("rate", RATE))), "run final offset"
    st = (orf.get("excerptStarterRender") or {})
    if "offset" in st:
        return int(st["offset"]), "run starter-render offset"
    osr = res.get("offset_search") or {}
    if "offset_samples" in osr:
        return int(osr["offset_samples"]), "run offset_search"
    return None


def search_offset(di48: np.ndarray, y48: np.ndarray, ref_sig: np.ndarray) -> tuple[int, str]:
    """The matcher's own search for where the DI sits in the reference, refined on the full render."""
    info = resolve_offset(di48, ref_sig.astype(np.float64), RATE, False, 0)
    search = int(PLACED_WINDOW_MS * RATE / 1000) if info["mode"] == "whole_song" else int(3.0 * RATE)
    r = refine_offset(y48.astype(np.float64), ref_sig.astype(np.float64), RATE, info["offset_samples"], start=0, search=search)
    off = r["offset"] if r["envPeakRatio"] >= 2.0 else info["offset_samples"]
    return int(off), f"searched ({info['mode']})"


# ---- metrics ------------------------------------------------------------------------------------------------------
def _terms_json(terms: dict | None) -> dict | None:
    if terms is None:
        return None
    keep = ("mode", "tight", "fizz", "polish", "flux", "crest", "floor", "total", "tightRaw", "fizzRaw", "polishRaw",
            "noteSet", "notes", "dropped")
    return {k: terms.get(k) for k in keep}


def feel_and_ltas(y48: np.ndarray, ref: Reference, off: int, di48: np.ndarray) -> dict:
    """Matcher-loss numbers on the DI-aligned overlap of the whole render: ``loss.ltas_error`` (level offset removed) and
    the feel terms (``reference.build_target`` + ``feel.evaluate``, the run's own construction on a full-length excerpt)."""
    start = max(0, -off)
    end = min(len(di48), len(ref.matched_sig) - off, len(y48))
    if end - start < 4 * L.NFFT:
        raise ValueError(f"DI and reference overlap by only {max(end - start, 0) / RATE:.2f} s at offset {off} samples")
    ex = Excerpt(start, end, 0, np.ascontiguousarray(di48[start:end], dtype=np.float32), {"note": "full-length overlap"})
    ref.offset_samples = int(off)
    tgt = build_target(ref, ex)
    seg = np.asarray(y48[start:end], dtype=np.float64)
    f = L.features(seg, tgt.starts, tgt.onsets)
    ltas, ltas_off = L.ltas_error(f.band_db, tgt.ref.band_db, tgt.hf_limit_hz)
    out = {"ltasLossDb": float(ltas), "ltasOffsetDb": float(ltas_off), "overlapS": [start / RATE, end / RATE], "feel": None}
    if tgt.feel is not None:
        total, terms = FEEL.evaluate(seg, tgt.feel)
        out["feel"] = {"weighted": float(total), **(_terms_json(terms) or {})}
    return out


class _Analyses:
    """Memoised tonecheck analyses (the run's ``aWeightedErrorDb`` is ``compare_to_reference`` of two of them)."""

    def __init__(self, targets: dict):
        self.targets, self._y, self._r = targets, {}, {}

    def y(self, name: str, y: np.ndarray, fs: int):
        if name not in self._y:
            self._y[name] = analyze(np.asarray(y, dtype=np.float64), fs, self.targets)
        return self._y[name]

    def r(self, name: str, ref: Reference):
        if name not in self._r:      # the run analyses ref.ltas_sig through a float32 WAV
            self._r[name] = analyze(ref.ltas_sig.astype(np.float32).astype(np.float64), RATE, self.targets)
        return self._r[name]

    def error(self, yname: str, y, fs: int, rname: str, ref: Reference) -> float:
        return float(compare_to_reference(self.y(yname, y, fs), self.r(rname, ref))["aWeightedErrorDb"])


def _lufs(y, fs) -> float:
    return float(integrated_lufs(np.asarray(y, dtype=np.float64), fs))


def _peak_db(y) -> float:
    p = float(np.max(np.abs(y))) if len(y) else 0.0
    return float(20 * np.log10(p)) if p > 0 else float("-inf")


# ---- the check ----------------------------------------------------------------------------------------------------
def pathcheck(result_path: str | Path, di_path: str | Path, ref_a: str | Path, ref_b: str | Path,
              ref_blend: str | Path | None = None, blend_db: tuple[float, float] = (0.0, 0.0),
              offset_ms: float | None = None, engine: Engine | None = None) -> dict:
    result_path = Path(result_path)
    res, preset, pname, x, fs = load_run(result_path, di_path)
    x = x if x.ndim == 1 else x[:, 0]                     # the run renders the first channel
    di48 = to48(x, fs)
    refs: dict[str, Reference] = {}
    try:
        for k, p in (("a", ref_a), ("b", ref_b), ("blend", ref_blend)):
            if p is not None:
                refs[k] = load_reference(p, matched="mono")           # as the run's --matched mono (clean mono target)
        ra, rb = RS.read_mono(ref_a), RS.read_mono(ref_b)
    except Exception as e:
        raise PathcheckError(f"cannot read a reference: {e}") from e
    targets = load_profile(DEFAULT_BASE)
    an = _Analyses(targets)
    state = path_state(preset)
    single = not (state["a"] and state["b"])
    live = "a" if state["a"] else ("b" if state["b"] else None)

    own = engine is None
    eng = engine or Engine(None, 1)
    try:
        y_full, rep_full = eng.render(preset, x, fs)
        lm = rep_full.get("levelMatch") or {}
        trims = {"a": float(lm.get("trimADb") or 0.0), "b": float(lm.get("trimBDb") or 0.0)}
        renders = {"full": y_full}
        if not single:
            for k in ("a", "b"):
                renders[k], _ = eng.render(single_preset(preset, k, trims), x, fs)
    finally:
        if own:
            eng.close()
    y48 = {k: to48(v, fs) for k, v in renders.items()}

    # DI offset
    ref_for_off = refs.get("blend") or refs["a"]
    if offset_ms is not None:
        off, src = int(round(offset_ms * RATE / 1000.0)), "--offset-ms"
    else:
        same = False
        try:
            same = Path(res.get("di", "")).resolve() == Path(di_path).resolve()
        except Exception:
            pass
        so = stored_offset(res) if same else None
        off, src = so if so is not None else search_offset(di48, y48["full"], ref_for_off.matched_sig)

    def score(name: str, rname: str) -> dict:
        d = {"ref": rname, "aWeightedErrorDb": an.error(name, renders[name], fs, rname, refs[rname]),
             "lufs": _lufs(renders[name], fs), "peakDb": _peak_db(renders[name])}
        try:
            d.update(feel_and_ltas(y48[name], refs[rname], off, di48))
        except Exception as e:                 # reporting only: keep the LTAS number
            d["feelError"] = str(e)
        return d

    out: dict = {"schema": "sawblade.pathcheck", "version": 1, "result": str(result_path), "preset": str(result_path.parent / pname),
                 "di": str(di_path), "diRate": fs, "blendDb": list(blend_db),
                 "refs": {"a": str(ref_a), "b": str(ref_b), "blend": None if ref_blend is None else str(ref_blend)},
                 "offset": {"samples48": off, "ms": 1000.0 * off / RATE, "source": src},
                 "runAfterDb": ((res.get("after") or [{}])[0]).get("aWeightedErrorDb"),
                 "levelMatchTrimsDb": trims, "singlePath": bool(single), "randomness": "none (deterministic)"}
    if single:
        out["path"] = live
        rname = "blend" if "blend" in refs else live
        out["full"] = score("full", rname)
        out["note"] = ("single-path winner: only the full render is scored, against "
                       + ("the blend reference" if rname == "blend" else f"ref-{live} (no --ref-blend given)"))
        return _finite_or_none(out)

    out["a"], out["b"] = score("a", "a"), score("b", "b")
    if "blend" in refs:
        out["full"] = score("full", "blend")
    out["swapped"] = {"aVsRefB": an.error("a", renders["a"], fs, "b", refs["b"]),
                      "bVsRefA": an.error("b", renders["b"], fs, "a", refs["a"])}
    ga, gb = (10 ** (blend_db[0] / 20), 10 ** (blend_db[1] / 20))
    n = min(len(ra[0]), len(rb[0]))
    ref_ratio = None
    if ra[1] == rb[1]:
        la, lb = _lufs(ra[0][:n] * ga, ra[1]), _lufs(rb[0][:n] * gb, rb[1])
        ref_ratio = la - lb if np.isfinite(la) and np.isfinite(lb) else None
    ca, cb = out["a"]["lufs"], out["b"]["lufs"]
    chosen = ca - cb if np.isfinite(ca) and np.isfinite(cb) else None
    out["ratio"] = {"chosenDb": chosen, "refDb": ref_ratio,
                    "diffDb": None if chosen is None or ref_ratio is None else chosen - ref_ratio,
                    "blendDb": list(blend_db), "chosenDefinition": "LUFS(A alone) - LUFS(B alone) on the renders",
                    "refDefinition": "LUFS(ref-a * gA) - LUFS(ref-b * gB)"}
    return _finite_or_none(out)


def _f(v, fmt="{:.2f}") -> str:
    return "n/a" if v is None else fmt.format(v)


def _feel_line(d: dict) -> str:
    fe = d.get("feel")
    if not fe:
        return d.get("feelError", "feel: n/a")
    return (f"feel tight {_f(fe.get('tight'), '{:.3f}')} fizz {_f(fe.get('fizz'), '{:.3f}')} polish "
            f"{_f(fe.get('polish'), '{:.3f}')} (weighted {_f(fe.get('weighted'), '{:.3f}')}"
            + (f"; dropped {','.join(fe['dropped'])}" if fe.get("dropped") else "") + ")")


def format_report(r: dict) -> str:
    lines = [f"pathcheck: {r['result']}", f"  DI offset {r['offset']['ms']:+.2f} ms ({r['offset']['source']}); "
             f"run's own after[0] A-weighted error {_f(r['runAfterDb'])} dB"]

    def row(label, d):
        lines.append(f"  {label:<14} vs ref-{d['ref']:<5} A-weighted {_f(d['aWeightedErrorDb'])} dB | LTAS loss "
                     f"{_f(d.get('ltasLossDb'))} dB | LUFS {_f(d['lufs'], '{:.1f}')} | {_feel_line(d)}")
    if r["singlePath"]:
        lines.append(f"  single-path winner (path {r['path'].upper()}): {r['note']}")
        row("full preset", r["full"])
        return "\n".join(lines)
    row("A alone", r["a"])
    row("B alone", r["b"])
    if "full" in r:
        row("full preset", r["full"])
    s, q = r["swapped"], r["ratio"]
    lines.append(f"  swapped pairing: A vs ref-b {_f(s['aVsRefB'])} dB, B vs ref-a {_f(s['bVsRefA'])} dB "
                 f"(primary: {_f(r['a']['aWeightedErrorDb'])} / {_f(r['b']['aWeightedErrorDb'])})")
    lines.append(f"  blend ratio A - B: chosen {_f(q['chosenDb'])} dB, reference {_f(q['refDb'])} dB, "
                 f"difference {_f(q['diffDb'], '{:+.2f}')} dB (faders {q['blendDb'][0]:+g} / {q['blendDb'][1]:+g} dB)")
    return "\n".join(lines)


def main(argv=None) -> int:
    argv = RS.glue_blend_db(argv)
    ap = argparse.ArgumentParser(prog="pathcheck", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--result", required=True, help="result.json of a matcher run")
    ap.add_argument("--di", required=True)
    ap.add_argument("--ref-a", required=True, help="the amp track of path A (e.g. HM2)")
    ap.add_argument("--ref-b", required=True, help="the amp track of path B (e.g. the body amp)")
    ap.add_argument("--ref-blend", default=None, help="the blend reference (refsum output)")
    ap.add_argument("--blend-db", default="0,0", help="A_DB,B_DB faders used for the reference ratio (as refsum)")
    ap.add_argument("--offset-ms", type=float, default=None, help="force the DI offset (default: the run's, or searched)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)
    try:
        bdb = RS.parse_blend_db(args.blend_db)
    except ValueError as e:
        print(f"pathcheck: {e}", file=sys.stderr)
        return 2
    try:
        r = pathcheck(args.result, args.di, args.ref_a, args.ref_b, args.ref_blend, bdb, args.offset_ms)
    except PathcheckError as e:
        print(f"pathcheck: {e}", file=sys.stderr)
        return 2
    print(format_report(r))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(r, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
