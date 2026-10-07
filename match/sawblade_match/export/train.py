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

The trainer's default network in 0.13 is the *packed* A2 WaveNet (a slimmable container holding the 3-channel and the
8-channel submodel).  Sawblade trains **A2 by default** (``arch="a2"``: ``PackedWaveNet`` + ``PackedLightningModule`` with
the trainer's own ``config_model_packed.json`` recipe, one best checkpoint per submodel like the trainer's
``PackedBestCheckpoint``, ``export_container``) and the classic **A1 WaveNet** (``feather`` / ``lite`` / ``standard``, NAM's
official A1 presets) for older loaders (``arch="a1"``).  The A1 layouts are NAM's official presets (``Architecture`` +
``get_wavenet_config`` in ``nam/train/core.py``); the pinned 0.13.0 wheel no longer ships them, they are copied from 0.12.3
(the last release that has them, byte-identical since 0.11.0), see ``A1_PRESETS`` below for file and line.

One A2 run writes three files: the container (``<basename>.a2.nam``) and the two submodels as standalone files
(``<basename>.a2_full.nam`` = 8 channels, ``<basename>.a2_lite.nam`` = 3 channels, same weights as the container's
submodels).  Per-submodel validation numbers are ``ESR_packed_i`` (the trainer's aggregate ``ESR`` / ``val_loss`` are SUMS
over the submodels and are never used).

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

# Official NAM A1 presets: `Architecture` (STANDARD/LITE/FEATHER/NANO) at nam/train/core.py:59-63 and
# `get_wavenet_config` at nam/train/core.py:845-955 of neural-amp-modeler 0.12.3 (STANDARD 847-873, LITE 874-900,
# FEATHER 901-927, NANO 928-954).  The pinned 0.13.0 has neither (its core.py always trains the packed A2 net,
# `_get_configs` at core.py:899, `_get_packed_model_config` at core.py:883); the block is byte-identical in 0.11.0 (nam/train/core.py:797), 0.12.0 (:814),
# 0.12.2 and 0.12.3 (:845).  Every layer: kernel_size 3, Tanh, not gated, head_scale 0.02, condition_size 1; array 1
# input_size 1, head_bias False; array 2 input_size = channels of array 1, head_size 1, head_bias True.
# Every preset has the same receptive field (4093 samples), only width and the dilation split differ.
_D_1_512 = (1, 2, 4, 8, 16, 32, 64, 128, 256, 512)                   # 10 layers
_D_1_64 = (1, 2, 4, 8, 16, 32, 64)                                   # 7 layers
_D_128_512_1_512 = (128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512)   # 13 layers
# size -> (array 1: channels, head_size, dilations | array 2: channels, dilations)   (array 1 head_size == array 2 input_size)
A1_PRESETS = {
    "feather": {"channels1": 8, "head1": 4, "dilations1": _D_1_64, "channels2": 4, "dilations2": _D_128_512_1_512},   # core.py:901-927
    "lite": {"channels1": 12, "head1": 6, "dilations1": _D_1_64, "channels2": 6, "dilations2": _D_128_512_1_512},     # core.py:874-900
    "standard": {"channels1": 16, "head1": 8, "dilations1": _D_1_512, "channels2": 8, "dilations2": _D_1_512},        # core.py:847-873
}
# (NANO, core.py:928-954, is official too: 4/2 channels, same dilation split as lite; not offered by Sawblade.)
SIZES = A1_PRESETS
SIZES_NOTE = ("feather/lite/standard are NAM's official A1 presets (neural-amp-modeler 0.12.3 nam/train/core.py "
              "get_wavenet_config; the pinned 0.13.0 no longer ships them)")
# Default epochs: sized from measured CPU speed (see README "NAM export"); the wall-time cap also applies.
DEFAULT_EPOCHS = {"feather": 40, "lite": 30, "standard": 22}
DEFAULT_MAX_MINUTES = {"feather": 15.0, "lite": 30.0, "standard": 55.0}


# --- A2 (the trainer's packed model, config_model_packed.json: submodels channels_3 (A2 Lite) and channels_8 (A2 Full)).
A2_SIZES = ("full", "lite")
A2_SUBMODEL_INDEX = {"lite": 0, "full": 1}          # PackedWaveNet submodel order (ascending max_value 0.5 / 1.0)
A2_SUBMODEL_NAMES = {"lite": "channels_3", "full": "channels_8"}
A2_LAYOUT = "a2-packed-channels_3+channels_8"
A1_LAYOUT = "a1-official-0.12.3"
# One run trains both sizes, so the budget does not depend on --size.  The packed net costs about 1.5x an A1 standard step.
A2_DEFAULT_EPOCHS = 22
A2_DEFAULT_MAX_MINUTES = 80.0
ARCHS = ("a2", "a1")
DEFAULT_SIZE = {"a2": "full", "a1": "standard"}


