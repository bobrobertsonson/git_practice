"""v0.8 I4a: input calibration options and the stereo-DI rule for the matcher.

The core owns the DSP (``render(..., calibration=, device_dbu=)``); this module only holds the option, the record
written to result.json and the offline stereo rule that mirrors the core's (``render.cpp``: the louder of the first two
channels by whole-file RMS, a tie picks L; ``L`` / ``R`` / ``mix`` = mean of channels 0 and 1).
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

# I4c flips this one constant ("calibrated"). Until then the matcher renders exactly as before.
DEFAULT_CALIBRATION = "legacy"
CALIBRATION_MODES = ("legacy", "calibrated")
ASSUMED_DEVICE_DBU = 12.0               # the core's kAssumedDeviceDbu (calibration.h)
DI_CHANNEL_RULES = ("auto", "L", "R", "mix")


@dataclass(frozen=True)
class CalibrationOptions:
    """``mode``: what the matcher renders with and writes into emitted presets (``calibration.mode``).
    ``device_dbu``: the interface level (dBu at 0 dBFS); ``None`` = the assumed +12 dBu (recorded as assumed)."""
    mode: str = DEFAULT_CALIBRATION
    device_dbu: float | None = None

    def __post_init__(self):
        if self.mode not in CALIBRATION_MODES:
            raise ValueError(f"calibration must be one of {CALIBRATION_MODES}, got {self.mode!r}")
        if self.device_dbu is not None and not (math.isfinite(self.device_dbu) and -60.0 <= self.device_dbu <= 60.0):
            raise ValueError(f"--device-dbu must be within -60..60 dBu, got {self.device_dbu!r}")

    @property
    def calibrated(self) -> bool:
        return self.mode == "calibrated"

    @property
    def effective_dbu(self) -> float:
        return ASSUMED_DEVICE_DBU if self.device_dbu is None else float(self.device_dbu)

    @property
    def assumed(self) -> bool:
        return self.device_dbu is None

    def render_kwargs(self) -> dict:
        """Keyword arguments for ``sawblade_core.render``: legacy forces the calibration-off render, calibrated forces on."""
        kw: dict = {"calibration": self.mode}
        if self.calibrated and self.device_dbu is not None:
            kw["device_dbu"] = float(self.device_dbu)
        return kw

    def record(self, di_channels: dict | None = None) -> dict:
        """The block written to result.json (``calibration``) and the run summary."""
        rec = {"mode": self.mode, "default": DEFAULT_CALIBRATION,
               "deviceDbu": self.effective_dbu if self.calibrated else None,
               "deviceAssumed": self.assumed if self.calibrated else None,
               "deviceDbuGiven": self.device_dbu}
        if self.calibrated and self.assumed:
            rec["note"] = (f"device level not given: the assumed +{ASSUMED_DEVICE_DBU:g} dBu at 0 dBFS was used; "
                           "pass --device-dbu with the interface's instrument-input level for a calibrated match")
        elif not self.calibrated and self.device_dbu is not None:
            rec["note"] = "--device-dbu has no effect with --calibration legacy"
        if di_channels is not None:
            rec["diChannel"] = di_channels
        return rec


def _rms_dbfs(x: np.ndarray) -> float | None:
    if len(x) == 0:
        return None
    ms = float(np.mean(np.square(x, dtype=np.float64)))
    return 10.0 * math.log10(ms) if ms > 0 else None


def pick_di_channel(x: np.ndarray, rule: str = "auto") -> tuple[np.ndarray, dict]:
    """One mono channel of a DI file, by the core's offline rule. Returns ``(mono float32, info)`` with ``info`` =
    ``{"rule", "used", "fileChannels", "rmsDbfsL", "rmsDbfsR"}`` (the last two None for a mono file or silence).
    ``auto``: the louder of channels 0/1 by whole-file RMS (a tie, or two silent channels, picks L); ``mix``: their mean.
    Channels beyond the second are dropped, as in the core."""
    if rule not in DI_CHANNEL_RULES:
        raise ValueError(f"DI channel rule must be one of {DI_CHANNEL_RULES}, got {rule!r}")
    x = np.asarray(x)
    if x.ndim == 1 or x.shape[1] == 1:
        mono = x if x.ndim == 1 else x[:, 0]
        return np.ascontiguousarray(mono, dtype=np.float32), {"rule": rule, "used": "mono", "fileChannels": 1,
                                                              "rmsDbfsL": None, "rmsDbfsR": None}
    left, right = x[:, 0], x[:, 1]
    ms_l = float(np.mean(np.square(left, dtype=np.float64)))
    ms_r = float(np.mean(np.square(right, dtype=np.float64)))
    used = rule if rule != "auto" else ("R" if ms_r > ms_l else "L")
    mono = left if used == "L" else right if used == "R" else 0.5 * (left.astype(np.float32) + right.astype(np.float32))
    return np.ascontiguousarray(mono, dtype=np.float32), {
        "rule": rule, "used": used, "fileChannels": int(x.shape[1]),
        "rmsDbfsL": _rms_dbfs(left), "rmsDbfsR": _rms_dbfs(right)}


def options_from_run(res: dict, preset: dict) -> CalibrationOptions:
    """The calibration a finished run used, so pathcheck / dynsweep re-render with the same one: result.json's record, else the
    emitted preset's ``calibration.mode`` (v5), else legacy (every older result)."""
    rec = res.get("calibration") or {}
    mode = rec.get("mode") or (preset.get("calibration") or {}).get("mode") or "legacy"
    dbu = rec.get("deviceDbuGiven")
    return CalibrationOptions(mode if mode in CALIBRATION_MODES else "legacy", dbu if isinstance(dbu, (int, float)) else None)


def di_rule_from_run(res: dict) -> str:
    """The DI-channel rule the run was made with (``auto`` for results that predate it)."""
    r = ((res.get("calibration") or {}).get("diChannel") or {}).get("di", {}).get("rule")
    return r if r in DI_CHANNEL_RULES else "auto"
