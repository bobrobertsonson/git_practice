"""Benchmark runner (spec B.1-B.3): ``run_bench(cases, root, out, *, pool, ...) -> scores dict`` (also written to ``out``)."""
from __future__ import annotations

import json
import statistics
import subprocess
import time
import traceback
from pathlib import Path

import numpy as np
import soundfile as sf

from ..matcher import refsum as RS
from ..matcher.engine import RATE, Engine, to48
from ..matcher.excerpt import select_excerpt
from ..matcher.offset import resolve_offset
from ..matcher.pathcheck import path_state, search_offset, single_preset
from ..matcher.profile import load_profile
from ..matcher.reference import load_reference
from ..matcher.run import Config, _finite_or_none, _json_default, run_match
from . import manifest as MF
from . import sections as SEC
from . import score as SC

SCHEMA = "sawblade.bench.scores"
VERSION = 1
CH_COL = {"left": 0, "right": 1}


class CaseError(Exception):
    """A case that cannot run (reported as status 'error')."""


# ---- preflight ----------------------------------------------------------------------------------------------------
def preflight(cases: list[dict], root: str | Path) -> dict:
    """``{case id: {"status": "ok"|"missing"|"error", "error": str|None, "files": [{what, path, status, rate, seconds}]}}``.
    missing: a file does not exist; error: unreadable, or the sample rates of one pair disagree."""
    root = Path(root)
    out = {}
    for c in cases:
        files, status, err = [], "ok", None
        if c["kind"] != "pathcheck":
            groups: dict[str, set] = {}
            for f in MF.case_files(c):
                p = root / f["path"]
                row = {"what": f["what"], "path": f["path"], "status": "ok", "rate": None, "seconds": None}
                if not p.is_file():
                    row["status"] = "missing"
                else:
                    try:
                        info = sf.info(str(p))
                        row["rate"], row["seconds"] = int(info.samplerate), float(info.duration)
                    except Exception as e:                     # soundfile raises RuntimeError / LibsndfileError
                        row["status"] = f"unreadable ({' '.join(str(e).split())})"
                files.append(row)
                if row["rate"]:
                    groups.setdefault(f["what"].split(" ")[0] if f["what"].startswith("transfer[") else "main", set()).add(row["rate"])
            if any(r["status"] == "missing" for r in files):
                status, err = "missing", "missing: " + ", ".join(r["path"] for r in files if r["status"] == "missing")
            elif any(r["status"] != "ok" for r in files):
                status, err = "error", "; ".join(f"{r['path']}: {r['status']}" for r in files if r["status"] != "ok")
            elif any(len(v) > 1 for v in groups.values()):
                bad = [k for k, v in groups.items() if len(v) > 1]
                status, err = "error", f"sample rates differ within a pair ({', '.join(bad)})"
        out[c["id"]] = {"status": status, "error": err, "files": files}
    return out


def format_preflight(cases: list[dict], pre: dict) -> str:
    rows = ["| case | file | status | rate | length | confirmed |", "|---|---|---|---|---|---|"]
    for c in cases:
        for f in pre[c["id"]]["files"]:
            rows.append(f"| {c['id']} | {f['path']} ({f['what']}) | {f['status']} | {f['rate'] or ''} | "
                        f"{'' if f['seconds'] is None else format(f['seconds'], '.1f') + ' s'} | {'yes' if c['confirmed'] else 'NO (name inferred)'} |")
        if c["kind"] == "pathcheck":
            rows.append(f"| {c['id']} | (scored from {c['reference']['parent']}) | ok | | | yes |")
    return "\n".join(rows)


# ---- loading -------------------------------------------------------------------------------------------------------
def _read(root: Path, rel: str) -> tuple[np.ndarray, int]:
    x, fs = sf.read(str(root / rel), dtype="float64", always_2d=True)
    return x, int(fs)


