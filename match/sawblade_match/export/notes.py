"""Export notes: every enabled stage of the preset that is NOT in the trained NAM model, in signal order, with its
settings in hardware units, so it can be rebuilt around a loader pedal.

Pure functions on the preset dict (schema ``sawblade.preset``) and the export plan (a ``plan.Plan`` or its
``to_json()`` dict); nothing here renders, trains or needs the C++ core.

Signal order: gate -> [pre-NAM stages] -> NAM -> cab IR (no-cab export) -> post EQ -> bus comp -> output.
The output gain is a pre-cab scalar that the no-cab export keeps inside the model, so it is never listed; it only
shifts the bus comp threshold reference (see ``_comp``).
"""
from __future__ import annotations

from pathlib import Path

NOTES_VERSION = 1
BEFORE, AFTER = "before NAM", "after NAM"
NOTHING = "Nothing to add: the trained model (and its IR, if any) contains the whole chain."
DISCLAIMER = ("Derived from TONE3000 captures; for personal use only. dB figures are digital (dBFS), "
              "so match levels by ear / meter on the device.")


def _g(x, nd: int = 2) -> str:
    """Compact number: 3 -> '3', 0.5 -> '0.5', -61.0 -> '-61'."""
    return f"{float(x):.{nd}f}".rstrip("0").rstrip(".") or "0"


def _plan_dict(plan) -> dict:
    return plan if isinstance(plan, dict) else plan.to_json()


def _capture_info(cap: dict | None) -> dict:
    cap = cap or {}
    src = cap.get("source") or {}
    out = {"file": (cap.get("file") or "").replace("\\", "/").rsplit("/", 1)[-1] or None,
           "title": src.get("title"), "creator": src.get("creator"), "license": src.get("license"),
           "provider": src.get("provider"), "toneId": src.get("id"), "url": src.get("url")}
    mic = src.get("mic") or src.get("mics")
    if mic:
        out["mic"] = mic
    return {k: v for k, v in out.items() if v is not None}


def _cap_label(ci: dict) -> str:
    name = ci.get("title") or ci.get("file") or "unnamed IR"
    bits = []
    if ci.get("file") and ci.get("title") and ci["file"] != ci["title"]:
        bits.append(f"file {ci['file']}")
    if ci.get("creator"):
        bits.append(f"by {ci['creator']}")
    if ci.get("license"):
        bits.append(ci["license"])
    if ci.get("toneId") is not None:
        bits.append(f"{ci.get('provider') or 'source'} id {ci['toneId']}")
    if ci.get("mic"):
        m = ci["mic"]
        bits.append("mic " + (", ".join(map(str, m)) if isinstance(m, (list, tuple)) else str(m)))
    return name + (f" ({'; '.join(bits)})" if bits else "")


def _eq_bands(bands) -> list[dict]:
    out = []
    for b in bands or []:
        if b.get("enabled", True) is False:
            continue
        t = b.get("type")
        e = {"type": t, "freqHz": float(b["freq"]), "q": float(b.get("q", 0.707))}
        if t in ("highPass", "lowPass"):
            e["slopeDbPerOct"] = 12
        else:
            e["gainDb"] = float(b.get("gainDb", 0.0))
        out.append(e)
    return out


def _eq_line(e: dict) -> str:
    t = e["type"]
    if "slopeDbPerOct" in e:
        name = "high-pass" if t == "highPass" else "low-pass"
        return f"{name} {_g(e['freqHz'])} Hz, {e['slopeDbPerOct']} dB/oct (Q {_g(e['q'], 3)})"
    name = {"peak": "peak", "lowShelf": "low shelf", "highShelf": "high shelf"}.get(t, str(t))
    return f"{name} {_g(e['freqHz'])} Hz, {e['gainDb']:+.1f} dB, Q {_g(e['q'], 3)}"


def _pre_eq_info(preset: dict) -> list[dict]:
    """Informational lines for the path pre-EQs (DI -> gate -> pre-EQ -> blocks): linear and before the amp, so they are
    trained into the NAM model. Not a stage to add on hardware (``inModel`` true, position "inside NAM")."""
    out = []
    for key, path in sorted((preset.get("paths") or {}).items()):
        if not isinstance(path, dict) or path.get("enabled", True) is False:
            continue
        bands = _eq_bands(path.get("preEq"))
        if bands:
            out.append({"stage": "preEq", "path": key, "position": "inside NAM", "inModel": True,
                        "settings": {"bands": bands},
                        "hardware": "pre-EQ (before the amp): trained into the model, nothing to add: "
                                    + "; ".join(_eq_line(e) for e in bands) + "."})
    return out


