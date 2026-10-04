"""Export planning: mode rules, refusals, what is bypassed, training-chain derivation, attribution.

Pure functions on preset dicts (schema ``sawblade.preset`` v1); nothing here renders or trains.
"""
from __future__ import annotations

import copy
import hashlib
import json
from dataclasses import dataclass, field

MODES = ("nocab", "withcab")

# Block types the Python side knows to be NAM-trainable.  The C++ registry is authoritative (its
# ``namTrainable`` trait shows up as a "not NAM-trainable" render warning, see ``core_trainability_problems``);
# a type that is neither here nor accepted by the core is refused.
TRAINABLE_TYPES = frozenset({"nam", "eq"})
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

    def to_json(self) -> dict:
        return {"mode": self.mode, "allowInexact": self.allow_inexact, "exact": self.exact,
                "bypassed": self.bypassed, "inexact": self.inexact, "warnings": self.warnings}


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
    elif cab.get("mode") == "perPath":
        for k in ("irA", "irB"):
            if cab.get(k):
                out.append((f"cab:{k}", cab[k]))
    return out


def make_plan(preset: dict, mode: str, allow_inexact: bool = False) -> Plan:
    if mode not in MODES:
        raise ExportRefused(f"unknown mode {mode!r} (expected one of {', '.join(MODES)})")
    reasons: list[str] = []
    plan = Plan(mode=mode, allow_inexact=allow_inexact)

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
        if cab.get("mode") != "shared":
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
    return "amp_pedal_cab" if plan.mode == "withcab" else "pedal_amp"


def sawblade_block(preset: dict, plan: Plan, size: str, seed: int, signal_seed: int, signal_sha256: str,
                   levels: dict, ir_file: str | None) -> dict:
    """The ``metadata.sawblade`` block written into the ``.nam``: provenance, mode, attribution and the licence note."""
    return {"exporter": "sawblade-export",
            "preset": {"name": preset.get("name"), "sha256": preset_hash(preset)},
            "exportMode": plan.mode, "exact": plan.exact, "size": size,
            "gateBypassed": True, "bypassed": [b["what"] for b in plan.bypassed],
            "seed": seed, "signalSeed": signal_seed, "signalSha256": signal_sha256, "levels": levels,
            "ir": ir_file, "attribution": attribution(preset), "licenceNote": licence_note(preset),
            "nonCommercial": bool(nc_captures(preset))}
