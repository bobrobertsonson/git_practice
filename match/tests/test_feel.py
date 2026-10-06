"""v0.4M Task A: feel features (tightness, fizz, polish) on synthetic signals. No captures, no core, no network."""
from __future__ import annotations

import json

import numpy as np
from scipy import signal

from sawblade_match.matcher import feel as F

FS = 48000
N = FS * 4
ONSETS = np.arange(0.25, 3.6, 0.4)          # 9 chugs, 400 ms apart (the last window is cut by the end of the signal)
HF_BP = signal.butter(4, [5000, 12000], btype="bandpass", fs=FS, output="sos")


def riff(seed, tau=0.03, fizz_db=None, gap_db=None, rolloff=5.0, onsets=ONSETS):
    """Palm-muted chugs: harmonic partials (steep roll-off above 3.5 kHz = smooth top), exponential decay ``tau``.
    ``fizz_db``: noise-like 5-12 kHz layer following the playing (re the signal rms); ``gap_db``: broadband noise floor."""
    rng = np.random.default_rng(seed)
    t = np.arange(N) / FS
    x = np.zeros(N)
    env_all = np.zeros(N)
    for k, o in enumerate(onsets):
        m = (t >= o) & (t < o + 0.38)
        tt = t[m] - o
        env = np.exp(-tt / tau) * (1 - np.exp(-tt / 0.002))
        f0 = 82.4 * (1 + (k % 3) * 0.335)
        for h in range(1, 100):
            f = f0 * h
            if f > 11000:
                break
            x[m] += env * np.sin(2 * np.pi * f * tt + rng.uniform(0, 6.28)) / h ** 0.7 * min(1.0, (3500 / f) ** rolloff)
        env_all[m] = np.maximum(env_all[m], env)
    x /= np.sqrt(np.mean(x[env_all > 0.05] ** 2))
    act = np.sqrt(np.mean(x ** 2))
    if fizz_db is not None:
        nz = signal.sosfilt(HF_BP, rng.standard_normal(N)) * env_all
        x = x + nz / (np.sqrt(np.mean(nz ** 2)) + 1e-12) * 10 ** (fizz_db / 20) * act
    if gap_db is not None:
        x = x + rng.standard_normal(N) * 10 ** (gap_db / 20) * act
    return x * 0.1


def activity(x):
    """50 ms frames within 30 dB of the 95th percentile (what tonecheck.activity_mask does)."""
    n = 2400
    nf = len(x) // n
    db = 10 * np.log10(np.mean(x[:nf * n].reshape(nf, n) ** 2, axis=1) + 1e-30)
    m = np.zeros(len(x), bool)
    m[:nf * n] = np.repeat(db >= np.percentile(db, 95) - 30, n)
    return m


def gaps_between_notes(onsets=ONSETS):
    return [(int((o + 0.3) * FS), int((o + 0.38) * FS)) for o in onsets[:-1]]


def target(ref, onsets=ONSETS, gaps=None, **kw):
    return F.make_target(ref, onsets, activity(ref), gaps, ref, **kw)


def terms_of(out, ft):
    return F.evaluate(out, ft)


# ---- A.1 tightness ---------------------------------------------------------------------------------------------------
def pure_chug(tau, f0=100.0):
    t = np.arange(N) / FS
    x = np.zeros(N)
    for o in ONSETS:
        m = t >= o
        x[m] += np.exp(-(t[m] - o) / tau) * np.sin(2 * np.pi * f0 * (t[m] - o))
    return 0.3 * x


def test_t12_matches_the_analytic_decay_and_floppy_scores_worse():
    """A 100 Hz chug with envelope exp(-t/tau): the 12 dB time is 12 / 8.686 * tau (within 5 ms; the band-pass rise
    shifts the peak a little, more for slower decays)."""
    meas = {}
    for tau in (0.02, 0.03):
        x = pure_chug(tau)
        plan = F._Plan(len(x), None, ONSETS)
        st = F.note_stats(F.lowband_power(x, plan.nf), plan)
        sel, note_set, counts = F.select_notes(st)
        assert note_set == "chugs" and counts["used"] >= 8
        meas[tau] = float(np.median(st.t12[sel]))
        assert abs(meas[tau] - 12.0 / 8.6859 * tau * 1000) < 5.0, (tau, meas[tau])
        assert np.all(st.sus[sel] < -10.0)
    assert meas[0.03] > meas[0.02] + 8.0

    ref = riff(1, tau=0.03)
    ft = target(ref)
    same_but_noise_phase, _ = F.evaluate(riff(2, tau=0.03), ft)
    floppy, tf = F.evaluate(riff(3, tau=0.06), ft)
    tighter, tt = F.evaluate(riff(4, tau=0.015), ft)
    assert tf["tight"] > 3.0 and tf["tightRaw"]["t12Ms"] > 15.0 and tf["tightRaw"]["sustainDb"] > 3.0
    assert tt["tight"] > 0.5                              # tighter than the reference is penalised too ...
    slow_ref, fast_ref = target(riff(1, tau=0.05)), target(riff(1, tau=0.02))
    _, floppy_vs_fast = F.evaluate(riff(2, tau=0.05), fast_ref)
    _, tight_vs_slow = F.evaluate(riff(2, tau=0.02), slow_ref)
    assert floppy_vs_fast["tight"] > 1.5 * tight_vs_slow["tight"] > 0.0     # ... but the same gap counts less that way round
    assert abs(F.asym(np.array([4.0]))[0] - 4.0) < 1e-12 and abs(F.asym(np.array([-4.0]))[0] - 2.0) < 1e-12
    assert floppy > same_but_noise_phase + 1.0
    assert ft.note_set == "chugs"