def load_signal(root: Path, f: dict) -> np.ndarray:
    """One file's channel (left = first column as ``run_match`` reads the DI, right, or the channel mean) at 48 kHz."""
    x, fs = _read(root, f["path"])
    ch = f["channel"]
    m = x.mean(axis=1) if ch == "mono" else x[:, min(CH_COL[ch], x.shape[1] - 1)]
    return to48(m, fs)


def load_reference_signal(root: Path, ref: dict) -> dict:
    """``{"sig": 48 kHz mono, "tracks": {"a","b"} isolated (blend only), "gains", "report"}``. A blend is ``refsum.blend_refs`` of the
    two tracks (mono channel mean, truncated to the shorter, faders ``gainDb``, the manifest's polarity)."""
    if "tracks" not in ref:
        return {"sig": load_signal(root, ref), "tracks": None, "gains": None, "report": None}
    tr = {t["role"]: t for t in ref["tracks"]}
    (a, fa), (b, fb) = RS.read_mono(root / tr["a"]["path"]), RS.read_mono(root / tr["b"]["path"])
    if fa != fb:
        raise CaseError(f"sample rates of the blend tracks differ ({fa} vs {fb} Hz)")
    gains = (tr["a"]["gainDb"], tr["b"]["gainDb"])
    s, rep = RS.blend_refs(a, b, fa, gains[0], gains[1], ref["polarity"])
    n = min(len(a), len(b))
    keep = ("gainsDb", "lagMs", "corrPolarity", "corr", "refRatioDb", "lufsA", "lufsB", "samples", "sampleRate")
    return {"sig": to48(s, fa), "tracks": {"a": to48(a[:n], fa), "b": to48(b[:n], fb)}, "gains": gains,
            "report": {**{k: rep[k] for k in keep}, "polarity": {k: rep["polarity"][k] for k in ("mode", "chosen", "lowBandDbAsis", "lowBandDbInvert")}}}


def _write(path: Path, x: np.ndarray) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), np.asarray(x, dtype=np.float32), RATE, subtype="FLOAT")
    return path


def _sec_samples(s) -> tuple[int, int]:
    return int(round(s[0] * RATE)), int(round(s[1] * RATE))


ACCEPT_RATIO = 2.0         # offset.refine_offset's own acceptance ratio for a peak


def final_offset(res: dict) -> tuple[int, str]:
    """The DI offset (48 kHz samples, relative to the cropped pair) the run settled on and where it came from. The run's final
    full-length refinement is reporting only (it has no acceptance rule), so it is used here only when its peak clears the same
    ratio the run applies to its starter-render refinement (``refine_offset``: waveform peak accepted, or envelope ratio >= 2);
    otherwise the starter-render offset under the same rule; otherwise 0, the coarse offset the fit ran with."""
    orf = res.get("offsetRefinement") or {}
    fin = ((orf.get("final") or {}).get("L")) or {}
    if "offsetSamples" in fin and (fin.get("fineAccepted") or float(fin.get("envPeakRatio", 0.0)) >= ACCEPT_RATIO):
        return int(round(fin["offsetSamples"] * RATE / fin.get("rate", RATE))), "run final refinement"
    st = orf.get("excerptStarterRender") or {}
    if "offset" in st and float(st.get("envPeakRatio", 0.0)) >= ACCEPT_RATIO:
        return int(st["offset"]), "run starter-render refinement"
    return 0, "coarse (no refinement above the acceptance ratio)"


# ---- one case ------------------------------------------------------------------------------------------------------
class Ctx:
    def __init__(self, root, out, pool, plan, quick, seed, threads, ir_library, ir_dirs, excerpt_s, log, scorer):
        self.root, self.out, self.pool, self.plan, self.quick, self.seed, self.threads = root, out, pool, plan, quick, seed, threads
        self.ir_library, self.ir_dirs, self.excerpt_s, self.log, self.scorer = ir_library, ir_dirs, excerpt_s, log, scorer