def sizes_for(arch: str) -> tuple[str, ...]:
    if arch == "a2":
        return A2_SIZES
    if arch == "a1":
        return tuple(SIZES)
    raise ValueError(f"unknown --arch {arch!r} (expected a2 or a1)")


def check_arch_size(arch: str, size: str | None) -> str:
    """The size for ``arch`` (default per arch when ``size`` is None); ``ValueError`` with a clear message otherwise."""
    if arch not in ARCHS:
        raise ValueError(f"unknown --arch {arch!r} (expected a2 or a1)")
    if size is None:
        return DEFAULT_SIZE[arch]
    if size not in sizes_for(arch):
        raise ValueError(f"--size {size} is not valid for --arch {arch} (valid: {', '.join(sizes_for(arch))})")
    return size


def layout_of(arch: str) -> str:
    return A2_LAYOUT if arch == "a2" else A1_LAYOUT


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
    """The official A1 preset ``size`` as a 0.13.0 ``WaveNet`` config (0.12.3's flat ``head_size`` / ``head_bias`` keys are
    the nested ``head`` dict here)."""
    p = SIZES[size]

    def arr(input_size, channels, dilations, head_out, head_bias):
        return {"input_size": input_size, "condition_size": 1, "channels": channels, "kernel_size": 3,
                "dilations": list(dilations), "activation": "Tanh", "gated": False,
                "head": {"out_channels": head_out, "kernel_size": 1, "bias": head_bias}}

    return {"layers_configs": [arr(1, p["channels1"], p["dilations1"], p["head1"], False),
                               arr(p["channels1"], p["channels2"], p["dilations2"], 1, True)],
            "head": None, "head_scale": 0.02}


def model_config(size: str, arch: str = "a1") -> dict:
    """The trainer's current default recipe (loss/optimiser/scheduler from its packaged config).  ``arch="a1"``: around
    the official A1 preset ``size``; ``arch="a2"``: the packed A2 net exactly as shipped (``size`` is irrelevant: both
    submodels are trained together)."""
    import importlib.resources as res
    import_nam()
    with res.files("nam.train._resources").joinpath("config_model_packed.json").open() as fp:
        cfg = json.load(fp)
    if arch == "a1":
        cfg["net"] = {"name": "WaveNet", "config": wavenet_config(size)}
    elif arch != "a2":
        raise ValueError(f"unknown arch {arch!r}")
    return cfg


@dataclass
class TrainConfig:
    size: str = "standard"               # a1: feather|lite|standard; a2: full|lite (which standalone file is the primary one)
    arch: str = "a1"                     # a1 | a2 (the CLI defaults to a2)
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
        if self.arch == "a2":
            c.epochs = self.epochs if self.epochs is not None else A2_DEFAULT_EPOCHS
            c.max_minutes = self.max_minutes if self.max_minutes is not None else A2_DEFAULT_MAX_MINUTES
        else:
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
    arch: str = "a1"
    files: dict = field(default_factory=dict)          # a2: {"container", "full", "lite"} paths; a1: {"primary"}
    submodels: dict = field(default_factory=dict)      # a2: {"lite"|"full": {"parameters", "bestEpoch", "bestValEsr", ...}}


