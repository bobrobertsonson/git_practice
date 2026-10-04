"""Python demucs reference with demucs.cpp-matching inference settings (runs in the venv).

usage: run_python.py --model htdemucs|htdemucs_6s --in mix.wav --out-dir dir [--shift-offset N] [--threads T]

Settings (read from demucs.cpp src/model.hpp / model_apply.cpp):
  segment 7.8 s (SEGMENT_LEN_SECS), overlap 0.25, transition_power 1.0, split=True, float32,
  input normalised with ref=wav.mean(0): (wav-ref.mean())/ref.std() then undone.
  demucs.cpp ALWAYS applies one time shift (rand() % 22050, unseeded => 4033 on glibc), whereas the
  spec asks for shifts=0. --shift-offset omitted => shifts=0 (spec). --shift-offset N => the shift
  path of demucs.apply.apply_model with random.randint patched to return N (matches C++ exactly).
"""
import argparse, json, time, resource, random
from pathlib import Path
import numpy as np, soundfile as sf, torch
from demucs.pretrained import get_model
from demucs import apply as dapply

ap = argparse.ArgumentParser()
ap.add_argument("--model", required=True); ap.add_argument("--in", dest="inp", required=True)
ap.add_argument("--out-dir", required=True); ap.add_argument("--shift-offset", type=int, default=None)
ap.add_argument("--threads", type=int, default=None)
a = ap.parse_args()
if a.threads: torch.set_num_threads(a.threads)

t0 = time.perf_counter()
model = get_model(a.model); model.eval()  # BagOfModels with 1 model
load_s = time.perf_counter() - t0
x, sr = sf.read(a.inp, dtype="float32", always_2d=True)
assert sr == 44100 and x.shape[1] == 2
wav = torch.from_numpy(x.T.copy())
ref = wav.mean(0)
wavn = (wav - ref.mean()) / ref.std()

kw = dict(shifts=0, split=True, overlap=0.25, transition_power=1.0, progress=False, device="cpu", num_workers=0)
if a.shift_offset is not None:
    kw["shifts"] = 1
    dapply.random.randint = lambda lo, hi: a.shift_offset  # noqa: demucs.apply does `random.randint`
t0 = time.perf_counter()
with torch.no_grad():
    out = dapply.apply_model(model, wavn[None], **kw)[0]
sep_s = time.perf_counter() - t0
out = out * ref.std() + ref.mean()
d = Path(a.out_dir); d.mkdir(parents=True, exist_ok=True)
for i, name in enumerate(model.sources):
    sf.write(d / f"{name}.wav", out[i].numpy().T, 44100, subtype="FLOAT")
dur = x.shape[0] / 44100
print("RESULT", json.dumps(dict(model=a.model, segment=float(model.models[0].segment), threads=torch.get_num_threads(),
      load_s=round(load_s, 3), sep_s=round(sep_s, 3), audio_s=round(dur, 3), rtf=round(sep_s / dur, 3),
      sep_s_per_min=round(sep_s / dur * 60, 2), peak_rss_mb=round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024))))
