"""NAM training through ``neural-amp-modeler`` (pinned 0.13.0, optional extra ``match[export]``).

API path
--------
``nam.train.core.train`` (the "simplified trainer" behind the GUI/Colab) only accepts NAM's own standard input files
(it hashes the input wav against the known v1-v4 files and calibrates latency on their blips), and importing it needs
``tkinter``.  Sawblade trains on its *own* signal, so it uses the lower-level pieces the same trainer is built from:

* ``nam.data.Dataset`` built directly from arrays (no wav round trip, so no 24-bit quantisation or clipping),
  train (``ny = 8192`` samples per datum) and validation (whole held-out segment as one datum),
* ``nam.data.NormalizeJointDatasetOutput(-18 dBFS)``: the trainer normalises the output level for learning and
  registers an export hook that undoes it, so the exported ``.nam`` has the true level,
* ``nam.train.lightning_module.LightningModule`` + ``pytorch_lightning.Trainer`` (CPU), with the loss / optimiser /
  scheduler recipe that ships in the trainer's A2 packed-model default config (``config_model_packed.json``, applied here to an A1 net: ESR validation loss,
  MR-STFT 5e-4, Adam lr 4e-3, ExponentialLR 0.994),
* ``net.export`` for the ``.nam`` (with ``other_metadata`` for the ``sawblade`` block).

The trainer's default network in 0.13 is the *packed* A2 WaveNet (slimmable container); Sawblade trains the classic
**A1 WaveNet** (``feather`` / ``lite`` / ``standard``, Sawblade's own approximations of the community sizes,
recalled from memory, NOT NAM's official presets: two layer arrays, 10 dilations 1..512,
kernel 3, Tanh) because every NAM loader pedal plays A1.  The same trainer can train the packed A2 model
(``PackedWaveNet`` + ``export_container``); that is **possible with this pin but not enabled** here (see README).

``tkinter`` is stubbed when absent (headless machines): it is only used by a GUI warning dialog in ``nam.train.core``.
"""
from __future__ import annotations

import copy
import json
import os
import random
import sys
import time
import types
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import progress as PG
from . import resume as R
from . import stop as STOP

# The training box is usually shared with other jobs: spinning OpenMP workers collapse (>5x slower epochs) as soon as one
# of them is descheduled, so wait passively.  Effective only if torch has not been imported yet (the CLI guarantees that).
os.environ.setdefault("OMP_WAIT_POLICY", "PASSIVE")


class _Cancel(BaseException):
    """Raised from the batch hook on SIGINT: Lightning tears down and re-raises it, with no validation pass and no
    epoch-end hooks (a ``should_stop`` would still run both)."""


def batch_cancel(run, num_batches: int, batch_idx: int) -> None:
    """Batch-end cancel decision.  Stop requested on a non-last batch: ``interrupted`` and raise ``_Cancel`` (the
    partial epoch is dropped).  On the last batch the epoch's training is complete, so only ``cancel_after_epoch`` is
    set: validation and the checkpoint still run and ``epoch_end_cancel`` raises afterwards (costs one validation pass)."""
    if not STOP.stop_requested():
        return
    if batch_idx + 1 >= num_batches:
        run.cancel_after_epoch = True
        return
    run.interrupted = True
    raise _Cancel()


def epoch_end_cancel(run, cdir) -> None:
    """After the epoch's checkpoint + progress write: when a cancel was deferred, mark ``progress.json``
    ``"interrupted": true`` and raise ``_Cancel``."""
    if not run.cancel_after_epoch:
        return
    prog = R.read_progress(cdir)
    if prog is not None:
        R.write_progress(cdir, {**prog, "interrupted": True})
    run.interrupted = True
    raise _Cancel()


NAM_PIN = "0.13.0"
RATE = 48000
NY = 8192
BATCH = 16
TARGET_RMS_DBFS = -18.0
DILATIONS = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]

SIZES_NOTE = ("feather/lite/standard are Sawblade's own approximations of the community A1 sizes, recalled from memory; "
              "they are not NAM's official presets")
# channels of array 1, head size of array 1 (= channels of array 2), channels of array 2
SIZES = {"feather": (8, 4, 4), "lite": (12, 6, 6), "standard": (16, 8, 8)}
# Default epochs: sized from measured CPU speed (see README "NAM export"); the wall-time cap also applies.
DEFAULT_EPOCHS = {"feather": 40, "lite": 30, "standard": 22}
DEFAULT_MAX_MINUTES = {"feather": 15.0, "lite": 30.0, "standard": 55.0}


