#!/usr/bin/env python3
"""Regenerates the NAM A2 / A1 test fixtures with the PINNED trainer (neural-amp-modeler 0.13.0, torch 2.5.1, CPU).

    python -m venv .venv && .venv/bin/pip install -e 'match[export]' -c match/constraints-export.txt
    .venv/bin/python tests/fixtures/a2/generate.py            # writes next to this script (or pass --out DIR)
    .venv/bin/python tests/fixtures/a2/generate.py --check     # regenerate into a temp dir and compare (.nam bytes exact, ref_*.wav within 1e-5)

Synthetic only: no TONE3000 data, no recorded audio.  Weights are the trainer's seeded random initialisation (the null
tests check the player against the trainer's forward pass, not tone quality).  For every model the reference output
(``ref_<name>.wav``) is the trainer's own forward pass, computed from the EXPORTED .nam file read back through the
trainer's own loader (``nam.models.init_from_nam``), in float32 on CPU with the trainer's default zero start-up history
(``pad_start``), so it covers the export format and the weight round trip, not just the in-memory network.

Files (see README.md next to this script for the full description):
  input.wav                 1 s, 48 kHz, mono, float32: 0.4 s seeded noise + 0.6 s log sine sweep
  a2_container.nam          SlimmableContainer: A2 Lite (max_value 0.5) + A2 Full (max_value 1.0), packed-trainer export
  a2_full.nam / a2_lite.nam the two submodels as standalone WaveNet files (8 / 3 channels)
  a1_standard.nam           A1 WaveNet "standard" (the official preset, via sawblade_match.export.train)
  ref_a2_full.wav, ref_a2_lite.wav, ref_a1_standard.wav
  manifest.json             machine-readable index (architecture, receptive field, latency, hashes, seeds)
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.resources as res
import json
import sys
import tempfile
import types
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
RATE = 48000
SEEDS = {"input": 20261007, "a2": 20261008, "a1": 20261009}
FIXED_DATE = {"year": 2026, "month": 10, "day": 7, "hour": 0, "minute": 0, "second": 0}   # the trainer stamps `now`; pinned so regeneration is byte-identical
NAM_VERSION = "0.13.0"
TORCH_VERSION = "2.5.1"


def make_input() -> np.ndarray:
    """0.4 s of seeded Gaussian noise (rms ~ -20 dBFS) then a 0.6 s log sweep 50 Hz -> 16 kHz at -10 dBFS peak; 5 ms Hann
    fades at each end of both parts so the file starts and ends at exactly 0."""
    rng = np.random.default_rng(SEEDS["input"])
    n_noise, n_sweep, fade = int(0.4 * RATE), int(0.6 * RATE), int(0.005 * RATE)

    def faded(x):
        w = np.ones(len(x))
        ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
        w[:fade], w[-fade:] = ramp, ramp[::-1]
        return x * w

    noise = faded(0.1 * rng.standard_normal(n_noise))
    t = np.arange(n_sweep) / RATE
    f0, f1, dur = 50.0, 16000.0, n_sweep / RATE
    k = np.log(f1 / f0)
    sweep = faded(0.316 * np.sin(2 * np.pi * f0 * dur / k * (np.exp(t / dur * k) - 1.0)))
    return np.concatenate([noise, sweep]).astype(np.float32)


def write_wav(path: Path, y: np.ndarray) -> None:
    """Mono 32-bit float WAV (format tag 3), plain RIFF with no PEAK chunk: libsndfile stamps the time into float WAVs,
    which would make regeneration non-reproducible."""
    import struct
    data = np.asarray(y, dtype="<f4").tobytes()
    fmt = struct.pack("<HHIIHH", 3, 1, RATE, RATE * 4, 4, 32)
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE"
                     + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def import_nam():
    try:
        import tkinter  # noqa: F401
    except ImportError:
        sys.modules["tkinter"] = types.ModuleType("tkinter")
    import nam  # noqa: F401
    import torch
    if nam.__version__ != NAM_VERSION:
        raise SystemExit(f"fixtures are defined for neural-amp-modeler {NAM_VERSION}, found {nam.__version__}")
    return torch


def pin_dates(model: dict) -> None:
    model["metadata"]["date"] = dict(FIXED_DATE)
    for sub in model.get("config", {}).get("submodels", []):
        pin_dates(sub["model"])


def write_nam(path: Path, model: dict) -> None:
    pin_dates(model)
    path.write_text(json.dumps(model))


def forward(model_json: dict, x: np.ndarray) -> np.ndarray:
    """The trainer's own forward pass of an exported (standalone WaveNet) model file."""
    import torch
    from nam.models import init_from_nam
    net = init_from_nam(model_json)
    net.eval()
    with torch.no_grad():
        y = net(torch.tensor(x)[None, :])          # (1, L): zero start-up history ("pad_start" default of the WaveNet)
    return y[0].numpy().astype(np.float32)


