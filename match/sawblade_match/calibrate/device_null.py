"""``sawblade-calibrate device-null``: how close is a hardware loader (e.g. the Darkglass Anagram) to the plugin's render?

The user plays one DI through the device (interface out -> device -> interface in) and records it; the same DI goes
through the plugin (or through ``--preset`` + ``--di`` here, rendered by the core).  This tool

1. **aligns** the recording to the render: a coarse lag from the 1 kHz-decimated envelopes (robust to distortion and
   to the device's different filtering), refined on the band-limited waveforms to a fraction of a sample (parabolic peak,
   applied as an exact FFT phase ramp);
2. **gain-matches** it (least squares, and detects polarity: reported, or forced with ``--polarity``);
3. reports the **residual** (device minus gain-matched render) overall and per octave band, plus the per-band level
   difference (the device path's linear filtering) and the residual left after correcting each band's level (what is NOT
   a plain per-band level/filter difference: noise, non-linearity, time-varying behaviour);
4. writes a level-matched, aligned **A/B listening pair** of a 30 s excerpt.

Pure numpy/scipy analysis: no DSP of the amp chain here (the render comes from ``sawblade_core`` or a file).  Offline,
deterministic (no randomness).

Tolerance (a proposal, ``--tolerance-db``): overall residual <= -30 dB re the render counts as a match.  Clean converters
and a faithful device null much deeper (-40 dB and below); ~-20 dB is audibly different.
"""
from __future__ import annotations

import argparse
import json
import sys
from fractions import Fraction
from pathlib import Path
from typing import Sequence

import numpy as np
import soundfile as sf
from scipy import signal

RATE = 48000
ENV_RATE = 1000
ENV_SMOOTH_S = 0.005
BAND_EDGES_HZ = (0.0, 88.0, 177.0, 354.0, 707.0, 1414.0, 2828.0, 5657.0, 11314.0, RATE / 2)
BAND_CENTRES_HZ = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)
EDGE_S = 0.25                    # dropped at both ends of the comparison region (warm-up, FFT shift edges)
MIN_REGION_S = 1.0
FINE_SEARCH_MS = 5.0
MAIN_LOBE = RATE // 2000      # +-0.5 ms around the correlation peak count as the main lobe
AMBIGUOUS_BELOW = 1.05
DEFAULT_TOLERANCE_DB = -30.0
VERDICTS = ((-40.0, "indistinguishable by this measure (residual <= -40 dB)"),
            (-30.0, "very close (residual <= -30 dB)"),
            (-20.0, "close but likely audible on a careful A/B (residual <= -20 dB)"),
            (float("inf"), "clearly different (residual above -20 dB)"))


def _db(p: float) -> float:
    return float(10.0 * np.log10(max(p, 1e-30)))


def read_mono48(path, channel: str = "left") -> tuple[np.ndarray, int]:
    """(mono float64 at 48 kHz, original rate)."""
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    if channel == "mean":
        m = x.mean(axis=1)
    elif channel == "right":
        m = x[:, min(1, x.shape[1] - 1)]
    else:
        m = x[:, 0]
    return resample48(m, fs), int(fs)


def resample48(x: np.ndarray, fs: int) -> np.ndarray:
    if int(fs) == RATE:
        return np.asarray(x, np.float64)
    f = Fraction(RATE, int(fs))
    return signal.resample_poly(x, f.numerator, f.denominator)


def _xcorr(a: np.ndarray, b: np.ndarray, max_lag: int) -> tuple[np.ndarray, np.ndarray]:
    """Cross-correlation c[k] = sum_n a[n + k] * b[n] for k in [-max_lag, max_lag] (positive k: ``a`` is later than ``b``),
    via FFT.  Returns (lags, c)."""
    n = len(a) + len(b)
    nfft = 1 << int(np.ceil(np.log2(max(n, 2))))
    fa, fb = np.fft.rfft(a, nfft), np.fft.rfft(b, nfft)
    c = np.fft.irfft(fa * np.conj(fb), nfft)
    lags = np.arange(-max_lag, max_lag + 1)
    return lags, c[lags % nfft]