def import_nam():
    """Import ``nam`` (stubbing ``tkinter`` when it is missing) and return the module."""
    try:
        import tkinter  # noqa: F401
    except ImportError:
        sys.modules["tkinter"] = types.ModuleType("tkinter")
    try:
        import nam  # noqa: F401
    except ImportError as e:  # pragma: no cover - message path
        raise RuntimeError("neural-amp-modeler is not installed: pip install -e 'match[export]' "
                           "-c match/constraints-export.txt") from e
    return sys.modules["nam"]


def resolve_device(requested: str = "auto", cuda: bool | None = None, mps: bool | None = None) -> tuple[str, str]:
    """(device name, Lightning accelerator).  ``cuda`` / ``mps`` override availability (for tests)."""
    if requested not in ("auto", "cpu", "cuda", "mps"):
        raise ValueError(f"unknown device {requested!r}")
    if cuda is None or mps is None:
        import_nam()
        import torch
        cuda = torch.cuda.is_available() if cuda is None else cuda
        mps = bool(getattr(torch.backends, "mps", None) and torch.backends.mps.is_available()) if mps is None else mps
    if requested == "auto":
        requested = "cuda" if cuda else "mps" if mps else "cpu"
    if requested == "cuda" and not cuda or requested == "mps" and not mps:
        raise RuntimeError(f"device {requested} requested but not available")
    return requested, {"cpu": "cpu", "cuda": "gpu", "mps": "mps"}[requested]


def wavenet_config(size: str) -> dict:
    c1, h1, c2 = SIZES[size]

    def arr(input_size, channels, head_out, head_bias):
        return {"input_size": input_size, "condition_size": 1, "channels": channels, "kernel_size": 3,
                "dilations": list(DILATIONS), "activation": "Tanh", "gated": False,
                "head": {"out_channels": head_out, "kernel_size": 1, "bias": head_bias}}

    return {"layers_configs": [arr(1, c1, h1, False), arr(c1, c2, 1, True)], "head": None, "head_scale": 0.02}


def model_config(size: str) -> dict:
    """The trainer's current default recipe (loss/optimiser/scheduler from its packaged config) around an A1 net."""
    import importlib.resources as res
    import_nam()
    with res.files("nam.train._resources").joinpath("config_model_packed.json").open() as fp:
        cfg = json.load(fp)
    cfg["net"] = {"name": "WaveNet", "config": wavenet_config(size)}
    return cfg


@dataclass
class TrainConfig:
    size: str = "standard"
    epochs: int | None = None
    max_minutes: float | None = None
    seed: int = 0
    threads: int = 4
    batch_size: int = BATCH
    device: str = "auto"                 # auto (cuda > mps > cpu) | cpu | cuda | mps
    ny: int = NY
    target_esr: float | None = None      # optional early stop on validation ESR
    lr_gamma: float | None = None        # ExponentialLR gamma per epoch; None = anneal to ~5 % of lr over the epochs

    def resolved(self) -> "TrainConfig":
        c = copy.copy(self)
        c.epochs = self.epochs if self.epochs is not None else DEFAULT_EPOCHS[self.size]
        c.max_minutes = self.max_minutes if self.max_minutes is not None else DEFAULT_MAX_MINUTES[self.size]
        if c.lr_gamma is None:
            # The trainer's recipe (gamma 0.994) assumes ~100s of epochs.  On a CPU budget of a few dozen epochs the
            # learning rate would barely decay (validation ESR then plateaus noisily), so decay to 5 % by the last epoch.
            c.lr_gamma = float(min(0.994, max(0.8, 0.05 ** (1.0 / max(c.epochs, 1)))))
        return c


@dataclass
class TrainResult:
    nam_path: Path | None
    epochs_done: int
    best_epoch: int
    best_val_esr: float
    wall_s: float
    stopped_by: str
    params: int
    receptive_field: int
    history: list = field(default_factory=list)
    config: dict = field(default_factory=dict)


