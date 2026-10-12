#!/usr/bin/env python3
"""Which RASP preset comes closest to what a producer did to a lead vocal?

Measures the producer's EQ on the lead stem (same multi-input method as
mixprint.py: every stem against the final mix), renders the raw lead through
each preset with rasp_wav, measures RASP's EQ the same way (raw stem in,
render out), and scores each preset by the RMS difference over the bands
where the vocals are audible in the mix (coherence >= mixprint.MIN_COH).
Both curves are relative to 1 kHz, so level doesn't count, only tone.

  python3 tools/presetfit.py --mix final.mp3 --lead stems/lead.wav stems/*.wav
  python3 tools/presetfit.py --mix final.mp3 --lead lead.wav other.wav --presets huge,natural
  python3 tools/presetfit.py ... --try "huge+air:preset=huge,hicut=0.95,bright=2"

--try adds a candidate (label:rasp_wav settings, comma-separated); repeat it
to compare several tunings against the same producer measurement.

Prints the per-band difference (producer minus RASP: positive = the producer
has more there) for every preset, then the ranking. Needs numpy and ffmpeg,
and ./rasp_wav built (make).
"""
import argparse
import importlib.util
import pathlib
import subprocess
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("mixprint", ROOT / "tools" / "mixprint.py")
mp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mp)

PRESETS = ["huge", "natural", "dark", "bright", "live", "raw"]


def auto_in_db(x):
    """The input gain takes.py uses: loud-section RMS (p90 of 100 ms blocks
    above -45 dBFS) brought to -18 dBFS, like setting a preamp."""
    n = int(0.1 * mp.SR)
    b = x[: len(x) // n * n].reshape(-1, n)
    r = 10 * np.log10((b ** 2).mean(axis=1) + 1e-12)
    act = r[r > -45]
    return round(float(-18 - np.percentile(act, 90)), 1) if len(act) else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mix", required=True)
    ap.add_argument("--lead", required=True, help="the raw lead stem to fit (also counted as a stem)")
    ap.add_argument("stems", nargs="*", help="the session's other vocal stems")
    ap.add_argument("--presets", default=",".join(PRESETS))
    ap.add_argument("--in-db", type=float, help="input gain for the renders (default: auto)")
    ap.add_argument("--try", dest="tries", action="append", default=[],
                    help='candidate "label:preset=huge,hicut=0.95" (repeatable)')
    args = ap.parse_args()

    paths = [args.lead] + [s for s in args.stems if pathlib.Path(s).resolve() != pathlib.Path(args.lead).resolve()]
    raw = [mp.decode(p, 1)[:, 0] for p in paths]
    mix = mp.decode(args.mix, 2)
    mono = mix.mean(axis=1)
    cols = []
    for p, x in zip(paths, raw):
        col, lag, conf = mp.place_in_mix(x, mono)
        cols.append(col)
        if conf < 1.5:
            print(f"warning: {pathlib.Path(p).name} placed with low confidence ({conf:.1f})")
    nfft = 1 << 13
    H, coh = mp.transfer(np.stack(cols, axis=1), mix, nfft)
    f = np.fft.rfftfreq(nfft, 1 / mp.SR)
    cm = coh.mean(axis=1)
    bands = [b for b in mp.BANDS if np.mean(cm[(f >= b / 2 ** (1 / 3)) & (f < b * 2 ** (1 / 3))]) >= mp.MIN_COH]
    prod, _ = mp.eq_curve(H, f, 0)
    if len(bands) < 4:
        print("too few bands where the vocal is audible in the mix to fit presets")
        return

    lead = raw[0]
    in_db = args.in_db if args.in_db is not None else auto_in_db(lead)
    print(f"lead: {pathlib.Path(args.lead).name}   input gain {in_db:+.1f} dB   bands used: "
          + " ".join(mp.blabel(b) for b in bands))
    print(f"\nproducer minus RASP, dB (positive = the producer has more)")
    print(f"{'preset':<12}" + "".join(f"{mp.blabel(b):>6}" for b in bands) + "   rms")
    results = []
    with tempfile.TemporaryDirectory() as td:
        src = pathlib.Path(td) / "lead.wav"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", args.lead, "-ac", "1", "-c:a", "pcm_f32le",
                        "-map_metadata", "-1", str(src)], check=True)
        cands = [(n, [f"preset={n}"]) for n in args.presets.split(",") if n]
        for t in args.tries:
            label, _, spec = t.partition(":")
            cands.append((label, spec.split(",")))
        for i, (name, settings) in enumerate(cands):
            out = pathlib.Path(td) / f"c{i}.wav"
            subprocess.run([str(ROOT / "rasp_wav"), str(src), str(out)] + settings + [f"in={in_db}"],
                           check=True, capture_output=True)
            render = mp.decode(out, 2)
            lag = mp.find_lag(lead, render.mean(axis=1))
            Yr = mp.align(render, lag, len(lead))
            Hr, _ = mp.transfer(lead[:, None], Yr, nfft)
            ours, _ = mp.eq_curve(Hr, f, 0)
            d = np.array([prod[b] - ours[b] for b in bands])
            rms = float(np.sqrt(np.mean(d ** 2)))
            results.append((rms, name))
            print(f"{name[:12]:<12}" + "".join(f"{v:6.1f}" for v in d) + f"  {rms:5.1f}")
    results.sort()
    print("\nclosest: " + ", ".join(f"{n} ({r:.1f} dB)" for r, n in results))


if __name__ == "__main__":
    main()