def _match(ctx: Ctx, case: dict, di_path: Path, ref, cdir: Path) -> dict:
    cfg = Config(di=di_path, ref=ref, pool=ctx.pool, out=cdir / "match", seed=ctx.seed, excerpt_s=ctx.excerpt_s, threads=ctx.threads,
                 plan=ctx.plan, quick=ctx.quick, write_audio=False, refine_offsets=True, topology=case["topology"],
                 ir_library=ctx.ir_library, ir_dirs=tuple(ctx.ir_dirs))
    return run_match(cfg, lambda m: ctx.log(f"[{case['id']}] {m}"))


def _chosen(res: dict) -> dict:
    b = res["best"]
    return {"topology": b["topology"], "captures": b["captures"], "irMix": b.get("irMix"), "tightBoost": b.get("tightBoost"),
            "blend": b.get("blend"), "outputGainDb": b.get("outputGainDb")}


def _window(h: tuple[int, int], off: int, n_di: int, n_ref: int, n_y: int) -> tuple[int, int]:
    lo, hi = max(h[0], -off), min(h[1], n_ref - off, n_di, n_y)
    if hi - lo < SEC.MIN_SECTION_S * RATE / 2:
        raise CaseError(f"the held-out window {h[0] / RATE:.1f}-{h[1] / RATE:.1f} s has only {max(hi - lo, 0) / RATE:.2f} s inside "
                        "the DI-reference overlap")
    return lo, hi


def _held_numbers(ctx: Ctx, y: np.ndarray, di: np.ndarray, ref: np.ndarray, lo: int, hi: int, off: int, ref_path: Path,
                  profile: dict | None) -> dict:
    _write(ref_path, ref[lo + off:hi + off])
    return ctx.scorer.measure(y[lo:hi], di[lo:hi], ref_path, profile)


def _run_tier1(ctx: Ctx, case: dict, children: list[dict]) -> dict:
    cid, cdir = case["id"], ctx.out / case["id"]
    di = load_signal(ctx.root, case["di"])
    rd = load_reference_signal(ctx.root, case["reference"])
    ref = rd["sig"]
    rec: dict = {"referenceReport": rd["report"]}
    # offset: the manifest's, or the matcher's search
    if case["offsetMs"] is None:
        info = resolve_offset(di, ref.astype(np.float64), RATE, False, 0)
        off0, osrc = int(info["offset_samples"]), f"searched ({info['mode']})"
    else:
        off0, osrc = int(round(case["offsetMs"] * RATE / 1000.0)), "manifest"
    lo0, hi0 = max(0, -off0), min(len(di), len(ref) - off0)
    secs = SEC.resolve(di, RATE, case["fit"], case["heldOut"], case["heldOutMaxS"], lo0, hi0)
    fa, fb = _sec_samples(secs["fit"])
    if fa < lo0 or fb > hi0:
        raise CaseError(f"fit section {secs['fit']} lies outside the DI-reference overlap {lo0 / RATE:.1f}-{hi0 / RATE:.1f} s")
    rec["sections"] = {**secs, "timeBase": "di"}
    di_fit = _write(cdir / "fit" / "di.wav", di[fa:fb])
    _write(cdir / "fit" / "ref.wav", ref[fa + off0:fb + off0])
    mref = load_reference(cdir / "fit" / "ref.wav", channel="auto", matched="mono", offset_ms=0.0)
    res = _match(ctx, case, di_fit, mref, cdir)
    d, dsrc = final_offset(res)
    off = off0 + d
    rec["offset"] = {"manifestMs": case["offsetMs"], "source": osrc, "coarseSamples48": off0,
                     "refinementSamples48": d, "refinementSource": dsrc, "finalSamples48": off,
                     "finalMs": 1000.0 * off / RATE}
    rec["chosen"] = _chosen(res)
    rec["fitAWeightedErrorDb"] = res["after"][0]["aWeightedErrorDb"]
    preset = json.loads((cdir / "match" / res["best"]["preset"]).read_text())
    profile = load_profile(str(cdir / "match" / "profile.derived.json"))
    eng = Engine(None, 1)
    try:
        y, rep_full = eng.render(preset, di, float(RATE))
        h = _sec_samples(secs["heldOut"])
        lo, hi = _window(h, off, len(di), len(ref), len(y))
        rec["heldOutEffectiveS"] = [lo / RATE, hi / RATE]
        lis = cdir.parent / "listen" / cid
        rec["heldOut"] = _held_numbers(ctx, y, di, ref, lo, hi, off, lis / "ref.wav", profile)
        rec["listen"] = {**SC.write_listen(lis, ref[lo + off:hi + off], y[lo:hi]), "dir": f"listen/{cid}"}
        rec["pathchecks"] = _pathchecks(ctx, case, children, cdir, eng, preset, rep_full, rd, di, ref, lo, hi, off, profile)
        rec["transfer"] = [_transfer(ctx, case, k, t, eng, preset, profile, cdir) for k, t in enumerate(case["transfer"])]
    finally:
        eng.close()
    return rec


