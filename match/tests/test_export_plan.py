"""Export plan rules that need no sawblade_core (pure functions on preset dicts)."""
from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from sawblade_match.export import plan as P

REPO = Path(__file__).resolve().parents[2]
PRESETS = REPO / "tests" / "fixtures" / "presets"


def load(name: str) -> dict:
    return json.loads((PRESETS / f"{name}.json").read_text())


@pytest.fixture
def shared():
    p = load("golden_shared")
    p["busComp"]["enabled"] = False
    return p


def irmix_preset(base: dict, mix: float = 0.3) -> dict:
    p = copy.deepcopy(base)
    ir = lambda n: {"file": str((PRESETS.parent / "ir" / n).resolve())}
    p["cab"] = {"mode": "irMix", "irA": ir("ir_a.wav"), "irB": ir("ir_b.wav"), "mix": mix, "enabled": True,
                "normalize": True}
    return p


def test_irmix_is_live_compatible_nocab_allowed(shared):
    p = irmix_preset(shared)
    pl = P.make_plan(p, "nocab")
    assert pl.exact and not pl.inexact
    assert P.make_plan(p, "withcab").exact
    p["busComp"]["enabled"] = True                        # same rules as shared: comp still refuses a no-cab export
    with pytest.raises(P.ExportRefused, match="bus compressor"):
        P.make_plan(p, "nocab")


def test_irmix_captures_list_both_irs(shared):
    p = irmix_preset(shared)
    roles = [r for r, _ in P.captures(p)]
    assert "cab:irA" in roles and "cab:irB" in roles and "cab" not in roles
    caps = dict(P.captures(p))
    assert caps["cab:irA"]["file"].endswith("ir_a.wav") and caps["cab:irB"]["file"].endswith("ir_b.wav")


def test_perpath_still_refuses_nocab_and_lists_its_irs():
    p = load("golden_perpath")
    with pytest.raises(P.ExportRefused):
        P.make_plan(p, "nocab")
    assert {"cab:irA", "cab:irB"} <= {r for r, _ in P.captures(p)}
