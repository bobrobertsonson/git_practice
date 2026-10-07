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
from . import a2shape as A2S
from . import notes as N
from . import plan as P
from . import progress as PG
from . import resume as R
from . import signal as S
from . import stop as STOP
from . import train as T
from . import validate as V
from .audio import listening_ab
from .stop import ExportInterrupted  # noqa: F401  (re-exported for the CLI)
from .chain import RATE, fold_cab_post_eq, load_preset, probe_report, render48

REPORT_VERSION = 1
CACHE_ROOT = Path(os.environ.get("SAWBLADE_EXPORT_CACHE", "~/.cache/sawblade/export_cache")).expanduser()
DEFAULT_OUT_ROOT = Path("~/.cache/sawblade/exports").expanduser()
DI_DEFAULT = Path(__file__).resolve().parents[3] / "testdata" / "gatecreeper_cover" / "Guitar_L.wav"
DI_EXCERPT_S = 10.0


DI_BUILTIN = "builtin"
DI_FALLBACK_MSG = "DI excerpt: default test DI not found, using the built-in signal"


def resolve_di(di, log=print) -> Path | None:
    """Path of the DI wav for the excerpt validation, or ``None`` = the built-in signal's held-out segment
    (``--di builtin``, or no ``--di`` and the default test DI is missing)."""
    if di is not None and str(di) == DI_BUILTIN:
        return None
    if di is None:
        if DI_DEFAULT.is_file():
            return DI_DEFAULT
        log(DI_FALLBACK_MSG)
        return None
    return Path(di).expanduser()


def file_stem(name: str | None, preset: dict) -> str:
    """``--name`` or the preset slug; ``-nc`` is appended when any capture is non-commercially licensed."""
    stem = name or slug(preset.get("name", "preset"))
    if P.nc_captures(preset) and not stem.endswith("-nc"):
        stem += "-nc"
    return stem


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


def _cached_targets(tpreset: dict, base, spec_info: dict, tr, va, cache, log, check=None):
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
    if check:
        check()
    yv, _ = render48(tpreset, va, base, cache)
    info = {"renderSeconds": round(time.time() - t0, 1), "latencySamples": rt.get("latencySamples"),
            "align": rt.get("align"), "warnings": rt.get("warnings", []),
            "levels": {"trainOutRmsDbfs": float(10 * np.log10(np.mean(yt.astype(np.float64) ** 2) + 1e-30)),
                       "trainOutPeakDbfs": float(20 * np.log10(np.max(np.abs(yt)) + 1e-30))}}
    f.parent.mkdir(parents=True, exist_ok=True)
    np.savez(f, yt=yt, yv=yv, info=json.dumps(info))
    return yt, yv, {"cached": False, "key": key, **info}


def run_export(*args, progress_json=None, **kwargs) -> dict:
    """Run the export (parameters: see ``_run_export``).  ``progress_json``: path of the ``--progress-json`` file.
    Errors end with an ``error`` progress write, a cancel (``ExportInterrupted``) with ``cancelled``."""
    prog = PG.Progress(progress_json)
    try:
        return _run_export(prog, *args, **kwargs)
    except ExportInterrupted:
        raise
    except KeyboardInterrupt as e:
        _cancelled(prog, prog.state["outDir"] or None)
        raise ExportInterrupted("interrupted") from e
    except Exception as e:
        prog.update("error", message=str(e) or type(e).__name__)
        raise


def _cancelled(prog: PG.Progress, outdir=None) -> None:
    ck = R.ckpt_dir(outdir) if outdir else None
    prog.update("cancelled", message="cancelled", resumable=bool(ck and (ck / R.LAST).is_file()))


def _check_stop(prog: PG.Progress, outdir=None) -> None:
    if STOP.stop_requested():
        _cancelled(prog, outdir)
        raise ExportInterrupted("interrupted")


