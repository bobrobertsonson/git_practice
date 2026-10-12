"""Gear-class classifier (no title filters in the search itself; the classes only organise pre-screening quotas).

Pedals: ``fuzz``, ``distortion``, ``drive``, ``preamp`` or ``pedal_unknown`` (unknown pedals are offered to every pedal
slot like any other pedal; the class only decides which quota they compete in). Amps: ``amp_low`` / ``amp_high`` from
the gain heuristic in pool.gain_class plus a short list of high-gain families. Cabs: ``cab``.

Order of evidence: the model name first, then the tone title (packs such as "BOOST PEDAL PACK" hold models of several
kinds, so the name must win). Within a text the first matching rule wins: fuzz, preamp, distortion, drive.
"""
from __future__ import annotations

import re

_FUZZ = re.compile(r"fuzz|\bmuff\b|big ?muff|tone ?bender|rangemaster|\bfz-?\d|octavia|sun ?face", re.I)
_PREAMP = re.compile(r"sans ?amp|tech ?21|pre-?amp|amp[- ]in[- ]a[- ]box|\bgt-?\d|\bpsa\b|\bv2\b pre", re.I)
_DIST = re.compile(r"hm-?2|heavy metal|\brat\b|\bds-?\d|\bmt-?\d|metal ?zone|distortion|\bdist\b|chainsaw|\bml-?\d|"
                   r"proco|\bturbo\b", re.I)
_DRIVE = re.compile(r"\bts-?\d*|tube ?screamer|screamer|\bod\b|\bod-?\d|overdrive|boost|klon|centaur|\bsd-?\d|"
                    r"blues ?driver|drive|booster|\bpush\b|badass", re.I)
_HIGH_AMPS = re.compile(r"5150|5153|6505|recto|diezel|savage|engl|uberschall|jvm|friedman|revv|bogner|dual|invective|"
                        r"herbert|vh4|slo|\bbe-?100|orange|sunn|emperor|krank|peavey|\bxxx\b", re.I)

PEDAL_CLASSES = ("drive", "distortion", "fuzz", "preamp", "pedal_unknown")
AMP_CLASSES = ("amp_low", "amp_high")


def _pedal_class(text: str) -> str | None:
    for cls, rx in (("fuzz", _FUZZ), ("preamp", _PREAMP), ("distortion", _DIST), ("drive", _DRIVE)):
        if rx.search(text):
            return cls
    return None


def classify(gear: str, title: str, name: str) -> str:
    if gear == "cab":
        return "cab"
    if gear == "pedal":
        return _pedal_class(name) or _pedal_class(title) or "pedal_unknown"
    from .pool import gain_class
    g = gain_class(title, name)
    if g == "high":
        return "amp_high"
    if g in ("low", "medium"):
        return "amp_low"
    return "amp_high" if _HIGH_AMPS.search(f"{title} {name}") else "amp_low"
