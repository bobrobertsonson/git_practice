"""Export the htdemucs *core network* for one fixed-length segment to ONNX, verify it against torch.

Ported from spikes/separator/scripts/export_onnx.py (Phase 5.1a; same graph, hence the same pinned sha256
on the reference platform). Needs the `models` extra (torch 2.5.1, demucs 4.0.1, onnx, onnxruntime).

The STFT / iSTFT stay OUT of the graph (the C++ driver does them). Graph interface, batch 1,
fixed shapes (S = number of sources, 4 or 6):
  inputs : mix (1, 2, 343980)  float32   time-domain segment (already per-song normalised)
           mag (1, 4, 2048, 336) float32 HTDemucs._magnitude(HTDemucs._spec(mix)): complex-as-channels
                                         spectrogram (channel = c*2 + {re, im}, cac=True)
  outputs: x_freq (1, S, 4, 2048, 336)   frequency-branch output, de-normalised, BEFORE _mask/_ispec
           x_time (1, S, 2, 343980)      time-branch output, de-normalised
The remaining work (mask: cac -> complex, _ispec, x_time + istft(x_freq)) is in the C++ separator.

The wrapper's forward is a verbatim copy of demucs 4.0.1 HTDemucs.forward (eval path) from `mag`
to the two outputs; nothing else is changed. Nothing here is copied from sevagh/demucs.onnx.
"""
from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Callable

from .paths import OPSET, onnx_path, sidecar_path, torch_home
from .store import PINNED_ONNX_SHA256, sha256_file, write_sidecar

SEG = 343980
VERIFY_SEED = 0
VERIFY_LIMIT_DB = -60.0   # fail if the ORT-vs-torch residual is worse than this


class VerifyError(RuntimeError):
    pass


def residual_db(got, ref) -> float:
    import numpy as np
    r = np.asarray(got, dtype=np.float64) - np.asarray(ref, dtype=np.float64)
    ref = np.asarray(ref, dtype=np.float64)
    return float(20 * np.log10(np.sqrt((r ** 2).mean()) / np.sqrt((ref ** 2).mean()) + 1e-30))


def _core_module(th):
    from einops import rearrange

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

    return Core


def export_and_verify(model_id: str, directory: Path, *, threads: int = 4,
                      log: Callable[[str], None] = print) -> dict:
    """Export `<id>-core-opset17.onnx`, check ORT against torch on a seeded synthetic segment, and only then
    move it into place and write the sha256 sidecar. Returns a result dict (sha256, residuals, pin match)."""
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch as th
    from demucs.pretrained import get_model

    directory.mkdir(parents=True, exist_ok=True)
    final, sidecar = onnx_path(model_id, directory), sidecar_path(model_id, directory)
    sidecar.unlink(missing_ok=True)            # never leave a sidecar next to an unverified file
    part = final.with_name(final.name + ".partial")
    os.environ["TORCH_HOME"] = str(torch_home(directory))
    th.set_num_threads(max(1, threads))
    # Workaround (only change to the model path): in eval + no_grad nn.MultiheadAttention takes the fused
    # `aten::_native_multi_head_attention` fast path, which has no ONNX symbolic. Disabling the fast path
    # makes it run (and export) the equivalent decomposed attention. Same maths, same weights.
    th.backends.mha.set_fastpath_enabled(False)

    model = get_model(model_id).models[0].eval()
    assert int(model.segment * model.samplerate) == SEG and model.cac and model.use_train_segment
    core = _core_module(th)(model).eval()

    th.manual_seed(VERIFY_SEED)
    seg = th.randn(1, 2, SEG) * 0.3          # seeded synthetic segment: no external audio needed
    t0 = time.time()
    try:
        with th.no_grad():
            z = model._spec(seg)
            mag = model._magnitude(z)
            assert tuple(mag.shape) == (1, 4, 2048, 336)
            xf_ref, xt_ref = core(seg, mag)
            full_ref = model(seg)
            th.onnx.export(core, (seg, mag), str(part), opset_version=OPSET, dynamo=False,
                           input_names=["mix", "mag"], output_names=["x_freq", "x_time"],
                           do_constant_folding=True)
        log(f"exported {part.name} {part.stat().st_size / 1e6:.1f} MB in {time.time() - t0:.1f}s")
        onnx.checker.check_model(str(part))

        so = ort.SessionOptions()
        so.intra_op_num_threads = max(1, threads)
        so.add_session_config_entry("session.set_denormal_as_zero", "1")
        sess = ort.InferenceSession(str(part), so, providers=["CPUExecutionProvider"])
        xf, xt = sess.run(None, {"mix": seg.numpy(), "mag": mag.numpy()})
        with th.no_grad():
            full_ort = model._ispec(model._mask(z, th.from_numpy(xf)), SEG) + th.from_numpy(xt)
        res = {"x_freq": residual_db(xf, xf_ref.numpy()), "x_time": residual_db(xt, xt_ref.numpy()),
               "composed": residual_db(full_ort.numpy(), full_ref.numpy())}
        for k, v in res.items():
            log(f"verify {k}: residual {v:.1f} dB")
        worst = max(res.values())
        if not np.isfinite(worst) or worst > VERIFY_LIMIT_DB:
            raise VerifyError(f"{model_id}: ORT vs torch residual {worst:.1f} dB is worse than {VERIFY_LIMIT_DB:.0f} dB")
    except BaseException:
        part.unlink(missing_ok=True)
        raise
    os.replace(part, final)
    digest = sha256_file(final)
    write_sidecar(model_id, directory, digest)
    pinned = PINNED_ONNX_SHA256[model_id]
    match = digest == pinned
    log(f"sha256 {digest}")
    log("export matches the pin" if match else
        f"WARNING: export differs from the pinned sha256 {pinned} (other platform/versions?); the verification passed")
    log(f"versions: torch {th.__version__} onnx {onnx.__version__} onnxruntime {ort.__version__}")
    return {"model": model_id, "path": str(final), "sha256": digest, "pinned_match": match,
            "residual_db": res, "verify_seed": VERIFY_SEED, "seconds": time.time() - t0}