def _gate(g: dict) -> dict:
    thr = float(g.get("thresholdDb", -55.0))
    hyst = float(g.get("hysteresisDb", 6.0))
    s = {"mode": g.get("mode", "gate"), "thresholdDb": thr, "closeThresholdDb": thr - hyst, "hysteresisDb": hyst,
         "attackMs": float(g.get("attackMs", 0.5)), "holdMs": float(g.get("holdMs", 20.0)),
         "releaseMs": float(g.get("releaseMs", 60.0)), "rangeDb": float(g.get("rangeDb", -90.0)),
         "keyedOn": "DI (guitar signal before any pedal/amp)"}
    if s["mode"] == "expander":
        s["ratio"] = float(g.get("ratio", 4.0))
    if g.get("keyHighPassHz"):
        s["keyHighPassHz"] = float(g["keyHighPassHz"])
    if g.get("releaseCurve"):
        s["releaseCurve"] = g["releaseCurve"]
    kind = "Expander" if s["mode"] == "expander" else "Gate"
    h = (f"{kind} FIRST in the chain, keyed on the guitar (DI) before any pedal: open at {_g(thr)} dB, close at "
         f"{_g(s['closeThresholdDb'])} dB (hysteresis {_g(hyst)} dB), attack {_g(s['attackMs'])} ms, hold "
         f"{_g(s['holdMs'])} ms, release {_g(s['releaseMs'])} ms, range {_g(s['rangeDb'])} dB")
    if "ratio" in s:
        h += f", ratio {_g(s['ratio'])}:1"
    if "keyHighPassHz" in s:
        h += f", key high-pass {_g(s['keyHighPassHz'])} Hz"
    return {"stage": "gate", "position": BEFORE, "inModel": False, "settings": s, "hardware": h + "."}


def _cab(cab: dict, ir_name: str | None) -> dict:
    cm = cab.get("mode")
    s: dict = {"cabMode": cm, "normalize": cab.get("normalize", True)}
    if cm == "irMix":
        a, b = _capture_info(cab.get("irA")), _capture_info(cab.get("irB"))
        mix = float(cab.get("mix", 0.5))
        s.update({"irA": a, "irB": b, "mix": mix})
        # offsetSamplesB / invertB: core hook added in v0.4M B2.1 (docs/specs/v0_4m-tasks.md); read when present.
        for k in ("offsetSamplesB", "invertB"):
            if k in cab:
                s[k] = cab[k]
        h = (f"Load the cab IR: two mic IRs mixed into one, {_g((1 - mix) * 100)}% A + {_g(mix * 100)}% B "
             f"(A = {_cap_label(a)}; B = {_cap_label(b)})")
        extra = []
        if cab.get("offsetSamplesB"):
            extra.append(f"B offset {cab['offsetSamplesB']} samples")
        if cab.get("invertB"):
            extra.append("B polarity inverted")
        if extra:
            h += "; " + ", ".join(extra)
    else:
        ir = _capture_info(cab.get("ir"))
        s["ir"] = ir
        h = f"Load the cab IR {_cap_label(ir)}"
    if ir_name:
        s["exportedIr"] = ir_name
        h += (f". Easiest: load the exported {ir_name} instead; it already holds this cab (and the post EQ) as one "
              "IR, mono 48 kHz 32-bit float, loaded WITHOUT loudness normalisation")
    return {"stage": "cab", "position": AFTER, "inModel": False, "settings": s, "hardware": h + "."}


def _post_eq(bands: list[dict], folded: bool, ir_name: str | None) -> dict:
    h = "Post EQ after the cab: " + "; ".join(_eq_line(e) for e in bands)
    if folded:
        h += (f". Already folded into the exported IR ({ir_name or 'the .ir.wav'}); add it again only if you do not "
              "use that IR")
    return {"stage": "postEq", "position": AFTER, "inModel": False,
            "settings": {"bands": bands, "foldedIntoExportedIr": folded}, "hardware": h + "."}