def _pathchecks(ctx, case, children, cdir, eng, preset, rep_full, rd, di, ref, lo, hi, off, profile):
    if case["kind"] != "blend":
        return None
    st = path_state(preset)
    if not (st["a"] and st["b"]):
        return {"note": "single-path winner: no path checks"}
    lm = rep_full.get("levelMatch") or {}
    trims = {"a": float(lm.get("trimADb") or 0.0), "b": float(lm.get("trimBDb") or 0.0)}
    out: dict = {}
    ys = {}
    for k in ("a", "b"):
        ys[k], _ = eng.render(single_preset(preset, k, trims), di, float(RATE))
        iso = rd["tracks"][k]
        out[k] = _held_numbers(ctx, ys[k], di, iso, lo, hi, off, cdir / "heldout" / f"{k}.wav", profile)
        out[k]["ref"] = k
        child = next((c["id"] for c in children if c["reference"]["path"] == k), None)
        if child:
            out[k]["case"] = child
    a0, b0 = max(0, -off), min(len(di), len(ref) - off, len(ys["a"]))
    ca, cb = SC.lufs(ys["a"][a0:b0]), SC.lufs(ys["b"][a0:b0])
    ga, gb = rd["gains"]
    ra, rb = SC.lufs(rd["tracks"]["a"][a0 + off:b0 + off] * 10 ** (ga / 20)), SC.lufs(rd["tracks"]["b"][a0 + off:b0 + off] * 10 ** (gb / 20))
    fin = np.isfinite
    chosen, refd = (ca - cb if fin(ca) and fin(cb) else None), (ra - rb if fin(ra) and fin(rb) else None)
    out["ratio"] = {"chosenDb": chosen, "refDb": refd, "diffDb": None if chosen is None or refd is None else chosen - refd,
                    "busCompEnabled": bool((preset.get("busComp") or {}).get("enabled")),
                    "definition": "LUFS(A alone) - LUFS(B alone) on the DI-reference overlap; reference = LUFS(track a * gA) - LUFS(track b * gB)"}
    return out


def _transfer(ctx, case, k, t, eng, preset, profile, cdir) -> dict:
    """The winning preset on another take of the same song: held-out section = the automatic one of that pair."""
    try:
        di = load_signal(ctx.root, t["di"])
        rd = load_reference_signal(ctx.root, t["reference"])
        ref = rd["sig"]
        y, _ = eng.render(preset, di, float(RATE))
        off, osrc = search_offset(di, y, ref)
        lo0, hi0 = max(0, -off), min(len(di), len(ref) - off, len(y))
        secs = SEC.resolve(di, RATE, None, None, case["heldOutMaxS"], lo0, hi0)
        lo, hi = _window(_sec_samples(secs["heldOut"]), off, len(di), len(ref), len(y))
        num = _held_numbers(ctx, y, di, ref, lo, hi, off, cdir / f"transfer{k}" / "ref.wav", profile)
        return {"di": t["di"]["path"], "status": "ok", "offset": {"samples48": off, "ms": 1000.0 * off / RATE, "source": osrc},
                "heldOutS": [lo / RATE, hi / RATE], "heldOut": num}
    except Exception as e:                      # one failing transfer pair must not lose the case
        return {"di": t["di"]["path"], "status": "error", "error": f"{type(e).__name__}: {' '.join(str(e).split())}"}


