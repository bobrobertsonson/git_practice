"""Export the htdemucs *core network* for one fixed-length segment to ONNX (runs in the venv).

usage: export_onnx.py --model htdemucs|htdemucs_6s [--out-dir ~/.cache/sawblade/separator/onnx] [--verify]

The STFT / iSTFT stay OUT of the graph (the C++ driver does them). Graph interface, batch 1,
fixed shapes (S = number of sources, 4 or 6):
  inputs : mix (1, 2, 343980)  float32   time-domain segment (already per-song normalised)
           mag (1, 4, 2048, 336) float32 HTDemucs._magnitude(HTDemucs._spec(mix)): complex-as-channels
                                         spectrogram (channel = c*2 + {re, im}, cac=True)
  outputs: x_freq (1, S, 4, 2048, 336)   frequency-branch output, de-normalised, BEFORE _mask/_ispec
           x_time (1, S, 2, 343980)      time-branch output, de-normalised
The remaining work (mask: cac -> complex, _ispec, x_time + istft(x_freq)) is in separator_onnx.cpp.

The wrapper's forward is a verbatim copy of demucs 4.0.1 HTDemucs.forward (eval path) from `mag`
to the two outputs; nothing else is changed. Nothing here is copied from sevagh/demucs.onnx.
"""
import argparse, hashlib, os, sys, time
from fractions import Fraction
from pathlib import Path
import numpy as np, torch as th
import torch.nn.functional as F
from einops import rearrange
from demucs.pretrained import get_model

SEG = 343980

ap = argparse.ArgumentParser()
ap.add_argument("--model", required=True, choices=["htdemucs", "htdemucs_6s"])
ap.add_argument("--out-dir", default=str(Path.home() / ".cache/sawblade/separator/onnx"))
ap.add_argument("--opset", type=int, default=17)
ap.add_argument("--verify", action="store_true", help="compare ORT vs torch on a real segment")
ap.add_argument("--flops-only", action="store_true", help="count FLOPs of one segment (torch FlopCounterMode: conv/mm/bmm/sdpa) and exit")
ap.add_argument("--verify-wav", default=str(Path.home() / "sawblade-sep-data/loop70/mixture.wav"))
a = ap.parse_args()
cache = Path(os.environ.get("SAWBLADE_SEP_CACHE", Path.home() / ".cache/sawblade/separator"))
os.environ.setdefault("TORCH_HOME", str(cache / "torchhome"))
th.set_num_threads(max(1, os.cpu_count() or 1))
# Workaround (only change to the model path): in eval + no_grad nn.MultiheadAttention takes the fused
# `aten::_native_multi_head_attention` fast path, which has no ONNX symbolic. Disabling the fast path
# makes it run (and export) the equivalent decomposed attention. Same maths, same weights.
th.backends.mha.set_fastpath_enabled(False)


class Core(th.nn.Module):
    """HTDemucs.forward from `mag` to (x_freq, x_time); see module docstring."""

    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, mix, mag):
        m = self.m
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)
        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = th.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_upsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_downsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)
        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, SEG)
        xt = xt * stdt[:, None] + meant[:, None]
        return x, xt


bag = get_model(a.model)
model = bag.models[0].eval()
assert int(model.segment * model.samplerate) == SEG and model.cac and model.use_train_segment
core = Core(model).eval()
S = len(model.sources)

# real example inputs from the model's own _spec/_magnitude
if a.verify and Path(a.verify_wav).exists():
    import soundfile as sf
    w, _ = sf.read(a.verify_wav, dtype="float32", always_2d=True)
    w = th.from_numpy(w.T.copy())
    ref = w.mean(0)
    w = (w - ref.mean()) / ref.std()
    seg = w[:, 20000:20000 + SEG][None]
else:
    th.manual_seed(0)
    seg = th.randn(1, 2, SEG) * 0.3
with th.no_grad():
    z = model._spec(seg)
    mag = model._magnitude(z)
    print("mag", tuple(mag.shape), "z", tuple(z.shape))
    assert tuple(mag.shape) == (1, 4, 2048, 336)
    xf_ref, xt_ref = core(seg, mag)
    # sanity: the wrapper + _mask/_ispec + add reproduces the stock forward bit-for-bit
    full_ref = model(seg)
    full_mine = model._ispec(model._mask(z, xf_ref), SEG) + xt_ref
    print("wrapper vs stock forward max abs diff:", float((full_ref - full_mine).abs().max()))

if a.flops_only:
    from torch.utils.flop_counter import FlopCounterMode
    with FlopCounterMode(display=False) as fc, th.no_grad():
        core(seg, mag)
    print(f"{a.model}: {fc.get_total_flops()/1e9:.1f} GFLOP per 7.8 s segment (counted: conv, mm, bmm, sdpa; 2 FLOP per MAC)")
    for k, v in fc.get_flop_counts()["Global"].items():
        print(f"   {str(k):40s} {v/1e9:8.1f} GFLOP")
    sys.exit(0)

out = Path(a.out_dir); out.mkdir(parents=True, exist_ok=True)
path = out / f"{a.model}-core-opset{a.opset}.onnx"
t0 = time.time()
with th.no_grad():
    th.onnx.export(core, (seg, mag), str(path), opset_version=a.opset, dynamo=False,
                   input_names=["mix", "mag"], output_names=["x_freq", "x_time"],
                   do_constant_folding=True)
print(f"exported {path} {path.stat().st_size/1e6:.1f} MB in {time.time()-t0:.1f}s")
h = hashlib.sha256(path.read_bytes()).hexdigest()
print("sha256", h)
(out / f"{a.model}-core-opset{a.opset}.onnx.sha256").write_text(f"{h}  {path.name}\n")
import onnx, onnxruntime as ort
print("versions: torch", th.__version__, "onnx", onnx.__version__, "onnxruntime", ort.__version__)
onnx.checker.check_model(str(path))
ops = {}
for n in onnx.load(str(path), load_external_data=False).graph.node:
    ops[n.op_type] = ops.get(n.op_type, 0) + 1
print("ops:", dict(sorted(ops.items(), key=lambda kv: -kv[1])))

so = ort.SessionOptions()
so.intra_op_num_threads = 4
so.add_session_config_entry("session.set_denormal_as_zero", "1")
sess = ort.InferenceSession(str(path), so, providers=["CPUExecutionProvider"])
t0 = time.time()
xf, xt = sess.run(None, {"mix": seg.numpy(), "mag": mag.numpy()})
print(f"ORT run {time.time()-t0:.1f}s")
for name, got, ref in (("x_freq", xf, xf_ref.numpy()), ("x_time", xt, xt_ref.numpy())):
    r = got - ref
    db = 20 * np.log10(np.sqrt((r ** 2).mean()) / np.sqrt((ref ** 2).mean()) + 1e-30)
    print(f"verify {name}: shape {got.shape} residual {db:.1f} dB, max abs diff {np.abs(r).max():.3e}")
# also check the final composed output
full_ort = model._ispec(model._mask(z, th.from_numpy(xf)), SEG) + th.from_numpy(xt)
r = (full_ort - full_ref).numpy()
print("verify composed output: residual %.1f dB" % (20 * np.log10(np.sqrt((r ** 2).mean()) / np.sqrt((full_ref.numpy() ** 2).mean()) + 1e-30)))
