"""Export planning: mode rules, refusals, what is bypassed, training-chain derivation, attribution.

Pure functions on preset dicts (schema ``sawblade.preset`` v1); nothing here renders or trains.
"""
from __future__ import annotations

import copy
import hashlib
import json
from dataclasses import dataclass, field

MODES = ("nocab", "withcab")

# Cab modes that are live-compatible (one combined IR, so the no-cab export is exact). ``irMix`` is one convolver on
# ``(1-mix)*irA + mix*irB``; the cab fold renders through the core, so it needs no special handling here.
LIVE_CAB_MODES = frozenset({"shared", "irMix"})

# Block types the Python side knows to be NAM-trainable.  The C++ registry is authoritative (its
# ``namTrainable`` trait shows up as a "not NAM-trainable" render warning, see ``core_trainability_problems``);
# a type that is neither here nor accepted by the core is refused.
# every block type the core registry declares NAM-trainable (docs/PRESET_SCHEMA.md block table): captures, EQ and the modeled pedals
TRAINABLE_TYPES = frozenset({"nam", "eq", "pedal.hm", "pedal.muff", "pedal.ts", "pedal.hmx", "pedal.eye"})
MAX_TRAINABLE_RELEASE_MS = 150.0   # core kBusCompMaxTrainableReleaseMs

STUDIO_MESSAGE = "studio blend (per-path cab IRs): only the with-cab export is exact for studio blends"
NC_PREFIX = "cc-by-nc"   # cc-by-nc, cc-by-nc-sa, cc-by-nc-nd: allowed, but exports are marked NON-COMMERCIAL


def is_nc(license_: str | None) -> bool:
    return (license_ or "").lower().startswith(NC_PREFIX)


def nc_captures(preset: dict) -> list[str]:
    """Titles (or file names) of the non-commercially licensed captures in use."""
    out = []
    for role, cap in captures(preset):
        src = cap.get("source") or {}
        if is_nc(src.get("license")):
            out.append(f"{src.get('title') or (cap.get('file') or role).rsplit('/', 1)[-1]} ({src.get('license')})")
    return list(dict.fromkeys(out))


def licence_note(preset: dict) -> str:
    """Note stored in the .nam, the report and printed by the CLI (CLAUDE.md supersedes the phase 4 spec's string)."""
    note = ("Derived from TONE3000 captures; for the user's personal use only; sharing needs permission from the "
            "creators and TONE3000.")
    nc = nc_captures(preset)
    if nc:
        note += " NON-COMMERCIAL: contains non-commercially licensed captures: " + "; ".join(nc) + "."
    return note


class ExportRefused(Exception):
    """The requested export cannot / must not be done. ``str(e)`` is the message shown to the user."""

    def __init__(self, message: str, reasons: list[str] | None = None):
        super().__init__(message)
        self.reasons = reasons or [message]


@dataclass
class Plan:
    mode: str
    allow_inexact: bool
    bypassed: list[dict] = field(default_factory=list)   # things removed from the training chain (reported)
    inexact: list[dict] = field(default_factory=list)    # introduced error sources (only with --allow-inexact)
    warnings: list[str] = field(default_factory=list)
    exact: bool = True
    dynamics: str | None = None   # "live" | "record": the dynamics set the export follows; None when the preset has no dynamicsMode

    def to_json(self) -> dict:
        d = {"mode": self.mode, "allowInexact": self.allow_inexact, "exact": self.exact,
             "bypassed": self.bypassed, "inexact": self.inexact, "warnings": self.warnings}
        if self.dynamics is not None:          # absent key: the JSON is exactly what it was before Task G
            d["dynamics"] = self.dynamics
        return d


def flatten_dynamics(preset: dict) -> tuple[dict, str | None]:
    """(preset to plan / train / validate, label). A preset with a ``dynamicsMode`` key follows its ACTIVE dynamics set (Task
    G): the core resolver (``sawblade_match.core.resolve_dynamics``, the one the render path and the plugin use) gives the gate
    and bus comp of the set ``dynamicsMode`` selects, which replace the stored ones; ``liveDynamics`` is dropped and the mode
    becomes "record" so the engine runs exactly those objects as the one set (the training chain bypasses the gate, the bus
    comp follows the usual export rules). The label is "live" / "record" for the notes. A preset without the key is returned
    unchanged with label None (old files and the parity fixtures stay byte-identical). Call once on a raw preset."""
    if "dynamicsMode" not in preset:
        return preset, None
    from ..core import resolve_dynamics
    r = resolve_dynamics(preset)
    p = copy.deepcopy(preset)
    p["gate"], p["busComp"] = r["gate"], r["busComp"]
    p.pop("liveDynamics", None)
    p["dynamicsMode"] = "record"
    return p, "live" if r["mode"] == "live" else "record"


