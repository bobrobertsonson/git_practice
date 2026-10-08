"""Benchmark manifest (``docs/benchmark/cases.json``): loading and validation. Paths are relative to a root the user chooses."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

SCHEMA = "sawblade.bench.cases"
VERSION = 1
KINDS = ("single", "blend", "pathcheck")
CHANNELS = ("left", "right", "mono")
TOPOLOGIES = ("auto", "single", "blend")
POLARITIES = ("auto", "asis", "invert-a", "invert-b")
ID_RE = re.compile(r"^[a-z0-9_]+$")
DEFAULT_MANIFEST = Path(__file__).resolve().parents[3] / "docs" / "benchmark" / "cases.json"
BUILTIN_DEFAULTS = {"diChannel": "left", "offsetMs": 0.0, "topology": "auto", "heldOutMaxS": 30.0}


class ManifestError(ValueError):
    """Unreadable or invalid manifest."""


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_relative_path(p) -> bool:
    if not isinstance(p, str) or not p:
        return False
    if p.startswith(("/", "\\", "~")) or re.match(r"^[A-Za-z]:", p):
        return False
    return ".." not in re.split(r"[\\/]", p)


def _section(v, where: str):
    if v is None:
        return None
    if (not isinstance(v, (list, tuple)) or len(v) != 2 or not all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v)
            or not 0 <= v[0] < v[1]):
        raise ManifestError(f"{where}: a section is [startS, endS] with 0 <= start < end, got {v!r}")
    return [float(v[0]), float(v[1])]


def _file(f, where: str, default_channel: str) -> dict:
    if not isinstance(f, dict) or not is_relative_path(f.get("path")):
        raise ManifestError(f"{where}: needs a relative 'path' (no absolute path, no '..'), got {f!r}")
    ch = f.get("channel", default_channel)
    if ch not in CHANNELS:
        raise ManifestError(f"{where}: channel must be one of {CHANNELS}, got {ch!r}")
    return {"path": f["path"], "channel": ch}


def _reference(ref, kind: str, where: str, default_channel: str = "left") -> dict:
    if kind == "blend":
        tracks = ref.get("tracks") if isinstance(ref, dict) else None
        if not isinstance(tracks, list) or len(tracks) != 2:
            raise ManifestError(f"{where}: a blend reference needs 'tracks' with exactly two entries")
        out = []
        for t in tracks:
            if not isinstance(t, dict) or not is_relative_path(t.get("path")) or t.get("role") not in ("a", "b"):
                raise ManifestError(f"{where}: each track needs a relative 'path' and role 'a' or 'b', got {t!r}")
            g = t.get("gainDb", 0.0)
            if isinstance(g, bool) or not isinstance(g, (int, float)):
                raise ManifestError(f"{where}: gainDb must be a number, got {g!r}")
            out.append({"path": t["path"], "role": t["role"], "gainDb": float(g)})
        if sorted(t["role"] for t in out) != ["a", "b"]:
            raise ManifestError(f"{where}: the two tracks need roles a and b")
        pol = ref.get("polarity", "auto")
        if pol not in POLARITIES:
            raise ManifestError(f"{where}: polarity must be one of {POLARITIES}, got {pol!r}")
        return {"tracks": out, "polarity": pol}
    return _file(ref, where, default_channel)


def validate(m: dict) -> dict:
    """Validate and return a normalised copy (defaults merged into every case). Raises ManifestError."""
    if not isinstance(m, dict) or m.get("schema") != SCHEMA:
        raise ManifestError(f"not a benchmark manifest (schema must be {SCHEMA!r})")
    if m.get("version") != VERSION:
        raise ManifestError(f"unsupported manifest version {m.get('version')!r} (this runner reads {VERSION})")
    defaults = {**BUILTIN_DEFAULTS, **(m.get("defaults") or {})}
    cases_in = m.get("cases")
    if not isinstance(cases_in, list) or not cases_in:
        raise ManifestError("'cases' must be a non-empty list")
    ids = [c.get("id") if isinstance(c, dict) else None for c in cases_in]
    for i in ids:
        if not isinstance(i, str) or not ID_RE.match(i):
            raise ManifestError(f"case id {i!r} must match [a-z0-9_]+")
    dup = sorted({i for i in ids if ids.count(i) > 1})
    if dup:
        raise ManifestError(f"duplicate case ids: {dup}")
    kinds = {c["id"]: c.get("kind") for c in cases_in}
    cases = []
    for c in cases_in:
        cid, where = c["id"], f"case {c['id']}"
        kind, tier = c.get("kind"), c.get("tier")
        if kind not in KINDS:
            raise ManifestError(f"{where}: kind must be one of {KINDS}, got {kind!r}")
        if tier not in (1, 2):
            raise ManifestError(f"{where}: tier must be 1 or 2, got {tier!r}")
        n = {"id": cid, "tier": tier, "kind": kind, "style": str(c.get("style", "")), "session": str(c.get("session", "")),
             "counts": bool(c.get("counts", False)), "confirmed": bool(c.get("confirmed", True)), "notes": str(c.get("notes", ""))}
        if kind == "pathcheck":
            ref = c.get("reference")
            if tier != 1 or not isinstance(ref, dict) or ref.get("path") not in ("a", "b") or not isinstance(ref.get("parent"), str):
                raise ManifestError(f"{where}: a pathcheck is tier 1 with reference {{parent, path: 'a'|'b'}}")
            if kinds.get(ref["parent"]) != "blend":
                raise ManifestError(f"{where}: parent {ref['parent']!r} does not exist or is not a blend case")
            if n["counts"]:
                raise ManifestError(f"{where}: a pathcheck never counts")
            n["reference"] = {"parent": ref["parent"], "path": ref["path"]}
            cases.append(n)
            continue
        n["di"] = _file(c.get("di"), f"{where} di", defaults["diChannel"])
        n["reference"] = _reference(c.get("reference"), kind, f"{where} reference")
        n["offsetMs"] = c["offsetMs"] if "offsetMs" in c else defaults["offsetMs"]
        if n["offsetMs"] is not None and (isinstance(n["offsetMs"], bool) or not isinstance(n["offsetMs"], (int, float))):
            raise ManifestError(f"{where}: offsetMs must be a number or null")
        n["offsetMs"] = None if n["offsetMs"] is None else float(n["offsetMs"])
        n["fit"], n["heldOut"] = _section(c.get("fit"), f"{where} fit"), _section(c.get("heldOut"), f"{where} heldOut")
        if n["fit"] and n["heldOut"] and not (n["fit"][1] <= n["heldOut"][0] or n["heldOut"][1] <= n["fit"][0]):
            raise ManifestError(f"{where}: fit {n['fit']} and heldOut {n['heldOut']} overlap")
        n["heldOutMaxS"] = float(c.get("heldOutMaxS", defaults["heldOutMaxS"]))
        n["topology"] = c.get("topology", defaults["topology"])
        if n["topology"] not in TOPOLOGIES:
            raise ManifestError(f"{where}: topology must be one of {TOPOLOGIES}")
        if tier == 2 and n["counts"]:
            raise ManifestError(f"{where}: a tier 2 case never counts")
        tr = []
        for k, t in enumerate(c.get("transfer") or []):
            tr.append({"di": _file(t.get("di"), f"{where} transfer[{k}] di", defaults["diChannel"]),
                       "reference": _reference(t.get("reference"), kind, f"{where} transfer[{k}] reference")})
        n["transfer"] = tr
        cases.append(n)
    gaps = m.get("gaps") or []
    if not all(isinstance(g, dict) and "style" in g and "note" in g for g in gaps):
        raise ManifestError("'gaps' entries need 'style' and 'note'")
    return {"schema": SCHEMA, "version": VERSION, "rootHint": str(m.get("rootHint", "")), "defaults": defaults,
            "cases": cases, "gaps": list(gaps)}


def load_manifest(path: str | Path) -> dict:
    try:
        raw = json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        raise ManifestError(f"cannot read manifest {path}: {e}") from e
    return validate(raw)


def case_files(case: dict) -> list[dict]:
    """Every file a case reads: ``[{what, path}]`` (the root-relative paths), transfer pairs included. Path checks have none."""
    out: list[dict] = []
    if case["kind"] == "pathcheck":
        return out

    def ref_files(ref, prefix):
        if "tracks" in ref:
            for t in ref["tracks"]:
                out.append({"what": f"{prefix}track {t['role']}", "path": t["path"]})
        else:
            out.append({"what": f"{prefix}reference", "path": ref["path"]})
    out.append({"what": "di", "path": case["di"]["path"]})
    ref_files(case["reference"], "")
    for k, t in enumerate(case.get("transfer") or []):
        out.append({"what": f"transfer[{k}] di", "path": t["di"]["path"]})
        ref_files(t["reference"], f"transfer[{k}] ")
    return out