def rms_db(y: np.ndarray) -> float:
    return float(10 * np.log10(np.mean(y.astype(np.float64) ** 2) + 1e-30))


def describe(name, path, model, rf, params, x, y, ref_name, notes, extra=None):
    d = {"file": path.name, "architecture": model["architecture"], "version": model["version"],
         "sampleRate": model.get("sample_rate"), "receptiveFieldSamples": rf, "algorithmicLatencySamples": 0,
         "parameters": params, "metadataLoudnessDb": model["metadata"].get("loudness"),
         "metadataGain": model["metadata"].get("gain"), "input": "input.wav", "reference": ref_name,
         "referenceRmsDbfs": rms_db(y), "referencePeak": float(np.max(np.abs(y))), "sha256": sha256(path), "notes": notes}
    d.update(extra or {})
    return d


def generate(out: Path) -> dict:
    import soundfile as sf              # reading only
    torch = import_nam()
    from nam.models import metadata as md
    from nam.train import lightning_module as lm
    out.mkdir(parents=True, exist_ok=True)
    torch.set_num_threads(1)                         # fixed reduction order: bit-reproducible on one machine

    x = make_input()
    write_wav(out / "input.wav", x)
    manifest = {"generator": "tests/fixtures/a2/generate.py", "neuralAmpModeler": NAM_VERSION, "torch": TORCH_VERSION,
                "seeds": SEEDS, "sampleRate": RATE, "input": {"file": "input.wav", "samples": int(len(x)),
                "sha256": sha256(out / "input.wav"), "peak": float(np.max(np.abs(x))), "rmsDbfs": rms_db(x),
                "content": "0.4 s seeded Gaussian noise (sigma 0.1) + 0.6 s log sine sweep 50 Hz-16 kHz (peak 0.316)"},
                "models": {}}

    # ---- A2: the trainer's default packed network (nam/train/_resources/config_model_packed.json, submodels
    # channels_3 and channels_8, container_max_values "uniform" -> max_value 0.5 and 1.0).
    cfg = json.load(res.files("nam.train._resources").joinpath("config_model_packed.json").open())
    torch.manual_seed(SEEDS["a2"])
    mod = lm.PackedLightningModule.init_from_config(copy.deepcopy(cfg))
    net = mod.net
    net.sample_rate = float(RATE)
    net.eval()
    um = md.UserMetadata(name="Sawblade A2 fixture (random init)", modeled_by="Sawblade fixture generator",
                         gear_type=md.GearType.AMP, tone_type=md.ToneType.HI_GAIN)
    other = {"sawblade": {"fixture": True, "seed": SEEDS["a2"], "note": "synthetic, untrained weights"}}
    with tempfile.TemporaryDirectory() as td:
        net.export_container(Path(td), basename="a2_container", user_metadata=um, other_metadata=other)
        container = json.loads((Path(td) / "a2_container.nam").read_text())
        subs = {}
        for i, name in ((0, "a2_lite"), (1, "a2_full")):
            net.extract_submodel(i).export(Path(td), basename=name)
            subs[name] = json.loads((Path(td) / f"{name}.nam").read_text())
    assert [s["max_value"] for s in container["config"]["submodels"]] == [0.5, 1.0]
    write_nam(out / "a2_container.nam", container)
    rf = net.receptive_field
    for i, name in ((0, "a2_lite"), (1, "a2_full")):
        write_nam(out / f"{name}.nam", subs[name])
        # references come from the files on disk (what the player reads), not from the in-memory net
        sub_json = json.loads((out / f"{name}.nam").read_text())
        y = forward(sub_json, x)
        write_wav(out / f"ref_{name}.wav", y)
        params = int(sum(p.numel() for p in net.extract_submodel(i).parameters()))
        manifest["models"][name] = describe(
            name, out / f"{name}.nam", sub_json, rf, params, x, y, f"ref_{name}.wav",
            f"A2 {'Full (8 channels)' if i else 'Lite (3 channels)'} as a standalone WaveNet file "
            "(PackedWaveNet.extract_submodel(i).export), 23 layers, LeakyReLU, head kernel 16")
        # container submodel i must be the same network as the standalone file (same weights, same forward)
        csub = container["config"]["submodels"][i]["model"]
        assert csub["weights"] == sub_json["weights"] and csub["config"] == sub_json["config"]
    # the packed forward pass (one wide net with block masks) vs the extracted submodels: documents the float difference
    with torch.no_grad():
        packed = net(torch.tensor(x)[None, :])[0].numpy()               # (P, L)
    for i, name in ((0, "a2_lite"), (1, "a2_full")):
        ref, _ = sf.read(str(out / f"ref_{name}.wav"), dtype="float32")
        manifest["models"][name]["packedForwardMaxAbsDiff"] = float(np.max(np.abs(packed[i] - ref)))
    manifest["models"]["a2_container"] = describe(
        "a2_container", out / "a2_container.nam", container, rf,
        int(sum(p.numel() for p in net.parameters())), x, packed[1], "ref_a2_full.wav",
        "packed-trainer export (PackedWaveNet.export_container): architecture SlimmableContainer, submodels "
        "[{max_value 0.5, A2 Lite}, {max_value 1.0, A2 Full}]; a player that never calls SetSlimmableSize plays the last "
        "(Full) submodel, so ref_a2_full.wav is its reference; for Lite use ref_a2_lite.wav",
        {"submodels": [{"max_value": s["max_value"], "architecture": s["model"]["architecture"],
                        "channels": s["model"]["config"]["layers"][0]["channels"]}
                       for s in container["config"]["submodels"]],
         "defaultSubmodel": "a2_full", "referenceForSubmodel": {"0.5": "ref_a2_lite.wav", "1.0": "ref_a2_full.wav"},
         "metadataLoudnessNote": "container loudness/gain = the highest-quality (Full) submodel's "
                                 "(PackedWaveNet._sync_container_metadata_to_highest_quality_submodel)"})

    # ---- A1 standard: the official preset through Sawblade's own table (so the table is exercised end to end).
    from sawblade_match.export import train as T
    torch.manual_seed(SEEDS["a1"])
    a1 = lm.LightningModule.init_from_config(
        {"net": {"name": "WaveNet", "config": T.wavenet_config("standard")}, "loss": {"val_loss": "esr"},
         "optimizer": {"lr": 0.004}, "lr_scheduler": {"class": "ExponentialLR", "kwargs": {"gamma": 0.994}}}).net
    a1.sample_rate = float(RATE)
    a1.eval()
    with tempfile.TemporaryDirectory() as td:
        a1.export(Path(td), basename="a1_standard", user_metadata=md.UserMetadata(
            name="Sawblade A1 standard fixture (random init)", modeled_by="Sawblade fixture generator",
            gear_type=md.GearType.AMP, tone_type=md.ToneType.HI_GAIN),
            other_metadata={"sawblade": {"fixture": True, "seed": SEEDS["a1"], "size": "standard"}})
        a1_json = json.loads((Path(td) / "a1_standard.nam").read_text())
    write_nam(out / "a1_standard.nam", a1_json)
    a1_json = json.loads((out / "a1_standard.nam").read_text())
    y = forward(a1_json, x)
    write_wav(out / "ref_a1_standard.wav", y)
    manifest["models"]["a1_standard"] = describe(
        "a1_standard", out / "a1_standard.nam", a1_json, a1.receptive_field,
        int(sum(p.numel() for p in a1.parameters())), x, y, "ref_a1_standard.wav",
        "A1 WaveNet standard (NAM official preset: 16/8 channels, 10+10 dilations 1..512, kernel 3, Tanh); "
        "unlike many old A1 captures this file carries a top-level sample_rate (48000), as every 0.13.0 export does")

    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


