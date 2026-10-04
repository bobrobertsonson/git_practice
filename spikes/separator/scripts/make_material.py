"""Build test material (never committed). usage: make_material.py <data_dir> <musdb .stem.mp4>"""
import subprocess, sys
from pathlib import Path
import numpy as np, soundfile as sf
from scipy import signal

SR = 44100
data = Path(sys.argv[1]); stem_mp4 = sys.argv[2]
NAMES = ["mixture", "drums", "bass", "other", "vocals"]  # NI stem order, stream 0 = mixture


def write(d: Path, name: str, x: np.ndarray):
    d.mkdir(parents=True, exist_ok=True)
    sf.write(d / f"{name}.wav", x.astype(np.float32), SR, subtype="FLOAT")


# --- real clip: decode each AAC stream to float32 stereo ------------------------------------
real = {}
for i, n in enumerate(NAMES):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", stem_mp4, "-map", f"0:a:{i}", "-f", "f32le",
                          "-ac", "2", "-ar", str(SR), "-"], check=True, capture_output=True).stdout
    real[n] = np.frombuffer(raw, np.float32).reshape(-1, 2)
    write(data / "real7", n, real[n])
n = min(len(v) for v in real.values())
print("real7 frames", n, n / SR, "s")
# loop to 70 s (timing only)
reps = int(np.ceil(70 * SR / n))
write(data / "loop70", "mixture", np.tile(real["mixture"], (reps, 1))[: 70 * SR])

# --- synthetic 36 s mixture with exact stems ------------------------------------------------
rng = np.random.default_rng(5)
T = 36; N = T * SR; t = np.arange(N) / SR
bpm = 110; beat = 60 / bpm


def stereo(x, pan=0.5):
    return np.stack([x * np.sqrt(1 - pan), x * np.sqrt(pan)], 1)


def env_hits(times, f, dur):
    out = np.zeros(N)
    for s in times:
        i = int(s * SR); m = min(N - i, int(dur * SR))
        if m > 0: out[i:i + m] += f(np.arange(m) / SR)
    return out


beats = np.arange(0, T, beat)
kick = env_hits(beats[::2], lambda x: np.sin(2 * np.pi * (50 + 120 * np.exp(-x * 40)) * x) * np.exp(-x * 9), 0.4)
snare = env_hits(beats[1::2], lambda x: (rng.standard_normal(len(x)) * 0.6 + np.sin(2 * np.pi * 190 * x)) * np.exp(-x * 18), 0.3)
hat = env_hits(np.arange(0, T, beat / 2), lambda x: rng.standard_normal(len(x)) * np.exp(-x * 90), 0.08)
hat = signal.sosfilt(signal.butter(4, 7000, "hp", fs=SR, output="sos"), hat)
drums = stereo(0.8 * kick + 0.5 * snare + 0.15 * hat)

roots = [41.2, 41.2, 49.0, 36.7]  # E1 E1 G1 D1 per bar
bar = 4 * beat


def note_track(fn):
    out = np.zeros(N)
    for b in range(int(T / bar)):
        out += fn(roots[b % 4], b * bar)
    return out


def saw_seg(f, start, dur, extra=0):
    i = int(start * SR); m = min(N - i, int(dur * SR))
    x = np.arange(m) / SR
    return i, m, signal.sawtooth(2 * np.pi * f * x + extra)


def bass_fn(f, st):
    out = np.zeros(N)
    for k in range(8):
        i, m, w = saw_seg(f, st + k * beat / 2, beat / 2 * 0.9)
        e = np.exp(-np.arange(m) / SR * 6)
        out[i:i + m] += w * e
    return out


bass = bass_fn_out = note_track(bass_fn)
bass = signal.sosfilt(signal.butter(4, 400, "lp", fs=SR, output="sos"), bass) * 0.5
bass = stereo(bass)


def gtr_fn(f, st):  # power chord (root + fifth + octave), palm-muted 8ths
    out = np.zeros(N)
    for k in range(8):
        i = int((st + k * beat / 2) * SR); m = min(N - i, int(beat / 2 * 0.8 * SR))
        x = np.arange(m) / SR
        w = sum(signal.square(2 * np.pi * fr * x + ph, duty=0.4) for fr, ph in
                [(f * 4, 0), (f * 4 * 1.4983, 0.7), (f * 8, 1.3)])
        out[i:i + m] += w * np.exp(-x * 5)
    return out


raw = note_track(gtr_fn)
raw = signal.sosfilt(signal.butter(2, 120, "hp", fs=SR, output="sos"), raw)
dist = np.tanh(14 * raw / 3)                      # heavy clipping
dist = signal.sosfilt(signal.butter(4, 5000, "lp", fs=SR, output="sos"), dist)  # cab-ish rolloff
dist = np.tanh(3 * dist) * 0.5
gtr = np.stack([dist, np.roll(dist, 40)], 1) * 0.6  # slight Haas widening

# "other": slow pad (keys-like) with triads
pad = np.zeros(N)
for b in range(int(T / bar)):
    r = roots[b % 4] * 8
    for fr in (r, r * 1.189, r * 1.4983):
        i = int(b * bar * SR); m = min(N - i, int(bar * SR)); x = np.arange(m) / SR
        pad[i:i + m] += np.sin(2 * np.pi * fr * x) * np.minimum(1, x * 4) * np.minimum(1, (bar - x) * 4)
other = stereo(pad * 0.12, 0.4)

# "vocal-like": vowel formant-filtered buzz with vibrato, phrases every 2 bars
voc = np.zeros(N)
f0 = 180 + 25 * np.sin(2 * np.pi * 5.5 * t)
buzz = signal.sawtooth(2 * np.pi * np.cumsum(f0) / SR)
gate = ((t % (2 * bar)) < bar * 1.2).astype(float)
for fc, bw, g in [(700, 120, 1.0), (1220, 150, 0.6), (2600, 200, 0.3)]:
    voc += g * signal.sosfilt(signal.butter(2, [fc - bw, fc + bw], "bp", fs=SR, output="sos"), buzz)
vocals = stereo(voc * gate * 0.5)

stems = {"drums": drums, "bass": bass, "other": other, "vocals": vocals, "guitar": gtr}
mix = sum(stems.values())
peak = np.abs(mix).max(); k = 0.8 / peak
for nme, s in stems.items(): write(data / "synth", nme, s * k)
write(data / "synth", "mixture", mix * k)
print("synth frames", N, "peak scale", k)
