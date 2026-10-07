# NAM A2 / A1 test fixtures (synthetic)

Small `.nam` files for the A2 playback tests (v0.6 Task B), each with a fixed input and **the trainer's own forward pass**
as the null-test reference. Everything is synthetic: seeded random initial weights from the pinned trainer, no TONE3000
data and no recorded audio, so the files are fine to commit. They check the *player* against the trainer, not tone quality.

Regenerate (or verify) with the pinned trainer; the output is byte-identical on a machine with the same torch/numpy:

```
python3.11 -m venv .venv && .venv/bin/pip install -e 'match[export]' -c match/constraints-export.txt
.venv/bin/python tests/fixtures/a2/generate.py            # rewrite these files
.venv/bin/python tests/fixtures/a2/generate.py --check    # regenerate to a temp dir and compare (.nam as JSON: structure exact, weights within 1e-6, only the forward-pass-derived metadata loudness / gain (and the manifest's reference RMS / peak copies) within 1e-4 rel, the packed-vs-standalone forward diagnostic `packedForwardMaxAbsDiff` within 1e-6 absolute (runner CPUs differ by about 1e-8 in forward-pass outputs), every other float within 1e-9 relative (abs floor 1e-12: summation-order noise such as the input's rmsDbfs differing by 1.8e-15 between CPUs), ints / strings / structure exact, files missing from either side flagged; input.wav exact; ref_*.wav within 1e-5; differences are printed with key path and max abs diff)
```

Provenance: `neural-amp-modeler==0.13.0`, `torch==2.5.1` (CPU, 1 thread), model-file version `0.7.0`, seeds
`input 20261007`, `a2 20261008`, `a1 20261009` (also in `manifest.json`). The trainer stamps the export time into the
`.nam` metadata; the generator pins that date to 2026-10-07 00:00:00 so regeneration is reproducible. Nothing else in
the files is edited after the trainer wrote them.

| File | Architecture string | What it is | Receptive field | Latency | Params | Size |
|---|---|---|---|---|---|---|
| `a2_container.nam` | `SlimmableContainer` | packed-trainer export: `submodels = [{max_value 0.5, A2 Lite}, {max_value 1.0, A2 Full}]` | 6347 | 0 | 22 783 (both) | 309 KB |
| `a2_full.nam` | `WaveNet` | A2 Full: 1 layer array, 23 layers, 8 channels, LeakyReLU, kernel 6 (15 at layers 15-16), head kernel 16 | 6347 | 0 | 12 145 | 265 KB |
| `a2_lite.nam` | `WaveNet` | A2 Lite: same shape, 3 channels | 6347 | 0 | 1 870 | 43 KB |
| `a1_standard.nam` | `WaveNet` | A1 standard: two arrays 16/8 channels, 10 + 10 dilations 1..512, kernel 3, Tanh | 4093 | 0 | 13 801 | 301 KB |

Input and references (mono, 48 kHz, **32-bit float WAV**, 48 000 samples = 1 s; readable by `core`'s wav reader):

| File | Content |
|---|---|
| `input.wav` | 0.4 s seeded Gaussian noise (sigma 0.1, 5 ms fades) followed by a 0.6 s log sine sweep 50 Hz to 16 kHz (peak 0.316). Peak 0.433, RMS -14.7 dBFS |
| `ref_a2_full.wav` | trainer forward pass of `a2_full.nam` (= the container's Full submodel). RMS -26.8 dBFS |
| `ref_a2_lite.wav` | trainer forward pass of `a2_lite.nam` (= the container's Lite submodel). RMS -31.3 dBFS |
| `ref_a1_standard.wav` | trainer forward pass of `a1_standard.nam`. RMS -37.3 dBFS |

How the references are made: the exported `.nam` is read back with the trainer's own loader (`nam.models.init_from_nam`)
and run in float32 on CPU over `input.wav` with the trainer's default zero start-up history. Expect a player that
prewarms with zeros (the core's `ResetAndPrewarm`) to agree from sample 0 up to float rounding; if a player's start-up
convention differs, compare only after the first `receptiveFieldSamples` samples (the input is far longer). The packed
trainer's own forward pass (one wide masked net) gives the same samples as the extracted submodels
(`packedForwardMaxAbsDiff` in the manifest).

## How "A2 Lite" and "A2 Full" come out of the trainer

The trainer does not use these names. `nam/train/_resources/config_model_packed.json` defines one `PackedWaveNet` with two
submodels, `channels_3` and `channels_8` (identical in every other respect), and `export.container_max_values: "uniform"`.
`PackedWaveNet.export_container` writes **one file** (`SlimmableContainer`) holding both, with `max_value` 0.5 for the
3-channel net and 1.0 for the 8-channel net. "Lite" = the 3-channel submodel and "Full" = the 8-channel submodel is the
mapping the pinned core's A2 fast path uses (`NAM/wavenet/a2_fast.h`, see `docs/reports/v0_6/audit_core.md`); TONE3000 and
Darkglass are not documented in the trainer. The standalone `a2_full.nam` / `a2_lite.nam` are the same two networks as
plain `WaveNet` files (`PackedWaveNet.extract_submodel(i).export`), identical in weights and config to the container's
submodels (asserted by the generator). A container metadata block carries `loudness` / `gain` of the Full submodel.

`a2_container.nam` also carries `metadata.sawblade = {fixture: true, ...}` and user metadata (name, `modeled_by`,
`gear_type: amp`, `tone_type: hi_gain`) so loaders are tested with a Sawblade-style metadata block on a container.
