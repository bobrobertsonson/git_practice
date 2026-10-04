# Separator spike results (phase 5.0)

Measured on the dev box on 2026-10-04. Everything here was produced by the scripts in
`spikes/separator/scripts/`; audio, stems and weights are outside the repo.

> **Phase 5.1a (engine bake-off) results are at the end of this file** ("Phase 5.1a": patched demucs.cpp with Eigen
> or OpenBLAS vs ONNX Runtime, null test against stock Python, speed/memory, Apple Silicon estimate). The 5.0 content below
> is kept as it was; its numbers came from a different (faster) host and from the unpatched demucs.cpp.

## TL;DR

- demucs.cpp (pinned `f1206e9a`) **reproduces Python Demucs**: with Python forced to the same settings the
  residual is **-48 to -72 dB on every stem except one at -38 dB (synth 6s `other`, see the null section)**. With
  the spec's `shifts=0` Python the residual is only about -16 to -28 dB (worse on low-level stems) because of two
  behaviours of demucs.cpp that differ from Python (below). Neither costs much SDR: on the real clip C++ and
  Python agree to within 0.3 dB (0.03 dB when the shift is matched).
- It is **slow and does not scale with cores** through its library API: 4 cores, `-march=native`
  (AVX-512): 261 s of compute per minute of audio on 1 thread, 241 s at 4 OpenMP threads (RTF 4.0). Its own
  song-splitting `demucs_mt` path gets 85 s/min on 4 cores but is a different, slightly worse algorithm
  (0 to 1.6 dB lower SDR) and uses 6 GB RAM. Python/torch CPU does 20 s/min (4 threads) and 64 s/min (1 thread).
  Shippable-baseline builds are slower again: `-march=x86-64-v3` (AVX2/FMA) is 262 s/min (6s) and 574 s/min (4s)
  on 1 thread; plain no-`-march` is 359 s/min (6s) and 978 s/min (4s, **an outlier**, see below). The 4s slowness
  in non-native builds is **denormal floats in the 4-source model's GEMMs**: setting flush-to-zero/denormals-are-zero
  (`separator_spike --ftz`) cut the 6.8 s clip from 165 s to 69 s (no-`-march`) and from 102 s to 49 s (v3), and
  does nothing for 6s. Not yet re-timed on the 70 s loop with `--ftz`.
- Weights: **no separate weights licence is stated**; repo is MIT; trained on MUSDB18-HQ (academic use only) +
  800 extra songs. Fine for personal non-commercial use; needs re-clearing for anything commercial or
  redistributed. See `docs/THIRD_PARTY.md`.

## Deviations from the spec / things the spec did not expect

1. **demucs.cpp always applies a time shift** (`rand() % 22050`, never seeded, so it is **4033** on glibc for the
   first call). The spec's Python reference is `shifts=0`. The eval therefore uses three Python references:
   `shifts=0` (spec), `shift=4033` (the same shift as C++, by patching `random.randint`), and `cppmatch`
   (shift 4033 plus the padding behaviour below).
2. **Short-chunk padding differs.** For a chunk shorter than the 7.8 s segment (the last chunk of any file, and
   the only chunk of a clip under 7.8 s) Python's `TensorChunk.padded` fills the padding with the real audio
   around the chunk (zeros only beyond the track ends). demucs.cpp pads with zeros. Evidence: with shift 4033
   the C++-vs-Python residual of the real clip is -54 to -70 dB in every half-second up to 5.5 s and -27 / -21 dB
   in the last two (the final chunk); making Python zero-pad (`--zero-pad-chunks`) drops the whole-file
   residual to -62..-72 dB on the 4-source model. Float
   noise is not the cause: Python fp32 vs Python fp64 is -127 dB. Effect on a real song: the last chunk of
   every song (up to about 7.8 s at the end) is separated with slightly less context.
