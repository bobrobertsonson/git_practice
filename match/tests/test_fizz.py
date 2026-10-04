"""Phase 3.4 (fizz fix): stem reference basis, HF texture loss, full-mix ceiling, roll-off search, fizz_texture rule.
No network, no demucs: synthetic stems."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

from sawblade_match.calibrate.channels import stem_guitar_signal
from sawblade_match.matcher import loss as L
from sawblade_match.matcher.reference import build_target, load_reference, make_excerpt
from sawblade_match.matcher.space import Space, post_eq
from sawblade_match.tonecheck import analysis as A
from sawblade_match.tonecheck.rules import FIZZ_TEXTURE_PENDING, fizz_texture_rule

FS = 48000


def _guitar(seed: int, fizz_db: float | None = None, n: int = FS * 8) -> np.ndarray:
    """Palm-muted chord stand-in: harmonic partials with per-note decay, smooth above ~4 kHz. ``fizz_db``: adds a
    noise-like 5-12 kHz layer that far below the 1-3 kHz level (the amp-sim fizz / cymbal-like texture)."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS
    x = np.zeros(n)
    for k in range(16):
        t0 = k * 0.5
        env = np.where(t >= t0, np.exp(-(t - t0) * 3.0), 0.0) * (t < t0 + 0.5)
        f0 = 82.4 * (1 + (k % 3) * 0.335)
        for h in range(1, 120):
            f = f0 * h
            if f < 11000:        # partials roll off steeply above 3.5 kHz (cab + mic): smooth, tonal, not noisy
                x += env * np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) / h ** 0.7 * min(1.0, (3500 / f) ** 5)
    x /= np.sqrt(np.mean(x ** 2))
    if fizz_db is not None:
        hp = signal.butter(4, [5000, 12000], btype="band", fs=FS, output="sos")
        nz = signal.sosfilt(hp, rng.standard_normal(n))
        nz *= np.abs(x) > 0.05            # follows the playing
        nz /= np.sqrt(np.mean(nz ** 2)) + 1e-12
        x = x + nz * 10 ** (fizz_db / 20)
    return x * 0.1


def _target(x, **kw):
    starts = L.segment_starts(len(x), None)
    return L.Target(starts, L.features(x, starts, None), None, None, None, **kw)


def test_texture_term_ranks_smooth_chain_above_fizzy_chain():
    """Against the smooth stem target, a fizzy render (v3 stand-in) scores clearly worse than a smooth one (v0)."""
    stem = _guitar(1)
    smooth, fizzy = _guitar(2), _guitar(3, fizz_db=-18.0)
    tgt = _target(stem, texture=True)
    rs, rf = L.evaluate(smooth, tgt), L.evaluate(fizzy, tgt)
    assert rs.tex < 1.0 and rf.tex > 3.0
    assert rf.total > rs.total + 2.0
    f_s, f_f = L.features(smooth, tgt.starts, None), L.features(fizzy, tgt.starts, None)
    assert f_f.flat > f_s.flat + 0.1 and f_f.hf_db > f_s.hf_db + 10
    off = L.evaluate(fizzy, _target(stem, texture=False))
    assert off.tex == 0.0 and off.total < rf.total      # the term is what separates them


def test_ltas_hf_limit_is_one_sided_ceiling():
    nb = len(L.BAND_CENTRES)
    ref = np.zeros(nb)
    hi = L.BAND_UPPER > L.HF_LIMIT_HZ
    assert hi.any() and (~hi).any()
    darker, brighter = ref.copy(), ref.copy()
    darker[hi] -= 12.0
    brighter[hi] += 12.0
    assert L.ltas_error(darker, ref, L.HF_LIMIT_HZ)[0] == pytest.approx(0.0, abs=1e-9)
    assert L.ltas_error(brighter, ref, L.HF_LIMIT_HZ)[0] > 3.0
    assert L.ltas_error(darker, ref)[0] > 3.0            # without the limit, darker is penalised too
    e, off = L.ltas_error(ref + 5.0, ref, L.HF_LIMIT_HZ)
    assert e == pytest.approx(0.0, abs=1e-9) and off == pytest.approx(5.0)


def test_fullmix_fallback_never_rewards_brightness_and_ignores_texture_term():
    mix = _guitar(1, fizz_db=-12.0)               # "cymbals" above 5 kHz in the reference
    tgt = _target(mix, hf_limit_hz=L.HF_LIMIT_HZ)
    r_dark = L.evaluate(_guitar(2), tgt)
    r_bright = L.evaluate(_guitar(3, fizz_db=-6.0), tgt)
    assert r_dark.tex == 0.0 and r_bright.tex >= 0.0
    assert r_dark.total < r_bright.total


def test_stem_channel_choice_and_offsets():
    rng = np.random.default_rng(0)
    a, b = rng.standard_normal(FS), rng.standard_normal(FS)
    sig, kind, off, info = stem_guitar_signal(np.stack([a, b], 1))          # hard-panned, uncorrelated
    assert kind == "side" and off == pytest.approx(3.0103, abs=1e-3) and info["sideMidDb"] > -1
    c = a[:, None] * np.ones((1, 2))
    sig, kind, off, _ = stem_guitar_signal(c + 1e-4 * rng.standard_normal((FS, 2)))   # centred
    assert kind == "mid" and off == 0.0