def _comp(c: dict, out_gain_db: float, gain_before: bool) -> dict:
    thr = float(c.get("thresholdDb", -12.0))
    s = {"thresholdDb": thr, "thresholdReference": "pre-headroom chain level (the 6 dB sum headroom is not counted)",
         "ratio": float(c.get("ratio", 2.0)), "attackMs": float(c.get("attackMs", 10.0)),
         "releaseMs": float(c.get("releaseMs", 100.0)), "kneeDb": float(c.get("kneeDb", 6.0)),
         "makeupDb": float(c.get("makeupDb", 0.0)), "detector": "peak, feed-forward, soft knee"}
    # The compressor sees the level before the output gain; the no-cab export has that gain baked in before the IR.
    thr_out = thr + (out_gain_db if gain_before else 0.0)
    s["thresholdDbFsOut"] = thr_out
    h = (f"Bus compressor LAST (after the cab / post EQ): threshold {_g(thr)} dB re the chain's pre-headroom level = "
         f"{_g(thr_out)} dB re 0 dBFS at the exported output, ratio {_g(s['ratio'])}:1, attack {_g(s['attackMs'])} ms, "
         f"release {_g(s['releaseMs'])} ms, knee {_g(s['kneeDb'])} dB, make-up {s['makeupDb']:+.1f} dB, peak detector")
    if gain_before and out_gain_db:
        h += f" (the output gain of {out_gain_db:+.1f} dB is already inside the model)"
    return {"stage": "busComp", "position": AFTER, "inModel": False, "settings": s, "hardware": h + "."}


def build_export_notes(preset: dict, plan, nam_name: str | None = None, ir_name: str | None = None) -> dict:
    """``exportNotes`` for ``preset`` exported under ``plan`` (a ``Plan`` or its JSON). ``nam_name`` / ``ir_name`` are
    the exported file names, used in the loader-order line."""
    pj = _plan_dict(plan)
    nocab = pj.get("mode") == "nocab"
    bypassed = {b.get("what") for b in pj.get("bypassed", [])}
    stages: list[dict] = []

    gate = preset.get("gate")
    if isinstance(gate, dict) and gate.get("enabled", True):                       # the gate is never trained
        stages.append(_gate(gate))

    cab = preset.get("cab") or {}
    eq = _eq_bands(preset.get("postEq"))
    out_gain = float((preset.get("output") or {}).get("gainDb", 0.0))
    if nocab:
        if cab.get("enabled", True) is not False:
            stages.append(_cab(cab, ir_name))
        if eq:
            stages.append(_post_eq(eq, True, ir_name))
    comp = preset.get("busComp") or {}
    if comp.get("enabled") and (nocab or "busComp" in bypassed):
        stages.append(_comp(comp, out_gain, gain_before=nocab))

    have = {s["stage"] for s in stages}
    parts = ["gate"] if "gate" in have else []
    parts.append(f"NAM ({nam_name})" if nam_name else "NAM model")
    if "cab" in have:
        parts.append(f"cab IR + post EQ ({ir_name})" if ir_name and "postEq" in have else
                     (f"cab IR ({ir_name})" if ir_name else "cab IR"))
        if "postEq" in have and not ir_name:
            parts[-1] = "cab IR -> post EQ"
    elif "postEq" in have:
        parts.append("post EQ")
    if "busComp" in have:
        parts.append("bus comp")
    notes = {"version": NOTES_VERSION, "mode": pj.get("mode"), "stages": stages,
             "loaderOrder": "Loader order: " + " -> ".join(parts)}
    if not stages:
        notes["message"] = NOTHING
    inside = _pre_eq_info(preset)
    if inside:
        notes["inModel"] = inside
    return notes


def format_notes_txt(notes: dict, preset_name: str | None = None, licence_note: str | None = None) -> str:
    """Human text for ``<name>.export_notes.txt`` (the same content as ``exportNotes``)."""
    L = [f"Sawblade export notes{f' - {preset_name}' if preset_name else ''} ({notes.get('mode')} export)", "",
         "Stages of the preset that are NOT in the trained model, in signal order:", ""]
    if not notes["stages"]:
        L += [notes.get("message", NOTHING), ""]
    for i, st in enumerate(notes["stages"], 1):
        L += [f"{i}. {st['stage']} [{st['position']}]", f"   {st['hardware']}", ""]
    for st in notes.get("inModel", []):
        L += [f"In the model (nothing to add) - path {st['path']}: {st['hardware']}", ""]
    L.append(notes["loaderOrder"])
    L += ["", licence_note or DISCLAIMER]
    return "\n".join(L) + "\n"