3. The library API has **no threading knob**. `--threads N` in the driver sets OpenMP/Eigen GEMM threads
   (`omp_set_num_threads`, `Eigen::setNbThreads`) for `--mode single`, and N worker threads for `--mode split`
   (demucs.cpp's `demucs_mt` path; the driver includes its `cli-apps/threaded_inference.hpp`).
4. **Eigen alignment trap**: `-march=native` must be the same in every TU that sees Eigen types (driver
   included) or the first GEMM heap-corrupts. `-march=native` is therefore a PUBLIC compile option of
   `demucs_cpp_lib`.
5. Upstream's Release flags are `-Ofast -march=native -fno-unsafe-math-optimizations -freciprocal-math
   -fno-signed-zeros` with OpenBLAS (`EIGEN_USE_BLAS`) by default. We build with `-O3 -DNDEBUG -freciprocal-math
   -fno-signed-zeros`, OpenMP instead of OpenBLAS (only the reference libblas3 is installed here; a fast BLAS
   was not pursued), so the speed numbers are **for Eigen's own GEMM, not for upstream's recommended OpenBLAS build**.
   A BLAS build could be faster; not measured.
6. Eigen 3.4.0 (the project's) was used instead of demucs.cpp's Eigen submodule (`dd8c71e6`, post-3.4). It builds
   and the null test shows it is harmless.
7. The converter script is named `-f16` and the checkpoints really are stored as float16 (the converter log
   prints `dtype: float16` for every tensor); the C++ loader expands to float32 on load, so C++ and Python run
   identical weights.
8. **Test material** (D4): MUSDB18 on Zenodo is blocked. Real material with ground truth is only the **6.8 s
   MUSDB18-7 sample "Music Delta - 80s Rock"** shipped in the `sigsep-mus-eval` git repo (rock, has
   guitars; the dataset's `other` stem lumps guitars with everything else, so there is no guitar-only truth).
   A 36 s **synthetic** mixture (distorted guitar, pad, bass, drums, vocal-like) supplies exact stems and 30 s+
   length, but **synthetic results are weak evidence**: the model does not treat the synthetic bass as bass
   (bass SDR 0.0) or the synthetic guitar as a guitar (6s guitar SDR 0.02), so the synth SDR numbers say
   little about quality on real guitars. Timing used the real clip looped to 70 s. **We have not measured
   guitar-stem quality on a real distorted guitar with ground truth; that remains open.** (Cambridge-MT,
   huggingface, zenodo blocked; no other ground-truth multitrack was reachable over git/pypi.)
9. The spec's D5 says museval median; done (`museval.metrics.bss_eval`, 1 s windows, nanmedian), plus global SDR.
   For 6s the "other" truth is compared with `other+guitar+piano` summed (a 6-stem model splits what the dataset
   calls `other`); individual 6s stems are listed as global SDR only where noted.

## Environment

- CPU: `Intel(R) Xeon(R) Processor @ 2.10GHz` (lscpu; flags include AVX-512 incl. fp16/bf16), 4 cores, 15 GB RAM, Ubuntu 24.04,
  g++ 13.3.0, cmake 3.28, Ninja, Python 3.11, ffmpeg 6.1.1.
- Python venv `~/.venvs/sawblade-demucs`: demucs 4.0.1, torch 2.5.1 (CPU use, pypi wheel), torchaudio 2.5.1,
  numpy 2.4.6, scipy 1.17.1, soundfile 0.14.0, museval 0.4.1, musdb 0.4.3, stempeg 0.2.6, julius 0.2.8,
  einops 0.8.2, openunmix 1.3.0. Installed with `-c match/constraints-separation.txt`.
- demucs.cpp `f1206e9adeea103aef4a636b9e62297cf1f8e34e` (2024-12-01). Build: Release, `-O3 -march=native`
  (also a `-DSAWBLADE_SEPARATOR_NATIVE=OFF` portable build), OpenMP 4.5.

## Files and hashes

| File | URL / source | Size | sha256 |
|---|---|---|---|
| htdemucs checkpoint `955717e8-8726e21a.th` | https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th | 84,141,911 | `8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4` (verified; matches the filename suffix) |
| htdemucs_6s checkpoint `5c90dfd2-34c22ccb.th` | https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/5c90dfd2-34c22ccb.th | 54,996,327 | `34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd` (verified) |
| `ggml-model-htdemucs-4s-f16.bin` (converted) | demucs.cpp `scripts/convert-pth-to-ggml.py` | 83,994,361 | `72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064` |
| `ggml-model-htdemucs-6s-f16.bin` (converted) | same, `--six-source` | 54,855,129 | `09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856` |
| `Music Delta - 80s Rock.stem.mp4` | `sigsep/sigsep-mus-eval` @ `2716132b2f4125d4174b99d01fb5ed2e9de17adb`, `tests/data/MUSDB18-7-SAMPLE/train/` | 1,121,359 | `56d6678c95101b04a9a76bd07788e0cd1f2a1f2c25f89821b919c33c117872eb` |
| `real7/mixture.wav` (decoded stream 0, 6.80 s) | `fetch_material.sh` | 2,400,344 | `fde3f33074b6589f3aa1329212280099ae76c322f034aee67484253b6510f422` |
| `synth/mixture.wav` (36 s synthetic) | `make_material.py` (seeded) | | `bc9f2cee656a5c36271d51a12992709d100737b47826ab13378e6c691164fdd3` |
| `loop70/mixture.wav` (real clip looped to 70 s, timing only) | `make_material.py` | 24,696,088 | `0ebc920c8bbe49576453220b8a51edee8cc1b3817a231ed5ca8438481bf85ee3` |

## Commands

```
cmake -S . -B ~/sawblade-sep-data/build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON -DSAWBLADE_BUILD_TESTS=OFF
cmake -S . -B ~/sawblade-sep-data/build-portable ... -DSAWBLADE_SEPARATOR_NATIVE=OFF
spikes/separator/scripts/fetch_weights.sh ; spikes/separator/scripts/fetch_material.sh
spikes/separator/scripts/run_matrix.sh     # C++ (4 threads) + Python shifts=0 / shift4033 / cppmatch stems
spikes/separator/scripts/eval.py           # tables below
spikes/separator/scripts/run_timing.sh     # speed / memory on the 70 s loop
```

## Quality: SDR and null test

Method: museval BSSEval v4 (`museval.metrics.bss_eval`, 1 s windows and hop, median over frames, nan ignored),
plus global SDR `10 log10(sum s^2 / sum (s-s_hat)^2)` over the whole track and both channels. Ground truth
provided by the datasets: `real7` has drums, bass, other (everything else, including guitars), vocals;
`synth` has drums, bass, guitar, pad ("other") and vocal-like. For `real7` and 4s the `other` row is the
dataset's `other`; for 6s the `other` row is the estimate `other+guitar+piano` summed (an `other`-only stem and
the guitar stem are listed as global SDR only). For `synth`, "other" truth is pad+guitar. Columns are C++ (single
mode, the shift is 4033), Python `shifts=0` (the spec's setting) and Python with C++'s shift.
Null columns: residual `20 log10(rms(cpp-py)/rms(py))`, max abs difference, cross-correlation lag in samples
(always 0), and lengths. The "C++ settings" null (same shift, zero-padded short chunks) is the apples-to-apples
comparison.


### SDR (dB), dataset `real7`, model htdemucs 4s

| stem group | C++ museval | C++ global | Py shifts=0 museval | Py shifts=0 global | Py shift=4033 museval | Py shift=4033 global |
|---|---|---|---|---|---|---|
| drums | 15.07 | 14.51 | 15.03 | 14.55 | 15.07 | 14.52 |
| bass | 13.09 | 12.66 | 13.01 | 12.61 | 13.09 | 12.66 |
| other | 9.35 | 9.00 | 9.32 | 8.95 | 9.35 | 8.99 |
| vocals | 14.31 | 14.58 | 14.39 | 14.64 | 14.32 | 14.60 |

#### Null test `real7` 4s: C++ vs Python with C++ settings: same time shift (4033) and zero-padded short chunks

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -69.1 | 4.53e-04 | 0 | 300032 / 300032 |
| bass | -64.3 | 3.18e-04 | 0 | 300032 / 300032 |
| other | -65.8 | 2.49e-04 | 0 | 300032 / 300032 |
| vocals | -72.4 | 2.86e-04 | 0 | 300032 / 300032 |

#### Null test `real7` 4s: C++ vs Python with the same time shift as C++ (offset 4033)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -38.6 | 4.07e-02 | 0 | 300032 / 300032 |
| bass | -29.7 | 2.57e-02 | 0 | 300032 / 300032 |
| other | -29.7 | 2.45e-02 | 0 | 300032 / 300032 |
| vocals | -37.9 | 2.51e-02 | 0 | 300032 / 300032 |

#### Null test `real7` 4s: C++ vs Python shifts=0 (spec setting)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -28.0 | 5.36e-02 | 0 | 300032 / 300032 |
| bass | -23.6 | 3.53e-02 | 0 | 300032 / 300032 |
| other | -21.8 | 3.18e-02 | 0 | 300032 / 300032 |
| vocals | -28.1 | 5.49e-02 | 0 | 300032 / 300032 |

### SDR (dB), dataset `real7`, model htdemucs 6s

| stem group | C++ museval | C++ global | Py shifts=0 museval | Py shifts=0 global | Py shift=4033 museval | Py shift=4033 global |
|---|---|---|---|---|---|---|
| drums | 14.72 | 14.32 | 15.00 | 14.48 | 14.72 | 14.35 |
| bass | 11.44 | 11.01 | 11.52 | 11.11 | 11.44 | 11.08 |
| other | 9.14 | 8.78 | 9.11 | 8.80 | 9.12 | 8.78 |
| vocals | 14.65 | 14.59 | 14.67 | 14.65 | 14.65 | 14.64 |
| other (6s 'other' stem only) (global only) | - | -0.00 | - | -0.00 | - | -0.00 |
| guitar (vs GT other; not a pure-guitar GT) (global only) | - | 8.78 | - | 8.80 | - | 8.78 |

#### Null test `real7` 6s: C++ vs Python with C++ settings: same time shift (4033) and zero-padded short chunks

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -67.3 | 7.92e-04 | 0 | 300032 / 300032 |
| bass | -62.9 | 5.18e-04 | 0 | 300032 / 300032 |
| other | -49.1 | 1.45e-05 | 0 | 300032 / 300032 |
| vocals | -71.4 | 3.95e-04 | 0 | 300032 / 300032 |
| guitar | -64.4 | 4.25e-04 | 0 | 300032 / 300032 |
| piano | -50.0 | 6.53e-06 | 0 | 300032 / 300032 |

#### Null test `real7` 6s: C++ vs Python with the same time shift as C++ (offset 4033)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -36.2 | 4.72e-02 | 0 | 300032 / 300032 |
| bass | -28.4 | 6.32e-02 | 0 | 300032 / 300032 |
| other | -15.7 | 8.28e-04 | 0 | 300032 / 300032 |
| vocals | -33.4 | 4.70e-02 | 0 | 300032 / 300032 |
| guitar | -30.0 | 4.72e-02 | 0 | 300032 / 300032 |
| piano | -17.5 | 6.20e-04 | 0 | 300032 / 300032 |

#### Null test `real7` 6s: C++ vs Python shifts=0 (spec setting)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -28.2 | 7.21e-02 | 0 | 300032 / 300032 |
| bass | -23.3 | 6.08e-02 | 0 | 300032 / 300032 |
| other | -1.4 | 9.60e-04 | 0 | 300032 / 300032 |
| vocals | -27.6 | 6.30e-02 | 0 | 300032 / 300032 |
| guitar | -21.5 | 7.28e-02 | 0 | 300032 / 300032 |
| piano | -5.2 | 7.16e-04 | 0 | 300032 / 300032 |

### SDR (dB), dataset `synth`, model htdemucs 4s

| stem group | C++ museval | C++ global | Py shifts=0 museval | Py shifts=0 global | Py shift=4033 museval | Py shift=4033 global |
|---|---|---|---|---|---|---|
| drums | 9.76 | 9.89 | 9.72 | 9.78 | 9.76 | 9.89 |
| bass | 0.00 | 0.00 | 0.01 | 0.01 | 0.00 | 0.00 |
| vocals | 3.69 | 3.57 | 3.46 | 3.57 | 3.69 | 3.55 |
| other | 8.98 | 8.90 | 8.95 | 8.95 | 8.99 | 8.91 |

#### Null test `synth` 4s: C++ vs Python with C++ settings: same time shift (4033) and zero-padded short chunks

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -61.4 | 1.81e-03 | 0 | 1587600 / 1587600 |
| bass | -71.1 | 1.01e-04 | 0 | 1587600 / 1587600 |
| other | -69.4 | 1.87e-03 | 0 | 1587600 / 1587600 |
| vocals | -57.8 | 3.90e-04 | 0 | 1587600 / 1587600 |

#### Null test `synth` 4s: C++ vs Python with the same time shift as C++ (offset 4033)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -45.3 | 1.81e-02 | 0 | 1587600 / 1587600 |
| bass | -42.5 | 2.64e-03 | 0 | 1587600 / 1587600 |
| other | -49.1 | 2.02e-02 | 0 | 1587600 / 1587600 |
| vocals | -36.0 | 6.91e-03 | 0 | 1587600 / 1587600 |

#### Null test `synth` 4s: C++ vs Python shifts=0 (spec setting)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -19.1 | 1.64e-01 | 0 | 1587600 / 1587600 |
| bass | -25.2 | 9.11e-03 | 0 | 1587600 / 1587600 |
| other | -26.6 | 1.65e-01 | 0 | 1587600 / 1587600 |
| vocals | -15.7 | 2.96e-02 | 0 | 1587600 / 1587600 |

### SDR (dB), dataset `synth`, model htdemucs 6s

| stem group | C++ museval | C++ global | Py shifts=0 museval | Py shifts=0 global | Py shift=4033 museval | Py shift=4033 global |
|---|---|---|---|---|---|---|
| drums | 10.20 | 10.42 | 10.40 | 10.42 | 10.19 | 10.41 |
| bass | 2.65 | 2.14 | 1.82 | 1.04 | 2.63 | 2.04 |
| vocals | 4.22 | 3.87 | 4.30 | 3.95 | 4.22 | 3.84 |
| other | 3.66 | 3.93 | 2.83 | 3.13 | 3.50 | 3.76 |
| guitar (6s guitar stem vs GT guitar) (global only) | - | 0.02 | - | 0.02 | - | 0.02 |
| pad (6s other+piano vs GT pad) (global only) | - | -3.43 | - | -2.38 | - | -3.28 |

#### Null test `synth` 6s: C++ vs Python with C++ settings: same time shift (4033) and zero-padded short chunks

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -58.8 | 2.34e-03 | 0 | 1587600 / 1587600 |
| bass | -48.0 | 2.25e-03 | 0 | 1587600 / 1587600 |
| other | -38.0 | 8.30e-03 | 0 | 1587600 / 1587600 |
| vocals | -55.9 | 3.30e-04 | 0 | 1587600 / 1587600 |
| guitar | -67.7 | 2.34e-05 | 0 | 1587600 / 1587600 |
| piano | -59.5 | 1.18e-04 | 0 | 1587600 / 1587600 |

#### Null test `synth` 6s: C++ vs Python with the same time shift as C++ (offset 4033)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -45.8 | 1.25e-02 | 0 | 1587600 / 1587600 |
| bass | -30.7 | 3.88e-02 | 0 | 1587600 / 1587600 |
| other | -22.3 | 7.85e-02 | 0 | 1587600 / 1587600 |
| vocals | -33.4 | 7.79e-03 | 0 | 1587600 / 1587600 |
| guitar | -52.1 | 3.42e-04 | 0 | 1587600 / 1587600 |
| piano | -39.7 | 6.60e-03 | 0 | 1587600 / 1587600 |

#### Null test `synth` 6s: C++ vs Python shifts=0 (spec setting)

| stem | residual dB (20log10 rms(cpp-py)/rms(py)) | max abs diff | xcorr lag (samples) | len cpp / py |
|---|---|---|---|---|
| drums | -20.4 | 1.50e-01 | 0 | 1587600 / 1587600 |
| bass | -12.2 | 1.67e-01 | 0 | 1587600 / 1587600 |
| other | -7.5 | 2.10e-01 | 0 | 1587600 / 1587600 |
| vocals | -16.4 | 3.95e-02 | 0 | 1587600 / 1587600 |
| guitar | -35.1 | 2.76e-03 | 0 | 1587600 / 1587600 |
| piano | -30.0 | 6.65e-03 | 0 | 1587600 / 1587600 |


Caveats on the nulls: the `other` stem of the 6s model on the real clip is almost silent (the model puts the
guitar-like energy in `guitar`), so its residuals are at a very low absolute level (max abs diff 1e-5 to 1e-3).
The remaining weak cell, `synth` 6s `other` at -38 dB, is not a quiet stem (rms -22.3 dBFS, as loud as bass at
-23.4; the guitar and piano stems are -42.8): the model is unsure on this synthetic material (its SDR is only
3.7 dB), so tiny numeric differences are amplified; likely cause, not proven. Max abs diff 8e-3.

### demucs.cpp `demucs_mt` split mode quality (4 workers; 4-source model)

SDR (museval median / dB), vs single-pass C++: real7 drums 13.46 vs 15.07, bass 11.52 vs 13.09, other 8.72 vs
9.35, vocals 14.25 vs 14.31; synth drums 9.45 vs 9.76, bass 0.0 vs 0.0, other 8.87 vs 8.98, vocals 3.68 vs 3.69.
Split vs single null: -13 to -26 dB. The real clip is the worst case (6.8 s cut into 4 parts of 1.7 s); the
loss is smaller on longer material. Wall time on 36 s of synth: 49 s (RTF 1.37), 6.2 GB peak RSS.

## Speed and memory (70 s looped mixture)

Median of the runs listed (2 to 3 per configuration; the long single-thread C++ ones only 2). Load time is
reported separately (separation time excludes it). Peak RSS is `getrusage` `ru_maxrss` of the whole process
(including the 70 s input and output tensors). "single" is the library API `demucs_inference()` (real 7.8 s
segments, 25 % overlap, one segment at a time; threads = OpenMP/Eigen GEMM threads); "split" is demucs.cpp's
`demucs_mt` (song cut into N parts, N std::threads). "native" = `-march=native` (AVX-512 here, **not
shippable**); "portable" = no `-march`. Python: demucs 4.0.1 / torch 2.5.1 CPU, default 4 torch threads, and 1.

| config | runs | load s | separation s for 70 s audio (per run) | s per min of audio (median) | RTF | peak RSS MB |
|---|---|---|---|---|---|---|
| python m=4s threads=4(default) | 3 | 0.34 | 24.3, 23.7, 23.7 | 20.3 | 0.34 | 1673 |
| python m=4s threads=1 | 2 | 0.37 | 75.1, 74.3 | 64.0 | 1.07 | 2076 |
| native-single m=4s mode=single threads=4 | 2 | 0.35 | 277.9, 285.0 | 241.2 | 4.02 | 2914 |
| native-split m=4s mode=split threads=4 | 3 | 0.33 | 106.3, 98.7, 97.9 | 84.6 | 1.41 | 6026 |
| python m=6s threads=4(default) | 3 | 0.24 | 19.0, 18.4, 18.3 | 15.8 | 0.26 | 1823 |
| python m=6s threads=1 | 2 | 0.26 | 60.7, 59.9 | 51.7 | 0.86 | 2198 |
| native-single m=6s mode=single threads=4 | 2 | 0.17 | 269.2, 269.1 | 230.7 | 3.85 | 2928 |
| native-split m=6s mode=split threads=4 | 3 | 0.19 | 95.9, 96.2, 95.1 | 82.2 | 1.37 | 6081 |
| native-single m=4s mode=single threads=1 | 2 | 0.32 | 304.5, 304.8 | 261.1 | 4.35 | 2113 |
| native-single m=6s mode=single threads=1 | 2 | 0.18 | 285.9, 287.5 | 245.8 | 4.10 | 2155 |
| portable-single m=4s mode=single threads=1 | 2 | 0.32 | 1142.3, 1140.5 | 978.3 | 16.30 | 2103 |
| portable-split m=4s mode=split threads=4 | 3 | 0.32 | 355.3, 360.7, 354.7 | 304.6 | 5.08 | 5624 |
| portable-single m=6s mode=single threads=1 | 2 | 0.16 | 422.7, 415.8 | 359.4 | 5.99 | 2155 |
| portable-split m=6s mode=split threads=4 | 3 | 0.16 | 132.8, 131.4, 133.2 | 113.8 | 1.90 | 6375 |

Short-clip single runs for reference (6.8 s real clip, C++ native): 46.3 s at 1 thread, 43.3 s (4s) and 43.0 s
(6s) at 4 threads, RTF 6.3 to 6.8 (the clip is one 7.8 s-padded segment plus a short one, so RTF is worse than the long-file RTF).

`x86-64-v3` rows (`-DSAWBLADE_SEPARATOR_ARCH=x86-64-v3`, 2 runs each, same 70 s loop, same box):

| config | runs | load s | separation s for 70 s audio (per run) | s per min of audio (median) | RTF | peak RSS MB |
|---|---|---|---|---|---|---|
| v3-single m=4s threads=1 | 2 | 0.32 | 670.4, 668.4 | 573.7 | 9.56 | 2113 |
| v3-single m=4s threads=4 | 2 | 0.32 | 379.4, 379.5 | 325.2 | 5.42 | 2914 |
| v3-split m=4s 4 workers | 2 | 0.31 | 219.0, 214.3 | 185.7 | 3.09 | 5448 |
| v3-single m=6s threads=1 | 2 | 0.16 | 307.4, 305.5 | 262.7 | 4.38 | 2155 |
| v3-single m=6s threads=4 | 2 | 0.16 | 269.0, 267.6 | 230.0 | 3.83 | 2929 |
| v3-split m=6s 4 workers | 2 | 0.16 | 103.5, 102.6 | 88.3 | 1.47 | 5896 |

Why is the 4-source model 2.7x slower than 6s in non-native builds (978 vs 359 s/min no-`-march`; 574 vs 263
v3)? No `perf` on this box, so I sampled with gdb (24 stack samples per run, 6.8 s clip, no-`-march`, 1 thread):
4s: 22 of 24 samples innermost in `Eigen::internal::gebp_kernel` (GEMM), all under
`demucscpp::common_encoder_layer` (the cross-transformer); 6s: 10 of 24 in `gebp_kernel`, the rest spread over
`apply_dconv`, decoders, `memset`, `erff`. Same layer, so it is data-dependent: a diagnostic `--ftz` switch
(MXCSR flush-to-zero + denormals-are-zero) made 4s no-`-march` 165 s -> 69 s and 4s v3 102 s -> 49 s, and
left 6s unchanged (64 s either way). So **denormals in the 4-source model's transformer GEMMs are the main
cause**. Not explained: why `-march=native` (AVX-512) 4s does not show it (45 s on the same clip), and the
remaining gap 69 s vs 45 s. For 5.1: set FTZ/DAZ on every inference thread (it is one line, but OpenMP/worker
threads each need it), or flush tiny weights/activations; the numbers above are without it.

For scale, a 4-minute song: Python 4 threads about 1.4 min (4s) / 1.1 min (6s); C++ native single 4 threads
about 16 min; C++ native split 4 workers about 5.6 min; C++ portable split 4 workers 20 min (4s) / 7.6 min (6s).

## API notes (what 5.1 would embed)

Header `src/model.hpp` (namespace `demucscpp`), source of `libdemucs_cpp_lib.a`:

```
struct demucs_model;                                  // ~80 MB of float tensors, caller-owned, default-constructed ({})
bool load_demucs_model(const std::string& ggml_path, demucs_model* m);   // fopen/fread, prints to stdout, false on failure
using ProgressCallback = std::function<void(float progress, const std::string& message)>;
Eigen::Tensor3dXf demucs_inference(const demucs_model&, const Eigen::MatrixXf& audio /*2 x N, 44.1 kHz*/, ProgressCallback);
//   returns Tensor<float,3>(n_stems, 2, N), n_stems = m.is_4sources ? 4 : 6 (drums,bass,other,vocals[,guitar,piano])
```

- Inputs: 44.1 kHz only (no resampler); stereo matrix (mono must be duplicated by the caller); whole song in
  memory, no streaming. Output is one allocated tensor for the whole song (4 or 6 x 2 x N floats).
- Threading: none in the library. Eigen GEMM follows OpenMP if built with it (little effect, see above). No
  global state was seen apart from the `rand()` shift (not a full audit); the model is `const` in inference, so one loaded model can be
  shared by several threads each calling `demucs_inference` on a different part of the audio (that is what
  `demucs_mt` does; each part gets its own normalisation, shift and zero padding).
- Progress: the callback is called from the inference thread about 85 to 90 times per 6.8 s clip, with a message
  string per layer (so it allocates/formats strings: fine for a background thread). **No cancellation**; the
  only way to stop is to let it finish or kill the thread/process. 5.1 would need to add a cancel check inside
  the segment loop (a patch to a vendored copy, or a fork).
- Memory ownership: model is caller-owned (value semantics, copyable but heavy); inference allocates per segment
  (std::vector / Eigen tensors, many MB) and frees them; the result is returned by value. It also writes many
  `std::cout` lines (offsets, layers) and uses `rand()`: both would have to be removed or silenced for a
  library. Not real-time safe in any way (and not meant to be); it must run on a worker thread, never in
  `process()`.
- Determinism: the shift comes from `rand()`; unseeded, it is 4033 for the first call in a process and then
  continues the glibc sequence. Not equal to Python's `shifts=0`. For reproducible, matcher-grade stems we would
  want the shift fixed (or 0) and a documented seed.
- Peak memory: 2.1 to 2.9 GB for the 70 s song in single mode (1.8 to 2.6 GB for the 6.8 s clip, so the
  working set is dominated by per-segment tensors, not the song length), 5.6 to 6.4 GB with 4 split workers.
  Python: 1.6 to 2.2 GB.

## Notes for 5.1 (proposals, not decisions)

- Correctness is not the problem; speed is. Before committing to demucs.cpp, decide the budget: separation is a
  background, once-per-song job, so a 4-minute song in 5 to 6 min (native, 4 workers) or 16 min (portable single
  call, 4-source) may or may not be acceptable. Python/torch is 4 to 10x faster on the same CPU.
- The ONNX Runtime fallback (spec: not built here) **looks needed** as a comparison, or at least a BLAS-backed
  demucs.cpp build (OpenBLAS/MKL/AOCL, which upstream recommends) measured on the same material. Torch's 20 s/min
  on 4 threads shows the hardware is capable; the C++ port's scalar layers (group norm, im2col, transformer
  loops; a stack sampling of the run showed time spread across `common_encoder_layer`, `conv2d`/`im2col`,
  `group_norm`) are not.
- Portable builds lose 1.5 to 3.7x versus `-march=native`; shippable code needs runtime dispatch or per-ISA builds.
- If demucs.cpp is kept: fix the seed/shift, add the Python-compatible context padding for short chunks (or accept
  the last chunk difference), add a cancel flag, remove stdout logging, resample to 44.1 kHz outside it.
- Quality still to measure on real distorted guitar with ground truth (none reachable here). The proposed route is
  the user's own multitracks, kept off-repo (docs/TEST_SOURCES.md).
- Licence gate: weights licence is not stated and the training data is academic/non-commercial; fine while
  Sawblade stays personal. Users must fetch the weights themselves.

---

# Phase 5.1a: engine bake-off (demucs.cpp patched, Eigen or OpenBLAS, vs ONNX Runtime)

Spec: `docs/specs/phase5_1a_separator_engine.md`. Measured on 2026-10-04 with the scripts in
`spikes/separator/scripts/` (commands in `README.md`). Audio, stems, weights, `.onnx` files and build dirs are outside
the repo. **This section gives data; the recommendation is the lead's.**

## TL;DR

- **Both candidates reproduce stock Python Demucs (`apply_model`, shifts=0, split, overlap 0.25) on every stem**:
  worst null residual per engine is **-42.8 dB (a, Eigen and OpenBLAS) and -56.0 dB (b, ORT)**, both on the quietest
  synthetic stem (6s `other`); the real clip is at -49.5 dB or better (a) and -57.6 dB or better (b). Target was
  <= -40 dB, so nothing needed an investigation. Per-stem SDR on `real7` equals Python's to 0.01 dB for all three.
  (5.0's -16 to -28 dB against `shifts=0` Python came from the random shift and zero padding; both are patched out.)
- **Speed on this box, 6s model, 4 threads, 70 s loop: ORT 34.0 s per minute of audio, Python 36.7, OpenBLAS 455,
  Eigen 437.** demucs.cpp is **about 13x slower than ORT** and does not scale with threads (1 -> 4 threads: 1.0 to 1.2x;
  ORT 2.3x, Python 2.4x). OpenBLAS barely helps (-4% to +10%, depending on model and threads) because GEMM is only 13% (OpenBLAS) to 25% (Eigen) of the
  time in a sampling profile; the rest is scalar layers (transformer layer loops, group norm, tensor ops, im2col copies).
- **Memory:** ORT's default session (arena + memory-pattern planner) peaks at **4971 MB**; with both off (the driver's
  default, 10% slower than ORT's defaults) **2600 MB on the 70 s loop and 3086 MB on a 240 s song** (about 0.5% over a 3 GiB bar; the
  excess is the driver's own song-length buffers, see the memory notes). demucs.cpp is 2.0 to 2.8 GB on the 70 s loop; on a 240 s song OpenBLAS is 2671 MB and Eigen GEMM 3344 MB.
- **Apple Silicon (estimate, not measured):** (a) with Accelerate about 160-320 s/min for 6s (bar 60: **fails**);
  (b) ORT CPU EP about 25-45 s/min (**passes**, uncertainty about 1.5x either way); CoreML EP is an upside to verify on
  the Mac, with a real fp16 accuracy risk on quiet stems (proxy measurement below).
- Determinism: same engine + same threads is bit-identical for all three; 1 vs 4 threads is bit-identical for ORT and
  OpenBLAS, and differs at -124 to -132 dB for Eigen's own GEMM.
- Cancel works in both; latency is bounded by one segment's compute (0.5 to 0.99 of a segment).

## Bars (from the spec)

| Bar | (a) demucs.cpp patched | (b) ONNX Runtime |
|---|---|---|
| 6s model <= 60 s of compute per minute of audio on the Mac | **No** (estimate 160-320 s/min with Accelerate; measured 437-455 s/min here, flat in threads) | **Yes** (estimate 25-45 s/min; measured 34.0 s/min on 4 threads here) |
| Peak RSS <= 3 GB | OpenBLAS: 2.1 GB at 70 s, 2.7 GB at 240 s (yes). Eigen GEMM: 2.8 GB at 70 s, **3.3 GB at 240 s (no)** | 2600 MB at 70 s; 3086 MB at 240 s (marginal, trimmable) with the arena off; 4971 MB with ORT defaults |
| Null <= -40 dB vs stock Python, matched settings | Yes (worst -42.8 dB) | Yes (worst -56.0 dB) |

## Environment, versions, hashes

| Item | Value |
|---|---|
| Machine | Intel Xeon (family 6 model 85 stepping 7, Cascade Lake) @ 2.80 GHz, AVX2 + AVX-512 (F/BW/CD/DQ/VL/VNNI), 4 vCPU (hypervisor), 15 GB RAM, Linux 6.18, Ubuntu 24.04, g++ 13.3.0. **A different host from 5.0's 2.10 GHz Xeon: do not compare absolute 5.0 and 5.1a seconds.** Python 6s 4-thread went from 16 s/min (5.0 box) to 36.7 s/min here, so this host is about 2.3x slower for torch. |
| Builds | Release, `-march=x86-64-v3` for demucs.cpp (shippable baseline, `-O3 -DNDEBUG -freciprocal-math -fno-signed-zeros`, OpenMP); the ORT driver has no `-march` (ORT dispatches its own kernels). All Sawblade targets built with `-Wall -Wextra -Wpedantic -Werror`, zero warnings. |
| demucs.cpp | `f1206e9adeea103aef4a636b9e62297cf1f8e34e` + `patches/demucscpp-f1206e9a-sawblade.patch` (540 lines incl. header) |
| OpenBLAS | Ubuntu 24.04 apt 0.3.26+ds-1ubuntu0.1, **OpenMP variant** (`libopenblas0-openmp`, linked by path), kernels `*_SKYLAKEX` |
| ONNX Runtime | 1.30.0 official `onnxruntime-linux-x64-1.30.0.tgz`, sha256 `a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd` (git `f2c39fe2f838cf35ce7da92824f5a5e3ee6e88a7`) |
| Python reference | demucs 4.0.1, torch 2.5.1+cu124 (CPU only), numpy 2.4.6, museval 0.4.1; unpatched `apply_model(model, mix, shifts=0, split=True, overlap=0.25)`, no `--zero-pad-chunks`, no shift |
| Export | torch 2.5.1 legacy TorchScript exporter (`dynamo=False`), onnx 1.23.1, onnxruntime (Python) 1.30.0, opset 17, fixed shapes, fp32, constant folding on |
| htdemucs checkpoint | `955717e8-8726e21a.th` sha256 `8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4` (verified by `fetch_weights.sh`) |
| htdemucs_6s checkpoint | `5c90dfd2-34c22ccb.th` sha256 `34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd` (verified) |
| ggml 4s / 6s (converted, f16 on disk) | `ggml-model-htdemucs-4s-f16.bin` sha256 `72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064` (83,994,361 B); `ggml-model-htdemucs-6s-f16.bin` sha256 `09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856` (54,855,129 B) |
| ONNX core 4s / 6s (fp32) | `htdemucs-core-opset17.onnx` sha256 `79189af3c584b1a2145ae5e4182a50c0204f88b76e2829bd27e4d4a88ede427d` (174,264,732 B); `htdemucs_6s-core-opset17.onnx` sha256 `d23996ba2e9396d393e2bd53c29f1411bd33b8cf3451854ad32d746ad3d06132` (114,560,314 B). A second export of 6s gave the identical sha256 (the export is reproducible). |
| Material | Rebuilt with 5.0's scripts: `real7` (6.8 s MUSDB18-7 clip), `synth` (36 s), `loop70` (clip looped to 70 s), plus `loop240` (loop70 tiled to 240 s, memory only) |
| Dev-venv pins added | `onnx==1.23.1`, `onnxruntime==1.30.0` (verification only) |

## What was built

**Candidate (a): patch against demucs.cpp** (`spikes/separator/patches/`, applied by CMake at configure time; the patch
header lists every change):
1. Shift fixed at 0: `rand()` and the shift padding are gone (also from the V3 path, which we do not use).
2. Python context padding: each segment, including the last short one, is filled with the real neighbouring audio
   (`TensorChunk.padded`: left pad `delta // 2`, output trimmed with `center_trim`), zeros only beyond the track ends.
3. Cancel flag: `demucs_inference(model, audio, cb, const std::atomic<bool>* cancel = nullptr)`, polled between
   segments; a cancelled call returns an **empty tensor** (`dimension(0) == 0`). Driver: `--cancel-after S`.
4. No stdout/stderr from the library (model load, inference, invariant checks; the latter `abort()` silently).
5. FTZ/DAZ: set on the caller thread and, through one `#pragma omp parallel` over `omp_get_max_threads()`, on every
   OpenMP worker; restored on return. x86 MXCSR bits 15 and 6; ARM `FPCR.FZ` (compiled in, **not built or run here**).
   Checked live with gdb: all four threads of a 4-thread OpenBLAS run had `MXCSR` 0x9ff0/0x9ff8 (FTZ and DAZ set).
6. BLAS: `EIGEN_USE_BLAS` against apt OpenBLAS (`SAWBLADE_SEPARATOR_BLAS`, default OFF). **OpenMP variant of OpenBLAS**
   so its workers are libgomp threads and receive FTZ/DAZ from the same OpenMP team (the pthread variant's threads
   would not be covered by our region). `openblas_set_num_threads(N)` and `omp_set_num_threads(N)` are both set.
- Normalisation: done in the library exactly as Python (`ref = mix.mean(0)`, `(mix - ref.mean()) / ref.std()`, unbiased std,
  float32), undone afterwards. The ORT driver does the same in double accumulation then stores float32.
- Not touched: weights, thread-count semantics (N = OpenMP/Eigen/OpenBLAS threads for one call on the whole song);
  `--mode split` and `--ftz` of the 5.0 driver are removed (split is another algorithm; FTZ is always on now).

**Candidate (b): ONNX Runtime** (`scripts/export_onnx.py`, `separator_onnx.cpp`):
- Graph = the htdemucs core for one 343,980-sample segment, batch 1. Inputs `mix (1,2,343980)` and
  `mag (1,4,2048,336)` = `HTDemucs._magnitude(_spec(mix))` (complex as channels, `cac=True`); outputs `x_freq
  (1,S,4,2048,336)` and `x_time (1,S,2,343980)`, both de-normalised, before `_mask`/`_ispec`. The wrapper's forward is a
  verbatim copy of `HTDemucs.forward` from `mag` to the two outputs; with the stock `_mask`/`_ispec`/add it reproduces the
  stock forward with **max abs diff 0.0**. Nothing was taken from sevagh/demucs.onnx.
- **ONNX export blocker and workaround:** in eval + `no_grad`, `nn.MultiheadAttention` takes the fused fast path
  `aten::_native_multi_head_attention`, which has no ONNX symbolic ("Exporting the operator ... is not supported").
  Workaround: `torch.backends.mha.set_fastpath_enabled(False)` at the top of the export script (same maths, same weights).
  No other op failed. Graph ops after export: 1009 Constant, 339 Mul, 299 Reshape, 259 Add, 135 Transpose, 92 Conv,
  74 InstanceNormalization, 54 MatMul, 26 LayerNormalization, 10 Softmax, 10 Gemm, 8 ConvTranspose, 56 Erf, ... (full list
  printed by the script).
- **ORT vs torch on a real segment (script check): x_freq -87.1 dB, x_time -88.3 dB, composed output -89.2 dB (4s);
  6s: -91.9 / -87.5 / -90.0 dB.** Spec asked for <= -80 dB: met.
- Driver (our code): normalisation, segmentation with Python padding, linear-ramp overlap-add (`transition_power` 1),
  shift 0, STFT/iSTFT (n_fft 4096, hop 1024, Hann periodic, `normalized=True`, reflect padding, `_spec` crop of the last
  bin and the 2 + 2 frames, `_ispec` zero-bin + 2 + 2 frame pad, window-envelope normalisation, trim to `[pad, pad+len)`),
  `cac` mask to complex, add the time branch. **FFT: PFFFT, the project's existing pin** (4096-point real transform); no new
  library. `verify_segment.py` against torch on the first segment: **STFT magnitude -134.1 dB (max abs 3.8e-6)**; composed
  segment output -85.8 to -94.8 dB (4s).
- Session options (recorded): CPU EP only; `intra_op_num_threads = N`, `inter_op_num_threads = 1`, sequential execution,
  graph optimisation `ORT_ENABLE_ALL`, `session.set_denormal_as_zero = 1`, **CPU memory arena OFF, memory-pattern planner
  OFF** (defaults would be 4971 MB, see the ablation), `allow_spinning` left at ORT's default (on). The driver also sets
  FTZ/DAZ on the calling thread. gdb check of a 4-thread run: the caller + 3 pool threads had `MXCSR` 0x9fe0 (FTZ and DAZ
  set); one extra non-inference helper thread did not.
- Cancel flag checked between segments; progress callback = one call per segment (the demucs.cpp library calls its
  callback about 45 times per segment, so the call counts are not comparable).
- ORT's `Run` is 97% of the separation time (profile: `model_run` 6.61 s of 6.77 s on `real7`, 4 threads); our STFT,
  iSTFT and overlap-add are the rest.

## 1. Speed and memory (70 s loop, load time separate, medians, `nice`, idle box)

Seconds of compute per minute of audio = separate s / 70 x 60; RTF = separate s / 70. Load time is the engine's model
load (ORT includes graph optimisation). Individual runs are in the last column.

| engine | build | model | threads | runs | load s | separate s (70 s audio) | s per min of audio | RTF | peak RSS MB | individual s/min |
|---|---|---|---|---|---|---|---|---|---|---|
| (a) Eigen GEMM, FTZ | x86-64-v3 | 4s | 1 | 2 | 0.48 | 622.7 | 533.8 | 8.90 | 2494 | 531.7, 535.9 |
| (a) Eigen GEMM, FTZ | x86-64-v3 | 4s | 4 | 2 | 0.44 | 522.1 | 447.5 | 7.46 | 2726 | 454.2, 440.8 |
| (a) Eigen GEMM, FTZ | x86-64-v3 | 6s | 1 | 2 | 0.24 | 564.3 | 483.7 | 8.06 | 2546 | 481.1, 486.3 |
| (a) Eigen GEMM, FTZ | x86-64-v3 | 6s | 4 | 2 | 0.24 | 510.3 | 437.4 | 7.29 | 2767 | 444.4, 430.4 |
| (a) OpenBLAS, FTZ | x86-64-v3 | 4s | 1 | 2 | 0.44 | 558.4 | 478.6 | 7.98 | 2014 | 494.9, 462.4 |
| (a) OpenBLAS, FTZ | x86-64-v3 | 4s | 4 | 2 | 0.41 | 501.7 | 430.0 | 7.17 | 2018 | 423.7, 436.3 |
| (a) OpenBLAS, FTZ | x86-64-v3 | 6s | 1 | 2 | 0.23 | 539.2 | 462.2 | 7.70 | 2088 | 459.7, 464.7 |
| (a) OpenBLAS, FTZ | x86-64-v3 | 6s | 4 | 2 | 0.24 | 531.2 | 455.3 | 7.59 | 2091 | 442.5, 468.1 |
| (b) ORT CPU EP | ORT dispatch | 4s | 1 | 3 | 1.41 | 113.6 | 97.3 | 1.62 | 2654 | 99.8, 96.4, 97.3 |
| (b) ORT CPU EP | ORT dispatch | 4s | 4 | 3 | 1.38 | 40.3 | 34.5 | 0.58 | 2657 | 38.1, 33.6, 34.5 |
| (b) ORT CPU EP | ORT dispatch | 6s | 1 | 3 | 1.04 | 91.4 | 78.3 | 1.31 | 2598 | 82.2, 77.8, 78.3 |
| (b) ORT CPU EP | ORT dispatch | 6s | 4 | 3 | 1.07 | 39.7 | 34.0 | 0.57 | 2600 | 34.2, 34.0, 32.9 |
| Python torch (stock, shifts=0) | torch 2.5.1 | 4s | 1 | 3 | 0.59 | 123.8 | 106.1 | 1.77 | 2129 | 106.1, 105.6, 106.2 |
| Python torch (stock, shifts=0) | torch 2.5.1 | 4s | 4 | 3 | 0.58 | 48.7 | 41.7 | 0.70 | 1704 | 41.6, 41.7, 43.3 |
| Python torch (stock, shifts=0) | torch 2.5.1 | 6s | 1 | 3 | 0.44 | 104.7 | 89.7 | 1.50 | 2211 | 89.7, 89.7, 90.1 |
| Python torch (stock, shifts=0) | torch 2.5.1 | 6s | 4 | 3 | 0.41 | 42.9 | 36.7 | 0.61 | 1831 | 35.7, 36.7, 37.2 |

Observations (a segment is 7.8 s, stride 5.85 s, so 12 segments for 70 s):
- demucs.cpp does not scale with threads: Eigen 1 -> 4 threads 1.19x (4s) and 1.11x (6s); OpenBLAS 1.11x and 1.02x.
  The 5.0 box showed the same (1.0-1.1x for the 6s model). ORT 2.8x (4s) and 2.3x (6s); Python 2.5x and 2.4x.
- The 4s and 6s demucs.cpp models now cost about the same (FTZ removes 4s's denormal slow-down; 5.0 saw it as 2x).
- **ORT vs Python (stock torch): ORT is 1.15x faster at 1 thread (6s: 78.3 vs 89.7) and 1.08x faster at 4 threads
  (34.0 vs 36.7); roughly equal.** On the 240 s song: ORT 6s 4 threads 40.7 s/min, 1 thread 85.1 s/min; Python 6s 4 threads
  30.1 s/min (so Python wins there; the ORT figure includes the single-threaded STFT/iSTFT/overlap-add, and per-segment
  time was 17% higher than on the 70 s loop, which may be noise from this shared host).
- Song-length extrapolation (6s, 4 threads): a 4-minute song takes about 2.7 min with ORT (measured 162.7 s), 2.0 min with
  Python (measured 120.4 s), and about 29 min with demucs.cpp (437-455 s/min x 4; the two 240 s demucs.cpp runs took 33 min, while the box was also compiling).

### Peak RSS vs song length (6s, 4 threads, MB = MiB; `getrusage` maxrss)

| Engine | 70 s loop | 240 s song |
|---|---|---|
| ORT, arena + memory pattern OFF (driver default) | 2600 | 3086 |
| Python torch | 1831 | 2941 |
| (a) OpenBLAS | 2091 | 2671 |
| (a) Eigen GEMM | 2767 | 3344 |

The 240 s runs are single runs (the two demucs.cpp ones, 495-498 s/min, overlapped with a default build and ctest, so their speed is not a headline number). For ORT the song-length part is the driver's own buffers (6 stems x 2 ch x N float
accumulators = 508 MB for 240 s, the input copy, the WAV writer's interleave buffer): +486 MB from 70 s to 240 s. The
ORT working set itself is about 2.2 GB for any length. A 5.1b implementation that streams stems to disk and avoids
the extra copies has about 0.5 GB of headroom against a 3 GiB bar. Not tried: ORT `SetTerminate`, session-level
`arena.extend_strategy`, MALLOC tuning (`MALLOC_MMAP_THRESHOLD_=1 MiB` changed nothing: 2587 vs 2601 MB).

### ORT session-option ablation (6s, 70 s loop, 4 threads, 2 runs each)

| Arena | Memory pattern | s/min (runs) | Peak RSS MB |
|---|---|---|---|
| off | off (driver default) | 34.0 (34.2, 34.0, 32.9; three runs) | 2600 |
| off | on | 33.8 (32.7, 34.9) | 2776 |
| on | off | 26.1 (26.4, 25.8) | 3611 |
| on | on (ORT defaults) | 30.9 (31.4, 30.3) | 4971 |
| off, off, `allow_spinning=0` | | 37.2 (36.7, 37.7) | 2602 |

The arena alone is worth about 23% in speed and costs 1 GB; the memory-pattern planner makes things bigger and, with the
arena, slower. With the bar at 3 GB the arena stays off for the headline numbers; 5.1b can revisit.

## 2. Null test vs stock Python (shifts=0), 4 threads

Residual = 20 log10(rms(engine - python) / rms(python)) in dB, then max abs diff, then cross-correlation lag in samples
(0 everywhere). Python reference = `run_python.py` with no flags. Target <= -40 dB on every stem.

#### Null test `real7` 4s: residual dB / max abs diff / xcorr lag

| stem | eigen | blas | onnx |
|---|---|---|---|
| drums | -69.3 / 4.5e-04 / 0 | -69.3 / 4.5e-04 / 0 | -92.0 / 1.9e-05 / 0 |
| bass | -65.2 / 2.8e-04 / 0 | -65.2 / 2.8e-04 / 0 | -85.8 / 2.4e-05 / 0 |
| other | -67.7 / 1.5e-04 / 0 | -67.7 / 1.5e-04 / 0 | -88.0 / 1.7e-05 / 0 |
| vocals | -71.9 / 2.6e-04 / 0 | -71.9 / 2.6e-04 / 0 | -94.8 / 1.6e-05 / 0 |


#### Null test `real7` 6s: residual dB / max abs diff / xcorr lag

| stem | eigen | blas | onnx |
|---|---|---|---|
| drums | -66.8 / 7.1e-04 / 0 | -66.8 / 7.1e-04 / 0 | -95.4 / 4.8e-05 / 0 |
| bass | -62.4 / 5.8e-04 / 0 | -62.4 / 5.8e-04 / 0 | -89.4 / 2.3e-05 / 0 |
| other | -49.5 / 2.1e-06 / 0 | -49.5 / 2.1e-06 / 0 | -65.1 / 9.1e-08 / 0 |
| vocals | -72.0 / 4.2e-04 / 0 | -72.0 / 4.2e-04 / 0 | -96.3 / 2.3e-05 / 0 |
| guitar | -64.2 / 6.4e-04 / 0 | -64.2 / 6.4e-04 / 0 | -89.9 / 2.8e-05 / 0 |
| piano | -49.5 / 5.7e-06 / 0 | -49.5 / 5.7e-06 / 0 | -57.6 / 9.1e-07 / 0 |


#### Null test `synth` 4s: residual dB / max abs diff / xcorr lag

| stem | eigen | blas | onnx |
|---|---|---|---|
| drums | -51.0 / 2.9e-02 / 0 | -51.0 / 2.9e-02 / 0 | -81.9 / 1.0e-04 / 0 |
| bass | -62.7 / 1.1e-03 / 0 | -62.7 / 1.1e-03 / 0 | -81.4 / 1.4e-05 / 0 |
| other | -62.1 / 1.8e-02 / 0 | -62.1 / 1.8e-02 / 0 | -89.5 / 9.9e-05 / 0 |
| vocals | -53.5 / 9.8e-04 / 0 | -53.5 / 9.8e-04 / 0 | -79.4 / 2.5e-05 / 0 |


#### Null test `synth` 6s: residual dB / max abs diff / xcorr lag

| stem | eigen | blas | onnx |
|---|---|---|---|
| drums | -50.2 / 2.5e-02 / 0 | -50.2 / 2.5e-02 / 0 | -81.0 / 8.0e-05 / 0 |
| bass | -51.2 / 1.7e-03 / 0 | -51.2 / 1.7e-03 / 0 | -61.0 / 4.0e-04 / 0 |
| other | -42.8 / 1.9e-02 / 0 | -42.8 / 1.9e-02 / 0 | -56.0 / 8.5e-04 / 0 |
| vocals | -56.3 / 9.4e-04 / 0 | -56.3 / 9.4e-04 / 0 | -79.6 / 1.8e-05 / 0 |
| guitar | -70.1 / 3.3e-04 / 0 | -70.1 / 3.3e-04 / 0 | -86.7 / 3.4e-06 / 0 |
| piano | -64.7 / 2.2e-04 / 0 | -64.7 / 2.2e-04 / 0 | -79.8 / 7.2e-06 / 0 |

- Every stem meets the target, so none needed an investigation. The largest residuals are on the quietest stems (`other`
  and `piano` of the 6s model, synthetic `drums`), where a fixed absolute error is a larger fraction of a small signal.
  The worst case for (a) is synthetic 6s `other` at -42.8 dB (2.8 dB margin); for (b) -56.0 dB.
- (b) is 8-31 dB closer to Python than (a) on every stem (smallest gap: the quiet real-clip `piano`). Not investigated further: 5.0 showed that float32 vs float64
  in Python itself is -127 dB, so the gap is implementation-level differences inside demucs.cpp (its own STFT in float,
  `-freciprocal-math`, its GELU/GroupNorm implementations), not weight precision (both load the same f16-stored weights).
- Eigen and OpenBLAS give bit-different but practically identical results (the same residual to 0.1 dB).

## 3. SDR (museval BSSEval v4 median / global, dB), real clip

#### SDR (dB), dataset `real7`, model htdemucs 4s (museval median / global)

| stem group | Python shifts=0 | eigen | blas | onnx |
|---|---|---|---|---|
| drums | 15.03 / 14.55 | 15.03 / 14.55 | 15.03 / 14.55 | 15.03 / 14.55 |
| bass | 13.01 / 12.61 | 13.01 / 12.61 | 13.01 / 12.61 | 13.01 / 12.60 |
| other | 9.32 / 8.95 | 9.32 / 8.95 | 9.32 / 8.95 | 9.32 / 8.95 |
| vocals | 14.39 / 14.64 | 14.39 / 14.64 | 14.39 / 14.64 | 14.39 / 14.64 |


#### SDR (dB), dataset `real7`, model htdemucs 6s (museval median / global)

| stem group | Python shifts=0 | eigen | blas | onnx |
|---|---|---|---|---|
| drums | 15.00 / 14.48 | 15.00 / 14.48 | 15.00 / 14.48 | 15.00 / 14.48 |
| bass | 11.52 / 11.11 | 11.52 / 11.11 | 11.52 / 11.11 | 11.52 / 11.11 |
| other | 9.11 / 8.80 | 9.11 / 8.80 | 9.11 / 8.80 | 9.11 / 8.80 |
| vocals | 14.67 / 14.65 | 14.67 / 14.65 | 14.67 / 14.65 | 14.67 / 14.65 |


All three engines equal Python's SDR to <= 0.01 dB on `real7` (spec: within 0.1 dB). For the 6s model the `other` row is
`other + guitar + piano` (the dataset's `other` stem lumps them; there is no guitar-only truth, see the 5.0 gaps).
Synthetic-material tables (weak evidence: the model does not recognise the synthetic bass; engines still agree with Python to <= 0.02 dB):

#### SDR (dB), dataset `synth`, model htdemucs 4s (museval median / global)

| stem group | Python shifts=0 | eigen | blas | onnx |
|---|---|---|---|---|
| drums | 9.72 / 9.78 | 9.72 / 9.77 | 9.72 / 9.77 | 9.72 / 9.78 |
| bass | 0.01 / 0.01 | 0.01 / 0.01 | 0.01 / 0.01 | 0.01 / 0.01 |
| vocals | 3.46 / 3.57 | 3.45 / 3.56 | 3.45 / 3.56 | 3.46 / 3.57 |
| other | 8.95 / 8.95 | 8.95 / 8.95 | 8.95 / 8.95 | 8.95 / 8.95 |


#### SDR (dB), dataset `synth`, model htdemucs 6s (museval median / global)

| stem group | Python shifts=0 | eigen | blas | onnx |
|---|---|---|---|---|
| drums | 10.40 / 10.42 | 10.41 / 10.42 | 10.41 / 10.42 | 10.40 / 10.42 |
| bass | 1.82 / 1.04 | 1.83 / 1.06 | 1.83 / 1.06 | 1.83 / 1.05 |
| vocals | 4.30 / 3.95 | 4.30 / 3.94 | 4.30 / 3.94 | 4.30 / 3.95 |
| other | 2.83 / 3.13 | 2.85 / 3.15 | 2.85 / 3.15 | 2.84 / 3.14 |


## 4. Determinism and thread count

`run_determinism.sh` on `real7`, both models, files compared with SHA-256 and, if different, the residual.

| Engine | Same input, same threads, twice (4 threads) | 1 thread vs 4 threads |
|---|---|---|
| (a) Eigen GEMM | **bit-identical**, all 10 stems | not identical: residual -124 to -132 dB (4s); 6s -124 to -131 dB, except `other` -77.9 and `piano` -85.4 dB (max abs diff <= 7.3e-7) |
| (a) OpenBLAS | **bit-identical**, all 10 stems | **bit-identical**, all 10 stems |
| (b) ORT | **bit-identical**, all 10 stems | **bit-identical**, all 10 stems |

## 5. Cancel latency (70 s loop, flag set 5.0 s after inference starts)

Time from setting the flag to `demucs_inference` / the driver loop returning. "One segment" is the median
separation time of the 70 s timing runs divided by 12. Cancel is polled between segments, so the worst case is one
segment; all rows are inside that.

| engine | model | threads | one segment of compute (s) | stop latency after the flag was set (s) | latency / segment |
|---|---|---|---|---|---|
| (a) Eigen | 4s | 1 | 51.9 | 46.6 | 0.90 |
| (a) Eigen | 4s | 4 | 43.5 | 40.2 | 0.92 |
| (a) Eigen | 6s | 1 | 47.0 | 45.6 | 0.97 |
| (a) Eigen | 6s | 4 | 42.5 | 38.9 | 0.91 |
| (a) OpenBLAS | 4s | 1 | 46.5 | 40.3 | 0.87 |
| (a) OpenBLAS | 4s | 4 | 41.8 | 39.0 | 0.93 |
| (a) OpenBLAS | 6s | 1 | 44.9 | 38.4 | 0.86 |
| (a) OpenBLAS | 6s | 4 | 44.3 | 35.7 | 0.81 |
| (b) ORT | 4s | 1 | 9.5 | 5.7 | 0.60 |
| (b) ORT | 4s | 4 | 3.4 | 3.3 | 0.99 |
| (b) ORT | 6s | 1 | 7.6 | 4.0 | 0.52 |
| (b) ORT | 6s | 4 | 3.3 | 2.3 | 0.69 |

## 6. The library writes nothing (acceptance 7)

`separator_spike --quiet` prints nothing itself. Run on `real7` with the 6s model, 4 threads, stdout and stderr redirected
to files: **stdout 0 bytes, stderr 0 bytes for both the Eigen and OpenBLAS builds** (model load, a 2-segment separation, and
the stem writing all silent; exit 0). Before the patch the library printed "apply model w/ split, offset ..." lines and
model-load messages.

## 7. Where the time goes (for the Apple estimate)

**(a), gdb sampling on `real7`, 6s model, 1 thread** (`gdb_sample.sh`, 70-80 samples each, so about +-5 percentage points):

| Share of samples | Eigen GEMM (80 samples) | OpenBLAS (70 samples) |
|---|---|---|
| GEMM/GEMV kernels (`gebp`, `sgemm_kernel_SKYLAKEX`, ...) | 25% | 13% |
| memset/memcpy (zero-initialised result matrices, copies) | 8% | 10% |
| everything else (scalar layers, tensor ops, im2col, norms) | 68% | 77% |
| first `demucscpp::` frame: `common_encoder_layer` (the transformer layer) | 45% | 31% |
| `conv2d` | 22% | 29% |
| `group_norm` | 12% | 13% |

**(b), ORT profiler (`--profile`), 6s, `real7`** (per-node kernel time by op type, 2 segments):

| Op type | 1 thread | 4 threads |
|---|---|---|
| Conv | 22.2% | 18.8% |
| FusedMatMul (attention + linear) | 17.8% | 15.0% |
| ConvTranspose | 10.8% | 13.3% |
| Gemm | 10.3% | 10.9% |
| **GEMM-class subtotal** | **61%** | **58%** |
| Mul / Add / Split / Transpose / Slice | 23% | 22% |
| InstanceNormalization | 4.7% | 10.1% |
| Softmax, Gelu, Sigmoid, other | the rest | the rest |

FLOPs per 7.8 s segment (torch `FlopCounterMode`; counts conv and linear, **not** the attention score matmuls, so a
lower bound): 192.5 GFLOP (6s), 249.8 GFLOP (4s). Attention scores add roughly 20-30% (2688 x 2688 x 8 heads x 64).
ORT's measured GEMM-class time of about 4.7 s per segment (1 thread, 61% of 7.6 s) is about 40-55 GFLOP/s sustained
on one core.

## 8. Apple Silicon estimate (reasoning, **not a measurement**: no Mac here)

Target: the **6s model, seconds of compute per minute of audio**, on the user's Apple Silicon Mac (model not known;
assume 4 to 8 performance cores). Bar: 60 s/min.

Measured facts used (this box, 6s, 70 s loop): ORT 78.3 s/min at 1 thread and 34.0 at 4 threads, 61% / 58% of node time in
GEMM-class ops; demucs.cpp 462-484 s/min at 1 thread and 437-455 at 4 threads, GEMM 13-25% of samples; the ORT thread
scaling (2.3x at 4 threads) implies a serial/non-scaling fraction of about 25% (Amdahl).

Published or sourced facts:
- Apple P-cores issue up to four 128-bit NEON instructions per cycle on Firestorm (Dougall Johnson's microbenchmarks,
  cited by corsix/amx `README.md`: "issue up to four per cycle on Firestorm"): 4 x 4 lanes x 2 = 32 FP32 FLOP/cycle/core;
  at 3.2 (M1) to about 4.4 GHz (M4) that is about 100-140 GFLOP/s peak per core. This Xeon core with 2 x 512-bit FMA:
  64 FLOP/cycle at about 2.5 GHz = 160 GFLOP/s peak (assumes two AVX-512 FMA ports, as on server Cascade Lake; not
  verified in the VM). The clock figures are from memory, not re-fetched.
- AMX is an undocumented Apple matrix coprocessor reachable only through Apple's own libraries (Accelerate); the same
  README says so ("neither documented nor supported by Apple"). I do not have a sourced AMX FLOP/s figure and do not
  use one: for (a) I only use the bound "GEMM time goes to about zero".
- ANE peak (hollance/neural-engine `docs/supported-devices.md`, built from Apple's announcements, retrieved 2026-10-04):
  M1/M1 Pro/M1 Max 11 TOPS, M2 family 15.8, M3/M3 Pro/M3 Max 15.8 (that page), M4 family 38 TOPS, Ultra variants double.
  (apple.com, wikipedia and onnxruntime.ai are blocked from this box; the ORT/CoreML EP docs were read from the
  `microsoft/onnxruntime` gh-pages source on GitHub.)
- ORT's CPU EP on arm64 uses its own MLAS NEON kernels (`SgemmKernelNeon.S`, `SconvKernelNeon.S` in
  `cmake/onnxruntime_mlas.cmake`, v1.30.0); an optional KleidiAI build flag exists. It does **not** call Accelerate/AMX.

**Assumptions** (each is a guess with large uncertainty): (A1) GEMM-class kernels run at about 0.6-0.9x the speed of this
Xeon core (from the peak ratio above at equal efficiency); (A2) memory/elementwise/normalisation ops and scalar code run
1.3-2.0x faster per core than here (higher clock, IPC and bandwidth); (A3) thread scaling follows this box's Amdahl fit:
2.3x at 4 threads, about 2.7x at 6, about 2.9x at 8.

| Candidate | Arithmetic | Estimate (6s) |
|---|---|---|
| (a) demucs.cpp + Accelerate (`EIGEN_USE_BLAS`) + NEON | 1 thread here 462-484 s/min; make the 13-25% GEMM share nearly free; scale the rest by A2 (/1.3 to /2.0); threads add <= 10% (measured 1.02-1.14x) | **about 160-320 s/min** (4-minute song: 11-21 min). Even the optimistic end is 2.7x over the bar. Not fixable by BLAS: it is the scalar layers. |
| (b) ORT CPU EP (NEON MLAS) | 1 thread: 78.3 x (0.61 / (0.6..0.9) + 0.39 / (1.3..2.0)) = 69-103 s/min; then 4 P-cores /2.3, 8 P-cores /2.9 | **about 30-45 s/min on 4 P-cores, 24-36 on 8 P-cores; call it 25-45 s/min.** Passes the bar by 1.3-2.4x. Uncertainty about 1.5x either way. |
| (b) ORT CoreML EP (MLProgram; ANE and/or GPU, fp16) | Compute floor: about 0.25 TFLOP per segment x 10.3 segments/min = 2.6 TFLOP per minute of audio; at 3-19 TFLOP/s effective (25-50% of the ANE peaks above, a guess) = 0.15-0.9 s/min. Real time is dominated by everything else (below). | **5-20 s/min if the graph is almost entirely accepted; between that and the CPU EP number if it is split into many partitions.** Unmeasured; treat as an upside, not the plan. |

**CoreML EP caveats** (what 5.1b has to verify):
- *Op coverage.* I dumped the graph ORT hands to an EP (level-1 "basic" optimisations: 1451 nodes for 6s, constant folding
  removes the 1009 Constant and the shape-arithmetic ops) and compared its op types against the `MLProgram` table in the
  CoreML EP docs (`coreml_coverage.py`): **1422 of 1451 nodes (98%) have an op builder**; the exceptions are `Gather` x26
  and `Pad` x3 (`op_builder_factory.cc` in v1.30.0 does list Gather and Pad builders, so the docs table may be
  conservative). The table's constraints (constant weights, 4D only, ...) are not checked, so 98% is an upper bound.
  Counting runs of consecutive supported nodes in topological order gives at most 30 CoreML/CPU partitions; each
  boundary copies tensors (x_freq is 66 MB for 6s).
- *ANE limits.* The time branch works on a 343,980-sample axis; Core ML's ANE has tensor-size limits (I recall about 16k
  per dimension; **not verified here**), so under `MLComputeUnits=ALL` those ops probably run on GPU or CPU. The
  frequency-branch transformer works on 8 x 336 = 2688 tokens.
- *fp16 vs the null target.* A proxy measured here: run the same core under torch CPU autocast (conv/linear/matmul in
  fp16, norms fp32) on real-clip segments (`fp16_check.py`). Composed-output residual vs fp32: **drums -62.2, bass -61.3,
  vocals -63.5, guitar -53.8 dB, but `other` -5.3 dB and `piano` -11.8 dB** (the two quietest stems; the bar is -40 dB per
  stem). The ANE's fp16 kernels differ from torch's CPU ones (and may accumulate in fp32), so this is only a warning:
  quiet stems may fail a per-stem relative -40 dB target in fp16. `AllowLowPrecisionAccumulationOnGPU` should stay 0.
- *First-run compile.* Core ML compiles the model on first load (no measurement here); the EP's `ModelCacheDirectory`
  caches the compiled result.
- *How 5.1b verifies on the Mac:* official macOS arm64 ORT 1.30.0 tarball (ships the CoreML EP); register the EP with
  `ModelFormat=MLProgram`, `MLComputeUnits` in {`CPUOnly`, `CPUAndGPU`, `CPUAndNeuralEngine`, `ALL`},
  `RequireStaticInputShapes=1`, `ProfileComputePlan=1` (logs the device each op is dispatched to), `ModelCacheDirectory`;
  count partitions in the verbose log; time first and second load; run `eval.py` (null <= -40 dB, per stem) and
  `verify_segment.py`; measure RSS; compare to the CPU EP on the same Mac.

## 9. Problems hit and decisions

1. **`FetchContent` patch step is not idempotent** (re-configure re-ran `git apply` and failed): wrapped in
   `patches/apply_patch.cmake` (skips when the reverse patch already applies).
2. **`find_library` and a pre-set empty cache variable**: the OpenBLAS auto-detect silently produced an empty path and
   link errors; uses a separate internal variable now.
3. **`libopenblas-dev` only pulls the pthread variant**; the OpenMP variant is installed separately and linked by path
   (CMake adds the RUNPATH, `ldd` confirms `openblas-openmp/libopenblas.so.0`).
4. **5.0's "denormal" finding re-read.** The 5.0 clip fits in one (zero-padded) segment after the shift padding, so "102 s ->
   49 s with `--ftz`" was per single segment; with the real two-segment run the FTZ'd time here is ~51 s per segment, consistent with that (not re-measured
   without FTZ). FTZ is confirmed active (gdb `MXCSR`) and does not change the headline: demucs.cpp is slow for other reasons.