def train_nam(x_train, y_train, x_valid, y_valid, cfg: TrainConfig, outdir, scratch, user_metadata=None,
              other_metadata=None, log=print, basename: str = "model", ckpt_dir=None, resume: bool = False,
              identity: dict | None = None, progress: PG.Progress | None = None) -> TrainResult:
    """Train one A1 WaveNet on (x, y) pairs (float32 mono, 48 kHz, sample aligned) and export ``<outdir>/<basename>.nam``.

    Seeded: ``pytorch_lightning.seed_everything(seed)`` before model init and shuffling.  CPU training is
    repeatable for the same seed, thread count and library versions (verified by the smoke test); it is not
    guaranteed bit-identical across machines/thread counts (BLAS reductions).  The .nam also holds a date stamp.

    Resumable: after every epoch ``<ckpt_dir>`` (default ``<scratch>/checkpoint``) gets ``last.ckpt`` (Lightning checkpoint
    plus the history, best-so-far bookkeeping, elapsed time and torch/numpy/python/DataLoader RNG states),
    ``best.ckpt`` and ``progress.json`` (``identity`` = preset/signal sha, mode, size is stored in it), all written
    atomically.  ``resume=True`` continues from ``last.ckpt``; ``cfg.max_minutes`` counts the elapsed training time of
    all sessions.

    Cancel: when ``stop.stop_requested()`` (SIGINT) the batch hook raises ``_Cancel``, which aborts ``fit`` at the end of
    the current batch without a validation pass or epoch-end hooks (SIGINT in the *last* batch of an epoch lets that
    epoch validate and checkpoint first, costing one validation pass, then cancels); the partial epoch writes no checkpoint
    (``progress.json`` only gets ``"interrupted": true``), nothing is exported and the result has
    ``stopped_by == "interrupt"`` and ``nam_path None``.  ``progress`` (optional) receives the train-stage updates.
    """
    cfg = cfg.resolved()
    import_nam()
    import pytorch_lightning as pl
    import torch
    from nam.data import Dataset, NormalizeJointDatasetOutput
    from nam.train import lightning_module as lm

    torch.set_num_threads(max(1, int(cfg.threads)))
    pl.seed_everything(cfg.seed, workers=True)
    mcfg = model_config(cfg.size)
    mcfg["lr_scheduler"]["kwargs"]["gamma"] = cfg.lr_gamma
    model = lm.LightningModule.init_from_config(copy.deepcopy(mcfg))
    rf = model.net.receptive_field
    n_params = int(sum(p.numel() for p in model.parameters()))

    def tens(a):
        return torch.tensor(np.asarray(a, np.float32))

    ds_train = Dataset(tens(x_train), tens(y_train), nx=rf, ny=cfg.ny, sample_rate=RATE,
                       require_input_pre_silence=None)
    ds_val = Dataset(tens(x_valid), tens(y_valid), nx=rf, ny=None, sample_rate=RATE, require_input_pre_silence=None)
    NormalizeJointDatasetOutput(TARGET_RMS_DBFS).apply(dataset_train=ds_train, dataset_validation=ds_val)
    model.net.sample_rate = float(RATE)
    for ds in (ds_train, ds_val):
        ds.handshake(model.net)
        model.net.handshake(ds)

    gen = torch.Generator()
    gen.manual_seed(cfg.seed)
    if len(ds_train) < 1:
        raise ValueError(f"training signal too short: {len(x_train)} samples for receptive field {rf} + ny {cfg.ny}")
    batch = max(1, min(cfg.batch_size, len(ds_train)))           # tiny smoke signals have < batch_size datums
    dl_train = torch.utils.data.DataLoader(ds_train, batch_size=batch, shuffle=True, drop_last=True,
                                           num_workers=0, generator=gen)
    dl_val = torch.utils.data.DataLoader(ds_val)

    scratch = Path(scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    cdir = Path(ckpt_dir) if ckpt_dir is not None else scratch / R.CKPT_DIRNAME
    cdir.mkdir(parents=True, exist_ok=True)
    identity = dict(identity or {})
    cap_s = float(cfg.max_minutes) * 60.0
    device, accel = resolve_device(cfg.device)

    class Run(pl.Callback):
        """History, best-so-far tracking, per-epoch atomic checkpoints, total-time cap and resume state."""

        def __init__(self):
            self.history: list[dict] = []
            self.best_val_loss = float("inf")
            self.best_epoch = 0
            self.prior_s = 0.0               # training time of earlier sessions
            self.t0 = time.time()
            self.improved = False
            self.interrupted = False
            self.cancel_after_epoch = False
            self.start_epoch = 0             # epochs complete when this session started
            self.start_elapsed = 0.0
            self.prior_epoch_s: float | None = None    # per-epoch time recorded by the resumed checkpoint

        def elapsed(self) -> float:
            return self.prior_s + (time.time() - self.t0)

        # --- resume state (stored inside every Lightning checkpoint)
        def state_dict(self):
            return {"history": copy.deepcopy(self.history), "bestValLoss": self.best_val_loss,
                    "bestEpoch": self.best_epoch, "elapsedS": self.elapsed(),
                    "rng": {"torch": torch.get_rng_state(), "numpy": np.random.get_state(), "python": random.getstate(),
                            "loader": gen.get_state()}}

        def load_state_dict(self, st):
            self.history = list(st["history"])
            self.best_val_loss = float(st["bestValLoss"])
            self.best_epoch = int(st["bestEpoch"])
            self.prior_s = float(st["elapsedS"])
            self.t0 = time.time()
            rng = st["rng"]
            torch.set_rng_state(rng["torch"])
            np.random.set_state(rng["numpy"])
            random.setstate(rng["python"])
            gen.set_state(rng["loader"])

        def _report(self, trainer, batch_frac: float, force: bool) -> None:
            if progress is None:
                return
            done = len(self.history)
            el = self.elapsed()
            best = min((r["valEsr"] for r in self.history), default=None)
            eta = PG.eta_seconds(el - self.start_elapsed, done - self.start_epoch, done, cfg.epochs, el, cap_s,
                                  batch_frac=batch_frac, prior_epoch_s=self.prior_epoch_s)
            t = PG.train_progress(done, batch_frac, cfg.epochs, el, cap_s)
            progress.update("train", PG.stage_fraction("train", t), eta=eta, epoch=done, epochs=cfg.epochs,
                            best_esr=best, resumable=(cdir / R.LAST).is_file(), force=force,
                            message=f"epoch {min(done + 1, cfg.epochs)}/{cfg.epochs}")

        def on_train_batch_end(self, trainer, module, outputs, batch, batch_idx):
            batch_cancel(self, trainer.num_training_batches, batch_idx)
            if self.cancel_after_epoch:
                return
            if self.elapsed() >= cap_s:
                trainer.should_stop = True
            self._report(trainer, (batch_idx + 1) / max(trainer.num_training_batches, 1), force=False)

        def on_validation_batch_end(self, trainer, module, outputs, batch, batch_idx, dataloader_idx=0):
            if not trainer.sanity_checking:
                self._report(trainer, 0.0, force=False)          # heartbeat (throttled to 1 Hz by Progress)

        def on_exception(self, trainer, module, exception):
            if isinstance(exception, (KeyboardInterrupt, _Cancel)):
                self.interrupted = True

        def on_validation_end(self, trainer, module):
            if trainer.sanity_checking or self.interrupted:
                return
            m = trainer.callback_metrics
            row = {"epoch": trainer.current_epoch + 1, "valEsr": float(m["ESR"]), "valLoss": float(m["val_loss"]),
                   "elapsedS": round(self.elapsed(), 1)}
            self.history.append(row)
            if row["valLoss"] < self.best_val_loss:
                self.best_val_loss, self.best_epoch, self.improved = row["valLoss"], row["epoch"], True
            log(f"  epoch {row['epoch']:3d}  val ESR {row['valEsr']:.5f}  ({row['elapsedS']:.0f} s)")
            if cfg.target_esr is not None and row["valEsr"] <= cfg.target_esr:
                trainer.should_stop = True
            self._report(trainer, 0.0, force=True)

        def on_train_epoch_end(self, trainer, module):
            if self.interrupted:                 # partial epoch: keep the last complete checkpoint untouched
                prog = R.read_progress(cdir)
                if prog is not None:
                    R.write_progress(cdir, {**prog, "interrupted": True})
                return
            tmp = cdir / (R.LAST + ".tmp")
            trainer.save_checkpoint(str(tmp))
            if self.improved:
                R.atomic_copy(tmp, cdir / R.BEST)
                self.improved = False
            os.replace(tmp, cdir / R.LAST)
            best = min(self.history, key=lambda r: r["valLoss"]) if self.history else {}
            R.write_progress(cdir, {**identity, "progressVersion": 1, "epoch": len(self.history),
                                    "bestEpoch": best.get("epoch"), "bestValEsr": best.get("valEsr"),
                                    "elapsedTrainingS": round(self.elapsed(), 1), "complete": False,
                                    "config": {"seed": cfg.seed, "batchSize": cfg.batch_size, "epochs": cfg.epochs,
                                               "lrGamma": cfg.lr_gamma, "maxMinutes": cfg.max_minutes,
                                               "threads": cfg.threads, "ny": cfg.ny, "device": device}})
            self._report(trainer, 0.0, force=True)               # now resumable
            epoch_end_cancel(self, cdir)

    run = Run()
    resume_from = None
    if resume:
        resume_from = cdir / R.LAST
        if not resume_from.is_file():
            raise ValueError(f"cannot resume: {resume_from} does not exist")
        prog = R.read_progress(cdir) or {}
        log(f"resuming from epoch {prog.get('epoch', '?')} ({prog.get('elapsedTrainingS', 0):.0f} s of training already spent)")
        st = torch.load(resume_from, map_location="cpu", weights_only=False)["callbacks"][run.state_key]
        run.prior_s = float(st["elapsedS"])
        run.history = list(st["history"])
        run.prior_epoch_s = PG.prior_epoch_seconds(run.prior_s, len(run.history))
    run.start_epoch = len(run.history)
    run.start_elapsed = run.prior_s
    history = run.history

    log(f"training A1 WaveNet '{cfg.size}': {n_params} parameters, receptive field {rf}, "
        f"{len(ds_train)} datums/epoch of {cfg.ny}, up to {cfg.epochs} epochs / {cfg.max_minutes:g} min, "
        f"seed {cfg.seed}, {cfg.threads} threads, device {device}")
    already_done = (len(history) >= cfg.epochs or run.elapsed() >= cap_s or
                    (cfg.target_esr is not None and history and history[-1]["valEsr"] <= cfg.target_esr))
    if not already_done:
        trainer = pl.Trainer(max_epochs=cfg.epochs, accelerator=accel, devices=1, callbacks=[run], logger=False,
                             enable_checkpointing=False, enable_progress_bar=False, enable_model_summary=False, default_root_dir=str(scratch),
                             deterministic="warn", num_sanity_val_steps=0)
        if STOP.stop_requested():
            run.interrupted = True
        else:
            try:
                trainer.fit(model, dl_train, dl_val, ckpt_path=str(resume_from) if resume_from else None)
            except (_Cancel, KeyboardInterrupt, SystemExit):
                # Lightning turns a KeyboardInterrupt into exit(1); our SIGINT handler normally prevents it, but a
                # second SIGINT reaches here.  Anything else (a real SystemExit) propagates.
                if not (run.interrupted or STOP.stop_requested()):
                    raise
                run.interrupted = True
    history = run.history                # load_state_dict replaces the list on resume
    wall = run.elapsed()
    if run.interrupted:
        best_row = min(history, key=lambda r: r["valLoss"]) if history else {}
        return TrainResult(nam_path=None, epochs_done=len(history), best_epoch=best_row.get("epoch", 0),
                           best_val_esr=best_row.get("valEsr", float("nan")), wall_s=wall, stopped_by="interrupt",
                           params=n_params, receptive_field=int(rf), history=history, config={})
    best_path = cdir / R.BEST
    if not best_path.is_file():
        raise RuntimeError("training stopped before the first validation pass (time cap too small?)")
    done = len(history)
    if cfg.target_esr is not None and history and history[-1]["valEsr"] <= cfg.target_esr:
        stopped = "target_esr"
    else:
        stopped = "max_epochs" if done >= cfg.epochs else "max_time"

    best = lm.LightningModule.load_from_checkpoint(str(best_path), **lm.LightningModule.parse_config(
        copy.deepcopy(mcfg)))
    best.cpu()
    best.eval()
    best.net.sample_rate = float(RATE)
    for ds in (ds_train, ds_val):
        ds.handshake(best.net)
        best.net.handshake(ds)
    best_row = min(history, key=lambda r: r["valLoss"])
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    best.net.export(outdir, basename=basename, user_metadata=user_metadata, other_metadata=other_metadata)
    return TrainResult(nam_path=outdir / f"{basename}.nam", epochs_done=done, best_epoch=best_row["epoch"],
                       best_val_esr=best_row["valEsr"], wall_s=wall, stopped_by=stopped, params=n_params,
                       receptive_field=int(rf), history=history,
                       config={"size": cfg.size, "epochs": cfg.epochs, "maxMinutes": cfg.max_minutes, "seed": cfg.seed,
                               "threads": cfg.threads, "batchSize": batch, "ny": cfg.ny,
                               "device": device, "sizesNote": SIZES_NOTE, "targetEsr": cfg.target_esr, "lrGamma": cfg.lr_gamma, "net": wavenet_config(cfg.size),
                               "recipe": {k: mcfg[k] for k in ("loss", "optimizer", "lr_scheduler")},
                               "namVersion": NAM_PIN, "torch": torch.__version__,
                               "outputNormalisationDbfs": TARGET_RMS_DBFS})
