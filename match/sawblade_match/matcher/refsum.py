"""Blend reference: the sum of two same-take amp tracks (v0.4M Task F.1).

    python -m sawblade_match.matcher.refsum --a <hm2.wav> --b <body.wav> --out <blend.wav> [--blend-db A_DB,B_DB]
        [--polarity auto|asis|invert-a|invert-b] [--json r.json]

The album guitar is a blend of two amp tracks per side; the matcher's blend runs match against this sum. Both tracks are
reduced to mono the way the matcher does for ``--matched mono`` (channel mean), truncated to the shorter length and summed
``a * 10^(A_DB/20) + b * 10^(B_DB/20)`` (default 0,0: unity faders). The output is a float32 WAV: no clipping, no
normalisation, the peak is reported.

``--polarity auto|asis|invert-a|invert-b`` (default auto). The Bloodbath tracks are raw multitracks (no fader or polarity
choices were made), so the recorded phase has no authority: auto forms both candidate sums, a+b and a-b, and keeps the one
with the higher 60-250 Hz level (band-passed mono sum over the alignment-analysis region, the loudest 30 s), because two amps
on the same source add their lows coherently when in phase and cancel them when not. ``asis`` sums as recorded; ``invert-a`` /
``invert-b`` force the flip (the two sums differ only by overall sign; which one was used is recorded). The JSON's ``polarity``
block records ``mode``, ``chosen`` ("asis" | "invert-a" | "invert-b"; auto picks "asis" or "invert-b") and both low-band levels.
There is no time alignment: the lag stays as recorded (reported below). Faders stay at unity unless ``--blend-db`` is given.

The alignment check is report only: the tracks are never shifted. It reports the lag of the maximum |cross-correlation|
within +-50 ms (on the loudest 30 s, of the tracks as summed), its sign (``corrPolarity``) and the normalised correlation;
|lag| > 2 ms or a negative peak prints a ``WARNING:`` line, the sum is still written. Lag convention: positive = ``b`` is later than ``a`` (``b[n] ~ a[n - lag]``).

``refRatioDb = LUFS(a * gA) - LUFS(b * gB)`` (BS.1770, ``loudness.integrated_lufs``): at unity faders it is the two tracks'
loudness difference, not 0 dB. Exit codes: 0 ok, 2 unreadable input or sample rates differ. Deterministic (no randomness).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal

from .loudness import integrated_lufs

POLARITY_MODES = ("auto", "asis", "invert-a", "invert-b")
LOW_BAND_HZ = (60.0, 250.0)   # the band the auto polarity decision compares
MAX_LAG_S = 0.050          # alignment search range, +-50 ms
LAG_WARN_MS = 2.0          # |lag| above this prints a WARNING
WINDOW_S = 30.0            # the loudest section used for the alignment check


def read_mono(path: str | Path) -> tuple[np.ndarray, int]:
    """Any channel count -> mono float64 by the channel mean (as ``reference.load_reference(matched="mono")``)."""
    x, fs = sf.read(str(path), dtype="float64", always_2d=True)
    return x.mean(axis=1), int(fs)


def parse_blend_db(spec: str) -> tuple[float, float]:
    """``"A_DB,B_DB"`` -> (a, b). ValueError when it is not two finite numbers."""
    parts = str(spec).split(",")
    if len(parts) != 2:
        raise ValueError(f"--blend-db wants two numbers A_DB,B_DB, got {spec!r}")
    try:
        a, b = float(parts[0]), float(parts[1])
    except ValueError:
        raise ValueError(f"--blend-db wants two numbers A_DB,B_DB, got {spec!r}") from None
    if not (np.isfinite(a) and np.isfinite(b)):
        raise ValueError(f"--blend-db wants two finite numbers, got {spec!r}")
    return a, b


def loudest_window(a: np.ndarray, b: np.ndarray, fs: int, length_s: float = WINDOW_S) -> tuple[int, int]:
    """Sample range of the ``length_s`` section with the most energy in ``a`` and ``b`` together (1 s resolution);
    the whole signal when it is not longer."""
    n, w = len(a), int(round(length_s * fs))
    if n <= w:
        return 0, n
    hop = fs
    nb = n // hop
    e = np.array([float(np.sum(a[i * hop:(i + 1) * hop] ** 2) + np.sum(b[i * hop:(i + 1) * hop] ** 2)) for i in range(nb)])
    wb = max(1, int(round(length_s)))
    cs = np.concatenate([[0.0], np.cumsum(e)])
    best = int(np.argmax(cs[wb:] - cs[:-wb])) if nb >= wb else 0
    s = best * hop
    return s, min(n, s + w)


def alignment(a: np.ndarray, b: np.ndarray, fs: int) -> dict:
    """Lag (ms, +: b later than a), polarity (+1 / -1) and normalised correlation at the peak of |xcorr| within +-50 ms."""
    s, e = loudest_window(a, b, fs)
    a, b = a[s:e], b[s:e]
    na, nb_ = float(np.sqrt(np.sum(a * a))), float(np.sqrt(np.sum(b * b)))
    if na == 0.0 or nb_ == 0.0 or len(a) < 2:
        return {"lagMs": 0.0, "polarity": 1, "corr": None, "windowS": [s / fs, e / fs]}
    m = int(round(MAX_LAG_S * fs))
    nfft = int(2 ** np.ceil(np.log2(len(a) + m + 1)))
    # r[k] = sum_n a[n] b[n + k]  (peak at k = +d when b is a delayed by d)
    r = np.fft.irfft(np.conj(np.fft.rfft(a, nfft)) * np.fft.rfft(b, nfft), nfft)
    lags = np.arange(-m, m + 1)
    c = r[lags % nfft]
    k = int(np.argmax(np.abs(c)))
    corr = float(c[k] / (na * nb_))
    return {"lagMs": 1000.0 * float(lags[k]) / fs, "polarity": 1 if c[k] >= 0 else -1, "corr": corr,
            "windowS": [s / fs, e / fs]}


def _lufs(x: np.ndarray, fs: int) -> float:
    return float(integrated_lufs(x, fs))


def _json_num(v):
    if isinstance(v, float) and not np.isfinite(v):
        return None
    return v


def settings_key(a_db: float, b_db: float, polarity: str = "auto") -> str:
    """The settings a blend reference was built with, as one string stored in its JSON (``settingsKey``): the validation
    script rebuilds a stale ``<side>_blend.wav`` when this differs (faders or polarity mode changed; a JSON from before the
    polarity option has no key at all, so it counts as different)."""
    return f"gains={a_db:g},{b_db:g};polarity={polarity}"


def low_band_db(a: np.ndarray, b: np.ndarray, fs: int, sign_b: float) -> float:
    """RMS level (dB, re full scale) of the 60-250 Hz band of ``a + sign_b * b`` (``a`` and ``b`` are the analysis region)."""
    sos = signal.butter(4, list(LOW_BAND_HZ), btype="band", fs=fs, output="sos")
    y = signal.sosfilt(sos, a + sign_b * b)
    return float(10 * np.log10(max(float(np.mean(y * y)), 1e-30)))


def choose_polarity(a: np.ndarray, b: np.ndarray, fs: int, mode: str) -> dict:
    """The ``polarity`` block: ``mode``, ``chosen`` ("asis" | "invert-a" | "invert-b") and the low-band levels of both candidate
    sums a+b (``lowBandDbAsis``) and a-b (``lowBandDbInvert``), measured on the loudest 30 s (the alignment region). Auto keeps
    a+b unless a-b has the higher 60-250 Hz level (ties keep a+b); the two forced flips are the same sum up to overall sign."""
    s0, e0 = loudest_window(a, b, fs)
    aw, bw = a[s0:e0], b[s0:e0]
    asis, inv = low_band_db(aw, bw, fs, 1.0), low_band_db(aw, bw, fs, -1.0)
    chosen = {"asis": "asis", "invert-a": "invert-a", "invert-b": "invert-b"}.get(mode) or ("invert-b" if inv > asis else "asis")
    return {"mode": mode, "chosen": chosen, "lowBandDbAsis": asis, "lowBandDbInvert": inv, "lowBandHz": list(LOW_BAND_HZ),
            "windowS": [s0 / fs, e0 / fs]}


def blend_refs(a: np.ndarray, b: np.ndarray, fs: int, a_db: float = 0.0, b_db: float = 0.0,
               polarity: str = "auto") -> tuple[np.ndarray, dict]:
    """(float32 sum, report dict) of two mono float64 tracks at the same rate. ``polarity``: see the module docstring; the
    flip is applied BEFORE summing and the alignment report describes the tracks as summed. Loudness, so ``refRatioDb``, does
    not depend on polarity."""
    if polarity not in POLARITY_MODES:
        raise ValueError(f"polarity must be one of {POLARITY_MODES}, got {polarity!r}")
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    pol = choose_polarity(a * 10 ** (a_db / 20), b * 10 ** (b_db / 20), fs, polarity)     # judged at the faders in use
    if pol["chosen"] == "invert-a":
        a = -a
    elif pol["chosen"] == "invert-b":
        b = -b
    ga, gb = 10 ** (a_db / 20), 10 ** (b_db / 20)
    out = (a * ga + b * gb).astype(np.float32)
    al = alignment(a, b, fs)
    la, lb = _lufs(a * ga, fs), _lufs(b * gb, fs)
    peak = float(np.max(np.abs(out))) if n else 0.0
    rep = {"gainsDb": [a_db, b_db], "polarity": pol,
           "settingsKey": settings_key(a_db, b_db, polarity), "lagMs": al["lagMs"], "corrPolarity": al["polarity"], "corr": al["corr"],
           "peakDb": float(20 * np.log10(peak)) if peak > 0 else float("-inf"),
           "lufsA": la, "lufsB": lb, "refRatioDb": (la - lb) if np.isfinite(la) and np.isfinite(lb) else float("nan"),
           "sampleRate": fs, "samples": int(n), "alignmentWindowS": al["windowS"]}
    return out, rep


def warnings_for(rep: dict) -> list[str]:
    w = []
    if abs(rep["lagMs"]) > LAG_WARN_MS:
        w.append(f"WARNING: the tracks are {rep['lagMs']:+.2f} ms apart (|lag| > {LAG_WARN_MS:g} ms); the sum is a comb filter, "
                 "check the interface latency / alignment of the two amp tracks")
    if rep["corrPolarity"] < 0:
        w.append("WARNING: the correlation peak of the summed tracks is negative (they are in opposite polarity as summed; "
                 "--polarity auto would have flipped one unless the low band says otherwise)")
    return w


def glue_blend_db(argv) -> list[str]:
    """argparse takes ``--blend-db -3,2`` for a flag; glue the value on (``--blend-db=-3,2``)."""
    argv, out, i = list(sys.argv[1:] if argv is None else argv), [], 0
    while i < len(argv):
        if argv[i] == "--blend-db" and i + 1 < len(argv):
            out.append(f"--blend-db={argv[i + 1]}")
            i += 2
        else:
            out.append(argv[i])
            i += 1
    return out


def main(argv=None) -> int:
    argv = glue_blend_db(argv)
    ap = argparse.ArgumentParser(prog="refsum", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", default=None, help="first amp track (e.g. the HM2 amp)")
    ap.add_argument("--b", default=None, help="second amp track (e.g. the body amp)")
    ap.add_argument("--out", default=None, help="output float32 WAV (mono)")
    ap.add_argument("--polarity", default="auto", choices=POLARITY_MODES,
                    help="auto (default): keep a+b or a-b, whichever has more 60-250 Hz; asis: as recorded; invert-a / invert-b: "
                         "force the flip")
    ap.add_argument("--settings-key", action="store_true",
                    help="print the settings key of --blend-db / --polarity and exit (no files read); the script compares it "
                         "with the settingsKey stored in an existing reference's JSON")
    ap.add_argument("--blend-db", default="0,0", help="A_DB,B_DB faders (default 0,0)")
    ap.add_argument("--json", default=None, help="write the report here")
    args = ap.parse_args(argv)
    try:
        a_db, b_db = parse_blend_db(args.blend_db)
    except ValueError as e:
        print(f"refsum: {e}", file=sys.stderr)
        return 2
    if args.settings_key:
        print(settings_key(a_db, b_db, args.polarity))
        return 0
    if not (args.a and args.b and args.out):
        print("refsum: --a, --b and --out are required", file=sys.stderr)
        return 2
    try:
        a, fs = read_mono(args.a)
        b, fb = read_mono(args.b)
        if fs != fb:
            print(f"refsum: sample rates differ ({args.a}: {fs} Hz, {args.b}: {fb} Hz); resample first", file=sys.stderr)
            return 2
    except Exception as e:                    # unreadable file (soundfile raises RuntimeError / LibsndfileError)
        print(f"refsum: cannot read input: {e}", file=sys.stderr)
        return 2
    out, rep = blend_refs(a, b, fs, a_db, b_db, args.polarity)
    rep.update({"a": str(args.a), "b": str(args.b), "out": str(args.out),
                "randomness": "none (deterministic)"})
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(args.out), out, fs, subtype="FLOAT")
    wl = warnings_for(rep)
    rat = rep["refRatioDb"]
    print(f"refsum: {len(out) / fs:.1f} s at {fs} Hz, faders {a_db:+.1f} / {b_db:+.1f} dB"
          + f" -> {args.out}")
    print(f"  peak {rep['peakDb']:.1f} dBFS; LUFS A {rep['lufsA']:.1f}, B {rep['lufsB']:.1f}; "
          f"reference ratio A - B = {rat:+.2f} dB")
    pl = rep["polarity"]
    print(f"  polarity {pl['mode']} -> {pl['chosen']}: 60-250 Hz level as recorded {pl['lowBandDbAsis']:.1f} dB, "
          f"b inverted {pl['lowBandDbInvert']:.1f} dB")
    print(f"  alignment: lag {rep['lagMs']:+.2f} ms (not shifted), correlation sign {rep['corrPolarity']:+d}, corr "
          + ("n/a" if rep["corr"] is None else f"{rep['corr']:+.3f}"))
    for line in wl:
        print(line)
    rep["warnings"] = wl
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({k: _json_num(v) for k, v in rep.items()}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
