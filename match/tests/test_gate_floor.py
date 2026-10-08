"""v0.4M Task H.1 (matcher side): the DI floor on the gate's own detector. Needs the core built with peak_floor_db."""
from __future__ import annotations

import numpy as np
import pytest
from scipy import signal

core = pytest.importorskip("sawblade_match.core", reason="sawblade_core not built")
if getattr(core, "_core", None) is None or not hasattr(core._core, "peak_floor_db"):
    pytest.skip("sawblade_core built before Task H.1 (no peak_floor_db)", allow_module_level=True)
from sawblade_match.matcher import known_answer as K                   # noqa: E402
from sawblade_match.matcher.engine import Engine                      # noqa: E402
from sawblade_match.matcher.gatesweep import DEFAULT_CELL, GATE_OFFSETS_DB, cell_gate   # noqa: E402
from sawblade_match.matcher.run import gate_envelope_floor_db, gate_floor          # noqa: E402
from sawblade_match.matcher.space import GATE_OPEN_OFFSET_DB, gate_preset          # noqa: E402
from test_matcher import fixture_pool                                  # noqa: E402

FS = 48000
NOISE_DB = -49.5            # the user's L DI: RMS floor (dBFS)


def white(seconds, rms_db, seed=1):
    return (np.random.default_rng(seed).standard_normal(int(seconds * FS)) * 10 ** (rms_db / 20)).astype(np.float32)


def pink(seconds, rms_db, seed=2):
    x = signal.lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400],
                       np.random.default_rng(seed).standard_normal(int(seconds * FS) + 4000))[4000:]
    return (x / np.sqrt(np.mean(x ** 2)) * 10 ** (rms_db / 20)).astype(np.float32)


def gain_db_per_frame(x, gate, live=False, frame_s=0.05):
    """Per-50 ms-frame gain (dB) of an empty chain with ``gate``, re the same chain with no gate."""
    eng = Engine(None, 1)
    try:
        pool = fixture_pool()
        p = eng.chain_preset([], pool.cabs[0], "a", gate)
        q = eng.chain_preset([], pool.cabs[0], "a", None)
        if live:                                         # a match preset: the core derives the floor-following live gate
            p["version"], p["origin"] = 4, "match"
        y, _ = eng.render(p, x, FS, dynamics="live" if live else "record")
        r, _ = eng.render(q, x, FS)
    finally:
        eng.close()
    n = int(frame_s * FS)
    nf = len(x) // n
    yy = np.asarray(y[:nf * n], np.float64).reshape(nf, n)
    rr = np.asarray(r[:nf * n], np.float64).reshape(nf, n)
    return 10 * np.log10(np.maximum(np.mean(yy ** 2, axis=1), 1e-30) / np.maximum(np.mean(rr ** 2, axis=1), 1e-30))


def test_peak_floor_of_noise_and_the_recorded_gate_floor():
    x = white(2.0, NOISE_DB)
    f = gate_floor(x, FS)
    assert f["peakDb"] == pytest.approx(-42.3, abs=0.4) and f["rmsDb"] == pytest.approx(NOISE_DB, abs=0.2)
    assert gate_envelope_floor_db(x, FS) == f["peakDb"]
    g = gate_preset(f["peakDb"])
    assert GATE_OPEN_OFFSET_DB == 10.0 and g["hysteresisDb"] == 6.0
    assert g["thresholdDb"] == pytest.approx(f["peakDb"] + 10.0, abs=0.01)
    assert DEFAULT_CELL[0] == 10.0 and 10.0 in GATE_OFFSETS_DB and GATE_OFFSETS_DB == (6.0, 8.0, 10.0, 12.0, 16.0, 20.0, 24.0, 28.0)
    assert cell_gate(f["peakDb"], DEFAULT_CELL[0]) == g
    old_open, old_close = NOISE_DB + 4.0, NOISE_DB + 4.0 - 6.0                 # the old rule: RMS floor + 4, hysteresis 6
    print(f"gate for a {NOISE_DB} dBFS RMS floor: old open {old_open:.2f} / close {old_close:.2f} dBFS; "
          f"new open {g['thresholdDb']:.2f} / close {g['thresholdDb'] - g['hysteresisDb']:.2f} dBFS (peak floor {f['peakDb']:.2f})")
    assert g["thresholdDb"] - g["hysteresisDb"] > f["peakDb"] + 3.0            # closes above the noise's own peaks