def _envelope(x: np.ndarray) -> np.ndarray:
    k = max(1, int(round(ENV_SMOOTH_S * RATE)))
    e = np.convolve(np.abs(x), np.ones(k) / k, mode="same")
    d = RATE // ENV_RATE
    e = e[: len(e) // d * d].reshape(-1, d).mean(axis=1)
    return e - e.mean()


def coarse_lag_ms(rec: np.ndarray, ref: np.ndarray, max_lag_ms: float) -> float:
    """Lag (ms) of ``rec`` relative to ``ref`` from the envelopes (positive = the recording is later)."""
    er, ef = _envelope(rec), _envelope(ref)
    lags, c = _xcorr(er, ef, int(max_lag_ms * ENV_RATE / 1000))
    return float(lags[int(np.argmax(c))] * 1000.0 / ENV_RATE)


def _bandlimit(x: np.ndarray, lo: float = 80.0, hi: float = 6000.0) -> np.ndarray:
    sos = signal.butter(2, [lo, hi], btype="bandpass", fs=RATE, output="sos")
    return signal.sosfiltfilt(sos, x)


def fine_lag_samples(rec: np.ndarray, ref: np.ndarray, coarse_ms: float) -> tuple[float, float, float]:
    """(lag in samples, normalised correlation at the peak signed (-1..1), confidence = peak / second-best-peak ratio)
    of ``rec`` vs ``ref`` within +-5 ms of the coarse lag; sub-sample by parabolic interpolation of |c|."""
    a, b = _bandlimit(rec), _bandlimit(ref)
    centre = int(round(coarse_ms * RATE / 1000))
    half = int(FINE_SEARCH_MS * RATE / 1000)
    lags, c = _xcorr(a, b, abs(centre) + half + 2)
    sel = (lags >= centre - half) & (lags <= centre + half)
    ls, cs = lags[sel], c[sel]
    i = int(np.argmax(np.abs(cs)))
    frac = 0.0
    if 0 < i < len(cs) - 1:
        y0, y1, y2 = np.abs(cs[i - 1]), np.abs(cs[i]), np.abs(cs[i + 1])
        den = y0 - 2 * y1 + y2
        if den != 0:
            frac = float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5))
    # normalise by the energies of the overlapping part
    lag = int(ls[i])
    n0, n1 = max(0, lag), min(len(a), len(b) + lag)
    ea = float(np.sum(a[n0:n1] ** 2)) if n1 > n0 else 0.0
    eb = float(np.sum(b[n0 - lag:n1 - lag] ** 2)) if n1 > n0 else 0.0
    rho = float(cs[i] / max(np.sqrt(ea * eb), 1e-30))
    others = np.abs(cs).copy()
    others[max(0, i - MAIN_LOBE):i + MAIN_LOBE + 1] = 0.0
    conf = float(np.abs(cs[i]) / max(float(others.max()), 1e-30)) if len(others) else float("inf")
    return lag + frac, rho, conf


def shift(x: np.ndarray, d: float, n_out: int) -> np.ndarray:
    """y[n] = x[n - d] (``d`` may be fractional, positive = delay) for n in [0, n_out), by an exact FFT phase ramp on a
    zero-padded copy (the padding keeps the circular wrap away from the output)."""
    pad = int(np.ceil(abs(d))) + 64
    m = len(x) + 2 * pad
    nfft = 1 << int(np.ceil(np.log2(max(m, n_out + 2 * pad))))
    buf = np.zeros(nfft)
    buf[pad:pad + len(x)] = x
    k = np.fft.rfftfreq(nfft)
    y = np.fft.irfft(np.fft.rfft(buf) * np.exp(-2j * np.pi * k * d), nfft)
    return y[pad:pad + n_out]


def _band_masks(n: int) -> list[np.ndarray]:
    f = np.fft.rfftfreq(n, 1.0 / RATE)
    return [(f >= lo) & (f < hi) if hi < RATE / 2 else (f >= lo) for lo, hi in zip(BAND_EDGES_HZ[:-1], BAND_EDGES_HZ[1:])]