5. **ORT default memory** is 4971 MB (arena + planner). The arena alone is worth 25% in speed for +1 GB (ablation above).
6. **ORT 1.30.0** is newer than anything I had seen; it is what PyPI and GitHub list as latest stable on 2026-10-04, and
   the Python wheel and the C++ tarball are the same version.
7. The CPU of this box differs from 5.0's (see Environment). Relative numbers are the finding.
8. `real7` is 6.8 s, shorter than a segment, so its two-segment runs (offsets 0 and 257,985) exercise the padding paths on
   both segments; the 36 s `synth` and the 70 s loop cover the rest.

## 10. Notes for 5.1b (proposals, not done)

- Engine data favours (b) by 13x in speed and 8-31 dB in null; (a) misses the speed bar by 2.7x or more even on Apple
  with Accelerate. The lead decides.
- Ship/download both `.onnx` files as derivative weights (never committed): 174 MB (4s) and 115 MB (6s) fp32, vs the 84/55 MB
  f16 checkpoints. Conversion needs torch once (`export_onnx.py`, reproducible sha256); a C++ converter is not needed if
  the `sawblade-models fetch` route from `match/` is used.
- Streaming the stems to disk (and no whole-song float copies) would put a 4-minute song comfortably under 3 GiB.
- `Ort::RunOptions::SetTerminate()` would cut cancel latency below one segment (we poll between segments only).
- Intra-op threads = P-cores minus one; leave `allow_spinning` at its default (disabling it cost 9%).
- The 6s `guitar`-stem quality on distorted guitar is still unmeasured (needs the user's multitracks).
- Add `--provider coreml` to `separator_onnx` on the Mac and run the verification list above.