def _blocks(preset: dict):
    for key in ("a", "b"):
        path = preset["paths"][key]
        for blk in path.get("blocks", []):
            yield key, path, blk


def captures(preset: dict) -> list[tuple[str, dict]]:
    """(role, capture) for every capture in use: ``path.<id>`` NAM models and the cab IR(s)."""
    out = []
    for key, path, blk in _blocks(preset):
        if blk.get("type") == "nam" and blk.get("model"):
            out.append((f"{blk.get('slot') or 'nam'}:{key}:{blk['id']}", blk["model"]))
    cab = preset.get("cab", {})
    if cab.get("mode") == "shared" and cab.get("ir"):
        out.append(("cab", cab["ir"]))
    elif cab.get("mode") in ("perPath", "irMix"):
        for k in ("irA", "irB"):
            if cab.get(k):
                out.append((f"cab:{k}", cab[k]))
    return out


def make_plan(preset: dict, mode: str, allow_inexact: bool = False) -> Plan:
    if mode not in MODES:
        raise ExportRefused(f"unknown mode {mode!r} (expected one of {', '.join(MODES)})")
    reasons: list[str] = []
    preset, dyn = flatten_dynamics(preset)         # the active dynamics set (no-op without a dynamicsMode key)
    plan = Plan(mode=mode, allow_inexact=allow_inexact, dynamics=dyn)

    # non-trainable blocks
    for key, path, blk in _blocks(preset):
        if blk.get("bypass") or not path.get("enabled", True):
            continue
        if blk.get("type") not in TRAINABLE_TYPES:
            reasons.append(f"block {blk.get('id')!r} (type {blk.get('type')!r}) is not NAM-trainable")

    gate = preset.get("gate") or {}
    if gate.get("enabled"):
        plan.bypassed.append({"what": "gate", "why": "the gate is never trained into a NAM model (bypassed in the "
                              "training chain)", "original": {k: gate[k] for k in sorted(gate)}})

    cab = preset.get("cab") or {}
    comp = preset.get("busComp") or {}
    comp_on = bool(comp.get("enabled"))
    release = float(comp.get("releaseMs", 100.0))

    if mode == "nocab":
        if cab.get("mode") not in LIVE_CAB_MODES:
            reasons.insert(0, STUDIO_MESSAGE)
        if comp_on:
            msg = ("bus compressor is enabled; it sits after the cab and is nonlinear, so a no-cab export cannot be "
                   "exact (turn it off, or pass --allow-inexact to drop it and report the error)")
            if allow_inexact:
                plan.exact = False
                plan.inexact.append({"what": "busComp", "why": "dropped from the no-cab export (sits after the cab)",
                                     "original": dict(comp)})
                plan.bypassed.append({"what": "busComp", "why": "after the cab, nonlinear; dropped (--allow-inexact)"})
                plan.warnings.append("busComp dropped: the export is inexact; see validation for the error introduced")
            else:
                reasons.append(msg)
        if cab.get("enabled", True) is False:
            plan.warnings.append("cab.enabled is false: the exported IR contains only the post EQ")
    else:  # withcab
        if comp_on and release > MAX_TRAINABLE_RELEASE_MS:
            reasons.append(f"busComp releaseMs {release:g} > {MAX_TRAINABLE_RELEASE_MS:g} ms is not NAM-trainable "
                           "(long-release compression must not be trained into a NAM model)")
        if cab.get("mode") == "perPath":
            plan.warnings.append("studio blend (per-path IRs): the with-cab export is exact; no-cab is not available")

    if reasons:
        raise ExportRefused("; ".join(dict.fromkeys(reasons)), list(dict.fromkeys(reasons)))
    return plan


NON_TONE_KEYS = ("name", "notes", "export", "playAlong", "category")


def _tone_hash(preset: dict, drop_comp: bool) -> str:
    q = {k: v for k, v in preset.items() if k not in NON_TONE_KEYS and not (drop_comp and k == "busComp")}
    if isinstance(q.get("output"), dict):      # the plugin clears the derived auto trim from the exported preset
        q["output"] = {k: v for k, v in q["output"].items() if k not in ("autoTrimDb", "autoTrimHash")}
    return preset_hash(q)


