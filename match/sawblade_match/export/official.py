"""Training on the NAM project's standard input file (v0.6 decision 22), through the pinned trainer's own data pipeline.

What is the trainer's (``neural-amp-modeler`` 0.13.0, ``nam/train/core.py``, the code behind ``nam.train.core.train``):
version recognition (replicated in ``standard_input.recognise`` so the signature table is injectable for tests; the
trainer's table is a local literal), blip latency calibration (``_analyze_latency`` / ``_get_final_latency``), the data
checks (``_check_data``), the train / validation split and its dataset config (``_get_data_config``), the dataset objects
(``init_dataset``) and the joint -18 dBFS output normalisation hook (``data_config["joint"]``).

What is NOT the monolithic ``train()``: its loop, callbacks and export.  Sawblade keeps its own (``train.train_nam``) because
it needs resume, cancel, progress, per-submodel best checkpoints, ``PackedWaveNet.export_container`` and the ``sawblade``
metadata block, none of which ``train()`` exposes.  The model config (``config_model_packed.json``), loss and optimiser
recipe are the same as ``train()``'s.  Those underscore functions are private; the pin (exactly 0.13.0) is what makes this safe,
and a test compares them with ``train()``'s own use.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path

from .plan import ExportRefused
from .train import import_nam


class OfficialData:
    """Datasets for one (standard input, rendered output) pair.  ``build(rf, ny)`` returns ``(train, validation)`` datasets."""

    def __init__(self, input_path, output_path, version: str, latency: int | None = None, log=print):
        self.input_path, self.output_path, self.version = str(input_path), str(output_path), version
        self.latency, self.log = latency, log
        self.info: dict = {}

    def _call(self, fn, *a, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                return fn(*a, **kw)
            finally:
                for line in buf.getvalue().splitlines():
                    if line.strip():
                        self.log(f"  trainer: {line}")

    def build(self, rf: int, ny: int):
        import_nam()
        from nam.train import core
        from nam.train._version import Version
        ver = Version.from_string(self.version)
        try:
            analysis = self._call(core._analyze_latency, self.latency, ver, self.input_path, self.output_path, silent=True)
            final = self._call(core._get_final_latency, analysis)
        except core._FinalLatencyError as e:
            raise ExportRefused("the trainer could not calibrate the latency from the blips of the rendered output "
                                f"({e}); the chain's response to the standard input's blips is too weak or missing") from e
        checks = self._call(core._check_data, self.input_path, self.output_path, ver, analysis.calibration.recommended, True)
        if checks is not None and not checks.passed and ver.major == 3:
            raise ExportRefused("the trainer's data checks failed for the rendered output (the two validation replicates "
                                "differ: the chain is not repeatable, e.g. it has a time effect or noise)")
        cfg = core._get_data_config(ver, Path(self.input_path), Path(self.output_path), ny, final)
        cfg["common"]["nx"] = rf
        ds_train = core._init_dataset(cfg, core._Split.TRAIN)
        ds_val = core._init_dataset(cfg, core._Split.VALIDATION)
        core._apply_joint_dataset_hooks(dataset_train=ds_train, dataset_validation=ds_val,
                                        hooks=core._get_joint_dataset_hooks(cfg.get("joint", [])))
        cal = analysis.calibration
        self.info = {"version": self.version, "latencySamples": int(final), "latencyManual": self.latency,
                     "calibrationDelays": [int(d) for d in cal.delays], "calibrationRecommended": cal.recommended,
                     "dataChecks": None if checks is None else {"passed": bool(checks.passed)},
                     "trainSplit": {k: v for k, v in cfg["train"].items()}, "validationSplit": dict(cfg["validation"]),
                     "deprecatedVersion": ver.major != 3,
                     "outputNormalisation": "joint hook nam.data.normalize_joint_dataset_output, -18 dBFS (trainer's)"}
        return ds_train, ds_val
