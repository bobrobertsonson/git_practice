# Separator spike (phase 5.0)

Standalone measurement of **demucs.cpp** (C++ port of htdemucs, Eigen, CPU) against Python Demucs.
Spec: `docs/specs/phase5_0_separator_spike.md`. Results: `RESULTS.md`. Licences: `docs/THIRD_PARTY.md`
("Separator spike"). Nothing here is part of `core/`, the plugin or the normal build.

Everything big lives **outside the repo**:

| What | Default location | Override |
|------|------------------|----------|
| venv (demucs 4.0.1, torch, museval) | `~/.venvs/sawblade-demucs` | `SAWBLADE_SEP_VENV` |
| checkpoints, ggml weights, demucs.cpp checkout | `~/.cache/sawblade/separator/` | `SAWBLADE_SEP_CACHE` |
| test audio, stems, build dir | `~/sawblade-sep-data/` | `SAWBLADE_SEP_DATA` |

## Build (option OFF by default; the normal build never fetches demucs.cpp)

```
cmake -S . -B ~/sawblade-sep-data/build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON -DSAWBLADE_BUILD_TESTS=OFF
cmake --build ~/sawblade-sep-data/build-spike --target separator_spike
```

Spike-only CMake options: `SAWBLADE_SEPARATOR_NATIVE` (default ON, `-march=native`; **not shippable**,
`-DSAWBLADE_SEPARATOR_NATIVE=OFF` for a no-`-march` build), `SAWBLADE_SEPARATOR_ARCH` (e.g. `x86-64-v3`, overrides NATIVE) and `SAWBLADE_SEPARATOR_OPENMP` (default ON).
Eigen's alignment depends on `-march`, so the flag is PUBLIC on `demucs_cpp_lib`; every TU that sees
Eigen types must match or the first GEMM heap-corrupts.

## One-time setup

```
python3 -m venv ~/.venvs/sawblade-demucs
~/.venvs/sawblade-demucs/bin/pip install demucs==4.0.1 torch==2.5.1 torchaudio==2.5.1 \
    -c match/constraints-separation.txt museval==0.4.1 musdb==0.4.3 stempeg==0.2.6 \
    soundfile==0.14.0 numpy==2.4.6 scipy==1.17.1
spikes/separator/scripts/fetch_weights.sh      # checkpoints (dl.fbaipublicfiles.com), sha256, ggml convert
spikes/separator/scripts/fetch_material.sh     # test material (git clone of sigsep-mus-eval + synthetic)
```

## Run

```
separator_spike --model ~/.cache/sawblade/separator/ggml/ggml-model-htdemucs-4s-f16.bin \
                --in ~/sawblade-sep-data/real7/mixture.wav --out-dir /tmp/stems [--threads N] [--mode single|split]
spikes/separator/scripts/run_matrix.sh                       # all C++ / Python stem sets for the eval
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/eval.py   # SDR + null-test tables
```

Input must be 44.1 kHz stereo WAV (other rates are rejected). One float32 stereo WAV per stem is written.
Weights, audio and stems are git-ignored (`spikes/separator/.gitignore`).
