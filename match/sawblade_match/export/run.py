"""End-to-end export: plan -> training signal -> target render (core) -> [IR fold] -> NAM training -> validation."""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import re
import shutil
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from ..core import CaptureCache
from ..core import _core as _core_mod
from ..matcher.excerpt import select_excerpt
from ..tonecheck.rules import load_targets
from . import plan as P
from . import signal as S
from . import train as T
from . import validate as V
from .audio import listening_ab
from .chain import RATE, fold_cab_post_eq, load_preset, probe_report, render48

REPORT_VERSION = 1
CACHE_ROOT = Path(os.environ.get("SAWBLADE_EXPORT_CACHE", "~/.cache/sawblade/export_cache")).expanduser()
DEFAULT_OUT_ROOT = Path("~/.cache/sawblade/exports").expanduser()
DI_DEFAULT = Path(__file__).resolve().parents[3] / "testdata" / "gatecreeper_cover" / "Guitar_L.wav"
DI_EXCERPT_S = 10.0


def slug(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-") or "preset"


def _core_id() -> str:
    f = Path(_core_mod.__file__)
    st = f.stat()
    return f"{f.name}:{st.st_size}:{int(st.st_mtime)}"


def _cached_signal(spec: S.SignalSpec, log):
    key = hashlib.sha256(json.dumps(spec.to_json(), sort_keys=True).encode()).hexdigest()[:16]
    f = CACHE_ROOT / "signals" / f"{key}.npz"
    if f.exists():
        d = np.load(f, allow_pickle=False)
        return d["train"], d["valid"], json.loads(str(d["info"]))
    log("generating the training signal ...")
    tr, va, info = S.generate(spec)
    f.parent.mkdir(parents=True, exist_ok=True)
    np.savez(f, train=tr, valid=va, info=json.dumps(info))
    return tr, va, info


def _cached_targets(tpreset: dict, base, spec_info: dict, tr, va, cache, log):
    key = hashlib.sha256((P.preset_hash(tpreset) + spec_info["trainSha256"] + spec_info["validSha256"] +
                          _core_id()).encode()).hexdigest()[:20]
    f = CACHE_ROOT / "targets" / f"{key}.npz"
    if f.exists():
        d = np.load(f, allow_pickle=False)
        log(f"training targets: cache hit ({f.name})")
        return d["yt"], d["yv"], {"cached": True, "key": key, **json.loads(str(d["info"]))}
    t0 = time.time()
    log(f"rendering the training target through the chain ({len(tr) / RATE:.0f} s + {len(va) / RATE:.0f} s) ...")
    yt, rt = render48(tpreset, tr, base, cache)
    yv, _ = render48(tpreset, va, base, cache)
    info = {"renderSeconds": round(time.time() - t0, 1), "latencySamples": rt.get("latencySamples"),
            "align": rt.get("align"), "warnings": rt.get("warnings", []),
            "levels": {"trainOutRmsDbfs": float(10 * np.log10(np.mean(yt.astype(np.float64) ** 2) + 1e-30)),
                       "trainOutPeakDbfs": float(20 * np.log10(np.max(np.abs(yt)) + 1e-30))}}
    f.parent.mkdir(parents=True, exist_ok=True)
    np.savez(f, yt=yt, yv=yv, info=json.dumps(info))
    return yt, yv, {"cached": False, "key": key, **info}


def run_export(preset_path, mode: str = "nocab", size: str = "standard", out=None, name: str | None = None,
               allow_inexact: bool = False, epochs: int | None = None, max_minutes: float | None = None, seed: int = 0,
               threads: int = 4, di=None, validate: bool = True, signal_seed: int = 1, target_esr: float | None = None,
               keep_scratch: bool = False, log=print, signal_spec: S.SignalSpec | None = None,
               lr_gamma: float | None = None) -> dict:
    t_all = time.time()
    preset, base = load_preset(preset_path)
    plan = P.make_plan(preset, mode, allow_inexact)           # raises ExportRefused
    tpreset = P.training_preset(preset, plan)
    cache = CaptureCache()

    probe = probe_report(tpreset, base, cache)
    bad = P.core_trainability_problems(probe)
    if bad:
        raise P.ExportRefused("; ".join(bad), bad)
    for w in probe.get("warnings", []):
        plan.warnings.append(f"core: {w}")

    pname = name or slug(preset.get("name", "preset"))
    stamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    outdir = Path(out).expanduser() if out else DEFAULT_OUT_ROOT / f"{pname}-{mode}-{size}-{stamp}"
    outdir.mkdir(parents=True, exist_ok=True)
    scratch = outdir / "_scratch"
    log(f"export {mode}/{size} -> {outdir}")
    for b in plan.bypassed:
        log(f"  bypassed: {b['what']} ({b['why']})")

    tr, va, sinfo = _cached_signal(signal_spec or S.SignalSpec(seed=signal_seed), log)
    yt, yv, tinfo = _cached_targets(tpreset, base, sinfo, tr, va, cache, log)

    report: dict = {"reportVersion": REPORT_VERSION, "tool": "sawblade-export", "created": stamp,
                    "preset": {"path": str(Path(preset_path).resolve()), "name": preset.get("name"),
                               "sha256": P.preset_hash(preset)},
                    "mode": mode, "size": size, "seed": seed, "signalSeed": signal_seed,
                    "plan": plan.to_json(), "signal": sinfo, "target": tinfo, "coreBuild": _core_id(),
                    "attribution": P.attribution(preset), "licenceNote": P.LICENCE_NOTE}

    ir_info = None
    ir_path = None
    if mode == "nocab":
        ir, ir_info = fold_cab_post_eq(preset, base, cache)
        ir_path = outdir / f"{pname}-{mode}.ir.wav"
        sf.write(str(ir_path), ir, RATE, subtype="FLOAT")
        ir_info["file"] = ir_path.name
        ir_info["format"] = "mono WAV, 48 kHz, 32-bit float; contains cab IR (core-normalised) (*) post EQ; load it WITHOUT loudness normalisation"
        log(f"  IR (cab * post EQ): {ir_info['samples']} samples, peak {ir_info['peakDb']:.1f} dB")
    report["ir"] = ir_info

    ref_for_io = {"inputRmsDbfs": sinfo["train"]["rmsDbfs"], "inputPeakDbfs": sinfo["train"]["peakDbfs"],
                  "outputRmsDbfs": tinfo["levels"]["trainOutRmsDbfs"], "outputPeakDbfs": tinfo["levels"]["trainOutPeakDbfs"],
                  "inputLevelDbu": None, "outputLevelDbu": None,
                  "note": "digital chain without an analog reference: input_level_dbu / output_level_dbu are left empty"}
    sawblade_meta = P.sawblade_block(preset, plan, size, seed, signal_seed, sinfo["trainSha256"], ref_for_io,
                                     ir_info and ir_info.get("file"))

    T.import_nam()
    from nam.models.metadata import GearType, ToneType, UserMetadata
    um = UserMetadata(name=f"{preset.get('name', 'Sawblade')} ({mode}, {size})", modeled_by="Sawblade",
                      gear_type=GearType(P.gear_type(plan, preset)), gear_make="Sawblade",
                      gear_model=str(preset.get("name", "")), tone_type=ToneType.HI_GAIN)
    cfg = T.TrainConfig(size=size, epochs=epochs, max_minutes=max_minutes, seed=seed, threads=threads,
                        target_esr=target_esr, lr_gamma=lr_gamma)
    tres = T.train_nam(tr, yt, va, yv, cfg, outdir, scratch, user_metadata=um,
                       other_metadata={"sawblade": sawblade_meta}, log=log, basename=f"{pname}-{mode}-{size}")
    log(f"trained in {tres.wall_s / 60:.1f} min, {tres.epochs_done} epochs, best val ESR {tres.best_val_esr:.5f} "
        f"(epoch {tres.best_epoch}), stopped by {tres.stopped_by}")
    report["training"] = {"namFile": tres.nam_path.name, "epochsDone": tres.epochs_done, "bestEpoch": tres.best_epoch,
                          "validationEsr": tres.best_val_esr, "wallSeconds": round(tres.wall_s, 1),
                          "stoppedBy": tres.stopped_by, "parameters": tres.params,
                          "receptiveField": tres.receptive_field, "config": tres.config, "history": tres.history,
                          "a2Export": "supported by the pinned trainer (PackedWaveNet + export_container) but not "
                                      "enabled: A1 only"}
    # patch the final ESR into the .nam metadata (the trainer's own 'training' block convention)
    nam = json.loads(tres.nam_path.read_text())
    nam["metadata"].setdefault("training", {})["validation_esr"] = tres.best_val_esr
    tres.nam_path.write_text(json.dumps(nam))

    if validate:
        report["validation"] = _validate(preset, base, plan, tres.nam_path, ir_path, outdir, scratch, size, cache,
                                         va, di or DI_DEFAULT, log)
    report["totalWallSeconds"] = round(time.time() - t_all, 1)
    (outdir / "export_report.json").write_text(json.dumps(report, indent=2, default=float))
    if not keep_scratch:
        shutil.rmtree(scratch, ignore_errors=True)
    log(f"report: {outdir / 'export_report.json'}")
    return report


def _validate(preset, base, plan, nam_path, ir_path, outdir, scratch, size, cache, va_, di_path, log) -> dict:
    scratch.mkdir(parents=True, exist_ok=True)
    targets = load_targets(V.targets_path())
    ref_preset = P.reference_preset(preset, plan)
    check = V.export_check_preset(nam_path, ir_path, scratch)
    rdir = outdir / "validation_renders"
    out: dict = {"reference": "original chain with the gate bypassed" +
                 (" (bus comp left on)" if plan.inexact else "")}
    log("validating on the held-out segment ...")
    ho, _, _ = V.compare_signals("heldout", va_, RATE, ref_preset, base, check, cache, targets, renders_dir=rdir,
                                 drop=int(1.0 * RATE))
    out["heldOut"] = ho
    out["metricsSkipFirstS"] = 1.0
    di, fs = sf.read(str(di_path), dtype="float32")
    if di.ndim > 1:
        di = di[:, 0]
    a, b, info = select_excerpt(di, fs, DI_EXCERPT_S + V.PREROLL_S)
    pre = int(V.PREROLL_S * fs)
    log(f"validating on the cover DI excerpt ({info.get('startS', 0):.1f}-{info.get('endS', 0):.1f} s of {Path(di_path).name}) ...")
    orig_with_gate = {"vsOriginalWithGate": preset} if (preset.get("gate") or {}).get("enabled") else None
    dres, ref_o, out_e = V.compare_signals("di_excerpt", di[a:b], fs, ref_preset, base, check, cache, targets,
                                           extra_refs=orig_with_gate, drop=int(V.PREROLL_S * RATE), renders_dir=rdir)
    dres["excerpt"] = {"file": str(di_path), "startS": a / fs, "endS": b / fs, "prerollDroppedS": V.PREROLL_S, **info}
    out["diExcerpt"] = dres
    out["listening"] = listening_ab(ref_o, out_e, outdir / "listen")
    out["acceptance"] = V.acceptance(size, ho["esr"], dres["ltas"]["aWeightedErrorDb"])
    out["exportedModelLoadsInCore"] = True
    log(f"  held-out ESR {ho['esr']:.5f}; DI excerpt ESR {dres['esr']:.5f}, LTAS {dres['ltas']['aWeightedErrorDb']:.3f} dB")
    return out
