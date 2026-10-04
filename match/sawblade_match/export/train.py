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
  scheduler recipe that ships in the trainer's own default config (``config_model_packed.json``: ESR validation loss,
  MR-STFT 5e-4, Adam lr 4e-3, ExponentialLR 0.994),
* ``net.export`` for the ``.nam`` (with ``other_metadata`` for the ``sawblade`` block).

The trainer's default network in 0.13 is the *packed* A2 WaveNet (slimmable container); Sawblade trains the classic
**A1 WaveNet** (``feather`` / ``lite`` / ``standard``, the community sizes: two layer arrays, 10 dilations 1..512,
kernel 3, Tanh) because every NAM loader pedal plays A1.  The same trainer can train the packed A2 model
(``PackedWaveNet`` + ``export_container``); that is **possible with this pin but not enabled** here (see README).

``tkinter`` is stubbed when absent (headless machines): it is only used by a GUI warning dialog in ``nam.train.core``.
"""
from __future__ import annotations

import copy
import json
import os
import sys
import time
import types
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

# The training box is usually shared with other jobs: spinning OpenMP workers collapse (>5x slower epochs) as soon as one
# of them is descheduled, so wait passively.  Effective only if torch has not been imported yet (the CLI guarantees that).
os.environ.setdefault("OMP_WAIT_POLICY", "PASSIVE")

NAM_PIN = "0.13.0"
RATE = 48000
NY = 8192
BATCH = 16
TARGET_RMS_DBFS = -18.0
DILATIONS = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]

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
    nam_path: Path
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
              other_metadata=None, log=print, basename: str = "model") -> TrainResult:
    """Train one A1 WaveNet on (x, y) pairs (float32 mono, 48 kHz, sample aligned) and export ``<outdir>/<basename>.nam``.

    Seeded: ``pytorch_lightning.seed_everything(seed)`` before model init and shuffling.  CPU training is
    repeatable for the same seed, thread count and library versions (verified by the smoke test); it is not
    guaranteed bit-identical across machines/thread counts (BLAS reductions).  The .nam also holds a date stamp.
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
    history: list[dict] = []
    t0 = time.time()

    class Rec(pl.Callback):
        def on_validation_end(self, trainer, module):
            if trainer.sanity_checking:
                return
            m = trainer.callback_metrics
            row = {"epoch": trainer.current_epoch + 1, "valEsr": float(m["ESR"]), "valLoss": float(m["val_loss"]),
                   "elapsedS": round(time.time() - t0, 1)}
            history.append(row)
            log(f"  epoch {row['epoch']:3d}  val ESR {row['valEsr']:.5f}  ({row['elapsedS']:.0f} s)")
            if cfg.target_esr is not None and row["valEsr"] <= cfg.target_esr:
                trainer.should_stop = True

    ckpt = pl.callbacks.ModelCheckpoint(dirpath=str(scratch / "ckpt"), filename="best", monitor="val_loss",
                                        save_top_k=1, mode="min")
    trainer = pl.Trainer(max_epochs=cfg.epochs, max_time={"seconds": int(cfg.max_minutes * 60)}, accelerator="cpu",
                         devices=1, callbacks=[ckpt, Rec()], logger=False, enable_progress_bar=False,
                         enable_model_summary=False, default_root_dir=str(scratch), deterministic="warn",
                         num_sanity_val_steps=0)
    log(f"training A1 WaveNet '{cfg.size}': {n_params} parameters, receptive field {rf}, "
        f"{len(ds_train)} datums/epoch of {cfg.ny}, up to {cfg.epochs} epochs / {cfg.max_minutes:g} min, "
        f"seed {cfg.seed}, {cfg.threads} threads")
    trainer.fit(model, dl_train, dl_val)
    wall = time.time() - t0
    if not ckpt.best_model_path:
        raise RuntimeError("training stopped before the first validation pass (time cap too small?)")
    done = len(history)
    if cfg.target_esr is not None and history and history[-1]["valEsr"] <= cfg.target_esr:
        stopped = "target_esr"
    else:
        stopped = "max_epochs" if done >= cfg.epochs else "max_time"

    best = lm.LightningModule.load_from_checkpoint(ckpt.best_model_path, **lm.LightningModule.parse_config(
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
                               "targetEsr": cfg.target_esr, "lrGamma": cfg.lr_gamma, "net": wavenet_config(cfg.size),
                               "recipe": {k: mcfg[k] for k in ("loss", "optimizer", "lr_scheduler")},
                               "namVersion": NAM_PIN, "torch": torch.__version__,
                               "outputNormalisationDbfs": TARGET_RMS_DBFS})