REF_ATOL = 1e-5      # --check: ref_*.wav samples (float32 forward passes) may differ by this much (other CPU / BLAS)
META_RTOL = 1e-4     # --check: ONLY the forward-pass-derived floats (see FORWARD_DERIVED); every other number is exact
WEIGHT_ATOL = 1e-6   # --check: weights; seeded init, so normally identical, but a different CPU's float rounding is tolerated
# Forward-pass-derived values (computed by running the exported model on a test signal, so they depend on the CPU's float
# rounding): a .nam's metadata.loudness and metadata.gain (also inside container submodels), and the manifest's copies of them
# plus the reference output's RMS / peak.  Nothing else is derived from a forward pass (the date is pinned, input_level_dbu /
# output_level_dbu are null, config / version / architecture / sample_rate come from the seeded config).
FORWARD_DERIVED_SUFFIXES = ("/metadata/loudness", "/metadata/gain")
FORWARD_DERIVED_MANIFEST_KEYS = ("metadataLoudnessDb", "metadataGain", "referenceRmsDbfs", "referencePeak")
# Diagnostics that are themselves tiny forward-pass differences (packed vs standalone forward, ~0 .. 1e-8): absolute tolerance,
# a relative one of a ~1e-8 number is meaningless.  CI run 247 saw 2.05e-8 on one runner CPU where the committed value is 0.0:
# forward-pass outputs differ by ~1e-8 between runner CPUs, which is also what moved loudness / gain / ref_*.wav before.
FORWARD_DIAGNOSTIC_KEYS = ("packedForwardMaxAbsDiff",)
DIAGNOSTIC_ATOL = 1e-6