@pytest.mark.parametrize("kind", ["white", "pink"])
def test_record_gate_on_noise_alone_is_closed(kind):
    x = (white if kind == "white" else pink)(6.0, NOISE_DB)
    g = gate_preset(gate_floor(x, FS)["peakDb"])
    gain = gain_db_per_frame(x, g)
    frames = gain[int(0.5 / 0.05):]                                             # after 0.5 s
    closed = float(np.mean(frames <= g["rangeDb"] + 1.0))
    assert closed > 0.95, (kind, closed)


def _live_closed_fraction(noise_db, seconds=14.0, after_s=9.0):
    x = white(seconds, noise_db)
    g = gate_preset(gate_floor(x, FS)["peakDb"])
    gain = gain_db_per_frame(x, g, live=True)
    return float(np.mean(gain[int(after_s / 0.05):] <= -40.0 + 1.0)), float(np.median(gain[int(after_s / 0.05):]))


def test_live_gate_attenuates_noise_alone_once_the_follower_has_learned_the_floor():
    """Live set (origin match, floor-following EXPANDER, ratio 4, range -40 dB), noise at -70 dBFS RMS (peak floor ~ -63): once
    the follower has converged the noise is attenuated. NOTE for the lead: an expander does not close the way the
    record gate does; with open = floor + 10 the noise (peaks at the floor) sits only ~10 dB below the threshold, so it is
    attenuated by about 11 dB (printed), not driven to the -40 dB range; the 'closed > 95 %' criterion only holds for the record gate."""
    closed, median = _live_closed_fraction(-70.0)
    print(f"live gate on -70 dBFS RMS noise after 9 s: median gain {median:.1f} dB, {100 * closed:.0f} % of the frames at the range")
    assert median <= -10.0                         # attenuated by ~11 dB: the follower learned the floor
    assert closed < 0.5                            # ... but it is an expander, not a closed gate (documented, see above)


def test_report_live_follower_on_the_users_floor():
    """REPORT, no claim: the follower qualifies only frames below estimate + 20 dB and starts at -70 dBFS, so noise whose 50 ms peak
    statistic (about -42 dBFS for a -49.5 dBFS RMS floor) is more than 20 dB above the seed is not learned until the 10 s leak has
    raised the estimate (+1 dB/s). Prints how the live gate treats the user's floor over 30 s."""
    closed, median = _live_closed_fraction(NOISE_DB, seconds=30.0, after_s=25.0)
    print(f"live follower on a {NOISE_DB} dBFS RMS floor, 25-30 s: closed {100 * closed:.0f} % of the frames, median gain {median:.1f} dB")