def notes_preset_problems(trained: dict, notes: dict, mode: str = "nocab") -> list[str]:
    """Why ``notes`` (the preset the export notes are written from) cannot stand in for ``trained`` (the preset that is
    trained). They may differ only in the bus comp (a no-cab "drop" export trains a copy with the comp off) and in
    non-tone keys (name, notes, export, playAlong, category). Empty list = consistent."""
    out = []
    if mode != "nocab":
        out.append("--notes-preset is only for no-cab exports (it lists a bus comp dropped from the model)")
    if _tone_hash(trained, True) != _tone_hash(notes, True):
        out.append("the notes preset differs from the trained preset beyond the bus comp (and name/notes/export/"
                   "playAlong/category): it must be the same rig")
    elif (trained.get("busComp") or {}).get("enabled") and \
            json.dumps(trained.get("busComp"), sort_keys=True) != json.dumps(notes.get("busComp"), sort_keys=True):
        out.append("the trained preset keeps its bus comp but the notes preset has different bus comp settings")
    return out


def notes_only(trained: dict, notes: dict) -> list[dict]:
    """Stages listed in the export notes that the trained model does not contain only because the caller switched them
    off in the trained copy (today: the bus comp). Does not change validation or the reference."""
    nc, tc = notes.get("busComp") or {}, trained.get("busComp") or {}
    if nc.get("enabled") and not tc.get("enabled"):
        return [{"what": "busComp", "why": "switched off in the trained preset; listed in the export notes with its settings",
                 "original": dict(nc)}]
    return []


def level_match_info(render_report: dict) -> dict | None:
    """Trims and make-up the core measured (phase 10.1 render report: ``levelMatch`` and ``blend.makeupDb``); None for
    older reports without them."""
    lm = render_report.get("levelMatch")
    if not isinstance(lm, dict):
        return None
    bl = render_report.get("blend")
    bl = bl if isinstance(bl, dict) else {}
    return {"mode": lm.get("mode"), "trimADb": lm.get("trimADb"), "trimBDb": lm.get("trimBDb"),
            "blendLaw": bl.get("law"), "makeupDb": bl.get("makeupDb")}


def level_match_lines(render_report: dict) -> list[str]:
    """Console lines for the trims / make-up baked into the trained signal (empty when the report has none)."""
    info = level_match_info(render_report)
    if info is None:
        return []
    f = lambda x: "n/a" if x is None else f"{x:+.1f} dB"
    line = f"level match ({info['mode']}): A {f(info['trimADb'])}, B {f(info['trimBDb'])}"
    if info["blendLaw"]:
        line += f"; blend law {info['blendLaw']}"
    out = [line]
    if info["makeupDb"]:
        out.append("  make-up at blend 0/.25/.5/.75/1: " + ", ".join(f"{m:+.1f}" for m in info["makeupDb"]) + " dB")
    return out


def _audible_paths(preset: dict) -> list[str]:
    """Paths whose signal reaches the output: enabled, and with a non-zero blend weight."""
    b = float(preset.get("blend", 0.0) or 0.0)
    out = []
    for k in ("a", "b"):
        if (preset.get("paths", {}).get(k) or {}).get("enabled", True) is False:
            continue
        if (k == "a" and b >= 1.0) or (k == "b" and b <= 0.0):
            continue
        out.append(k)
    return out


def _path_out_ref(plan_blocks: list[dict], device_dbu: float) -> tuple[float, bool]:
    """(dBu at 0 dBFS after the path's last planned block, known?) from the core report's per-block plan. A block that sets
    the reference is a NAM capture or a modelled pedal with a declared output: ``captureOutputDbu``. When that block lacks
    the output level the reference is not known (the core carried the previous one), so it is not guessed."""
    ref, known = float(device_dbu), True
    for b in plan_blocks:
        if b.get("kind") in ("nam", "nominalOutput"):
            if b.get("captureOutputDbu") is not None:
                ref, known = float(b["captureOutputDbu"]), True
            else:
                known = False
    return ref, known


