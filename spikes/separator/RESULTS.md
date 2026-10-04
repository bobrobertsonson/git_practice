# Separator spike results (phase 5.0)

Measured on the dev box on 2026-10-04. Everything here was produced by the scripts in
`spikes/separator/scripts/`; audio, stems and weights are outside the repo.

## TL;DR

- demucs.cpp (pinned `f1206e9a`) **reproduces Python Demucs**: with Python forced to the same settings the
  residual is **-48 to -72 dB on every stem except one low-level stem at -38 dB** (see the null section). With
  the spec's `shifts=0` Python the residual is only about -16 to -28 dB (worse on low-level stems) because of two
  behaviours of demucs.cpp that differ from Python (below). Neither costs much SDR: on the real clip C++ and
  Python agree to within 0.3 dB (0.03 dB when the shift is matched).
- It is **slow and does not scale with cores** through its library API: 4 cores, `-march=native`
  (AVX-512): 261 s of compute per minute of audio on 1 thread, 241 s at 4 OpenMP threads (RTF 4.0). Its own
  song-splitting `demucs_mt` path gets 85 s/min on 4 cores but is a different, slightly worse algorithm
  (0 to 1.6 dB lower SDR) and uses 6 GB RAM. Python/torch CPU does 20 s/min (4 threads) and 64 s/min (1 thread).
  A portable (no `-march=native`) build is 1.5 to 3.7x slower again (4-source model: 978 s/min on 1 thread).
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
The remaining weak cell, `synth` 6s `other` at -38 dB, is a low-energy stem (the model got it wrong by 3.4 dB
SDR); max abs diff 8e-3. Not investigated further.

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

Unexplained: the portable 1-thread 4-source run (978 s/min) is 3.7x slower than native while the 6-source one
is 1.5x slower; both reps agree. Not investigated.

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
