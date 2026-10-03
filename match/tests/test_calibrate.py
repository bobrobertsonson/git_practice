"""Tests for sawblade_match.calibrate (phase 2A): section selection, LTAS recovery, proposal logic, CLI."""
import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.calibrate import cli, propose as P, sections as S, separation as SEP
from sawblade_match.calibrate.measure import measure
from sawblade_match.tonecheck import analysis as A
from sawblade_match.tonecheck.rules import evaluate_rules, load_targets, parse_expr

FS = 48000
SEED = 20261003
REPO = Path(__file__).resolve().parents[2]
LIVE_TARGETS = REPO / "docs" / "tone_targets.json"
TARGETS = load_targets(Path(__file__).parent / "fixtures" / "tone_targets_v1.json")   # frozen v1


# --- synthetic material -----------------------------------------------------------------------------
def guitar(n, seed, level=0.1):
    """Known-spectrum 'guitar': noise through HP80 / LP3k / +6 dB shelf-ish bump, with a chugging envelope."""
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(n)
    x = signal.sosfilt(signal.butter(2, 80, "highpass", fs=FS, output="sos"), x)
    x = signal.sosfilt(signal.butter(3, 3000, "lowpass", fs=FS, output="sos"), x)
    x = x + 1.0 * signal.sosfilt(signal.butter(2, (900, 1800), "bandpass", fs=FS, output="sos"), x)
    t = np.arange(n) / FS
    env = 0.8 + 0.2 * np.exp(-((t * 4.0) % 1.0) / 0.25)        # 4 gentle chugs / s, never silent
    return level * env * x / np.std(x)


def kick(n):
    t = np.arange(n) / FS
    return 0.6 * np.exp(-t / 0.08) * np.sin(2 * np.pi * (55 + 60 * np.exp(-t / 0.02)) * t)


def snare(n, rng):
    t = np.arange(n) / FS
    return 0.5 * np.exp(-t / 0.06) * (0.6 * np.sin(2 * np.pi * 200 * t) + 0.8 * rng.standard_normal(n))