def _run_tier2(ctx: Ctx, case: dict) -> dict:
    cid, cdir = case["id"], ctx.out / case["id"]
    di = load_signal(ctx.root, case["di"])
    ref = load_reference_signal(ctx.root, case["reference"])["sig"]
    secs = SEC.resolve(ref, RATE, case["fit"], case["heldOut"], case["heldOutMaxS"])
    fa, fb = _sec_samples(secs["fit"])
    _write(cdir / "fit" / "ref.wav", ref[fa:fb])
    di_path = _write(cdir / "fit" / "di.wav", di)
    uref = load_reference(cdir / "fit" / "ref.wav", channel="auto")
    res = _match(ctx, case, di_path, uref, cdir)
    preset = json.loads((cdir / "match" / res["best"]["preset"]).read_text())
    eng = Engine(None, 1)
    try:
        y, _ = eng.render(preset, di, float(RATE))
    finally:
        eng.close()
    a, b, _info = select_excerpt(y, RATE, case["heldOutMaxS"]) if len(y) > case["heldOutMaxS"] * RATE else (0, len(y), None)
    ha, hb = _sec_samples(secs["heldOut"])
    rref = _write(cdir.parent / "listen" / cid / "ref.wav", ref[ha:hb])
    rec = {"sections": {**secs, "timeBase": "reference", "standInDiSectionS": [a / RATE, b / RATE]},
           "offset": {"source": "n/a (unmatched reference)"}, "chosen": _chosen(res),
           "fitAWeightedErrorDb": res["after"][0]["aWeightedErrorDb"], "pathchecks": None, "transfer": [],
           "heldOut": ctx.scorer.measure_unmatched(y[a:b], rref)}
    rec["listen"] = {**SC.write_listen(cdir.parent / "listen" / cid, ref[ha:hb], y[a:b]), "dir": f"listen/{cid}"}
    return rec


def _child_record(case: dict, parent: dict | None) -> dict:
    p, k = case["reference"]["parent"], case["reference"]["path"]
    if parent is None or parent.get("status") != "ok":
        return {"status": "skipped", "error": f"parent case {p} did not run"}
    pc = parent.get("pathchecks") or {}
    if k not in pc:
        return {"status": "error", "error": (pc.get("note") or "the parent has no path checks") + f" (parent {p})"}
    held = {x: v for x, v in pc[k].items() if x not in ("case", "ref")}
    return {"status": "ok", "parent": p, "path": k, "sections": parent["sections"], "offset": parent["offset"],
            "chosen": parent["chosen"], "fitAWeightedErrorDb": None, "heldOut": held, "pathchecks": None, "transfer": [],
            "listen": None}


# ---- the run -------------------------------------------------------------------------------------------------------
def git_sha() -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(Path(__file__).resolve().parent), "rev-parse", "HEAD"], capture_output=True, text=True, timeout=10)
        return r.stdout.strip() or None if r.returncode == 0 else None
    except Exception:
        return None


