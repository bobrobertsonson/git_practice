# Separator spike (phase 5.0 + 5.1a engine bake-off)

Standalone measurement of stem-separation engines for play-along / matching: **demucs.cpp** (C++ port of
htdemucs, Eigen, CPU) and **ONNX Runtime** against stock Python Demucs.
Specs: `docs/specs/phase5_0_separator_spike.md`, `docs/specs/phase5_1a_separator_engine.md`. Results: `RESULTS.md`
(5.0 sections, then "Phase 5.1a"). Licences: `docs/THIRD_PARTY.md` ("Separator spike" and "Phase 5.1a additions").
Nothing here is part of `core/`, the plugin or the normal build: with the options OFF nothing is fetched.

Everything big lives **outside the repo**:

| What | Default location | Override |
|------|------------------|----------|
| venv (demucs 4.0.1, torch, museval, onnx, onnxruntime) | `~/.venvs/sawblade-demucs` | `SAWBLADE_SEP_VENV` |
| checkpoints, ggml weights, exported `.onnx`, demucs.cpp checkout | `~/.cache/sawblade/separator/` | `SAWBLADE_SEP_CACHE` |
| test audio, stems, build dirs, logs | `~/sawblade-sep-data/` | `SAWBLADE_SEP_DATA` |

## One-time setup

```
sudo apt-get install -y libopenblas-dev libopenblas0-openmp   # Ubuntu 24.04, 0.3.26; only for the BLAS build
python3 -m venv ~/.venvs/sawblade-demucs
~/.venvs/sawblade-demucs/bin/pip install demucs==4.0.1 torch==2.5.1 torchaudio==2.5.1 \
    -c match/constraints-separation.txt museval==0.4.1 musdb==0.4.3 stempeg==0.2.6 \
    soundfile==0.14.0 numpy==2.4.6 scipy==1.17.1 onnx==1.23.1 onnxruntime==1.30.0
spikes/separator/scripts/fetch_weights.sh      # checkpoints (dl.fbaipublicfiles.com), sha256, ggml convert
spikes/separator/scripts/fetch_material.sh     # real7 clip, loop70, synth (git clone of sigsep-mus-eval + synthetic)
TORCH_HOME=~/.cache/sawblade/separator/torchhome \
  ~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/export_onnx.py --model htdemucs    --verify
TORCH_HOME=~/.cache/sawblade/separator/torchhome \
  ~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/export_onnx.py --model htdemucs_6s --verify
```

`export_onnx.py` writes `<cache>/onnx/<model>-core-opset17.onnx` (+ `.sha256`) and checks ORT against torch
(prints residual dB). The graph is the htdemucs core only; STFT/iSTFT live in `separator_onnx.cpp`.

## Build (all options OFF by default; the normal build never fetches demucs.cpp or ONNX Runtime)

```
# (a) demucs.cpp patched, Eigen GEMM + (b) ONNX Runtime driver, shippable x86-64-v3 baseline
cmake -S . -B ~/sawblade-sep-data/build-a-eigen -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON -DSAWBLADE_BUILD_TESTS=OFF \
      -DSAWBLADE_SEPARATOR_ARCH=x86-64-v3 -DSAWBLADE_BUILD_SEPARATOR_ONNX=ON
cmake --build ~/sawblade-sep-data/build-a-eigen
# (a) with OpenBLAS (OpenMP variant, linked by path) behind EIGEN_USE_BLAS
cmake -S . -B ~/sawblade-sep-data/build-a-blas -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON -DSAWBLADE_BUILD_TESTS=OFF \
      -DSAWBLADE_SEPARATOR_ARCH=x86-64-v3 -DSAWBLADE_SEPARATOR_BLAS=ON
cmake --build ~/sawblade-sep-data/build-a-blas --target separator_spike
```

Spike-only CMake options: `SAWBLADE_SEPARATOR_ARCH` (e.g. `x86-64-v3`, overrides NATIVE), `SAWBLADE_SEPARATOR_NATIVE`
(default ON, `-march=native`, **not shippable**), `SAWBLADE_SEPARATOR_OPENMP` (default ON),
`SAWBLADE_SEPARATOR_BLAS` (OFF; `SAWBLADE_OPENBLAS_LIBRARY=<path>` overrides the auto-detected OpenMP variant),
`SAWBLADE_BUILD_SEPARATOR_ONNX` (OFF; downloads the pinned ONNX Runtime 1.30.0 tarball, sha256-checked).
demucs.cpp is patched at configure time by `patches/demucscpp-f1206e9a-sawblade.patch` (see its header).
Eigen's alignment depends on `-march` (and `EIGEN_USE_BLAS` changes product dispatch), so both are PUBLIC on
`demucs_cpp_lib`; every TU that sees Eigen types must match or the first GEMM heap-corrupts.

## Run

```
separator_spike --model ~/.cache/sawblade/separator/ggml/ggml-model-htdemucs-6s-f16.bin \
                --in ~/sawblade-sep-data/loop70/mixture.wav --out-dir /tmp/stems [--threads N] [--cancel-after S] [--quiet]
separator_onnx  --model ~/.cache/sawblade/separator/onnx/htdemucs_6s-core-opset17.onnx \
                --in ~/sawblade-sep-data/loop70/mixture.wav --out-dir /tmp/stems [--threads N] [--cancel-after S] \
                [--profile PREFIX] [--arena] [--mem-pattern] [--no-spin] [--dump-seg DIR]
```

Both print `RESULT load_s sep_s audio_s rtf sep_s_per_min peak_rss_mb progress_cb_calls`. Input must be 44.1 kHz
stereo WAV; one float32 stereo WAV per stem is written. Weights, audio, stems, `.onnx`, `.tgz` are git-ignored.
(5.0's `--mode split` and `--ftz` flags are gone: split is a different algorithm, FTZ/DAZ is now always on.)

## Reproduce the 5.1a tables

```
spikes/separator/scripts/run_matrix.sh        # stems: eigen/blas/onnx (4 threads) + stock Python shifts=0, real7 + synth, 4s + 6s
spikes/separator/scripts/run_determinism.sh   # same engine twice, 1 vs 4 threads (real7)
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/eval.py --det det/eigen_4s_t4a:det/eigen_4s_t4b ...
spikes/separator/scripts/run_timing.sh        # 70 s loop speed/memory (onnx python blas eigen), raw lines in timing_51a.log
spikes/separator/scripts/run_cancel.sh        # cancel latency + library-silence check
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/verify_segment.py --model htdemucs --dump DIR   # STFT/iSTFT check
spikes/separator/scripts/gdb_sample.sh PID N DT OUT                                                          # sampling profile
python3 spikes/separator/scripts/summarize_timing.py      # timing_51a.log -> RESULTS table (run from anywhere)
(cd ~/sawblade-sep-data && python3 /path/to/spikes/separator/scripts/summarize_cancel.py)   # cancel.log -> table
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/coreml_coverage.py <core.onnx>   # CoreML EP op coverage
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/fp16_check.py --model htdemucs_6s # fp16 proxy
~/.venvs/sawblade-demucs/bin/python spikes/separator/scripts/export_onnx.py --model htdemucs_6s --flops-only
```

`eval_50.py` is the 5.0 evaluation (needs the 5.0 stem sets; the 5.0 scripts and their driver flags are in git
history at commit `ea88df8`).
