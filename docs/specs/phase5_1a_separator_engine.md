# Phase 5.1a: separator engine bake-off (demucs.cpp + BLAS vs ONNX Runtime)

Source: `docs/specs/phase5_0_separator_spike_REPORT.md`, "Recommendation for 5.1" §1.
Raw 5.0 data and the existing spike: `spikes/separator/` (README.md, RESULTS.md).

## Why
5.0 showed demucs.cpp matches Python Demucs (null −62..−72 dB with matched settings) but is
~10x slower than torch CPU and does not scale with threads. Before 5.1b embeds an engine in
`core/`, measure two candidates on the same material and pick **one**.

This is still a **spike**: everything lives under `spikes/separator/`, behind OFF-by-default
options. No `core/`, `plugin/`, `cli/`, `match/` or test changes. The normal
configure/build/ctest must be byte-for-byte unaffected (no new fetches with options OFF).

## Shared settings (both candidates, and the Python reference)
- Models: `htdemucs` (4s) and `htdemucs_6s` (6s), same checkpoints and sha256 as 5.0.
- Segment 7.8 s (htdemucs `segment`, 343980 samples at 44.1 kHz), overlap 0.25, Python's
  linear-ramp (`transition_power=1`) overlap-add weight, **shift = 0** (no random shift),
  per-song normalisation exactly as Python `apply_model`/`separate` does
  (`ref = mix.mean(0); (mix − ref.mean()) / ref.std()`, undone afterwards). State in
  RESULTS.md where each candidate does this.
- **Context padding as Python**: each segment, including the last short one, is padded with
  the real neighbouring audio (`TensorChunk.padded` semantics), zeros only beyond the track
  ends.
- The **Python reference** for the null test is demucs 4.0.1 `apply_model(model, mix,
  shifts=0, split=True, overlap=0.25)` on the same float32 input — i.e. the stock Python with
  no patches. With shift 0 and Python padding both candidates must null against stock Python.
- FTZ/DAZ (x86: MXCSR bits 15 and 6; ARM: FPCR.FZ) set on **every** thread that runs
  inference: the caller thread, every OpenMP worker (e.g. via a `#pragma omp parallel` once
  at start), every OpenBLAS thread if OpenBLAS threads are used (or use the OpenBLAS
  OpenMP variant / single-threaded OpenBLAS + our own threading — state which and why), and
  ORT intra-op threads (ORT session option `session.set_denormal_as_zero` = "1").

## Candidate (a): demucs.cpp, patched
Base: demucs.cpp `f1206e9a` as in 5.0. Patches are applied by CMake to the fetched tree
(`PATCH_COMMAND` with a `.patch` file in `spikes/separator/patches/`, or a vendored copy of
the changed files — either way the change is a reviewable diff in our repo). Required:
1. **Shift fixed at 0** (remove the `rand()` shift; no global RNG use left).
2. **Python-compatible context padding** for every segment (see shared settings).
3. **Cancel flag**: `demucs_inference` takes (or can see) a `const std::atomic<bool>*`;
   checked at least between segments; on cancel it returns promptly with an empty result
   and a way for the caller to know it was cancelled. The driver gets `--cancel-after S`
   (sets the flag from another thread after S seconds) and prints how long it took to stop.
4. **No stdout/stderr** from the library during load or inference (progress only via the
   callback). The driver may print.
5. **FTZ/DAZ** per shared settings.
6. **BLAS backend**: `EIGEN_USE_BLAS` against apt `libopenblas-dev` (Ubuntu 24.04,
   0.3.26), new spike option `SAWBLADE_SEPARATOR_BLAS` (default OFF). Measure with Eigen
   GEMM (BLAS OFF) and with OpenBLAS, so the BLAS effect is isolated.
- Build flags for the headline numbers: `-march=x86-64-v3` (shippable baseline). Native
  numbers optional.
- Threads: the "N threads" setting = OpenMP/Eigen/OpenBLAS threads for one
  `demucs_inference` call on the whole song. **No split mode** (`demucs_mt`) in the headline
  table.

## Candidate (b): ONNX Runtime
1. **Export** (`spikes/separator/scripts/export_onnx.py`, in the venv; add `onnx` and
   `onnxruntime` Python wheels pinned to exact versions for verification only):
   - Export the htdemucs core network for **one fixed-length segment**, with STFT/iSTFT moved
     **out** of the graph: inputs = time-domain segment `(1, 2, 343980)` and its
     complex-as-channels spectrogram exactly as `HTDemucs._spec` + `_magnitude` produce it;
     outputs = the time-branch output and the frequency-branch output before `_ispec`.
     (sevagh/demucs.onnx did this; you may read it for reference, record its licence/commit
     if you copy anything.) Opset ≥ 17, fixed shapes, fp32. One `.onnx` per model, written
     to the cache dir outside the repo; record the sha256 of each and the export
     torch/onnx versions.
   - Verify in Python: ORT output vs torch for the same segment ≤ −80 dB residual.
2. **C++ driver side** (our own code, `-Wall -Wextra -Wpedantic -Werror`, zero warnings):
   - Normalisation, segmentation with Python padding, overlap-add weights, shift 0.
   - STFT/iSTFT matching `demucs.spec.spectro` / `ispectro` (n_fft 4096, hop 1024,
     `normalized=True`, reflect padding and the `_spec`/`_ispec` crop/pad logic of
     HTDemucs, Hann window). Use a pinned FFT: the project's existing FFT if there is one,
     else Eigen's unsupported FFT (kissfft, already available with Eigen 3.4) or pocketfft
     pinned by FetchContent. Record which.
   - Mask/output combination as HTDemucs does (`_mask`, `cac` → complex, add time branch).
   - CPU EP; `intra_op_num_threads` = N, `inter_op_num_threads` = 1, graph optimisation
     level ALL, denormals as zero, no arena spinning surprises (record session options).
   - Same `--cancel-after` behaviour (checked between segments) and progress callback.