def test_decay_tail_is_not_gated_while_the_note_is_well_above_the_floor():
    """A plucked note decaying from -12 dBFS into the noise floor: no attenuation while its envelope is > 12 dB above the peak
    floor; the level (re the floor) where the attenuation starts is reported."""
    tau = 0.25
    tt = np.arange(int(3.5 * FS)) / FS
    note = np.concatenate([np.zeros(int(0.5 * FS)), 10 ** (-12 / 20) * np.exp(-tt / tau) * np.sin(2 * np.pi * 110 * tt)])
    noise = white(4.0, NOISE_DB, seed=5)
    x = (noise + note).astype(np.float32)
    floor = gate_floor(noise, FS)["peakDb"]                                     # the matcher sees the DI's gaps; here the noise alone
    g = gate_preset(floor)
    gain = gain_db_per_frame(x, g, frame_s=0.02)
    n = int(0.02 * FS)
    t_frame = (np.arange(len(gain)) * n + n / 2) / FS
    env_db = -12.0 - 8.686 * np.maximum(t_frame - 0.5, 0.0) / tau               # the note's peak envelope (dBFS)
    played = t_frame >= 0.5
    well_above = played & (env_db > floor + 12.0)
    assert well_above.sum() > 5
    assert float(np.min(gain[well_above])) >= -1.0                              # attenuation <= 1 dB while > 12 dB above the floor
    starts = np.nonzero(played & (gain < -1.0))[0]
    assert len(starts)
    level = float(env_db[starts[0]] - floor)
    print(f"decay tail: attenuation > 1 dB starts when the note's envelope is {level:+.1f} dB re the peak floor "
          f"(floor {floor:.1f} dBFS, open {g['thresholdDb']:.1f}, close {g['thresholdDb'] - g['hysteresisDb']:.1f})")
    assert level <= 12.0


# ---- gate_floor: which samples the floor is taken from -----------------------------------------------------------------------
def _notes_over(noise, n_notes_period=0.3, ring=0.12, level_db=-12.0):
    """Plucks (110 Hz, exponential decay) every 0.3 s over ``noise``: no real 120 ms silence in it (the bed is above -50 dBFS)."""
    x = noise.astype(np.float64).copy()
    t = np.arange(int(ring * FS)) / FS
    for k in range(int(len(noise) / FS / n_notes_period) - 1):
        s0 = int(k * n_notes_period * FS)
        x[s0:s0 + len(t)] += 10 ** (level_db / 20) * np.exp(-t / 0.03) * np.sin(2 * np.pi * 110 * t)
    return x.astype(np.float32)


def test_gate_floor_quietest_frames_fallback_for_a_noise_bed_under_the_playing():
    """The user's DI: noise at -49.5 dBFS RMS (10 ms RMS never stays under the -50 dBFS gap threshold for 120 ms), notes over it."""
    noise = white(8.0, NOISE_DB, seed=7)
    ref = gate_floor(noise, FS)["peakDb"]                                       # the noise-only peak floor, about -42.3
    f = gate_floor(_notes_over(noise), FS)
    assert f["source"] == "quietest 20 % of frames", f
    assert abs(f["peakDb"] - ref) <= 2.0, (f, ref)


def test_gate_floor_steady_signal_uses_the_whole_di_and_silence_is_clamped():
    assert gate_floor(white(3.0, NOISE_DB), FS)["source"] == "whole DI"
    z = np.zeros(3 * FS, np.float32)
    f = gate_floor(z, FS)
    floor = gate_envelope_floor_db(z, FS)
    assert np.isfinite(floor)
    assert floor == (f["peakDb"] if f["peakDb"] is not None else -90.0)
    g = cell_gate(floor, DEFAULT_CELL[0])
    assert -120.0 <= g["thresholdDb"] <= -6.0 and g["thresholdDb"] == pytest.approx(max(floor, -90.0) + 10.0, abs=0.01)


def test_gate_floor_of_a_gap_di_ignores_the_ring_out_tails():
    """K.gap_di (true noise -70 dBFS RMS, rests with a decaying ring-out): gap_regions admits the tails (they fall below -50 dBFS
    10 ms RMS), but the floor must be that of the stationary noise."""
    di = K.gap_di(8.0, seed=3, floor_db=-70.0)
    ref = gate_floor(white(8.0, -70.0, seed=11), FS)["peakDb"]                  # noise-only peak floor, about -62.8
    f = gate_floor(di, FS)
    assert f["source"] == "gaps"
    assert abs(f["peakDb"] - ref) <= 2.0, (f["peakDb"], ref)
    print(f"gap DI: peak floor {f['peakDb']:.2f} dBFS (noise only {ref:.2f}), rms {f['rmsDb']:.2f}")