def analyse(rec: np.ndarray, ref: np.ndarray, max_lag_ms: float = 1500.0, polarity: str = "auto",
            di: np.ndarray | None = None) -> dict:
    """Align, gain-match and compare.  ``rec`` / ``ref`` are mono float at 48 kHz.  Returns the report dict plus the aligned
    arrays under ``_rec`` / ``_ref`` (gain-matched render) for the listening files (not JSON)."""
    warnings: list[str] = []
    if len(rec) < RATE or len(ref) < RATE:
        raise ValueError("recordings shorter than 1 s cannot be compared")
    if np.max(np.abs(rec)) < 1e-4:
        raise ValueError("the recording is silent (peak below -80 dBFS): check the interface input / channel")
    if np.max(np.abs(rec)) >= 0.999:
        warnings.append("the recording clips (peak at full scale): lower the interface input gain and re-record; the residual "
                        "includes the clipping")
    coarse = coarse_lag_ms(rec, ref, max_lag_ms)
    lag, rho, conf = fine_lag_samples(rec, ref, coarse)
    sign = {"auto": 1.0 if rho >= 0 else -1.0, "normal": 1.0, "invert": -1.0}[polarity]
    edge = int(EDGE_S * RATE)
    n0 = max(int(np.ceil(lag)), 0) + edge
    n1 = min(len(rec), len(ref) + int(np.floor(lag))) - edge
    if n1 - n0 < MIN_REGION_S * RATE:
        raise ValueError(f"only {(n1 - n0) / RATE:.2f} s of the recording overlaps the render after alignment (lag "
                         f"{lag / RATE * 1000:.1f} ms): is this the recording of the same DI?")
    ref_d = shift(ref, lag, len(rec))
    a, b = rec[n0:n1], sign * ref_d[n0:n1]
    g = float(np.dot(a, b) / max(float(np.dot(b, b)), 1e-30))          # least squares gain, polarity already applied
    if polarity != "auto" and g < 0:
        warnings.append(f"the forced polarity '{polarity}' disagrees with the recording (it correlates inverted): the residual "
                        "below is for the forced polarity, i.e. very bad on purpose; use --polarity auto")
        g = abs(g)
    matched = g * b
    resid = a - matched
    p_sig, p_res, p_rec = float(np.sum(matched ** 2)), float(np.sum(resid ** 2)), float(np.sum(a ** 2))

    # per octave band (rfft partition of the region: bands are disjoint, sums are Parseval-consistent)
    A, B = np.fft.rfft(a), np.fft.rfft(b)
    bands = []
    after_gain_res = 0.0
    for fc, m in zip(BAND_CENTRES_HZ, _band_masks(len(a))):
        pa, pb = float(np.sum(np.abs(A[m]) ** 2)), float(np.sum(np.abs(B[m]) ** 2))
        pm = (g * g) * pb
        pr = float(np.sum(np.abs(A[m] - g * B[m]) ** 2))
        gb = float(np.real(np.sum(A[m] * np.conj(B[m]))) / max(pb, 1e-30))     # this band's own least-squares gain
        pr_b = float(np.sum(np.abs(A[m] - gb * B[m]) ** 2))
        after_gain_res += pr_b
        bands.append({"centreHz": fc, "renderDb": _db(pm), "deviceDb": _db(pa), "levelDiffDb": _db(pa) - _db(pm),
                      "residualDb": _db(pr) - _db(pm), "residualAfterBandGainDb": _db(pr_b) - _db(pm)})
    resid_after = _db(after_gain_res) - _db(float(np.sum(np.abs(B) ** 2)) * g * g)
    overall = _db(p_res) - _db(p_sig)
    spread = [bd["levelDiffDb"] for bd in bands if bd["renderDb"] > _db(p_sig) - 40.0]     # bands with real content
    out = {"alignment": {"latencyMs": lag / RATE * 1000.0, "latencySamples": float(lag), "coarseEnvelopeLagMs": coarse,
                         "correlation": rho, "peakConfidence": conf, "comparedSeconds": (n1 - n0) / RATE,
                         "note": "positive latency = the recording is later than the render (device + interface round trip)"},
           "polarity": {"mode": polarity, "applied": "inverted" if sign < 0 else "normal",
                        "detectedFromCorrelation": "inverted" if rho < 0 else "normal"},
           "gain": {"deviceVsRenderDb": 20 * np.log10(max(abs(g), 1e-30)), "linear": g,
                    "rmsDeviceDbfs": _db(p_rec / len(a)), "rmsRenderDbfs": _db(float(np.sum(b ** 2)) / len(a)),
                    "peakDeviceDbfs": 20 * np.log10(max(float(np.max(np.abs(rec))), 1e-30))},
           "residual": {"overallDb": overall, "esr": p_res / max(p_rec, 1e-30),
                        "afterPerBandGainDb": resid_after,
                        "note": "dB re the gain-matched render (more negative = better null); afterPerBandGain = what is left "
                                "when every octave band's level is also corrected, i.e. NOT a plain per-band level / filter difference"},
           "bands": bands,
           "bandLevelSpreadDb": (max(spread) - min(spread)) if spread else 0.0}
    if conf < AMBIGUOUS_BELOW:
        warnings.append(f"the alignment peak is ambiguous (confidence {conf:.2f}): check that the recording is of the same DI "
                        "and that the render was made with the same preset")
    if abs(rho) < 0.3:
        warnings.append(f"low waveform correlation ({rho:.2f}): the device sounds quite different, or the files do not match")
    if di is not None:
        d_lag = coarse_lag_ms(rec, di, max_lag_ms)
        out["alignment"]["lagVsDiMs"] = d_lag
        if abs(d_lag - coarse) > 2.0:
            warnings.append(f"the recording's lag vs the DI ({d_lag:.1f} ms) differs from its lag vs the render ({coarse:.1f} ms) "
                            "by more than 2 ms: the render is not sample-aligned to the DI (plugin latency not compensated?)")
    out["warnings"] = warnings
    out["_rec"], out["_ref"] = a, matched
    return out


