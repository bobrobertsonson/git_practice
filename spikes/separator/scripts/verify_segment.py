"""Check separator_onnx's STFT / mask / iSTFT against demucs' own HTDemucs on the dumped first segment.

usage: verify_segment.py --model htdemucs|htdemucs_6s --dump DIR     (DIR from `separator_onnx --dump-seg DIR`)
Compares (1) mag.f32 with HTDemucs._magnitude(_spec(mix)), (2) seg_out.f32 (our mask + iSTFT + time add of the
ORT outputs) with the stock torch forward of the same segment. Runs in the venv.
"""
import argparse, os
from pathlib import Path
import numpy as np, torch as th
from demucs.pretrained import get_model

ap = argparse.ArgumentParser()
ap.add_argument("--model", required=True); ap.add_argument("--dump", required=True)
a = ap.parse_args()
os.environ.setdefault("TORCH_HOME", str(Path.home() / ".cache/sawblade/separator/torchhome"))
d = Path(a.dump); SEG = 343980
m = get_model(a.model).models[0].eval()
mix = th.from_numpy(np.fromfile(d / "mix.f32", np.float32).reshape(1, 2, SEG))
with th.no_grad():
    mag_ref = m._magnitude(m._spec(mix)).numpy()
    full = m(mix).numpy()
mag = np.fromfile(d / "mag.f32", np.float32).reshape(mag_ref.shape)
seg = np.fromfile(d / "seg_out.f32", np.float32).reshape(full.shape[1:])


def db(x, ref):
    return 20 * np.log10(np.sqrt(((x - ref) ** 2).mean()) / np.sqrt((ref ** 2).mean()) + 1e-30)


print(f"magnitude (STFT):          residual {db(mag, mag_ref):.1f} dB, max abs diff {np.abs(mag - mag_ref).max():.2e}")
for s, name in enumerate(m.sources):
    print(f"segment output {name:7s}: residual {db(seg[s], full[0, s]):.1f} dB")
