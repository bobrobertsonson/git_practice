"""Gain ladders: the models of one TONE3000 tone that are the same amp at different gain settings.

``parse_ladder`` is pure and conservative: it returns ``None`` ("no ladder") for anything it
cannot prove is a single-knob sweep. It never guesses.
"""
from __future__ import annotations

import re
from typing import NamedTuple, Sequence

from .client import ARCH_A2, T3KClient
from .ids import require_id
from .types import Model

MAX_GAIN = 100.0   # a "gain" above this is a year / model number / percentage, not a knob position


class Rung(NamedTuple):
    model_id: int
    gain: float
    name: str


_NUM = r"(\d+(?:\.\d+)?)"
# keyword = gain | drive | g (a lone g glued to the number, or after a separator)
_KW = r"(?:gain|drive|g)"
_PATTERNS = (
    re.compile(rf"(?<![a-z0-9]){_KW}[\s_-]*[=:]?[\s_-]*{_NUM}(?![\d.]*\d)(?![a-z])"),   # Gain 6, G6, gain=6, Gain-06, g_6
    re.compile(rf"(?<![a-z0-9.]){_NUM}\s*{_KW}(?![a-z0-9])"),                      # 6 gain, 6g, 6.5 drive
    re.compile(rf"@\s*{_NUM}(?![\d.]*\d)(?![a-z])"),                               # @7
)


def _gain_and_rest(name: str) -> tuple[float, str] | None:
    """The single gain number in ``name`` and the normalised remainder, or None."""
    s = name.lower()
    spans: list[tuple[int, int, str]] = []
    for pat in _PATTERNS:
        for m in pat.finditer(s):
            # "5150 Gain 6": the keyword already belongs to the number after it, so a number-first
            # reading that overlaps an earlier (keyword-first) match is not a second gain token.
            if pat is _PATTERNS[1] and any(m.start() < en and st < m.end() for st, en, _ in spans):
                continue
            spans.append((m.start(), m.end(), m.group(1)))
    if not spans:
        return None
    spans.sort()
    merged = [spans[0]]
    for st, en, num in spans[1:]:
        lo, hi, n0 = merged[-1]
        if st < hi:                                   # overlapping reads of the same token
            if num != n0:
                return None
            merged[-1] = (lo, max(hi, en), n0)
        else:
            merged.append((st, en, num))
    if len(merged) != 1:                              # two gain-like tokens: ambiguous
        return None
    st, en, num = merged[0]
    gain = float(num)
    if gain > MAX_GAIN:
        return None
    rest = re.sub(r"[^a-z0-9]+", " ", s[:st] + " " + s[en:]).strip()
    return gain, rest


def parse_ladder(models: Sequence[Model], size: str, architecture: str | None = None) -> list[Rung] | None:
    """Gain ladder of ``models`` restricted to ``size`` (and ``architecture`` when given), sorted by gain.

    ``None`` unless: at least two such models; every one yields exactly one gain number from its
    name; the names are identical once that number token is removed; the gains are distinct.
    """
    cand = [m for m in models
            if m.size == size and (architecture is None or m.architecture_version == architecture)]
    if len(cand) < 2:
        return None
    parsed: list[tuple[Model, float, str]] = []
    for m in cand:
        r = _gain_and_rest(m.name)
        if r is None:
            return None
        parsed.append((m, r[0], r[1]))
    if len({rest for _, _, rest in parsed}) != 1:
        return None
    gains = [g for _, g, _ in parsed]
    if len(set(gains)) != len(gains) or len({m.id for m, _, _ in parsed}) != len(parsed):
        return None
    return sorted((Rung(m.id, g, m.name) for m, g, _ in parsed), key=lambda r: r.gain)


def gain_ladder(client: T3KClient, tone_id: int | str, size: str,
                architecture: str = ARCH_A2) -> list[Rung] | None:
    """List the tone's models for ``architecture`` and parse the gain ladder (None = no ladder)."""
    models = client.list_models(require_id(tone_id, "tone id"), architecture)
    return parse_ladder(models, size, architecture)