# ------------------------------------------------------------------ device profile: Darkglass Anagram (v0.6)
#
# ``exportNotes.deviceProfiles.anagram`` maps the generic stages onto the Anagram's PUBLISHED block list only (Neural Amp /
# Neural Pedal / Neural Loader, IR, compressor, gate, EQ; up to three NAM blocks).  Nothing here claims anything about the
# device's internals, its controls' scales or firmware behaviour: values are Sawblade's own (digital dBFS, ms, Hz) and
# the text says to match them by ear / meter.  Key layout (plugin reader: ``plugin/src/ExportNotes.cpp``
# ``anagramProfileOf``): {device, file, message, loaderOrder (string), stages[{stage, block, position, settings{},
# hardware}]}.
ANAGRAM_DEVICE = "Anagram"
MATCH_BY_EAR = "Values are Sawblade's (digital dBFS / ms / Hz): match levels by ear or meter on the device."


def _pos(i: int, n: int) -> str:
    return f"{i} (first in the chain)" if i == 1 and n > 1 else f"{i} (last in the chain)" if i == n and n > 1 else str(i)


def _anagram_gate(st: dict) -> dict:
    g = st["settings"]
    s = {"mode": g["mode"], "threshold dB": g["thresholdDb"], "close threshold dB": g["closeThresholdDb"],
         "attack ms": g["attackMs"], "hold ms": g["holdMs"], "release ms": g["releaseMs"], "range dB": g["rangeDb"],
         "keyed on": "guitar input (the signal before any pedal or amp)"}
    if "ratio" in g:
        s["ratio"] = g["ratio"]
    if "keyHighPassHz" in g:
        s["key high-pass Hz"] = g["keyHighPassHz"]
    return {"stage": "gate", "block": "Gate", "settings": s,
            "hardware": "Put the gate FIRST in the chain, before every NAM block, so it hears the guitar. " + MATCH_BY_EAR}


def _anagram_comp(st: dict) -> dict:
    c = st["settings"]
    s = {"threshold dBFS": c["thresholdDbFsOut"], "ratio": c["ratio"], "attack ms": c["attackMs"],
         "release ms": c["releaseMs"], "knee dB": c["kneeDb"], "make-up dB": c["makeupDb"], "detector": "peak"}
    return {"stage": "busComp", "block": "Compressor", "settings": s,
            "hardware": ("Put the compressor LAST, after the IR. The threshold is in dBFS at the exported output "
                         "level (the preset's output gain is already inside the model). " + MATCH_BY_EAR)}


