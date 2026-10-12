"""What would an fp16 core (CoreML on the ANE/GPU computes in fp16) cost against the -40 dB null target?

usage: fp16_check.py --model htdemucs|htdemucs_6s [--wav loop70/mixture.wav] [--segments N]

Runs the exported-core wrapper (same maths as export_onnx.py) on real-clip segments in fp32 and with
conv/linear/matmul computed in fp16 (torch CPU autocast; normalisations stay fp32), keeping STFT/mask/iSTFT in fp32 as separator_onnx does, and prints
the per-stem residual of the composed segment output (20*log10 rms(fp16-fp32)/rms(fp32)). Rough proxy only: CoreML/ANE has
its own fp16 kernels (and may keep some ops in fp32). Runs in the venv; slow (fp16 on CPU).
"""
import argparse, os, sys, copy
from pathlib import Path
import numpy as np, torch as th
sys.argv_backup = sys.argv
ap = argparse.ArgumentParser()
ap.add_argument("--model", required=True); ap.add_argument("--wav", default=str(Path.home() / "sawblade-sep-data/loop70/mixture.wav"))
ap.add_argument("--segments", type=int, default=2)
a = ap.parse_args()
os.environ.setdefault("TORCH_HOME", str(Path.home() / ".cache/sawblade/separator/torchhome"))
th.backends.mha.set_fastpath_enabled(False)
import soundfile as sf
from demucs.pretrained import get_model
SEG = 343980
src = Path(__file__).with_name("export_onnx.py").read_text()
# reuse the Core wrapper verbatim from export_onnx.py
start = src.index("class Core(th.nn.Module):"); end = src.index("bag = get_model")
ns = {"th": th, "rearrange": __import__("einops").rearrange, "SEG": SEG}
exec(src[start:end], ns)
Core = ns["Core"]
model = get_model(a.model).models[0].eval()
core32 = Core(model).eval()
core16 = core32  # same module, run under CPU autocast(float16): conv/linear/matmul inputs+weights in fp16, norms in fp32
w, _ = sf.read(a.wav, dtype="float32", always_2d=True)
w = th.from_numpy(w.T.copy()); ref = w.mean(0); w = (w - ref.mean()) / ref.std()
for i in range(a.segments):
    seg = w[:, 40000 + i * 600000: 40000 + i * 600000 + SEG][None]
    with th.no_grad():
        mag = model._magnitude(model._spec(seg))
        xf, xt = core32(seg, mag)
        full32 = model._ispec(model._mask(None, xf), SEG) + xt
        with th.autocast("cpu", dtype=th.float16):
            xf16, xt16 = core16(seg, mag)
        full16 = model._ispec(model._mask(None, xf16.float()), SEG) + xt16.float()
    r = (full16 - full32)[0]
    print(f"segment {i}: composed output residual per stem (dB):",
          ", ".join(f"{s} {20*np.log10(float(r[k].pow(2).mean().sqrt() / full32[0, k].pow(2).mean().sqrt()) + 1e-30):.1f}" for k, s in enumerate(model.sources)))