def run_bench(cases: list[dict], root: str | Path, out: str | Path, *, pool, plan=None, quick: bool = False, seed: int = 0,
              threads: int = 4, ir_library=None, log=None, excerpt_s: float = 6.0, ir_dirs=(), manifest_path: str | Path | None = None,
              args: dict | None = None) -> dict:
    """Run the matcher on every case's fit section and score its held-out section; writes ``out/scores.json`` and ``out/scores.md``
    and returns the scores dict. ``cases``: validated manifest cases (``manifest.load_manifest``). ``plan``/``excerpt_s`` are for
    tests (a small work plan); the CLI leaves them at the matcher's defaults."""
    t_all = time.time()
    root, out = Path(root), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    log = log or (lambda m: None)
    pre = preflight(cases, root)
    ctx = Ctx(root, out, pool, plan, quick, seed, threads, ir_library, ir_dirs, excerpt_s, log, SC.Scorer())
    results: dict[str, dict] = {c["id"]: {} for c in cases}
    times: dict[str, float] = {}
    by_id = {c["id"]: c for c in cases}
    order = [c for c in cases if c["kind"] != "pathcheck"] + [c for c in cases if c["kind"] == "pathcheck"]
    for c in order:
        cid, t0 = c["id"], time.time()
        base = {"tier": c["tier"], "kind": c["kind"], "style": c["style"], "counts": c["counts"], "confirmed": c["confirmed"]}
        try:
            if c["kind"] == "pathcheck":
                rec = _child_record(c, results.get(c["reference"]["parent"]) if c["reference"]["parent"] in by_id else None)
            elif pre[cid]["status"] != "ok":
                rec = {"status": pre[cid]["status"], "error": pre[cid]["error"]}
            else:
                log(f"[{cid}] start ({c['kind']}, tier {c['tier']})")
                kids = [k for k in cases if k["kind"] == "pathcheck" and k["reference"]["parent"] == cid]
                rec = {"status": "ok", **(_run_tier1(ctx, c, kids) if c["tier"] == 1 else _run_tier2(ctx, c))}
        except Exception as e:
            log(f"[{cid}] ERROR {type(e).__name__}: {e}\n{traceback.format_exc()}")
            rec = {"status": "error", "error": f"{type(e).__name__}: {' '.join(str(e).split())}"}
        times[cid] = time.time() - t0
        results[cid] = {**base, **rec, "runtimeS": times[cid]}
    scores = {"schema": SCHEMA, "version": VERSION,
              "manifest": None if manifest_path is None else {"path": str(manifest_path), "sha256": MF.sha256_file(manifest_path)},
              "gitSha": git_sha(), "args": args or {}, "seed": seed, "threads": threads,
              "mode": "quick" if quick else "thorough", "planOverride": plan is not None,
              "cases": results, "total": SC.total(results, cases),
              "timings": {"totalS": time.time() - t_all, "perCaseS": times}}
    (out / "scores.json").write_text(json.dumps(_finite_or_none(scores), indent=2, default=_json_default) + "\n")
    (out / "scores.md").write_text(scores_markdown(scores))
    return scores


def _f(v, fmt="{:.2f}"):
    return "-" if v is None else fmt.format(v)


def scores_markdown(s: dict) -> str:
    rows = ["| case | tier | status | topology | fit A-wt | held-out A-wt | LTAS | tight | fizz | polish | guardrail fails | transfer A-wt | s |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for cid, c in s["cases"].items():
        if c.get("status") != "ok":
            rows.append(f"| {cid} | {c.get('tier')} | {c.get('status')}: {c.get('error') or ''} |" + " |" * 10)
            continue
        h = c["heldOut"]
        fe = h.get("feel") or {}
        tr = ", ".join(_f(t["heldOut"]["aWeightedErrorDb"]) if t.get("status") == "ok" else "err" for t in c.get("transfer") or []) or "-"
        gf = (h.get("guardrails") or {}).get("fail")
        rows.append(f"| {cid}{'' if c['counts'] else ' *'} | {c['tier']} | ok | {c['chosen']['topology']} | {_f(c.get('fitAWeightedErrorDb'))} | "
                    f"{_f(h['aWeightedErrorDb'])} | {_f(h.get('ltasLossDb'))} | {_f(fe.get('tight'), '{:.3f}')} | {_f(fe.get('fizz'), '{:.3f}')} | "
                    f"{_f(fe.get('polish'), '{:.3f}')} | {'-' if gf is None else len(gf)} | {tr} | {c['runtimeS']:.0f} |")
    t = s["total"]
    return ("# Sawblade benchmark scores\n\n" + "\n".join(rows) + "\n\n`*` = not counted in the total (path check, forced variant or tier 2).\n\n"
            + f"seed {s['seed']}, threads {s['threads']}, mode {s['mode']}, git {s['gitSha']}\n\n" + SC.total_line(t) + "\n")