def test_identical_pair_scores_zero_and_evaluate_is_deterministic():
    ref = riff(1, fizz_db=-14, gap_db=-50)
    ft = target(ref, gaps=gaps_between_notes())
    v, t = F.evaluate(ref, ft)
    assert v == 0.0 and t["tight"] == 0.0 and t["fizz"] == 0.0 and t["polish"] == 0.0
    out = riff(2, tau=0.05, fizz_db=-8, gap_db=-40)
    a, ta = F.evaluate(out, ft)
    b, tb = F.evaluate(out.copy(), ft)
    assert a == b and a > 1.0 and ta == tb
    json.dumps(ta)                                           # terms are plain JSON


def test_feel_is_gain_invariant():
    ref = riff(1, fizz_db=-14, gap_db=-50)
    ft = target(ref, gaps=gaps_between_notes())
    out = riff(2, tau=0.05, fizz_db=-8, gap_db=-40)
    a, ta = F.evaluate(out, ft)
    b, tb = F.evaluate(out * 2.0, ft)                      # +6 dB
    c, _ = F.evaluate(out * 0.1, ft)
    assert abs(a - b) < 1e-6 and abs(a - c) < 1e-6, (a, b, c)
    for k in ("tight", "fizz", "polish", "floor", "flux", "crest"):
        assert (ta[k] is None) == (tb[k] is None)
        if ta[k] is not None:
            assert abs(ta[k] - tb[k]) < 1e-6, k


def test_fewer_than_three_notes_drops_tightness_and_records_it():
    ref = riff(1, onsets=ONSETS[:2])
    on = ONSETS[:2]
    ft = F.make_target(ref, on, activity(ref), None, ref)
    assert ft.sel is None and "tight" in ft.dropped
    v, t = F.evaluate(riff(2, tau=0.08, onsets=ONSETS[:2]), ft)
    assert t["tight"] is None and "tight" in t["dropped"] and t["noteSet"] is None
    assert np.isfinite(v)
    # with the notes the same render is penalised
    ft3 = target(riff(1))
    v3, t3 = F.evaluate(riff(2, tau=0.08), ft3)
    assert t3["tight"] > 1.0


# ---- A.2 fizz ---------------------------------------------------------------------------------------------------------
def test_fizz_feature_definitions_on_known_signals():
    n = FS * 2
    t = np.arange(n) / FS
    plan = F._Plan(n, None, None)
    two = 0.1 * np.sin(2 * np.pi * 6000 * t) + 0.1 * np.sin(2 * np.pi * 2000 * t)
    m = F.measure(two, plan, notes=False)
    assert abs(np.median(m.fizz["hfRatioDb"])) < 0.2                  # equal-power tones in 5-12 kHz and 1-4 kHz
    assert np.median(m.fizz["hfFlat"]) < 0.01 and np.median(m.fizz["hfMod"]) < 0.05
    rng = np.random.default_rng(0)
    white = signal.sosfilt(HF_BP, rng.standard_normal(n)) * 0.1
    mw = F.measure(white, plan, notes=False)
    assert 0.5 < np.median(mw.fizz["hfFlat"]) < 0.62                  # exp(-gamma) = 0.56 for noise
    assert np.median(mw.fizz["hfMod"]) > 0.2 > np.median(m.fizz["hfMod"]) + 0.15
    assert np.median(mw.fizz["hfRatioDb"]) > 20.0