def build_anagram_profile(preset: dict, plan, notes: dict, nam_name: str, ir_name: str | None = None,
                          drive_only: bool = False, model_label: str | None = None, file: str | None = None) -> dict:
    """The ``anagram`` device profile for ``notes`` (the generic ``exportNotes`` of the same export).

    Block mapping, in signal order: gate -> Gate block (first, before the NAM block); the trained model -> Neural Amp
    block (Neural Pedal when ``drive_only``: a no-cab export with no amp in the chain); the no-cab export's cab IR -> IR
    block right after the model (the exported IR also holds the post EQ, so no EQ block is needed then); a post EQ that is
    NOT folded into the IR -> EQ block after the IR; the bus comp -> Compressor block last.  Gate, model, IR, EQ and
    compressor each take one block, so the chain is at most 5 blocks with one NAM block."""
    by = {st["stage"]: st for st in notes["stages"]}
    blocks: list[dict] = []
    if "gate" in by:
        blocks.append(_anagram_gate(by["gate"]))
    kind = "Neural Pedal" if drive_only else "Neural Amp"
    label = f" ({model_label})" if model_label else ""
    blocks.append({"stage": "model", "block": kind, "settings": {"model": nam_name, "bypass": False},
                   "hardware": f"Load {nam_name}{label} from file into the {kind} block (a Neural block, not the TONE3000 block). "
                               "It is a local file for your own use: do not upload it to TONE3000 (models trained from "
                               "TONE3000 captures need the creators' permission to share). Up to three NAM blocks (Neural "
                               "Amp / Neural Pedal / Neural Loader) can run at once. Set the block's levels so the output "
                               "level matches the plugin by ear or meter."})
    cab, peq = by.get("cab"), by.get("postEq")
    if cab or (peq and peq["settings"].get("foldedIntoExportedIr")):
        folded = bool(peq and peq["settings"].get("foldedIntoExportedIr"))
        s = {"file": ir_name or "(cab IR)", "normalise": False}
        text = "Load the IR WITHOUT loudness normalisation."
        if ir_name:
            s["contains"] = "cab and post EQ" if folded else "cab"
            text += " The file already holds the " + ("cab and the post EQ" if folded else "cab") + "."
        else:
            text += " " + (cab or {}).get("hardware", "")
        blocks.append({"stage": "cab", "block": "IR", "settings": s, "hardware": text.strip()})
    if peq and not peq["settings"].get("foldedIntoExportedIr"):
        blocks.append({"stage": "postEq", "block": "EQ",
                       "settings": {f"band {i}": _eq_line(b) for i, b in enumerate(peq["settings"]["bands"], 1)},
                       "hardware": "Put the EQ after the IR. " + MATCH_BY_EAR})
    if "busComp" in by:
        blocks.append(_anagram_comp(by["busComp"]))
    n = len(blocks)
    for i, b in enumerate(blocks, 1):
        b["position"] = _pos(i, n)
        b.setdefault("hardware", "")
    out = [{"stage": b["stage"], "block": b["block"], "position": b["position"], "settings": b["settings"],
            **({"hardware": b["hardware"]} if b["hardware"] else {})} for b in blocks]
    prof = {"device": ANAGRAM_DEVICE, "message": "Blocks to set on the device, in signal order. Needs KosmOS 1.16 or later.", "stages": out,
            "loaderOrder": "Anagram chain: " + " -> ".join(b["block"] for b in blocks)}
    if file:
        prof["file"] = file
    return prof


def format_anagram_txt(profile: dict, preset_name: str | None = None, licence_note: str | None = None) -> str:
    """Text for ``<name>.anagram_notes.txt``; the same layout as the plugin's own rendering of the profile."""
    L = [f"Sawblade export notes for the Anagram{f' - {preset_name}' if preset_name else ''}", "",
         "Blocks to set on the device (KosmOS 1.16 or later), in signal order:", ""]
    for i, st in enumerate(profile["stages"], 1):
        L.append(f"{i}. {st['block']} [{st['position']}]" + (f"  ({st['stage']})" if st["stage"] != st["block"] else ""))
        for k in sorted(st["settings"]):
            v = st["settings"][k]
            L.append(f"   {k}: {'yes' if v is True else 'no' if v is False else _g(v, 3) if isinstance(v, (int, float)) else v}")
        if st.get("hardware"):
            L.append(f"   {st['hardware']}")
        L.append("")
    L += [profile["loaderOrder"], "", licence_note or DISCLAIMER]
    return "\n".join(L) + "\n"


def write_export_notes(preset: dict, plan, nam_path, ir_path=None, licence_note: str | None = None,
                       stem: str | None = None, model_label: str | None = None) -> tuple[dict, Path]:
    """Build the notes for the export at ``nam_path`` and write ``<stem>.export_notes.txt`` (generic) and
    ``<stem>.anagram_notes.txt`` (Anagram device profile) next to it; ``stem`` defaults to the .nam's stem (A1)."""
    from . import plan as P
    nam_path = Path(nam_path)
    stem = stem or nam_path.stem
    notes = build_export_notes(preset, plan, nam_path.name, Path(ir_path).name if ir_path else None)
    txt = nam_path.with_name(stem + ".export_notes.txt")
    txt.write_text(format_notes_txt(notes, preset.get("name"), licence_note), encoding="utf-8")
    notes["file"] = txt.name
    drive = P.drive_only(preset, plan)
    atxt = nam_path.with_name(stem + ".anagram_notes.txt")
    prof = build_anagram_profile(preset, plan, notes, nam_path.name, Path(ir_path).name if ir_path else None,
                                 drive_only=drive, model_label=model_label, file=atxt.name)
    atxt.write_text(format_anagram_txt(prof, preset.get("name"), licence_note), encoding="utf-8")
    notes["deviceProfiles"] = {"anagram": prof}
    return notes, txt