3. **ONNX Runtime binary**: official `onnxruntime-linux-x64-<ver>.tgz` from GitHub releases,
   one exact version (the latest stable that the official macOS build also ships with the
   CoreML EP), pinned by URL + sha256 in CMake (FetchContent `URL_HASH`). New option
   `SAWBLADE_BUILD_SEPARATOR_ONNX` (default OFF; only meaningful with the spike option ON).
   Record version and licence (MIT, from the tarball's LICENSE) and its bundled
   third-party notices file in `docs/THIRD_PARTY.md`.

## Driver
Either extend `separator_spike` with `--engine demucscpp|onnx` or add a second executable
`separator_onnx`; keep CLI flags the same (`--model`, `--in`, `--out-dir`, `--threads`,
`--cancel-after`). Both print load time, separation time, audio duration, RTF, and peak RSS
(`getrusage`). Inputs 44.1 kHz stereo only (as 5.0).

## Measurements (this box, under `nice`)
Material: rebuild 5.0's material with the existing scripts (real7 clip, synth 36 s, loop70).
1. **Speed/memory**, 70 s loop, each config ≥ 2 runs, median, load time separate:

   | engine | build | model | threads |
   |---|---|---|---|
   | (a) Eigen GEMM, FTZ | v3 | 4s, 6s | 1, 4 |
   | (a) OpenBLAS, FTZ | v3 | 4s, 6s | 1, 4 |
   | (b) ORT CPU EP | (ORT's own dispatch) | 4s, 6s | 1, 4 |
   | Python torch (stock, shifts=0) | | 4s, 6s | 1, 4 |

   Report s of compute per minute of audio, RTF, peak RSS MB.
2. **Null test** vs stock Python (shift 0) for both candidates, both models, real7 and synth:
   per-stem residual dB, max abs diff, xcorr lag. Target ≤ −40 dB on every stem. Any stem
   above −40 dB must be investigated and explained (not just recorded).
3. **Cancel**: time from flag set to return, for each candidate (≤ one segment's compute).
4. **SDR**: per-stem museval SDR on real7 for each candidate (sanity; should equal Python's
   within 0.1 dB).

## Apple Silicon estimate (no Mac here; reasoning, not measurement)
A section in RESULTS.md estimating s/min for 6s on the user's Apple Silicon Mac for:
(a) with Accelerate (`EIGEN_USE_BLAS` + Accelerate's BLAS) and NEON, and (b) ORT with the
CPU EP and with the CoreML EP. Use measured facts from this box (fraction of time in GEMM vs
other ops — profile with gdb sampling or ORT's profiler), published per-core/AMX/ANE numbers
with sources, and state the assumptions and the uncertainty. Note CoreML EP caveats
(op coverage for this graph, fp16 on ANE vs null target, first-run compile time) and how
5.1b would verify them on the Mac.

## Bars (for the recommendation)
- 6s model on the Mac: ≤ 60 s of compute per minute of audio.
- Peak RSS ≤ 3 GB.
- Null ≤ −40 dB vs Python with matched settings.
Decision rule (from the 5.0 report): pick (a) if it meets the bars (no runtime dependency,
small code); otherwise (b). RESULTS.md gives the data; the lead writes the recommendation.

## Deliverables
- `spikes/separator/`: CMake changes, patch(es), driver(s), `export_onnx.py`, scripts updated
  (`run_matrix.sh` / `run_timing.sh` / `eval.py` cover both candidates), README updated with
  exact commands, RESULTS.md gets a new **"Phase 5.1a"** section (environment, versions,
  hashes, tables, Apple estimate, problems hit). Keep the 5.0 content.
- `docs/THIRD_PARTY.md`: OpenBLAS (apt, version, BSD-3-Clause), ONNX Runtime (version, URL,
  sha256, MIT), FFT lib if new, onnx/onnxruntime Python wheels (dev only), demucs.onnx if
  anything was taken from it.
- `.gitignore`: add `*.onnx`, `*.ort`, `*.tgz` under `spikes/separator/`.

## Acceptance
1. Default `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
   && ctest --test-dir build` passes as before; configure log shows no demucs.cpp /
   onnxruntime fetch.
2. Spike ON (+ BLAS ON, + ONNX ON) builds with zero warnings on our targets.
3. Both engines separate real7 and loop70 with both models and write all stems.
4. Two separations of the same input with the same engine/threads are bit-identical
   (determinism); state whether 1 vs 4 threads is bit-identical or the residual.
5. Null ≤ −40 dB vs stock Python (shift 0) on every stem, or a justified explanation.
6. `--cancel-after` stops each engine within one segment's compute.
7. Library code of (a) writes nothing to stdout/stderr (show with a run redirecting the
   driver's own prints, or a test that captures fds).
8. RESULTS.md has every table above with measured numbers, plus the Apple estimate.
9. No audio, weights, ONNX files or tarballs in git. Build dirs cleaned up afterwards
   (disk is limited).

## Out of scope
`core/` integration, plugin, caching, model download UX, guitar ground-truth measurement
(needs the user's multitracks), CoreML EP build (no Mac). Propose in RESULTS.md "Notes for 5.1b".