def test_fizz_term_orders_white_hf_above_shaped_hf():
    ref = riff(1)
    ft = target(ref)
    smooth, white = riff(2), riff(3, fizz_db=-14)
    _, ts = F.evaluate(smooth, ft)
    _, tw = F.evaluate(white, ft)
    assert ts["fizz"] < 1.0 and tw["fizz"] > 10.0 * max(ts["fizz"], 0.1)
    for k in ("hfRatioDb", "hfFlat"):
        assert tw["fizzRaw"][k] > 5.0 * ts["fizzRaw"][k] + 0.01, k
    assert tw["fizzRaw"]["hfRatioDb"] > 5.0 and tw["fizzRaw"]["hfFlat"] > 0.2
    louder, tl = F.evaluate(riff(4, fizz_db=-6), ft)
    assert tl["fizz"] > tw["fizz"]                                      # more fizz, more penalty
    # symmetric in the sense that a fizzy reference penalises a smooth render too
    _, tr = F.evaluate(smooth, target(white))
    assert tr["fizz"] > 10.0


def test_fizz_off_when_the_reference_hf_is_not_usable():
    ref = riff(1)
    ft = target(ref, fizz_on=False, fizz_reason="full-mix reference")
    v, t = F.evaluate(riff(3, fizz_db=-14), ft)
    assert t["fizz"] is None and t["dropped"]["fizz"] == "full-mix reference"
    on, ton = F.evaluate(riff(3, fizz_db=-14), target(ref))
    assert on > v + 1.0 and ton["dropped"].get("fizz") is None


# ---- A.3 polish -------------------------------------------------------------------------------------------------------
def test_crest_and_flux_known_answers():
    n = FS * 3
    t = np.arange(n) / FS
    plan = F._Plan(n, None, None)
    sine = 0.5 * np.sin(2 * np.pi * 1000 * t)
    m = F.measure(sine, plan, notes=False, fizz=False)
    assert abs(np.median(m.crest) - 3.0103) < 0.1
    white = np.random.default_rng(0).standard_normal(n) * 0.1
    mw = F.measure(white, plan, notes=False, fizz=False)
    assert 10.0 < np.median(mw.crest) < 16.0
    assert np.median(mw.flux) > 4.5 and np.median(mw.flux) > 1.8 * np.median(m.flux)     # noise-like = high flux
    ft = F.make_target(white, None, None, None, white)
    v, terms = F.evaluate(sine * 0.3, ft)
    assert terms["flux"] > 3.0 and terms["crest"] > 3.0
    assert F.evaluate(white * 0.3, ft)[0] < 1e-6


def test_floor_orders_gated_below_ungated_and_is_one_sided():
    gaps = gaps_between_notes()
    gated, ungated = riff(1), riff(1, gap_db=-45)
    ft_gated = target(gated, gaps=gaps)
    ft_noisy = target(ungated, gaps=gaps)
    _, same = F.evaluate(gated, ft_gated)
    _, hiss = F.evaluate(ungated, ft_gated)                     # render hisses where the reference is quiet
    _, too_clean = F.evaluate(gated, ft_noisy)                   # render quieter than a noisy reference
    assert same["floor"] == 0.0
    assert hiss["floor"] > 3.0 and hiss["polishRaw"]["floorDiffDb"] > 20.0
    assert 0.0 < too_clean["floor"] < 0.5 * hiss["floor"]
    assert hiss["polishRaw"]["floorDbOut"] > hiss["polishRaw"]["floorDbRef"]


def test_floor_dropped_without_gaps_and_floor_term_formula():
    ref = riff(1)
    ft = target(ref, gaps=[(1000, 2000)])                        # 21 ms: < 100 ms
    assert "floor" in ft.dropped
    _, t = F.evaluate(riff(2, gap_db=-40), ft)
    assert t["floor"] is None and "floor" in t["dropped"]
    assert abs(F.floor_term(10.0, 4.0) - 1.0) < 1e-12 and abs(F.floor_term(-2.0, 4.0) - 0.25) < 1e-12


# ---- soft targets and robustness ------------------------------------------------------------------------------------------
def test_soft_target_compares_distributions_at_half_weight():
    ref = riff(1, fizz_db=-14)
    soft = (ref, activity(ref), ONSETS)
    di = riff(9)                                                  # a different take: other noise phases, same rhythm
    ft = F.make_target(di, ONSETS, activity(di), None, None, soft_ref=soft, cache={})
    assert ft.mode == "soft" and ft.scale == 0.5 and ft.ref_note_set == "chugs"
    v_same, t_same = F.evaluate(riff(1, fizz_db=-14), ft)
    assert t_same["weights"]["tight"] == 0.25 and t_same["weights"]["fizz"] == 0.25
    assert v_same < 0.05, t_same
    assert "floor" in t_same["dropped"] and t_same["floor"] is None
    v_bad, t_bad = F.evaluate(riff(2, tau=0.07), ft)             # floppy and smooth where the reference is fizzy
    assert v_bad > v_same + 1.0 and t_bad["tight"] > 1.0 and t_bad["fizz"] > 5.0
    cache = {}
    F.make_target(di, ONSETS, activity(di), None, None, soft_ref=soft, cache=cache)
    assert len(cache) == 1                                       # reference features memoised for the other excerpts