def reference_levels(preset: dict, render_report: dict, level_reduced_db: float = 0.0) -> dict:
    """The NAM trainer's ``input_level_dbu`` / ``output_level_dbu`` (REPORT A1: dBu RMS of a 1 kHz sine at 0 dBFS peak, the
    level that corresponds to 0 dBFS at the model's input / output) for the training render.

    Written ONLY when that render was calibrated (``render_report["calibration"]``, the core report of the training chain):
    ``input_level_dbu`` = ``calibration.deviceDbu`` (the DI's 0 dBFS in that render); ``output_level_dbu`` = the output
    reference of the chain, the planned ``refOutDbu`` of the last block of the audible path(s) (a blend whose paths end at
    different references has no single value), plus ``level_reduced_db`` when the reamp output had to be scaled down for
    24-bit. An uncalibrated render gets no dBu fields (never a guess) and ``notes`` says so; an assumed device level says which
    interface / loader level to set. Returns ``{"calibrated", "mode", "deviceDbu", "deviceAssumed", "inputLevelDbu",
    "outputLevelDbu", "notes"}``."""
    cal = render_report.get("calibration") if isinstance(render_report, dict) else None
    mode = (cal or {}).get("mode")
    out = {"calibrated": False, "mode": mode, "deviceDbu": None, "deviceAssumed": None, "inputLevelDbu": None,
           "outputLevelDbu": None, "notes": []}
    if not cal or mode != "calibrated" or cal.get("deviceDbu") is None:
        why = ("the core report has no calibration record" if not cal else
               f"the training render was not calibrated (calibration mode {mode})")
        out["notes"].append(f"input_level_dbu / output_level_dbu are NOT written: {why}. The model has no analog level "
                            "reference, so set the loader's input and output levels by ear / meter. Re-match or export a preset "
                            "with calibration.mode \"calibrated\" to get them.")
        return out
    dev = float(cal["deviceDbu"])
    out.update(calibrated=True, deviceDbu=dev, deviceAssumed=bool(cal.get("deviceAssumed")), inputLevelDbu=dev)
    notes = out["notes"]
    refs = []
    for k in _audible_paths(preset):
        r, known = _path_out_ref((cal.get("paths") or {}).get(k) or [], dev)
        refs.append((k, r, known))
    if not refs:
        notes.append("output_level_dbu is NOT written: no audible path in the chain.")
    elif not all(known for _, _, known in refs):
        miss = ", ".join(k.upper() for k, _, known in refs if not known)
        notes.append(f"output_level_dbu is NOT written: the last capture of path {miss} has no output level (dBu) metadata, so "
                     "the chain's output reference is unknown.")
    elif max(r for _, r, _ in refs) - min(r for _, r, _ in refs) > 1e-9:
        notes.append("output_level_dbu is NOT written: the blended paths end at different output references ("
                     + ", ".join(f"{k.upper()} {r:+.1f} dBu" for k, r, _ in refs) + "), so there is no single value.")
    else:
        out["outputLevelDbu"] = refs[0][1] + float(level_reduced_db)
        if level_reduced_db:
            notes.append(f"output_level_dbu includes +{level_reduced_db:.2f} dB because the reamp output was scaled down to fit 24-bit.")
    if cal.get("anyUncalibrated"):
        notes.append("some captures in the chain lack input or output dBu metadata; those blocks were driven neutrally (0 dB) in "
                     "the training render.")
    if out["deviceAssumed"]:
        notes.append(f"The interface level was assumed (+{dev:g} dBu at 0 dBFS), not given. On a loader or plugin that does not "
                     f"read input_level_dbu / output_level_dbu set its input calibration level to {dev:+g} dBu, and its output "
                     f"calibration to match your interface; if your interface's real level differs from {dev:+g} dBu the "
                     "model is driven by the difference.")
    return out


def core_trainability_problems(report: dict) -> list[str]:
    """Render-report warnings that mean the C++ registry flags a block / the bus comp as not NAM-trainable."""
    return [w for w in report.get("warnings", []) if "not NAM-trainable" in w]


def training_preset(preset: dict, plan: Plan) -> dict:
    """The chain the model learns: gate always off; ``nocab`` additionally removes cab, post EQ and bus comp
    (post EQ and cab are re-created by the exported IR). Output gain stays (it is a pre-cab scalar: exact)."""
    p = copy.deepcopy(preset)
    if "gate" in p:
        p["gate"]["enabled"] = False
    if plan.mode == "nocab":
        p["cab"]["enabled"] = False
        p["postEq"] = []
        if "busComp" in p:
            p["busComp"]["enabled"] = False
    p["name"] = f"{preset.get('name', 'preset')} [{plan.mode} training chain]"
    return p


def reference_preset(preset: dict, plan: Plan) -> dict:
    """What the exported model (+IR) is validated against: the original with only the gate bypassed (and, in
    ``--allow-inexact`` runs, the bus comp left on so its dropped contribution shows up in the error)."""
    p = copy.deepcopy(preset)
    if "gate" in p:
        p["gate"]["enabled"] = False
    return p


def folding_preset(preset: dict) -> dict:
    """Preset that is only ``cab -> post EQ`` (empty paths, blend 0, align off, no gate/comp, 0 dB output)."""
    cab = copy.deepcopy(preset["cab"])
    return {"schema": "sawblade.preset", "version": 1, "name": "cab+postEq fold",
            "paths": {"a": {"role": "saw", "blocks": []}, "b": {"role": "body", "blocks": []}},
            "align": {"mode": "off"}, "blend": 0.0, "cab": cab,
            "postEq": copy.deepcopy(preset.get("postEq", [])), "output": {"gainDb": 0.0}}