def _run_export(prog: PG.Progress, preset_path, mode: str = "nocab", size: str | None = None, out=None, name: str | None = None,
               allow_inexact: bool = False, epochs: int | None = None, max_minutes: float | None = None, seed: int = 0,
               threads: int = 4, di=None, validate: bool = True, signal_seed: int = 1, target_esr: float | None = None,
               keep_scratch: bool = False, log=print, signal_spec: S.SignalSpec | None = None,
               lr_gamma: float | None = None, batch_size: int = T.BATCH, device: str = "auto",
               resume: str | None = None, exports_root=None, arch: str = "a1") -> dict:
    """``arch`` ``"a2"`` (the CLI default) trains the packed A2 net and writes the container plus standalone ``a2_full`` /
    ``a2_lite`` files (``size`` = which standalone file is the primary one: ``full`` default / ``lite``); ``"a1"`` (the
    library default, unchanged behaviour) trains one A1 WaveNet (``size`` feather / lite / standard).  A bad
    arch / size pair raises ``ValueError``.

    ``resume``: None (fresh), ``"auto"`` (newest matching unfinished run under ``exports_root`` / the default
    exports dir, else fresh) or the output directory of an unfinished run (refused when preset, signal, mode, size or
    training settings differ)."""
    t_all = time.time()
    size = T.check_arch_size(arch, size)
    a2 = arch == "a2"
    prog.update("plan", message="planning", arch=arch)
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

    pname = file_stem(name, preset)
    dtag = f"a2-{size}" if a2 else size                      # output directory tag
    stem = f"{pname}-{mode}-{size}"                          # file name stem (a2: <stem>.a2.nam / .a2_full.nam / .a2_lite.nam)
    stamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    out_root = Path(exports_root).expanduser() if exports_root else DEFAULT_OUT_ROOT
    if out:
        prog.update(out_dir=Path(out).expanduser().resolve())
    elif resume is None:
        prog.update(out_dir=(out_root / f"{pname}-{mode}-{dtag}-{stamp}").resolve())
    elif resume != "auto":
        prog.update(out_dir=Path(resume).expanduser().resolve())
    _check_stop(prog)

    prog.update("signal", message="training signal")
    tr, va, sinfo = _cached_signal(signal_spec or S.SignalSpec(seed=signal_seed), log)

    cfg = T.TrainConfig(size=size, arch=arch, epochs=epochs, max_minutes=max_minutes, seed=seed, threads=threads,
                        target_esr=target_esr, lr_gamma=lr_gamma, batch_size=batch_size, device=device)
    rc = cfg.resolved()
    identity = {"presetSha256": P.preset_hash(preset), "signalSha256": sinfo["trainSha256"],
                "validSha256": sinfo["validSha256"], "mode": mode, "size": size, "arch": arch,
                "layout": T.layout_of(arch)}
    run_config = {"seed": seed, "batchSize": batch_size, "epochs": rc.epochs, "lrGamma": rc.lr_gamma}
    resumed_from = None
    resume_dir = None
    if resume == "auto":
        root = out_root
        found, notes = R.find_auto(root, identity, run_config)
        for n in notes:
            log(f"  resume auto: {n}")
        if found is None:
            log(f"resume auto: no matching unfinished run in {root}; starting fresh")
        else:
            resume_dir = found
            log(f"resume auto: resuming {found}")
    elif resume:
        resume_dir = Path(resume).expanduser()
        if out and Path(out).expanduser().resolve() != resume_dir.resolve():
            raise P.ExportRefused(f"--resume {resume_dir} and --out {out} differ: a resumed run continues in its own directory")
    if resume_dir is not None:
        resume_prog = R.validate_resume_dir(resume_dir, identity, run_config)     # raises ExportRefused
        resumed_from = {"dir": str(resume_dir), "epoch": resume_prog.get("epoch"),
                        "elapsedTrainingS": resume_prog.get("elapsedTrainingS")}
        outdir = resume_dir
    else:
        outdir = Path(out).expanduser() if out else out_root / f"{pname}-{mode}-{dtag}-{stamp}"
    outdir.mkdir(parents=True, exist_ok=True)
    prog.update(out_dir=outdir.resolve())
    _check_stop(prog, outdir)
    scratch = outdir / "_scratch"
    log(f"export {arch}/{mode}/{size} -> {outdir}")
    for b in plan.bypassed:
        log(f"  bypassed: {b['what']} ({b['why']})")
    for line in P.level_match_lines(probe):
        log(f"  {line}")

    prog.update("render", message="rendering the training target")
    with prog.heartbeat("render"):               # the core render reports nothing: keep the progress file alive
        yt, yv, tinfo = _cached_targets(tpreset, base, sinfo, tr, va, cache, log,
                                        check=lambda: _check_stop(prog, outdir))

    report: dict = {"reportVersion": REPORT_VERSION, "tool": "sawblade-export", "created": stamp,
                    "preset": {"path": str(Path(preset_path).resolve()), "name": preset.get("name"),
                               "sha256": P.preset_hash(preset)},
                    "mode": mode, "arch": arch, "size": size, "seed": seed, "signalSeed": signal_seed,
                    "plan": plan.to_json(), "signal": sinfo, "target": tinfo, "coreBuild": _core_id(),
                    "levelMatch": P.level_match_info(probe),
                    "attribution": P.attribution(preset), "licenceNote": P.licence_note(preset), "nonCommercial": bool(P.nc_captures(preset))}

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
    _check_stop(prog, outdir)

    ref_for_io = {"inputRmsDbfs": sinfo["train"]["rmsDbfs"], "inputPeakDbfs": sinfo["train"]["peakDbfs"],
                  "outputRmsDbfs": tinfo["levels"]["trainOutRmsDbfs"], "outputPeakDbfs": tinfo["levels"]["trainOutPeakDbfs"],
                  "inputLevelDbu": None, "outputLevelDbu": None,
                  "note": "digital chain without an analog reference: input_level_dbu / output_level_dbu are left empty"}
    ir_file = ir_info and ir_info.get("file")
    sawblade_meta = P.sawblade_block(preset, plan, size, seed, signal_seed, sinfo["trainSha256"], ref_for_io, ir_file,
                                     arch="a2" if a2 else None)

    T.import_nam()
    from nam.models.metadata import GearType, ToneType, UserMetadata
    um = UserMetadata(name=f"{preset.get('name', 'Sawblade')} ({mode}, {'A2' if a2 else size})", modeled_by="Sawblade",
                      gear_type=GearType(P.gear_type(plan, preset)), gear_make="Sawblade",
                      gear_model=str(preset.get("name", "")), tone_type=ToneType.HI_GAIN)
    prog.update("train", message="training")
    tres = T.train_nam(tr, yt, va, yv, cfg, outdir, scratch, user_metadata=um,
                       other_metadata={"sawblade": sawblade_meta}, log=log, basename=stem,
                       ckpt_dir=R.ckpt_dir(outdir), resume=resumed_from is not None, identity=identity, progress=prog)
    if tres.stopped_by == "interrupt":
        log(f"interrupted after {tres.epochs_done} epochs; resume with --resume {outdir}")
        _cancelled(prog, outdir)            # no report is written on cancel; the checkpoint stays
        raise ExportInterrupted("interrupted")
    _check_stop(prog, outdir)               # a SIGINT the trainer did not consume (final epoch, ...)
    log(f"trained in {tres.wall_s / 60:.1f} min, {tres.epochs_done} epochs, best val ESR {tres.best_val_esr:.5f} "
        f"(epoch {tres.best_epoch}), stopped by {tres.stopped_by}")
    report["resume"] = resumed_from
    report["training"] = {"namFile": tres.nam_path.name, "epochsDone": tres.epochs_done, "bestEpoch": tres.best_epoch,
                          "validationEsr": tres.best_val_esr, "wallSeconds": round(tres.wall_s, 1),
                          "stoppedBy": tres.stopped_by, "parameters": tres.params,
                          "receptiveField": tres.receptive_field, "config": tres.config, "history": tres.history,
                          "a2Export": ("A2: packed WaveNet (PackedWaveNet + export_container): container plus standalone "
                                       "Full (8 ch) and Lite (3 ch) files; per-submodel best epochs and ESR_packed_i"
                                       if a2 else "not used (--arch a1)")}
    # files (names relative to the output directory): a1 = {primary}; a2 = {primary, container, full, lite}
    paths = {k: Path(v) for k, v in tres.files.items()} if a2 else {"primary": tres.nam_path}
    if a2:
        paths = {"primary": paths[size], **paths}
        report["training"]["submodels"] = tres.submodels
        report["training"]["parametersContainer"] = tres.params
        # the pinned core's A2 fast path (port of is_a2_shape): the standalone files should take it
        report["a2FastPath"] = {sz: A2S.nam_file_fast_path(json.loads(paths[sz].read_text())) is not None for sz in T.A2_SIZES}
    report["arch"] = arch
    report["files"] = {k: v.name for k, v in paths.items()}
    sizes = list(T.A2_SIZES) if a2 else [size]
    models = {sz: paths[sz] for sz in sizes} if a2 else {size: tres.nam_path}
    if validate:
        prog.update("validate", message="validating", epoch=tres.epochs_done)
        if a2:
            ref_cache: dict = {}
            vals = {}
            for sz in sizes:             # primary first; both standalone files go through sawblade_core
                vals[sz] = _validate(preset, base, plan, models[sz], ir_path, outdir, scratch, sz, cache, va,
                                     resolve_di(di, log), log, prog, arch=arch, tag=sz, ref_cache=ref_cache,
                                     listen_name="ab_original_then_export" + ("" if sz == size else f"_{sz}"))
            report["validation"] = {sz: vals[sz] for sz in sizes}
        else:
            report["validation"] = _validate(preset, base, plan, tres.nam_path, ir_path, outdir, scratch, size, cache,
                                             va, resolve_di(di, log), log, prog)
    lic = P.licence_note(preset)
    for fk, fpath in ({k: v for k, v in paths.items() if k != "primary"} if a2 else {"a1": tres.nam_path}).items():
        nam = json.loads(fpath.read_text())
        meta = nam["metadata"]
        which = fk if a2 else size                  # a2: container | full | lite
        vsz = None if which == "container" else which
        best = {"container": "full"}.get(which, which) if a2 else None
        esr_b = tres.submodels[best]["bestValEsr"] if a2 else tres.best_val_esr
        if a2:
            label = {"container": "A2 container", "full": "A2 Full", "lite": "A2 Lite"}[which]
            meta["name"] = f"{preset.get('name', 'Sawblade')} ({mode}, {label})"
            meta["sawblade"] = {**meta["sawblade"], "size": which}
        meta.setdefault("training", {})["validation_esr"] = esr_b
        meta["training"]["validation_esr_source"] = ("trainer best validation ESR on level-normalised held-out data "
                                                     "(model output only: before the IR for nocab)" +
                                                     ("; the container carries the Full submodel's" if which == "container" else ""))
        if validate:
            v = report["validation"][vsz or "full"] if a2 else report["validation"]
            meta["sawblade"]["validation"] = {"heldOutEsr": v["heldOut"]["esr"], "diExcerptEsr": v["diExcerpt"]["esr"],
                                              "diLtasDb": v["diExcerpt"]["ltas"]["aWeightedErrorDb"],
                                              "acceptanceStatus": v["acceptance"]["status"],
                                              "note": "model + exported IR rendered through sawblade_core vs the original chain"
                                                      + ("; container: the Full submodel's numbers (the core plays the last submodel)" if which == "container" else "")}
        fpath.write_text(json.dumps(nam))
    if a2:
        report["exportNotes"], notes_path = N.write_export_notes(preset, plan, paths["primary"], ir_path, lic, stem=stem,
                                                                 model_label={"full": "A2 Full", "lite": "A2 Lite"}[size])
    else:
        report["exportNotes"], notes_path = N.write_export_notes(preset, plan, tres.nam_path, ir_path, lic)
    log(f"export notes: {notes_path}")
    report["totalWallSeconds"] = round(time.time() - t_all, 1)
    (outdir / "export_report.json").write_text(json.dumps(report, indent=2, default=float))
    if keep_scratch:
        ck_prog = R.read_progress(R.ckpt_dir(outdir))
        if ck_prog is not None:
            R.write_progress(R.ckpt_dir(outdir), {**ck_prog, "complete": True})     # kept, but never auto-resumed
    else:
        shutil.rmtree(scratch, ignore_errors=True)
        shutil.rmtree(R.ckpt_dir(outdir), ignore_errors=True)
    log(f"report: {outdir / 'export_report.json'}")
    val = report.get("validation")
    status = (val[size] if a2 else val)["acceptance"]["status"] if val else "done"
    prog.update("done", message=status, out_dir=outdir.resolve(), resumable=False, epoch=tres.epochs_done)
    return report


