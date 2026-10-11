#!/usr/bin/env python3
"""Run real vocal takes through RASP and compare them with finished references.

  render   Render every take through every preset (or the ones you name).
  analyze  Measure loudness, density, tone, width and sibilance.
  match    Compare a render against a reference vocal (ideally an isolated
           final vocal; a full mix works but the band skews the tone).

  python3 tools/takes.py render samples/raw/*.wav
  python3 tools/takes.py render samples/raw/verse.wav --presets huge,dark
  python3 tools/takes.py analyze renders/*.wav samples/ref/*.wav
  python3 tools/takes.py match samples/ref/final_vocal.wav renders/verse__huge.wav

Needs numpy and ffmpeg (any input format ffmpeg reads). Renders go to
rasp/renders/, which git ignores.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
RASP_WAV = ROOT / "rasp_wav"
PLAYGROUND = ROOT / "playground" / "index.html"
SR = 48000

# Playground parameter names -> rasp_wav names.
NAMES = {
    "loCut": "locut", "hiCut": "hicut", "drive": "drive", "brightness": "bright", "mix": "mix",
    "chop": "chop", "release": "release", "deEss": "deess", "bleed": "bleed",
    "controlGainDb": "controlgain", "spread": "spread", "diffuse": "diffuse", "darken": "darken",
    "width": "width", "thickenGainDb": "thickengain", "tail": "tail", "tailBlend": "tailblend",
    "repeats": "repeats", "delayBlend": "delayblend", "delayTime": "time", "bpm": "bpm",
    "tilt": "tilt", "duck": "duck", "flavourGainDb": "flavourgain", "glue": "glue", "outDb": "out",
    "controlOn": "control", "thickenOn": "thicken", "flavourOn": "flavour", "solo": "solo",
}


def load_presets():
    """Read PRESETS from the playground so both stay in sync."""
    html = PLAYGROUND.read_text()
    block = re.search(r"const PRESETS = (\{.*?\n\});", html, re.S).group(1)
    block = re.sub(r"([{,]\s*)([A-Za-z_]\w*)\s*:", r'\1"\2":', block)   # quote keys
    block = re.sub(r",(\s*[}\]])", r"\1", block)                         # trailing commas
    return json.loads(block)


def preset_args(p):
    out = []
    for k, v in p.items():
        if k not in NAMES:
            sys.exit(f"preset key {k} has no rasp_wav name")
        out.append(f"{NAMES[k]}={int(v) if isinstance(v, bool) else v}")
    return out


def decode(path, channels):
    """Any audio file -> float32 array (frames, channels) at 48 kHz via ffmpeg."""
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le",
           "-ac", str(channels), "-ar", str(SR), "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, channels)


def render(files, presets):
    if not RASP_WAV.exists():
        sys.exit("build the renderer first: make -C rasp")
    out_dir = ROOT / "renders"
    out_dir.mkdir(exist_ok=True)
    all_presets = load_presets()
    names = presets.split(",") if presets else list(all_presets)
    for f in files:
        f = pathlib.Path(f)
        with tempfile.TemporaryDirectory() as td:
            mono = pathlib.Path(td) / "in.wav"
            # rasp_wav reads 16-bit or float WAV; hand it 48 kHz mono float.
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(f), "-ac", "1", "-ar", str(SR),
                            "-c:a", "pcm_f32le", str(mono)], check=True)
            for n in names:
                if n not in all_presets:
                    sys.exit(f"unknown preset {n}; have {', '.join(all_presets)}")
                dst = out_dir / f"{f.stem}__{n}.wav"
                subprocess.run([str(RASP_WAV), str(mono), str(dst)] + preset_args(all_presets[n]),
                               check=True, capture_output=True)
                print(dst.relative_to(ROOT.parent))


# --------------------------------------------------------------------------- analysis
BANDS = [63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000,
         2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]
REGIONS = [("lows <250", 0, 250), ("low-mids 250-800", 250, 800), ("mids 800-2.5k", 800, 2500),
           ("presence 2.5-6k", 2500, 6000), ("air >6k", 6000, 20000)]


def ebur128(path):
    """Integrated loudness, loudness range and true peak from ffmpeg."""
    r = subprocess.run(["ffmpeg", "-nostats", "-i", str(path), "-af", "ebur128=peak=true", "-f", "null", "-"],
                       capture_output=True, text=True).stderr
    summ = r[r.rfind("Summary:"):]
    get = lambda key: float(re.search(key + r":\s+(-?[\d.]+|-inf)", summ).group(1).replace("-inf", "-200"))
    return get(r"I"), get(r"LRA"), get(r"Peak")


def active_mask(mono, win):
    n = len(mono) // win
    rms = np.sqrt(np.mean(mono[: n * win].reshape(n, win) ** 2, axis=1) + 1e-20)
    db = 20 * np.log10(rms)
    return db > db.max() - 30, n                        # frames within 30 dB of the loudest


def analyze(path):
    x = decode(path, 2)
    mono = x.mean(axis=1)
    lufs, lra, tp = ebur128(path)
    win = 4096
    act, n = active_mask(mono, win)
    frames = mono[: n * win].reshape(n, win)[act]
    xs = x[: n * win].reshape(n, win, 2)[act]
    spec = np.mean(np.abs(np.fft.rfft(frames * np.hanning(win), axis=1)) ** 2, axis=0)
    f = np.fft.rfftfreq(win, 1 / SR)
    band = {}
    for fc in BANDS:
        sel = (f >= fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        band[fc] = 10 * np.log10(spec[sel].sum() + 1e-20)
    ref = np.mean(list(band.values()))
    band = {k: v - ref for k, v in band.items()}         # tone only, level removed
    region = {}
    for name, lo, hi in REGIONS:
        sel = (f >= lo) & (f < hi)
        region[name] = 10 * np.log10(spec[sel].sum() / spec[(f >= 20)].sum() + 1e-20)
    act_samples = frames.ravel()
    crest = 20 * np.log10(np.abs(act_samples).max() / np.sqrt(np.mean(act_samples ** 2)) + 1e-20)
    mid, side = xs[..., 0] + xs[..., 1], xs[..., 0] - xs[..., 1]
    width = 10 * np.log10(np.mean(side ** 2) / (np.mean(mid ** 2) + 1e-20) + 1e-20)
    sib = 10 * np.log10(spec[(f >= 5000) & (f < 9000)].sum() / spec[(f >= 200) & (f < 5000)].sum() + 1e-20)
    return {"lufs": lufs, "lra": lra, "true_peak": tp, "crest": crest, "width_side_mid": width,
            "sibilance": sib, "regions": region, "bands": band}


def cmd_analyze(files):
    rows = [(pathlib.Path(p).name, analyze(p)) for p in files]
    w = max(len(n) for n, _ in rows)
    print(f"{'file':<{w}}  {'LUFS':>6} {'LRA':>5} {'TP':>6} {'crest':>6} {'width':>6} {'sib':>6}   "
          + "  ".join(f"{n.split()[0]:>9}" for n, _, _ in REGIONS))
    for n, a in rows:
        wd = a["width_side_mid"]
        print(f"{n:<{w}}  {a['lufs']:6.1f} {a['lra']:5.1f} {a['true_peak']:6.1f} {a['crest']:6.1f} "
              f"{'mono' if wd < -60 else f'{wd:6.1f}':>6} {a['sibilance']:6.1f}   "
              + "  ".join(f"{v:9.1f}" for v in a["regions"].values()))
    print("\nLUFS = integrated loudness, LRA = loudness range (lower = more even), TP = true peak dBTP,\n"
          "crest = peak-to-RMS dB (lower = denser), width = side vs mid dB, sib = 5-9 kHz vs 200 Hz-5 kHz dB,\n"
          "region columns = share of energy in dB.")


def cmd_match(ref, target):
    r, t = analyze(ref), analyze(target)
    print(f"reference: {ref}\ntarget:    {target}\n")
    print(f"{'':22}{'reference':>10}{'target':>10}{'change':>10}")
    for key, label in (("lufs", "loudness (LUFS)"), ("lra", "loudness range"), ("crest", "crest (dB)"),
                       ("width_side_mid", "width (side/mid dB)"), ("sibilance", "sibilance (dB)")):
        print(f"{label:22}{r[key]:10.1f}{t[key]:10.1f}{r[key] - t[key]:+10.1f}")
    print("\ntone, region by region (positive = the reference has more):")
    for name in r["regions"]:
        d = r["regions"][name] - t["regions"][name]
        print(f"  {name:20}{d:+6.1f} dB  {'#' * min(20, int(round(abs(d) * 2)))}")
    diffs = sorted(((r["bands"][b] - t["bands"][b], b) for b in BANDS), key=lambda z: -abs(z[0]))[:5]
    print("\nbiggest 1/3-octave differences:")
    for d, b in diffs:
        print(f"  {b:>6} Hz  {d:+5.1f} dB")
    hints = []
    if r["crest"] - t["crest"] < -1.5: hints.append("reference is denser: more chop or glue, or push output into the limiter")
    if r["crest"] - t["crest"] > 1.5: hints.append("reference is more dynamic: less chop or glue")
    if r["width_side_mid"] - t["width_side_mid"] > 2: hints.append("reference is wider: more spread or width, or more thicken level")
    if r["width_side_mid"] - t["width_side_mid"] < -2: hints.append("reference is narrower: less spread or width")
    if r["sibilance"] - t["sibilance"] < -2: hints.append("reference is less spitty: more de-esser")
    print("\nsuggestions:" if hints else "")
    for h in hints:
        print("  - " + h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("render"); r.add_argument("files", nargs="+"); r.add_argument("--presets")
    a = sub.add_parser("analyze"); a.add_argument("files", nargs="+")
    m = sub.add_parser("match"); m.add_argument("reference"); m.add_argument("target")
    args = ap.parse_args()
    if args.cmd == "render":
        render(args.files, args.presets)
    elif args.cmd == "analyze":
        cmd_analyze(args.files)
    else:
        cmd_match(args.reference, args.target)


if __name__ == "__main__":
    main()