GENERIC_FLOAT_RTOL = 1e-9    # every other float: summation-order noise only (CI run 248: input rmsDbfs differed by 1.8e-15)
GENERIC_FLOAT_ATOL = 1e-12   # ... with an absolute floor for values near 0


def _tol(path: str) -> tuple[float, float]:
    """(relative, absolute) tolerance for a FLOAT at ``path``; ints, strings, bools and structure are always exact."""
    if path.endswith("/weights"):
        return 0.0, WEIGHT_ATOL
    if path.rsplit("/", 1)[-1] in FORWARD_DIAGNOSTIC_KEYS:
        return 0.0, DIAGNOSTIC_ATOL
    if path.endswith(FORWARD_DERIVED_SUFFIXES) or path.rsplit("/", 1)[-1] in FORWARD_DERIVED_MANIFEST_KEYS:
        return META_RTOL, 0.0
    return GENERIC_FLOAT_RTOL, GENERIC_FLOAT_ATOL


def _walk(a, b, path, out):
    """Collect (path, max abs diff, count) for every difference between two parsed JSON values.  Rule: ints, strings, bools and
    structure (key sets, list lengths) exact; EVERY float within a tiny relative tolerance (``GENERIC_FLOAT_RTOL`` 1e-9, abs
    floor 1e-12), so rounding noise passes while a changed sample_rate, LeakyReLU slope or parameter count (orders of magnitude
    larger) is reported; weights within ``WEIGHT_ATOL``; forward-pass-derived values (``FORWARD_DERIVED_*``) within
    ``META_RTOL`` / ``DIAGNOSTIC_ATOL``."""
    num = lambda x: isinstance(x, (int, float)) and not isinstance(x, bool)
    isf = lambda x: isinstance(x, float)
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append((f"{path}/{k}", None, 1))
            else:
                _walk(a[k], b[k], f"{path}/{k}", out)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append((path, None, 1))
        elif a and all(num(x) for x in a + b):
            x, y = np.asarray(a, np.float64), np.asarray(b, np.float64)
            rt, at = _tol(path)
            floats = np.array([isf(u) or isf(v) for u, v in zip(a, b)])
            tol = np.where(floats, at + rt * np.maximum(np.abs(x), np.abs(y)), 0.0)
            d = np.abs(x - y)
            if (d > tol).any():
                out.append((path, float(d.max()), int((d > tol).sum())))
        else:
            for i, (u, v) in enumerate(zip(a, b)):
                _walk(u, v, f"{path}[{i}]", out)
    elif num(a) and num(b):
        rt, at = _tol(path)
        tol = (at + rt * max(abs(a), abs(b))) if (isf(a) or isf(b)) else 0.0
        if a != b and abs(a - b) > tol:
            out.append((path, abs(a - b), 1))
    elif a != b:
        out.append((path, None, 1))