def voice(n, f0=220.0, seed=0):
    """Harmonic tone up to 3 kHz with +-40 cent, 5.5 Hz vibrato."""
    t = np.arange(n) / FS
    ph = 2 * np.pi * np.cumsum(f0 * 2 ** (0.4 / 12 * np.sin(2 * np.pi * 5.5 * t))) / FS
    return 0.08 * sum(np.sin(k * ph) / k ** 0.5 for k in range(1, int(3000 // f0) + 1))


def add(dst, src, at_s):
    i = int(at_s * FS)
    m = min(len(src), len(dst) - i)
    dst[i:i + m] += src[:m]


def make_original(dur=40.0):
    """0-10 s guitar only | 10-20 drums over guitar | 20-30 vocal over guitar | 30-40 drums + guitar."""
    n = int(dur * FS)
    rng = np.random.default_rng(SEED)
    g = guitar(n, SEED)
    mix = g.copy()
    hits = []
    for a, b in ((10, 20), (30, 40)):
        for k, t in enumerate(np.arange(a + 0.25, b - 0.6, 0.5)):
            add(mix, kick(int(0.4 * FS)) if k % 2 == 0 else snare(int(0.3 * FS), rng), t)
            hits.append(t)
    add(mix, voice(10 * FS), 20.0)
    return mix, g, hits


def frames_of(t0, t1):
    return np.arange(int(t0 / S.FRAME_S), int(t1 / S.FRAME_S))


# --- section selection -----------------------------------------------------------------------------
def test_drum_hits_are_found_and_frames_excluded():
    mix, _, hits = make_original()
    nf = len(mix) // S.FRAME_N
    drums, found = S.drum_frames(mix, nf)
    hit_frames = np.array([int(t / S.FRAME_S) for t in hits])
    assert drums[hit_frames].mean() >= 0.95                       # recall of the hit frames
    # few false alarms (band-power fluctuations of the guitar noise) in the guitar-only and vocal sections
    assert drums[frames_of(0.5, 9.5)].mean() <= 0.30
    assert drums[frames_of(21, 29)].mean() <= 0.15


def test_original_selection_excludes_drums_and_voice():
    mix, _, hits = make_original()
    sel = S.original_guitar_frames(mix)
    nf = len(sel.mask)
    hit_frames = np.array([int(t / S.FRAME_S) for t in hits])
    assert not sel.mask[hit_frames].any()
    assert sel.mask[frames_of(20.5, 29.5)].mean() <= 0.10         # vocal frames (vibrato tone) excluded
    assert sel.mask[frames_of(0.5, 9.5)].mean() >= 0.70           # guitar-only kept (minus a few false drum alarms)
    assert sel.reasons["vocalLike"][frames_of(20.5, 29.5)].mean() >= 0.90
    assert sel.info["reliable"] is False                           # documented as unreliable on real vocals
    assert nf == len(mix) // S.FRAME_N


def test_constant_pitch_guitar_chord_is_not_voice():
    n = 10 * FS
    t = np.arange(n) / FS
    chord = 0.1 * sum(np.sin(2 * np.pi * 220 * k * t) / k for k in range(1, 12))   # steady, no vibrato
    v, _ = S.vocal_frames(chord)
    assert v.mean() < 0.1


def test_recovered_guitar_ltas_within_1db():
    mix, g, _ = make_original()
    sel = S.original_guitar_frames(mix)
    m = measure("syn", mix, TARGETS, mask=sel.sample_mask(len(mix)))
    known = A.analyze(g, FS, TARGETS)
    c = np.array(A.NOMINAL_CENTRES)
    band = (c >= 80) & (c <= 4000)   # the synthetic guitar is low-passed at 3 kHz: above 4 kHz it is ~noise floor
    d = m.rel_db - known.rel_db
    assert np.max(np.abs(d[band])) <= 1.0, np.round(d[band], 2)
    # the un-selected whole mix is far off (drums/voice dominate) - the selection is what fixes it
    whole = A.analyze(mix, FS, TARGETS)
    assert np.max(np.abs((whole.rel_db - known.rel_db)[band])) > 2.0


def test_user_sections_replace_the_heuristic():
    mix, _, _ = make_original()
    sel = S.original_guitar_frames(mix, ranges=S.parse_ranges("0:10, 20:25"))
    assert sel.info["mode"] == "user-sections"
    assert sel.mask[frames_of(0.1, 9.9)].all() and sel.mask[frames_of(20.1, 24.9)].all()
    assert not sel.mask[frames_of(10.5, 19.5)].any() and not sel.mask[frames_of(26, 39)].any()
    assert S.parse_ranges("0:12,95:110") == [(0.0, 12.0), (95.0, 110.0)]
    with pytest.raises(ValueError):
        S.parse_ranges("5:3")


def make_cover(dur=40.0, off_l=0.190, off_r=0.175):
    """Two DIs (note plucks + silence), guitars panned hard L/R delayed in the mix, centre drums + bass."""
    n = int(dur * FS)
    rng = np.random.default_rng(SEED + 1)

    def di_and_guitar(seed, spacing):
        env = np.full(n, 1e-3)
        for t in np.arange(1.0, dur - 2.0, spacing):
            i = int(t * FS)
            m = int(0.5 * FS)
            env[i:i + m] = np.maximum(env[i:i + m], np.exp(-np.arange(m) / FS / 0.25))
        env = np.convolve(env, np.ones(480) / 480, mode="same")
        di = env * np.random.default_rng(seed).standard_normal(n) * 0.2 + 1e-4 * rng.standard_normal(n)
        g = guitar(n, seed, 0.2) * env / env.max()
        return di, g
    di_l, g_l = di_and_guitar(11, 0.9)
    di_r, g_r = di_and_guitar(12, 1.1)
    mix_l, mix_r = np.zeros(n), np.zeros(n)
    add(mix_l, g_l, off_l)
    add(mix_r, g_r, off_r)
    hits = []
    for t in np.arange(0.5, dur - 1, 0.5):         # drums throughout, also where the DIs are silent
        d = kick(int(0.4 * FS)) if int(t * 2) % 2 == 0 else snare(int(0.3 * FS), rng)
        add(mix_l, d, t)
        add(mix_r, d, t)
        hits.append(t)
    return mix_l, mix_r, di_l, di_r, g_l, g_r, hits


def test_cover_selection_uses_dis_and_excludes_drums():
    mix_l, mix_r, di_l, di_r, g_l, g_r, hits = make_cover()
    sel = S.cover_guitar_frames(mix_l, mix_r, di_l, di_r)
    nf = len(sel.mask)
    # offsets are recovered to within the 5 ms envelope resolution
    assert sel.info["offsetsS"]["L"] == pytest.approx(0.190, abs=0.010)
    assert sel.info["offsetsS"]["R"] == pytest.approx(0.175, abs=0.010)
    hit_frames = np.array([int(t / S.FRAME_S) for t in hits])
    assert not sel.mask[hit_frames].any()
    assert 0.05 < sel.fraction < 0.9
    # recovered guitar LTAS (mid of the two guitars, drums excluded) within 1 dB of the known one
    n = min(len(mix_l), len(mix_r))
    mid = 0.5 * (mix_l[:n] + mix_r[:n])
    m = measure("cover", mid, TARGETS, mask=sel.sample_mask(n))
    known_mid = np.zeros(n)
    add(known_mid, 0.5 * g_l, 0.190)
    add(known_mid, 0.5 * g_r, 0.175)
    known = A.analyze(known_mid[:n], FS, TARGETS)
    c = np.array(A.NOMINAL_CENTRES)
    band = (c >= 100) & (c <= 4000)
    assert np.max(np.abs((m.rel_db - known.rel_db)[band])) <= 1.0, np.round((m.rel_db - known.rel_db)[band], 2)
    assert nf == n // S.FRAME_N


# --- proposal logic ---------------------------------------------------------------------------------
def groups_from_diffs(**kw):
    base = {"sub": -30.0, "thump": -3.0, "lowMid": -5.0, "dipMid": 0.0, "saw": 0.0, "presence": -5.0, "fizz": -25.0}
    base.update(kw)
    return base


def test_calibrated_offset_margin_equals_tolerance_rounded_outward():
    g = groups_from_diffs()
    for rule in TARGETS["rules"]:
        c = P.calibrated_offset(rule, g)
        e = parse_expr(rule["expr"])
        new = {**rule, "expr": P.format_expr(e.lhs, e.op, e.rhs, c)}
        r = evaluate_rules(g, [new])[0]
        tol = rule["toleranceDb"]
        assert tol - 1e-6 <= r["margin"] < tol + P.STEP_DB + 1e-6, (rule["id"], r["margin"])
        assert parse_expr(new["expr"]).offset == pytest.approx(c)


def test_proposal_original_passes_everything_and_contradictions_are_listed_not_changed():
    g = groups_from_diffs(dipMid=1.0)          # dipMid above saw: contradicts "dipMid <= saw - 2" (tol 1.5)
    prop, table, contra = P.propose(TARGETS, g, "original/sections", policy="tighten")
    assert [c["id"] for c in contra] == ["saw_over_dip"]
    by = {r["id"]: r for r in prop["rules"]}
    assert by["saw_over_dip"]["expr"] == "dipMid <= saw - 2"                       # unchanged
    assert by["saw_over_dip"]["calibration"]["proposedExprIfAccepted"] == "dipMid <= saw + 2.5"
    st = P.consistency_check(prop, g)
    assert all(v == "pass" for k, v in st.items() if k != "saw_over_dip")
    assert st["saw_over_dip"] == "fail"
    assert prop["version"] == TARGETS["version"] + 1 and "PROPOSAL" in prop["status"]
    assert "evidence" in contra[0] and "+1.0" in contra[0]["evidence"]
    # shapes are untouched: same groups, same rule ids/order, tolerances
    assert prop["analysis"] == TARGETS["analysis"]
    assert [r["id"] for r in prop["rules"]] == [r["id"] for r in TARGETS["rules"]]
    assert [r["toleranceDb"] for r in prop["rules"]] == [r["toleranceDb"] for r in TARGETS["rules"]]
    json.dumps(prop)


def test_loosen_only_changes_only_marginal_rules_and_lists_failures():
    # thump - saw = +5.3: thump_controlled (<= saw + 4, tol 2) is marginal; dipMid - saw = +0.7: saw_over_dip fails
    g = groups_from_diffs(thump=5.3, dipMid=0.7, saw=0.0)
    prop, table, contra = P.propose(TARGETS, g, "original/side")             # default policy
    assert prop["calibration"]["policy"] == "loosen-only"
    by = {r["id"]: r for r in table}
    assert by["thump_controlled"]["decision"] == "calibrated" and by["thump_controlled"]["proposedExpr"] == "thump <= saw + 7.5"
    assert [c["id"] for c in contra] == ["saw_over_dip"]
    for r in table:                                    # everything the original passes is untouched
        if r["currentStatusOnBasis"] == "pass":
            assert r["proposedExpr"] == r["currentExpr"] and r["decision"] == "unchanged"
    new = {r["id"]: r["expr"] for r in prop["rules"]}
    assert new["thump_present"] == "thump >= saw - 6" and new["saw_over_dip"] == "dipMid <= saw - 2"
    st = P.consistency_check(prop, g)
    assert all(v == "pass" for k, v in st.items() if k != "saw_over_dip")
    with pytest.raises(ValueError):
        P.propose(TARGETS, g, "x", policy="nope")


def test_side_channel_isolates_hard_panned_guitars():
    n = 20 * FS
    g_l, g_r = guitar(n, 31), guitar(n, 32)
    centre = np.zeros(n)
    rng = np.random.default_rng(SEED)
    for k, t in enumerate(np.arange(0.5, 19.0, 0.5)):
        add(centre, kick(int(0.4 * FS)) if k % 2 == 0 else snare(int(0.3 * FS), rng), t)
    centre += 0.3 * np.sin(2 * np.pi * 70 * np.arange(n) / FS)                   # bass
    side = 0.5 * ((g_l + centre) - (g_r + centre))
    m = measure("side", side, TARGETS)
    known = A.analyze(0.5 * (g_l - g_r), FS, TARGETS)
    c = np.array(A.NOMINAL_CENTRES)
    band = (c >= 25) & (c <= 4000)
    assert np.max(np.abs((m.rel_db - known.rel_db)[band])) < 0.01                # centred content cancels exactly


def test_proposal_does_not_mutate_input_and_is_deterministic():
    before = json.dumps(TARGETS, sort_keys=True)
    a, _, _ = P.propose(TARGETS, groups_from_diffs(), "x", policy="tighten")
    b, _, _ = P.propose(TARGETS, groups_from_diffs(), "x", policy="tighten")
    assert json.dumps(TARGETS, sort_keys=True) == before
    assert a == b


def test_format_expr_roundtrip():
    for off in (-4.5, 0.0, 3.0, 12.5):
        e = parse_expr(P.format_expr("lowMid", ">=", "thump", off))
        assert (e.lhs, e.op, e.rhs, e.offset) == ("lowMid", ">=", "thump", off)


# --- separation (method 1) ---------------------------------------------------------------------------
def test_separation_unavailable_without_demucs(monkeypatch, tmp_path):
    src = tmp_path / "x.wav"
    sf.write(src, 0.1 * np.ones((FS, 2), dtype=np.float32), FS)
    import builtins
    real = builtins.__import__

    def fake(name, *a, **k):
        if name.split(".")[0] in ("demucs", "torch"):
            raise ImportError("no module named " + name)
        return real(name, *a, **k)
    monkeypatch.setattr(builtins, "__import__", fake)
    r = SEP.separate_other(src, tmp_path / "stems")
    assert isinstance(r, SEP.Unavailable) and str(r).startswith("unavailable: ")


def test_separation_cache_hit_needs_no_demucs(tmp_path):
    src = tmp_path / "x.wav"
    sf.write(src, 0.1 * np.ones((FS, 2), dtype=np.float32), FS)
    cache = SEP.stem_cache_path(src, tmp_path / "stems")
    cache.parent.mkdir()
    sf.write(cache, 0.2 * np.ones((1000, 2), dtype=np.float32), 44100, subtype="FLOAT")
    r = SEP.separate_other(src, tmp_path / "stems")
    assert isinstance(r, SEP.Stem) and r.cached and r.rate == 44100 and r.audio.shape == (1000, 2)


def test_separation_download_failure_reports_unavailable(monkeypatch, tmp_path):
    pytest.importorskip("demucs")
    import demucs.pretrained as dp
    monkeypatch.setattr(dp, "get_model", lambda *a, **k: (_ for _ in ()).throw(OSError("403 blocked")))
    src = tmp_path / "x.wav"
    sf.write(src, 0.1 * np.ones((FS, 2), dtype=np.float32), FS)
    r = SEP.separate_other(src, tmp_path / "stems")
    assert isinstance(r, SEP.Unavailable) and "weights" in r.reason and "403" in r.reason


def test_separation_real_model_if_available(tmp_path):
    import os
    if os.environ.get("SAWBLADE_TEST_DEMUCS") != "1":
        pytest.skip("set SAWBLADE_TEST_DEMUCS=1 to run the real demucs model (downloads/loads ~80 MB weights)")
    pytest.importorskip("demucs")
    src = tmp_path / "x.wav"
    rng = np.random.default_rng(SEED)
    sf.write(src, (0.05 * rng.standard_normal((3 * 44100, 2))).astype(np.float32), 44100)
    r = SEP.separate_other(src, tmp_path / "stems")
    if isinstance(r, SEP.Unavailable):
        pytest.skip(str(r))
    assert r.audio.shape[1] == 2 and r.path.exists()
    r2 = SEP.separate_other(src, tmp_path / "stems")
    assert r2.cached and np.allclose(r2.audio, r.audio)


# --- CLI end to end on synthetic files ----------------------------------------------------------------
def test_cli_end_to_end_synthetic(tmp_path, capsys):
    mix, _, _ = make_original(40.0)
    mix_l, mix_r, di_l, di_r, *_ = make_cover(40.0)
    orig = tmp_path / "orig.wav"
    sf.write(orig, np.stack([mix + 0.5 * guitar(len(mix), 98), mix + 0.5 * guitar(len(mix), 99)], axis=1).astype(np.float32), FS)
    cm = tmp_path / "cover.wav"
    sf.write(cm, np.stack([mix_l, mix_r], axis=1).astype(np.float32), FS)
    sf.write(tmp_path / "L.wav", di_l.astype(np.float32), FS)
    sf.write(tmp_path / "R.wav", di_r.astype(np.float32), FS)
    smoke = tmp_path / "smoke.wav"
    sf.write(smoke, guitar(10 * FS, 5).astype(np.float32), FS)
    tfile = Path(__file__).parent / "fixtures" / "tone_targets_v1.json"
    before = LIVE_TARGETS.read_bytes()
    out = tmp_path / "out"
    rc = cli.main(["--original", str(orig), "--cover-mix", str(cm), "--di-l", str(tmp_path / "L.wav"),
                   "--di-r", str(tmp_path / "R.wav"), "--no-separation", "--score", str(smoke),
                   "--targets", str(tfile), "--out", str(out), "--stems-dir", str(tmp_path / "stems")])
    assert rc == 0
    assert LIVE_TARGETS.read_bytes() == before                                  # never touches the committed targets
    prop = json.loads((out / "tone_targets.proposed.json").read_text())
    assert prop["schema"] == "sawblade.tone_targets" and prop["calibration"]["basis"] == "original/side"
    assert prop["calibration"]["policy"] == "loosen-only"
    md = (out / "calibration_report.md").read_text()
    assert "unavailable: disabled with --no-separation" in md and "Smoke render scored" in md
    for f in ("ltas_original_vs_cover.png", "thresholds_current_vs_proposed.png", "calibration.json"):
        assert (out / f).stat().st_size > 0
    # the proposal loads as a targets file and the original's sections pass every non-contradicted rule
    cal = json.loads((out / "calibration.json").read_text())
    assert {"original/side", "cover/side", "original/sections", "cover/sections"} <= set(cal["results"])
    groups = cal["results"]["original/side"]["groupsDb"]
    # low-end metrics: cover/side uses DI onsets (non-null), original/side has no onset source (n/a)
    cm, om = cal["results"]["cover/side"]["metrics"], cal["results"]["original/side"]["metrics"]
    assert cm["lowTightnessMs"]["nOnsets"] > 0 and cm["lowDecayDbPerMs"]["value"] is not None
    assert om["lowTightnessMs"]["valueMs"] is None and om["lowDecayDbPerMs"]["value"] is None
    assert "no onset source" in om["lowDecayDbPerMs"]["note"]
    assert cal["results"]["cover/side"]["selectedFraction"] < 0.99          # side activity gate applied
    contra = {c["id"] for c in cal["contradicted"]}
    for r in evaluate_rules(groups, prop["rules"]):
        assert r["status"] == "pass" or r["id"] in contra


def test_live_targets_file_is_schema_valid():
    t = load_targets(LIVE_TARGETS)                       # only parses; thresholds are not asserted here
    assert t["rules"] and all(parse_expr(r["expr"]) for r in t["rules"])
    evaluate_rules({g: 0.0 for g in t["analysis"]["bandGroups"]}, t["rules"])


def test_cli_missing_input_exit_3(tmp_path, capsys):
    rc = cli.main(["--original", str(tmp_path / "nope.mp3"), "--out", str(tmp_path / "o")])
    assert rc == 3 and "error:" in capsys.readouterr().err


def test_measure_no_onsets_reports_na():
    x = guitar(8 * FS, 3)
    m = measure("x", x, TARGETS, no_onsets=True)
    assert m.metrics["lowTightnessMs"]["valueMs"] is None and m.metrics["lowDecayDbPerMs"]["value"] is None
    assert "no onset source" in m.metrics["lowTightnessMs"]["note"] and m.metrics["buzz"]["value"] > 0


# --- channels: side channel and DI onsets (review items) -------------------------------------------------
from sawblade_match.calibrate import channels as CH  # noqa: E402


def test_side_channel_formula_and_centred_cancellation():
    n = 10 * FS
    g_l, g_r = guitar(n, 41), guitar(n, 42)
    centre = 0.5 * np.sin(2 * np.pi * 60 * np.arange(n) / FS) + kick(n)
    l, r = g_l + centre, g_r + centre
    side = CH.side_channel(l, r)
    assert np.allclose(side, 0.5 * (l - r))
    assert np.allclose(side, 0.5 * (g_l - g_r), atol=1e-12)               # centred content cancels
    assert np.allclose(CH.mid_channel(l, r), 0.5 * (l + r))
    assert len(CH.side_channel(l[:100], r)) == 100                        # shorter channel wins


def test_side_activity_gating_matters():
    # side signal = guitar bursts + a low hum between them. The activity gate drops the hum frames; an all-true
    # mask (the mutant) keeps them and visibly changes the sub-band level.
    n = 20 * FS
    t = np.arange(n) / FS
    burst = ((t % 2.0) < 0.8).astype(float)
    x = guitar(n, 51, 0.1) * burst + 3.5e-3 * np.sin(2 * np.pi * 45 * t) * (1 - burst)
    gated = measure("g", x, TARGETS)
    ungated = measure("u", x, TARGETS, mask=np.ones(n, dtype=bool))
    assert gated.selected_fraction < 0.5 and ungated.selected_fraction == 1.0
    assert abs(gated.groups["sub"] - ungated.groups["sub"]) > 3.0


def test_cover_di_onsets_offset_sign_and_low_end_decay():
    n = 6 * FS
    di = 1e-4 * np.random.default_rng(SEED).standard_normal(n)
    plucks = [1.0, 2.5, 4.0]
    for p in plucks:
        i = int(p * FS)
        di[i:i + FS // 2] += 0.3 * np.exp(-np.arange(FS // 2) / FS / 0.1) * np.random.default_rng(int(p * 10)).standard_normal(FS // 2)
    off = 0.190
    on = CH.cover_di_onsets(di, di, {"L": off, "R": off})
    assert len(on) == 2 * len(plucks)
    assert np.max(np.abs(on[::2] - (np.array(plucks) + off))) < 0.010     # onset at DI time t -> t + offset
    # the mix's 120 Hz tail starts at t + offset: right sign finds it, wrong sign does not
    tau = 0.1
    out = np.zeros(n)
    for p in plucks:
        i = int((p + off) * FS)
        m = int(0.6 * FS)                                                  # each note rings 0.6 s, then silence
        out[i:i + m] += np.exp(-np.arange(m) / FS / tau) * np.sin(2 * np.pi * 120 * np.arange(m) / FS)
    good = np.unique(np.round(on, 4))
    t_ok, d_ok = A.low_end_decay(np.zeros(n), out, FS, onsets=good)
    expected = 1000 * tau * np.log(10)
    assert t_ok["nMeasured"] >= 2 and t_ok["valueMs"] == pytest.approx(expected, rel=0.15)
    bad = np.array(plucks) - off                                           # wrong sign: onsets in the silence before the notes
    t_bad, _ = A.low_end_decay(np.zeros(n), out, FS, onsets=bad)
    assert t_bad["nMeasured"] == 0
    # wrong sign applied through the helper: -offset instead of +offset also lands off the notes
    wrong = CH.cover_di_onsets(di, di, {"L": -off, "R": -off})
    assert np.max(np.abs(wrong[::2] - (np.array(plucks) + off))) > 0.3


# --- onset minimum-rise edges (tonecheck) --------------------------------------------------------------------
def _bed_with_pluck(rise_db, seed=7, dur=4.0, t0=2.0):
    """Steady noise bed plus one pluck whose initial power is (10^(rise/10) - 1) x the bed's."""
    n = int(dur * FS)
    rng = np.random.default_rng(seed)
    x = 0.05 * rng.standard_normal(n)
    i = int(t0 * FS)
    amp = 0.05 * np.sqrt(10 ** (rise_db / 10) - 1)
    x[i:] += amp * np.exp(-np.arange(n - i) / FS / 0.08) * rng.standard_normal(n - i)
    return x


def _has_onset_near(det, t, tol=0.02):
    return bool(np.any(np.abs(np.asarray(det) - t) <= tol))


def test_onset_min_rise_edges():
    assert A.ONSET_MIN_RISE_DB == 6.0
    # ~4 dB rise: below the 6 dB check -> suppressed; ~9 dB: kept (3 seeds each)
    for seed in (7, 8, 9):
        assert not _has_onset_near(A.detect_onsets(_bed_with_pluck(4.0, seed), FS), 2.0)
        assert _has_onset_near(A.detect_onsets(_bed_with_pluck(9.0, seed), FS), 2.0)