def train_nam(x_train, y_train, x_valid, y_valid, cfg: TrainConfig, outdir, scratch, user_metadata=None,
              other_metadata=None, log=print, basename: str = "model", ckpt_dir=None, resume: bool = False,
              identity: dict | None = None, progress: PG.Progress | None = None) -> TrainResult:
    """Train one A1 WaveNet (``cfg.arch == "a1"``) or the packed A2 WaveNet (``"a2"``) on (x, y) pairs (float32 mono, 48 kHz,
    sample aligned) and export ``<outdir>/<basename>.nam`` (A1), or for A2 the container ``<basename>.a2.nam`` plus the
    standalone ``<basename>.a2_full.nam`` / ``<basename>.a2_lite.nam`` (``TrainResult.nam_path`` = the one ``cfg.size``
    names, ``TrainResult.files`` all three).  A2 keeps one best checkpoint PER SUBMODEL (best ``val_loss_packed_i``) and
    exports each size from its own best epoch.

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

    class PackedBest(lm.PackedBestCheckpoint):
        """The trainer's per-submodel best checkpoints, plus resume state (the trainer's callback forgets its bookkeeping
        across a restart and would overwrite a better checkpoint with the first epoch after the resume)."""

        def state_dict(self):
            return {"best": copy.deepcopy(self.best)}

        def load_state_dict(self, st):
            self.best = {int(k): v for k, v in st["best"].items()}

    torch.set_num_threads(max(1, int(cfg.threads)))
    pl.seed_everything(cfg.seed, workers=True)
    packed = cfg.arch == "a2"
    if packed:
        check_arch_size("a2", cfg.size)
    mcfg = model_config(cfg.size, cfg.arch)
    mcfg["lr_scheduler"]["kwargs"]["gamma"] = cfg.lr_gamma
    lm_cls = lm.PackedLightningModule if packed else lm.LightningModule
    model = lm_cls.init_from_config(copy.deepcopy(mcfg))
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
            if packed:
                # per-submodel metrics: the aggregate ESR / val_loss are SUMS over the submodels, never used.  The
                # headline valEsr / valLoss are the Full submodel's (what A1 callers read).
                sub = {n: {"valEsr": float(m[f"ESR_packed_{i}"]), "valLoss": float(m[f"val_loss_packed_{i}"])}
                       for n, i in A2_SUBMODEL_INDEX.items()}
                row = {"epoch": trainer.current_epoch + 1, "valEsr": sub["full"]["valEsr"],
                       "valLoss": sub["full"]["valLoss"], "elapsedS": round(self.elapsed(), 1), "submodels": sub}
            else:
                row = {"epoch": trainer.current_epoch + 1, "valEsr": float(m["ESR"]), "valLoss": float(m["val_loss"]),
                       "elapsedS": round(self.elapsed(), 1)}
            self.history.append(row)
            if row["valLoss"] < self.best_val_loss:
                self.best_val_loss, self.best_epoch, self.improved = row["valLoss"], row["epoch"], True
            if packed:
                log(f"  epoch {row['epoch']:3d}  val ESR full {sub['full']['valEsr']:.5f}  lite {sub['lite']['valEsr']:.5f}"
                    f"  ({row['elapsedS']:.0f} s)")
            else:
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
            if self.improved and not packed:                     # a2: the per-submodel best checkpoints are PackedBest's
                R.atomic_copy(tmp, cdir / R.BEST)
            self.improved = False
            os.replace(tmp, cdir / R.LAST)
            best = min(self.history, key=lambda r: r["valLoss"]) if self.history else {}
            R.write_progress(cdir, {**identity, "progressVersion": 1, "epoch": len(self.history),
                                    "bestEpoch": best.get("epoch"), "bestValEsr": best.get("valEsr"),
                                    "elapsedTrainingS": round(self.elapsed(), 1), "complete": False,
                                    "arch": cfg.arch, "layout": layout_of(cfg.arch),
                                    "config": {"seed": cfg.seed, "batchSize": cfg.batch_size, "epochs": cfg.epochs,
                                               "lrGamma": cfg.lr_gamma, "maxMinutes": cfg.max_minutes,
                                               "threads": cfg.threads, "ny": cfg.ny, "device": device}})
            self._report(trainer, 0.0, force=True)               # now resumable
            epoch_end_cancel(self, cdir)

    run = Run()
    packed_best = PackedBest(cdir) if packed else None
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

    what = "A2 packed WaveNet (Full 8 ch + Lite 3 ch)" if packed else f"A1 WaveNet '{cfg.size}'"
    log(f"training {what}: {n_params} parameters, receptive field {rf}, "
        f"{len(ds_train)} datums/epoch of {cfg.ny}, up to {cfg.epochs} epochs / {cfg.max_minutes:g} min, "
        f"seed {cfg.seed}, {cfg.threads} threads, device {device}")
    already_done = (len(history) >= cfg.epochs or run.elapsed() >= cap_s or
                    (cfg.target_esr is not None and history and history[-1]["valEsr"] <= cfg.target_esr))
    if not already_done:
        callbacks = [run] + ([lm.PackedMaskCallback(), packed_best] if packed else [])
        trainer = pl.Trainer(max_epochs=cfg.epochs, accelerator=accel, devices=1, callbacks=callbacks, logger=False,
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
    done = len(history)
    if cfg.target_esr is not None and history and history[-1]["valEsr"] <= cfg.target_esr:
        stopped = "target_esr"
    else:
        stopped = "max_epochs" if done >= cfg.epochs else "max_time"
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    if packed:
        return _finish_a2(cfg, mcfg, lm, cdir, outdir, basename, ds_train, ds_val, history, done, stopped, wall, rf,
                          n_params, user_metadata, other_metadata, torch)
    best_path = cdir / R.BEST
    if not best_path.is_file():
        raise RuntimeError("training stopped before the first validation pass (time cap too small?)")

    best = lm.LightningModule.load_from_checkpoint(str(best_path), **lm.LightningModule.parse_config(
        copy.deepcopy(mcfg)))
    best.cpu()
    best.eval()
    best.net.sample_rate = float(RATE)
    for ds in (ds_train, ds_val):
        ds.handshake(best.net)
        best.net.handshake(ds)
    best_row = min(history, key=lambda r: r["valLoss"])
    best.net.export(outdir, basename=basename, user_metadata=user_metadata, other_metadata=other_metadata)
    nam_path = outdir / f"{basename}.nam"
    return TrainResult(nam_path=nam_path, epochs_done=done, best_epoch=best_row["epoch"],
                       best_val_esr=best_row["valEsr"], wall_s=wall, stopped_by=stopped, params=n_params,
                       receptive_field=int(rf), history=history, arch="a1", files={"primary": nam_path},
                       config=_result_config(cfg, mcfg, batch, device, torch.__version__))


def _result_config(cfg: TrainConfig, mcfg: dict, batch: int, device: str, torch_version: str) -> dict:
    out = {"arch": cfg.arch, "size": cfg.size, "epochs": cfg.epochs, "maxMinutes": cfg.max_minutes, "seed": cfg.seed,
           "threads": cfg.threads, "batchSize": batch, "ny": cfg.ny,
           "device": device, "targetEsr": cfg.target_esr, "lrGamma": cfg.lr_gamma,
           "recipe": {k: mcfg[k] for k in ("loss", "optimizer", "lr_scheduler")},
           "namVersion": NAM_PIN, "torch": torch_version, "outputNormalisationDbfs": TARGET_RMS_DBFS}
    if cfg.arch == "a2":
        out["net"] = {"name": "PackedWaveNet", "submodels": dict(A2_SUBMODEL_NAMES), "layout": A2_LAYOUT}
    else:
        out["sizesNote"] = SIZES_NOTE
        out["net"] = wavenet_config(cfg.size)
    return out


def _finish_a2(cfg, mcfg, lm, cdir, outdir, basename, ds_train, ds_val, history, done, stopped, wall, rf, n_params,
               user_metadata, other_metadata, torch) -> TrainResult:
    """Export the packed run: every submodel from its own best checkpoint (``packed_best_submodel_{i}.ckpt``), the
    container through ``PackedWaveNet.export`` (the dataset's output-scale hook is applied to every submodel and the
    container) and the two standalone files cut out of the container's submodels (same weights, hook applied)."""
    paths = [cdir / f"packed_best_submodel_{i}.ckpt" for i in range(len(A2_SUBMODEL_INDEX))]
    if not all(p.is_file() for p in paths):
        raise RuntimeError("training stopped before the first validation pass (time cap too small?)")
    cls = lm.PackedLightningModule

    def load(path):
        mod = cls.load_from_checkpoint(str(path), **cls.parse_config(copy.deepcopy(mcfg)))
        mod.cpu()
        mod.eval()
        mod.net.sample_rate = float(RATE)
        return mod

    base_i = A2_SUBMODEL_INDEX["full"]
    final = load(paths[base_i])
    for i, path in enumerate(paths):
        if i != base_i:
            final.net.import_submodel(i, load(path).net.extract_submodel(i))
    final.net.apply_mask()
    for ds in (ds_train, ds_val):
        ds.handshake(final.net)
        final.net.handshake(ds)
    container = final.net.export_container(outdir, basename=f"{basename}.a2", user_metadata=user_metadata,
                                           other_metadata=other_metadata)
    files = {"container": outdir / f"{basename}.a2.nam"}
    for size, i in A2_SUBMODEL_INDEX.items():
        sub = copy.deepcopy(container["config"]["submodels"][i]["model"])
        meta = copy.deepcopy(container["metadata"])
        for k in ("loudness", "gain"):                         # the container carries the Full submodel's; each file its own
            if k in sub.get("metadata", {}):
                meta[k] = sub["metadata"][k]
            else:
                meta.pop(k, None)
        sub["metadata"] = meta
        if "sample_rate" in container:
            sub.setdefault("sample_rate", container["sample_rate"])
        files[size] = outdir / f"{basename}.a2_{size}.nam"
        files[size].write_text(json.dumps(sub))
    submodels = {}
    for size, i in A2_SUBMODEL_INDEX.items():
        row = min(history, key=lambda r: r["submodels"][size]["valLoss"])
        submodels[size] = {"name": A2_SUBMODEL_NAMES[size], "bestEpoch": row["epoch"],
                           "bestValEsr": row["submodels"][size]["valEsr"],
                           "parameters": int(sum(p.numel() for p in final.net.extract_submodel(i).parameters())),
                           "maxValue": container["config"]["submodels"][i]["max_value"]}
    primary = files[cfg.size]
    return TrainResult(nam_path=primary, epochs_done=done, best_epoch=submodels[cfg.size]["bestEpoch"],
                       best_val_esr=submodels[cfg.size]["bestValEsr"], wall_s=wall, stopped_by=stopped, params=n_params,
                       receptive_field=int(rf), history=history, arch="a2", files=files, submodels=submodels,
                       config=_result_config(cfg, mcfg, cfg.batch_size, resolve_device(cfg.device)[0], torch.__version__))