def verdict(overall_db: float, tolerance_db: float, rep: dict) -> dict:
    label = next(t for lim, t in VERDICTS if overall_db <= lim)
    hints = []
    al, ga = rep["alignment"], rep["gain"]
    if abs(al["latencyMs"]) > 0.0:
        hints.append(f"latency: the recording is {al['latencyMs']:.2f} ms later than the render (already removed)")
    if abs(ga["deviceVsRenderDb"]) > 0.5:
        hints.append(f"level: the device output is {ga['deviceVsRenderDb']:+.1f} dB vs the render (already matched)")
    if rep["bandLevelSpreadDb"] > 1.5:
        top = max(b["renderDb"] for b in rep["bands"])
        worst = max((b for b in rep["bands"] if b["renderDb"] > top - 40.0), key=lambda b: abs(b["levelDiffDb"]))
        hints.append(f"filtering: octave-band levels differ by up to {rep['bandLevelSpreadDb']:.1f} dB between the device and the "
                     f"render (largest at {worst['centreHz']} Hz: {worst['levelDiffDb']:+.1f} dB): a tone / filter in the device path")
    if rep["residual"]["afterPerBandGainDb"] > overall_db - 3.0 and overall_db > tolerance_db:
        hints.append("the residual is not explained by per-band level: noise, non-linear behaviour or a model / settings difference")
    return {"withinTolerance": bool(overall_db <= tolerance_db), "toleranceDb": tolerance_db, "overallResidualDb": overall_db,
            "summary": label, "whatDiffers": hints}


def excerpt_window(x: np.ndarray, seconds: float) -> tuple[int, int]:
    """The loudest ``seconds`` window of ``x`` (whole signal if shorter)."""
    n = int(seconds * RATE)
    if len(x) <= n:
        return 0, len(x)
    hop = RATE // 2
    c = np.concatenate([[0.0], np.cumsum(x.astype(np.float64) ** 2)])
    starts = np.arange(0, len(x) - n + 1, hop)
    e = c[starts + n] - c[starts]
    s = int(starts[int(np.argmax(e))])
    return s, s + n


def write_listening(out_dir: Path, rec: np.ndarray, ref_matched: np.ndarray, seconds: float = 30.0) -> dict:
    """``listen/``: ``ab_render_then_device`` (render, gap, device: level-matched by RMS, peak-limited), plus the two aligned
    excerpts as separate files (device scaled by the matched gain: it sits at the render's level)."""
    from ..export.audio import listening_ab
    s, e = excerpt_window(ref_matched, seconds)
    r, d = ref_matched[s:e], rec[s:e]
    lst = out_dir / "listen"
    info = listening_ab(r, d, lst, name="ab_render_then_device")
    peak = max(float(np.max(np.abs(r))), float(np.max(np.abs(d))), 1e-9)
    g = min(1.0, 10 ** (-1.0 / 20) / peak)
    sf.write(str(lst / "render_aligned.wav"), (r * g).astype(np.float32), RATE, subtype="PCM_24")
    sf.write(str(lst / "device_aligned_levelmatched.wav"), (d * g).astype(np.float32), RATE, subtype="PCM_24")
    info.update({"excerptSeconds": (e - s) / RATE, "excerptStartS": s / RATE, "files": ["ab_render_then_device.wav",
                 "render_aligned.wav", "device_aligned_levelmatched.wav"]})
    return info


def _json(o):
    if isinstance(o, dict):
        return {k: _json(v) for k, v in o.items() if not k.startswith("_")}
    if isinstance(o, (list, tuple)):
        return [_json(v) for v in o]
    if isinstance(o, (np.floating, float)):
        return None if not np.isfinite(o) else round(float(o), 4)
    if isinstance(o, np.integer):
        return int(o)
    return o


def render_preset(preset_path, di_path) -> np.ndarray:
    """Render ``di_path`` through ``preset_path`` with the C++ core (the engine of the plugin and ``tonerender``), at 48 kHz."""
    from ..core import CaptureCache
    from ..export.chain import load_preset, render48
    preset, base = load_preset(preset_path)
    x, fs = sf.read(str(di_path), dtype="float32", always_2d=True)
    y, _ = render48(preset, np.ascontiguousarray(x[:, 0]), base, CaptureCache(), int(fs))
    return y.astype(np.float64)