def _json_diffs(new: Path, old: Path, skip: tuple = ()) -> list[tuple]:
    out: list[tuple] = []
    na, nb = json.loads(new.read_text()), json.loads(old.read_text())
    for k in skip:
        for d in (na, nb):
            _drop(d, k)
    _walk(na, nb, "", out)
    return out


def _drop(o, key):
    if isinstance(o, dict):
        o.pop(key, None)
        for v in o.values():
            _drop(v, key)
    elif isinstance(o, list):
        for v in o:
            _drop(v, key)


def check_dirs(new: Path, old: Path, log=print) -> list[str]:
    """Names that differ.  input.wav must match byte for byte (seeded generator, no forward pass).  A ``.nam`` is compared as
    parsed JSON: everything exact (architecture, config, version, sample_rate, ints, floats) except ``weights`` within
    ``WEIGHT_ATOL`` (the init is seeded, so they are normally bit-identical) and the forward-pass-derived floats
    (``FORWARD_DERIVED_*``: metadata loudness / gain) within ``META_RTOL`` relative.  ``ref_*.wav`` (float32 forward passes) within ``REF_ATOL``; manifest.json with
    its sha256 strings dropped (the .nam files are checked directly) and numbers within ``META_RTOL``.  Every difference is
    printed with its key path, max abs difference and number of differing values, so a red CI is diagnosable from the log."""
    import soundfile as sf
    bad = []

    def report(name, diffs):
        for path, mx, n in diffs[:12]:
            log(f"  {name}{path}: " + ("structure / value differs" if mx is None else f"max abs diff {mx:.3g}, {n} value(s) beyond tolerance"))
        if diffs:
            bad.append(name)

    for q in sorted(f for f in old.iterdir() if f.is_file() and f.suffix in (".nam", ".wav") + (".json",) and f.name != "rig_two_a2_full.json"
                    and not (new / f.name).is_file()):      # hand-written files (README, generate.py, the rig preset) are not outputs
        log(f"  {q.name}: only in the committed directory (the generator no longer writes it)")
        bad.append(q.name)
    for p in sorted(f for f in new.iterdir() if f.is_file()):
        q = old / p.name
        if not q.is_file():
            log(f"  {p.name}: missing")
            bad.append(p.name)
        elif p.suffix == ".nam":
            report(p.name, _json_diffs(p, q))
        elif p.name == "manifest.json":
            report(p.name, _json_diffs(p, q, skip=("sha256",)))
        elif p.name.startswith("ref_") and p.suffix == ".wav":
            x, y = sf.read(str(p), dtype="float32"), sf.read(str(q), dtype="float32")
            if x[1] != y[1] or x[0].shape != y[0].shape:
                report(p.name, [("", None, 1)])
            else:
                d = float(np.max(np.abs(x[0] - y[0])))
                report(p.name, [("", d, int((np.abs(x[0] - y[0]) > REF_ATOL).sum()))] if d > REF_ATOL else [])
        elif q.read_bytes() != p.read_bytes():
            log(f"  {p.name}: bytes differ")
            bad.append(p.name)
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(HERE))
    ap.add_argument("--check", action="store_true", help="regenerate into a temp dir and compare with --out: .nam as JSON (weights within 1e-6, loudness/gain 1e-4 rel, all else exact), input.wav byte for byte, ref_*.wav within 1e-5")
    a = ap.parse_args()
    if not a.check:
        m = generate(Path(a.out))
        for k, v in m["models"].items():
            print(f"{k:14s} {v['architecture']:18s} rf {v['receptiveFieldSamples']:5d}  params {v['parameters']:6d}  "
                  f"ref rms {v['referenceRmsDbfs']:.1f} dBFS  {v['file']}")
        return 0
    with tempfile.TemporaryDirectory() as td:
        generate(Path(td))
        bad = check_dirs(Path(td), Path(a.out))
    print("fixtures differ: " + ", ".join(bad) if bad
          else "fixtures are reproducible (input.wav byte-identical; .nam structure exact, weights within 1e-6, forward-derived loudness/gain within 1e-4 rel, every other float within 1e-9 rel, ints/strings/structure exact; ref_*.wav within 1e-5)")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