def test_non_finite_render_is_infinite_loss():
    ft = target(riff(1))
    bad = riff(2)
    bad[100] = np.nan
    assert F.evaluate(bad, ft)[0] == float("inf")


def test_loss_integration_adds_feel_to_the_total():
    from sawblade_match.matcher import loss as L
    x = riff(1, fizz_db=-14)
    starts = L.segment_starts(len(x), None)
    plain = L.Target(starts, L.features(x, starts, None), None, None, None)
    felt = L.Target(starts, L.features(x, starts, None), None, activity(x), None, feel=target(x))
    assert L.evaluate(x, felt).total < 1e-6 and L.evaluate(x, felt).feel == 0.0
    out = riff(2, tau=0.06, fizz_db=-6)
    a, b = L.evaluate(out, plain), L.evaluate(out, felt)
    assert a.feel == 0.0 and a.feel_terms is None and a.as_dict()["feelTerms"] is None
    assert b.feel > 1.0 and abs(b.total - (a.total + b.feel)) < 1e-9
    d = b.as_dict()
    assert d["feel"] == b.feel and d["feelTerms"]["tight"] is not None and "dropped" in d["feelTerms"]
    json.dumps(d)
    assert (L.W_TIGHT, L.W_FIZZ, L.W_POLISH) == (0.5, 0.5, 0.25)


# ---- reference basis: the fizz term must be ON for a clean amp track ---------------------------------------------------
def _wav(path, x):
    import soundfile as sf
    sf.write(str(path), x.astype(np.float32), FS, subtype="FLOAT")
    return path


def test_clean_mono_matched_reference_has_fizz_on_and_a_mix_has_it_off(tmp_path):
    from sawblade_match.matcher.reference import feel_fizz_state, load_reference
    p = _wav(tmp_path / "amp.wav", riff(1, fizz_db=-14))
    none = tmp_path / "no_stems"
    ref = load_reference(p, matched="mono", offset_ms=0.0, stems_dir=none)
    assert ref.clean and ref.basis == "mid" and ref.hf_limit_hz is None and not ref.texture
    assert feel_fizz_state(ref) == (True, None)
    mix = load_reference(p, matched="mono", offset_ms=0.0, stems_dir=none, clean=False)    # the pre-v0.4M reading of the same file
    assert not mix.clean and mix.hf_limit_hz == 4500.0
    on, why = feel_fizz_state(mix)
    assert not on and "4500" in why
    # a matched stereo channel is a mix unless declared isolated
    st = _wav(tmp_path / "stereo.wav", np.stack([riff(1), riff(2)], axis=1))
    m = load_reference(st, matched="left", offset_ms=0.0, stems_dir=none, channel="left")
    assert not feel_fizz_state(m)[0] and "full mix" in feel_fizz_state(m)[1]
    c = load_reference(st, matched="left", offset_ms=0.0, stems_dir=none, clean=True)
    assert c.clean and c.basis == "left" and c.hf_limit_hz is None and feel_fizz_state(c)[0]


def test_build_target_carries_the_feel_target(tmp_path):
    from sawblade_match.matcher import loss as L
    from sawblade_match.matcher.reference import build_target, load_reference, make_excerpt
    di = riff(7).astype(np.float32)
    p = _wav(tmp_path / "amp.wav", riff(1, fizz_db=-14))
    none = tmp_path / "no_stems"
    ref = load_reference(p, matched="mono", offset_ms=0.0, stems_dir=none)
    ex = make_excerpt(di, 3.0, window=(0, 3 * FS), ref=ref)
    tgt = build_target(ref, ex)
    assert tgt.feel is not None and tgt.feel.mode == "paired" and tgt.feel.fizz_on
    r = L.evaluate(riff(2, fizz_db=-14)[:3 * FS], tgt)
    assert np.isfinite(r.total) and r.feel_terms["fizz"] is not None and r.feel == r.feel_terms["total"]
    assert r.as_dict()["feelTerms"]["mode"] == "paired"
    mix = load_reference(p, matched="mono", offset_ms=0.0, stems_dir=none, clean=False)
    t2 = build_target(mix, ex)
    assert not t2.feel.fizz_on and "fizz" in t2.feel.dropped
    # no matched pair: soft target from the reference's own features, every weight halved
    soft_ref = load_reference(p, channel="mid", stems_dir=none)
    t3 = build_target(soft_ref, ex)
    assert t3.feel.mode == "soft" and t3.feel.scale == 0.5 and t3.feel.fizz_on
    assert np.isfinite(L.evaluate(riff(2)[:3 * FS], t3).total)
