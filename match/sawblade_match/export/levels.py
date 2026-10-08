"""Digital gain between the last level-defining block of a path and the audio a NAM model is trained on (v0.8 I4a follow-up).

``output_level_dbu`` of the exported model = (the path's output reference ``refOutDbu``) - (every digital gain after it) +
(the 24-bit scale-down of the reamp output). The gain after the reference is measured, not summed from preset fields, so that
no term is forgotten: the training chain is rendered by the C++ core with everything up to and including the last NAM block
bypassed, the other path off, and a 1 kHz sine (the field's own definition: a 1 kHz sine's RMS) is driven through it; the lock-in
gain at 1 kHz is the answer. That covers the path's tone stack and LEVEL, later EQ blocks, path EQ, path level, trims, the output
gain, post EQ and a cab IR (with-cab mode; core-normalised IR at 1 kHz). The last NAM block's own out gain (outputGainDb,
makeupDb, normalizeLoudness) is added from its fields (a bypassed block passes the signal untouched).

Anything that is not linear or not exactly knowable returns ``(None, reason)`` so the caller writes no ``output_level_dbu``.
The trainer's -18 dBFS output normalisation is not a term: it is applied for learning and undone on export. Verified in
neural-amp-modeler 0.13.0: ``nam/data.py`` ``Dataset.handshake`` (~line 602) adds ``_ScaleOutputHook(scale=1/y_scale)``, which
``ExportableMixin.export`` (``exportable.py:114``) and ``PackedWaveNet.export_container`` (``_packed_wavenet.py:170``) apply.
The global ``input.gainDb`` is not a term either (it is before every capture, so the model learns it).
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

import numpy as np

RATE = 48000
SINE_HZ = 1000.0
SINE_PEAK = 0.03           # -30 dBFS peak: far from clipping; the measured chain is linear and time-invariant
SINE_SECONDS = 5.0         # the longest IR the core loads is 2 s
# The pinned trainer's NormalizeJointDatasetOutput(-18 dBFS) is undone by its export hook (train.py header, official.py "joint hook").
# Verified in 0.13.0 (see the module docstring); if a trainer version ever stops doing this, set False: output_level_dbu is then not written.
TRAINER_EXPORT_UNDOES_OUTPUT_NORMALISATION = True


def _amp_index(blocks: list[dict]) -> int:
    """``sawblade::ampIndex``: the last block with slot "amp", else the last nam block, else -1."""
    for i in range(len(blocks) - 1, -1, -1):
        if blocks[i].get("slot") == "amp":
            return i
    for i in range(len(blocks) - 1, -1, -1):
        if blocks[i].get("type") == "nam":
            return i
    return -1


def _model_loudness(block: dict, base_dir) -> tuple[float | None, str | None]:
    """(``metadata.loudness`` of the block's .nam or None when it has none, problem)."""
    f = (block.get("model") or {}).get("file")
    if not f:
        return None, "the last capture's model file is not named"
    p = Path(f)
    p = p if p.is_absolute() else Path(base_dir) / p
    try:
        md = json.loads(p.read_text()).get("metadata") or {}
    except (OSError, ValueError):
        return None, f"cannot read {p.name} to get its loudness (normalizeLoudness is on)"
    v = md.get("loudness")
    return (float(v) if isinstance(v, (int, float)) else None), None


def lockin_gain_db(y: np.ndarray, peak: float = SINE_PEAK, hz: float = SINE_HZ, rate: int = RATE) -> float:
    """RMS gain (dB) at ``hz`` over the last whole second of ``y`` for a sine of peak ``peak`` (projection on sin/cos)."""
    n = rate  # 1 s = an integer number of cycles of 1 kHz
    seg = np.asarray(y[-n:], dtype=np.float64)
    t = np.arange(len(seg)) / rate
    c = np.mean(seg * np.exp(-2j * np.pi * hz * t))
    return float(20 * np.log10(max(2.0 * abs(c) / peak, 1e-30)))


def post_gain_db(tpreset: dict, path: str, probe: dict, base_dir, cache, render, last_id: str | None) -> tuple[float | None, str | None]:
    """Digital gain (dB, at 1 kHz) after the output reference of audible ``path`` in the training chain ``tpreset``.
    ``probe``: the core report of ``tpreset`` (trims, make-up). ``last_id``: id of the path's last NAM block (None: none).
    Returns ``(gain, None)`` or ``(None, "the term that is missing")``. ``render(preset, x, base_dir, cache)`` is ``chain.render48``."""
    p = tpreset.get("paths", {}).get(path) or {}
    blocks = p.get("blocks") or []
    if (tpreset.get("busComp") or {}).get("enabled"):
        return None, "the bus compressor is on in the training chain (level-dependent gain)"
    last = -1
    if last_id is not None:
        ids = [b.get("id") for b in blocks]
        if last_id not in ids:
            return None, f"the report's last capture {last_id!r} is not a block of the preset"
        last = ids.index(last_id)
    for i, b in enumerate(blocks):
        if i > last and not b.get("bypass") and b.get("type") != "eq":
            return None, f"block {b.get('id')!r} ({b.get('type')}) follows the last capture and is not linear"
    for b in blocks:
        if (b.get("model") or {}).get("ladder"):
            return None, "the amp block has a gain ladder (its rung residual is a level term that is not reported)"
    own = 0.0
    if last >= 0:
        lb = blocks[last]
        if lb.get("type") != "nam":
            return None, f"the last level-defining block is a modelled pedal ({lb.get('type')}): its output level term is not reported"
        own = float(lb.get("outputGainDb", 0.0)) + float(lb.get("makeupDb", 0.0))
        if lb.get("normalizeLoudness"):
            loud, prob = _model_loudness(lb, base_dir)
            if prob:
                return None, prob
            if loud is not None:
                own += -18.0 - loud
    both = all((tpreset.get("paths", {}).get(k) or {}).get("enabled", True) is not False for k in ("a", "b"))
    blend = float(tpreset.get("blend", 0.0) or 0.0)
    lm = probe.get("levelMatch") if isinstance(probe.get("levelMatch"), dict) else {}
    lm_on = lm.get("mode") not in (None, "off")
    law = (probe.get("blend") or {}).get("law")
    makeup = 0.0       # digital terms the probe chain cannot carry itself (it disables the partner path, which zeroes them)
    if both and (lm_on or law == "constantLoudness"):      # the core's rule for when the blend make-up applies
        mk = (probe.get("blend") or {}).get("makeupDb")
        if not (isinstance(mk, list) and len(mk) == 5):
            return None, "the blend make-up is not in the core report"
        makeup = float(mk[0] if blend <= 0.0 else mk[4])   # only the end points are reachable with a single audible path
    if both and lm_on:            # the audible path's level-match trim (the core zeroes trims once the partner is disabled)
        t = lm.get("trimADb" if path == "a" else "trimBDb")
        if t is None:
            return None, "the level-match trims are not in the core report"
        makeup += float(t)
    e = copy.deepcopy(tpreset)
    e["gate"] = {"enabled": False}
    if "busComp" in e:
        e["busComp"] = {**e["busComp"], "enabled": False}
    other = "b" if path == "a" else "a"
    e["paths"][other] = {"role": (e["paths"].get(other) or {}).get("role", "body"), "enabled": False, "blocks": []}
    ep = e["paths"][path]
    if last >= 0:
        ep.pop("preEq", None)         # before the capture: not a post term (with no capture it is one, and linear)
    if "input" in e:
        e["input"] = {**e["input"], "gainDb": 0.0}      # the global input gain is before every capture: the model learns it
    for i, b in enumerate(ep.get("blocks") or []):
        if i <= last:
            b["bypass"] = True
    ac = ep.get("ampControls")
    if ac:
        if _amp_index(ep["blocks"]) < last:
            ep.pop("ampControls")                 # the tone stack sits before the last capture: not a post term
        else:
            ac["gain"] = 5.0                      # the drive knob is before the amp: not a post term
            ac.pop("gainStep", None)
    e.pop("levelMatch", None)         # trims are added above; the partner is off, so the core would apply none anyway
    e["align"] = {"mode": "off"}
    t = np.arange(int(SINE_SECONDS * RATE)) / RATE
    x = (SINE_PEAK * np.sin(2 * np.pi * SINE_HZ * t)).astype(np.float32)
    try:
        y, _ = render(e, x, base_dir, cache)
    except Exception as ex:                       # noqa: BLE001 - a core refusal means "not measurable", never a wrong number
        return None, f"the post-capture gain could not be rendered ({ex})"
    g = lockin_gain_db(y)
    if not np.isfinite(g) or g < -200:
        return None, "the post-capture chain renders silent"
    return g + own + makeup, None