def _validate(preset, base, plan, nam_path, ir_path, outdir, scratch, size, cache, va_, di_path, log,
              prog: PG.Progress | None = None, arch: str = "a1", tag: str | None = None, ref_cache: dict | None = None,
              listen_name: str = "ab_original_then_export") -> dict:
    """``di_path`` None = excerpt of the built-in held-out signal ``va_`` (report file ``"builtin"``).  ``tag`` (a2:
    ``full`` / ``lite``) separates the renders of the two standalone files; ``ref_cache`` shares the reference renders."""
    prog = prog or PG.Progress()
    scratch.mkdir(parents=True, exist_ok=True)
    targets = load_targets(V.targets_path())
    ref_preset = P.reference_preset(preset, plan)
    check = V.export_check_preset(nam_path, ir_path, scratch)
    rdir = outdir / "validation_renders" / (tag or "")
    out: dict = {"reference": "original chain with the gate bypassed" +
                 (" (bus comp left on)" if plan.inexact else "")}
    log("validating on the held-out segment ...")
    ho, _, _ = V.compare_signals("heldout", va_, RATE, ref_preset, base, check, cache, targets, renders_dir=rdir,
                                 drop=int(1.0 * RATE), ref_cache=ref_cache)
    out["heldOut"] = ho
    _check_stop(prog, outdir)
    prog.update("validate", PG.stage_fraction("validate", 0.4), message="held-out segment done")
    out["metricsSkipFirstS"] = 1.0
    if di_path is None:
        di, fs, src = np.asarray(va_, np.float32), RATE, DI_BUILTIN
    else:
        di, fs = sf.read(str(di_path), dtype="float32")
        src = str(di_path)
        if di.ndim > 1:
            di = di[:, 0]
    a, b, info = select_excerpt(di, fs, DI_EXCERPT_S + V.PREROLL_S)
    log(f"validating on the DI excerpt ({info.get('startS', 0):.1f}-{info.get('endS', 0):.1f} s of {Path(src).name}) ...")
    orig_with_gate = {"vsOriginalWithGate": preset} if (preset.get("gate") or {}).get("enabled") else None
    _check_stop(prog, outdir)
    dres, ref_o, out_e = V.compare_signals("di_excerpt", di[a:b], fs, ref_preset, base, check, cache, targets,
                                           extra_refs=orig_with_gate, drop=int(V.PREROLL_S * RATE), renders_dir=rdir,
                                           ref_cache=ref_cache)
    dres["excerpt"] = {"file": src, "startS": a / fs, "endS": b / fs, "prerollDroppedS": V.PREROLL_S, **info}
    out["diExcerpt"] = dres
    _check_stop(prog, outdir)
    prog.update("validate", PG.stage_fraction("validate", 0.8), message="DI excerpt done")
    out["listening"] = listening_ab(ref_o, out_e, outdir / "listen", name=listen_name)
    out["acceptance"] = V.acceptance(size, ho["esr"], dres["ltas"]["aWeightedErrorDb"], arch)
    out["exportedModelLoadsInCore"] = True
    log(f"  held-out ESR {ho['esr']:.5f}; DI excerpt ESR {dres['esr']:.5f}, LTAS {dres['ltas']['aWeightedErrorDb']:.3f} dB")
    return out