def run(a: argparse.Namespace) -> int:
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rec, rec_fs = read_mono48(a.recording, a.channel)
    di = read_mono48(a.di, "left")[0] if a.di else None
    if a.render:
        ref, _ = read_mono48(a.render, "left")
        render_src = str(a.render)
    elif a.preset and a.di:
        ref = render_preset(a.preset, a.di)
        sf.write(str(out / "render.wav"), ref.astype(np.float32), RATE, subtype="FLOAT")
        render_src = f"{a.preset} rendered from {a.di} (core, written to {out / 'render.wav'})"
    else:
        raise ValueError("give --render RENDER.wav (the plugin's render of the same DI), or --preset P.json together with --di DI.wav")
    rep = analyse(rec, ref, a.max_lag_ms, a.polarity, di)
    rec_a, ref_m = rep["_rec"], rep["_ref"]
    v = verdict(rep["residual"]["overallDb"], a.tolerance_db, rep)
    listen = write_listening(out, rec_a, ref_m, a.excerpt_s)
    full = {"schema": "sawblade.device_null", "version": 1, "inputs": {"recording": str(a.recording), "recordingRateHz": rec_fs,
            "channel": a.channel, "render": render_src, "di": str(a.di) if a.di else None, "sampleRateHz": RATE},
            **{k: v_ for k, v_ in rep.items() if not k.startswith("_")}, "verdict": v, "listening": listen}
    (out / "device_null_report.json").write_text(json.dumps(_json(full), indent=2))
    al, ga = rep["alignment"], rep["gain"]
    print(f"latency {al['latencyMs']:+.3f} ms ({al['latencySamples']:+.2f} samples); gain {ga['deviceVsRenderDb']:+.2f} dB; "
          f"polarity {rep['polarity']['applied']}; correlation {al['correlation']:.3f}")
    print(f"residual {rep['residual']['overallDb']:.1f} dB re the render (ESR {rep['residual']['esr']:.5f}); "
          f"after per-band level correction {rep['residual']['afterPerBandGainDb']:.1f} dB")
    print("octave band (Hz): " + "  ".join(f"{b['centreHz']}: {b['residualDb']:+.0f} dB (level {b['levelDiffDb']:+.1f})" for b in rep["bands"]))
    print(f"verdict: {v['summary']}; tolerance {a.tolerance_db:g} dB: {'WITHIN' if v['withinTolerance'] else 'NOT within'}")
    for h in v["whatDiffers"]:
        print(f"  - {h}")
    for w in rep["warnings"]:
        print(f"  warning: {w}")
    print(f"wrote {out / 'device_null_report.json'} and {out / 'listen'}/")
    return 0


def build_parser(prog: str = "sawblade-calibrate device-null") -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog=prog, description=__doc__.split("\n\n")[0] +
                                " Aligns the device recording with the plugin render, matches gain, and reports the residual "
                                "(overall and per octave band) plus an A/B listening pair.")
    p.add_argument("--recording", required=True, help="the device recording (WAV): the DI re-amped through the device")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--render", help="the plugin's render of the same DI with the same preset (WAV, sample-aligned to the DI)")
    g.add_argument("--preset", help="preset JSON to render --di with (the core, same engine as the plugin / tonerender)")
    p.add_argument("--di", help="the DI that was played into the device (needed with --preset; with --render it is only a "
                                "latency cross-check)")
    p.add_argument("--channel", choices=("left", "right", "mean"), default="left", help="recording channel to use (default left)")
    p.add_argument("--polarity", choices=("auto", "normal", "invert"), default="auto",
                   help="auto: detected from the correlation and reported (default); or force it")
    p.add_argument("--max-lag-ms", type=float, default=1500.0, help="largest round-trip latency to search (default 1500 ms)")
    p.add_argument("--tolerance-db", type=float, default=DEFAULT_TOLERANCE_DB,
                   help=f"overall residual (dB re the render) that counts as a match (default {DEFAULT_TOLERANCE_DB:g})")
    p.add_argument("--excerpt-s", type=float, default=30.0, help="length of the listening excerpt (the loudest window; default 30)")
    p.add_argument("--out", required=True, help="output directory: device_null_report.json and listen/")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    try:
        return run(a)
    except (ValueError, OSError, RuntimeError) as e:
        print("error: " + " ".join(str(e).split()), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
