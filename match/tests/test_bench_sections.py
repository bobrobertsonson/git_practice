"""v0.7: automatic fit / held-out sections (spec A.2)."""
from __future__ import annotations

import numpy as np
import pytest

from sawblade_match.bench import sections as S

FS = 48000


def burst_signal(lead_s=3.0, active_s=40.0, tail_s=3.0, seed=0):
    rng = np.random.default_rng(seed)
    n = int((lead_s + active_s + tail_s) * FS)
    x = (1e-5 * rng.standard_normal(n)).astype(np.float32)                       # -100 dB noise floor
    a, b = int(lead_s * FS), int((lead_s + active_s) * FS)
    env = 0.05 + 0.2 * (np.sin(2 * np.pi * np.arange(b - a) / (FS * 5.0)) > 0)   # a dense half-second-ish pattern, louder in bursts
    x[a:b] += (env * rng.standard_normal(b - a)).astype(np.float32)
    return x


def test_active_region_finds_the_playing_inside_the_noise_floor():
    x = burst_signal()
    a, b = S.active_region(x, FS)
    assert abs(a / FS - 3.0) < 0.2 and abs(b / FS - 43.0) < 0.2


def test_auto_sections_first_half_fit_densest_30s_of_the_second_half_deterministic():
    x = burst_signal()
    r = S.resolve(x, FS, None, None, 30.0)
    assert r == S.resolve(x, FS, None, None, 30.0)
    (f0, f1), (h0, h1) = r["fit"], r["heldOut"]
    assert abs(f0 - 3.0) < 0.2 and abs(f1 - 23.0) < 0.3                              # first half of the 3-43 s active region
    assert f1 <= h0 and h1 <= 43.3 and h0 >= f1 - 1e-9
    assert h1 - h0 == pytest.approx(20.0, abs=0.3)                                   # the second half is shorter than 30 s: all of it
    assert r["source"] == {"fit": "auto", "heldOut": "auto"}
    long = burst_signal(active_s=120.0)
    r2 = S.resolve(long, FS, None, None, 30.0)
    assert r2["heldOut"][1] - r2["heldOut"][0] == pytest.approx(30.0, abs=0.1) and r2["heldOut"][0] >= r2["fit"][1]


def test_pinned_sections_are_kept_and_the_other_is_chosen_outside_them():
    x = burst_signal()
    r = S.resolve(x, FS, [4.0, 14.0], None, 10.0)
    assert r["fit"] == [4.0, 14.0] and r["heldOut"][0] >= 14.0 and r["source"] == {"fit": "manifest", "heldOut": "auto"}
    r = S.resolve(x, FS, None, [30.0, 40.0], 10.0)
    assert r["heldOut"] == [30.0, 40.0] and r["fit"][1] <= 30.0 and r["source"]["fit"] == "auto"
    assert S.resolve(x, FS, [0, 6], [6, 12], 30.0)["source"] == {"fit": "manifest", "heldOut": "manifest"}


def test_too_short_material_is_an_error():
    with pytest.raises(ValueError, match="shorter"):
        S.resolve(burst_signal(active_s=3.0), FS, None, None, 30.0)