def _write(path, x, fs=FS):
    sf.write(str(path), x, fs, subtype="FLOAT")


def test_load_reference_prefers_cached_stem_and_falls_back_to_full_mix(tmp_path):
    from sawblade_match.calibrate.separation import stem_cache_path
    rng = np.random.default_rng(0)
    gl, gr = _guitar(1, n=FS * 6), _guitar(2, n=FS * 6)
    cym = _guitar(9, fizz_db=-3.0, n=FS * 6)
    mix = tmp_path / "song.wav"
    _write(mix, np.stack([gl + cym, gr + cym * 0.5], 1))
    ref = load_reference(mix, channel="auto", stems_dir=tmp_path / "stems")        # no stem: fallback
    assert ref.basis == "side" and ref.hf_limit_hz == L.HF_LIMIT_HZ and not ref.texture
    sd = tmp_path / "stems"
    sd.mkdir()
    _write(stem_cache_path(mix, sd), np.stack([gl, gr], 1))
    ref = load_reference(mix, channel="auto", stems_dir=sd)
    assert ref.basis.startswith("stem:") and ref.stem_channel == "side"
    assert ref.texture and ref.hf_limit_hz is None
    assert ref.level_offset_db == pytest.approx(3.0103, abs=1e-3)
    # the stem target carries the smooth texture, the full-mix side channel the cymbals
    fb = load_reference(mix, channel="side")
    ex = make_excerpt(np.asarray(gl, np.float32), 5.0)
    t_stem, t_mix = build_target(ref, ex), build_target(fb, ex)
    assert t_stem.texture and t_stem.hf_limit_hz is None and t_mix.hf_limit_hz == L.HF_LIMIT_HZ
    assert t_stem.ref.flat < t_mix.ref.flat - 0.05


def test_matched_stft_is_limited_to_a_mix_channel_band():
    ref = _guitar(1)
    fizz = ref + _guitar(3, fizz_db=-6.0) - _guitar(3)
    wide = L.stft_loss(fizz, ref, fmax=8000.0)
    capped = L.stft_loss(fizz, ref, fmax=L.STFT_FMAX_MIX)
    assert wide > capped


def test_post_rolloff_params_are_neutral_by_default_and_emitted():
    sp = Space((1, 1))
    v = sp.default()
    assert not [b for b in post_eq(v) if b["type"] in ("highShelf", "lowPass")]
    assert sp.params[sp.idx["post.shelf_g"]].lo == -8.0 and sp.params[sp.idx["post.shelf_g"]].hi == 0.0
    assert (sp.params[sp.idx["post.shelf_f"]].lo, sp.params[sp.idx["post.shelf_f"]].hi) == (3000.0, 7000.0)
    assert (sp.params[sp.idx["post.lp"]].lo, sp.params[sp.idx["post.lp"]].hi) == (5000.0, 12000.0)
    assert "post.shelf_g" not in [p.name for p in sp.params if p.eq_gain]
    v.update({"post.shelf_g": -5.0, "post.shelf_f": 4500.0, "post.lp": 7000.0})
    bands = post_eq(v)
    assert {"type": "highShelf", "freq": 4500.0, "gainDb": -5.0, "q": 0.707} in bands
    assert {"type": "lowPass", "freq": 7000.0, "q": 0.707} in bands
    hi = sp.decode(np.ones(len(sp)))
    assert not [b for b in post_eq(hi) if b["type"] == "lowPass"]       # upper bound = low-pass off


def test_fizz_texture_metric_and_pending_rule():
    smooth, fizzy = _guitar(2), _guitar(3, fizz_db=-18.0)
    ms, mf = (A.fizz_texture(x, None) for x in (smooth, fizzy))
    assert mf["flatness5to10k"] > ms["flatness5to10k"] + 0.1
    row = fizz_texture_rule(mf, {})
    assert row["id"] == "fizz_texture" and row["status"] == "n/a" and row["valueStatus"] == FIZZ_TEXTURE_PENDING
    approved = {"metrics": {"fizzTexture": {"target": 0.05}}}
    assert fizz_texture_rule(mf, approved)["status"] == "fail" and fizz_texture_rule(ms, approved)["status"] == "pass"


def test_explicit_full_mix_channel_without_hf_limit_gets_a_note(tmp_path):
    mix = tmp_path / "song.wav"
    _write(mix, np.stack([_guitar(1, n=FS * 3), _guitar(2, n=FS * 3)], 1))
    hint = "--ref-hf-limit 4500"
    for ch in ("left", "right", "mid"):
        ref = load_reference(mix, channel=ch)
        assert ref.hf_limit_hz is None and any(hint in n and ch in n for n in ref.notes), ref.notes
    assert not any(hint in n for n in load_reference(mix, channel="mid", hf_limit_hz=4500.0).notes)
    assert not any(hint in n for n in load_reference(mix, channel="side").notes)
    assert not any(hint in n for n in load_reference(mix, channel="auto", stems_dir=tmp_path / "none").notes)