def preset_hash(preset: dict) -> str:
    """sha256 of the canonical preset JSON with machine-specific ``file`` paths removed (captures are identified by
    their ``sha256`` / TONE3000 ids), so the hash is the same on any machine."""
    def strip(o):
        if isinstance(o, dict):
            return {k: strip(v) for k, v in o.items() if k != "file"}
        if isinstance(o, list):
            return [strip(v) for v in o]
        return o
    blob = json.dumps(strip(preset), sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()


def attribution(preset: dict) -> list[dict]:
    """Deduplicated capture attribution list (title, creator, licence, TONE3000 URL) for every capture used."""
    seen: dict[tuple, dict] = {}
    for role, cap in captures(preset):
        src = cap.get("source") or {}
        key = (src.get("provider"), src.get("id"), src.get("modelId")) if src else ("file", cap.get("file"), None)
        ent = seen.setdefault(key, {
            "title": src.get("title"), "creator": src.get("creator"), "license": src.get("license"),
            "url": src.get("url"), "provider": src.get("provider"), "toneId": src.get("id"),
            "modelId": src.get("modelId"), "sha256": cap.get("sha256"), "roles": []})
        if is_nc(src.get("license")):
            ent["nonCommercial"] = True
        if not src:
            ent["title"] = ent["title"] or (cap.get("file") or "").rsplit("/", 1)[-1]
            ent["license"] = ent["license"] or "unknown"
        ent["roles"].append(role)
    return list(seen.values())


def gear_type(plan: Plan, preset: dict) -> str:
    """NAM GearType enum value for the metadata."""
    if drive_only(preset, plan):
        return "pedal"
    return "amp_pedal_cab" if plan.mode == "withcab" else "pedal_amp"


def drive_only(preset: dict, plan: "Plan | dict") -> bool:
    """True when the exported model is a drive / boost stage and not an amp: a no-cab export whose enabled chain has
    at least one non-EQ block and EVERY such block is explicitly a pedal or boost (``slot`` "pedal" / "boost", or a modeled
    pedal type ``pedal.*``); an unlabelled NAM block, an ``amp`` / ``fx`` slot or any other type makes it an amp.  A with-cab export
    contains the cab, so it is always amp-like.  Used to pick Neural Pedal over Neural Amp in the Anagram notes."""
    if (plan["mode"] if isinstance(plan, dict) else plan.mode) != "nocab":
        return False
    seen = False
    for _key, path, blk in _blocks(preset):
        if blk.get("bypass") or not path.get("enabled", True):
            continue
        if blk.get("type") == "eq":
            continue
        seen = True
        explicit = blk.get("slot") in ("pedal", "boost") or str(blk.get("type", "")).startswith("pedal.")
        if not explicit:                 # positive rule: an unlabelled NAM block (or any other slot) counts as an amp
            return False
    return seen


def sawblade_block(preset: dict, plan: Plan, size: str, seed: int, signal_seed: int, signal_sha256: str,
                   levels: dict, ir_file: str | None, arch: str | None = None,
                   training_signal: str | None = None) -> dict:
    """The ``metadata.sawblade`` block written into the ``.nam``: provenance, mode, attribution and the licence note.
    ``arch`` (a2 exports only) is appended as the last key; A1 blocks are unchanged."""
    blk = _sawblade_block(preset, plan, size, seed, signal_seed, signal_sha256, levels, ir_file)
    if arch is not None:
        blk["arch"] = arch
    if training_signal is not None:
        blk["trainingSignal"] = training_signal      # "nam-standard v3.0.0" | "sawblade-synthetic v<N>"
    return blk


def _sawblade_block(preset: dict, plan: Plan, size: str, seed: int, signal_seed: int, signal_sha256: str,
                    levels: dict, ir_file: str | None) -> dict:
    return {"exporter": "sawblade-export",
            "preset": {"name": preset.get("name"), "sha256": preset_hash(preset)},
            "exportMode": plan.mode, "exact": plan.exact, "size": size,
            "gateBypassed": True, "bypassed": [b["what"] for b in plan.bypassed],
            "seed": seed, "signalSeed": signal_seed, "signalSha256": signal_sha256, "levels": levels,
            "ir": ir_file, "attribution": attribution(preset), "licenceNote": licence_note(preset),
            "nonCommercial": bool(nc_captures(preset))}
