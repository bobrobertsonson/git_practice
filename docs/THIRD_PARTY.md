# Third-party dependencies

All dependencies are fetched with CMake FetchContent (`cmake/Dependencies.cmake`) and pinned to
exact commits. Headers are exposed as `SYSTEM`; third-party code is compiled without our
`-Werror` warning set.

> ### LICENSING GATE: JUCE 8 (plugin only)
>
> **JUCE 8 is dual-licensed: AGPLv3 or a commercial JUCE licence. Sawblade is a commercial
> product, so a JUCE commercial licence is required before any plugin binary (VST3, AU,
> Standalone, AAX) is distributed or shipped to anyone outside the project.** Development builds
> for internal use are fine. JUCE is only fetched and built with `-DSAWBLADE_BUILD_PLUGIN=ON`
> (default OFF); the core library, `tonerender` and the Catch2 suite contain no JUCE code, and
> `core/` must stay JUCE-free. Under the AGPLv3 option the whole plugin would have to be
> distributed under the AGPLv3 with its source. Decision owner: lead/user (see `docs/specs/phase2_plugin.md`).

| Name | Version / pin | License | Use |
|------|---------------|---------|-----|
| NeuralAmpModelerCore | `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` (sdatkinson) | MIT | `NAM/*.cpp` built into static lib `nam_core` with `NAM_SAMPLE_FLOAT` and `NAM_ENABLE_A2_FAST`; its tools and submodules are not built or fetched. Used by `NamBlock` (T2). |
| Eigen | 3.4.0, `3147391d946bb4b6c68edd901f2add6ac1f31f8c` (gitlab.com/libeigen/eigen) | MPL-2.0 | Header-only dependency of NAM core. We use the gitlab tag, not NAM core's Eigen submodule (which is not initialised). |
| nlohmann/json | v3.11.3, `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` | MIT | Preset parsing (T3) and NAM model JSON. NAM core includes it as `"json.hpp"`, so `single_include/nlohmann` is also on the include path. NAM core vendors 3.12.0; it builds fine against 3.11.3. |
| Catch2 | v3.7.1, `9827c148c397289df17d3967692619a198032a24` | BSL-1.0 | Unit/golden tests. Tests only. |
| PFFFT (marton78/pffft) | `aa16fd3db58de4ba5dae8b0438440bb9da46b6fa` (v1.1.0-58) | BSD-style (FFTPACK/Pommier; see `LICENSE.txt` in the repo) | FFT for the partitioned convolver (T2). Float-only C sources (SIMD auto-selected by the library) `pffft.c` + `pffft_common.c` only. |
| JUCE | 8.0.15, `91ad83ae34a81e0833b1a2b0866f54846370ae53` (release tag `8.0.15`, github.com/juce-framework/JUCE; shallow fetch of the pinned commit) | **AGPLv3 or commercial (every module we link declares `AGPLv3/Commercial`). A commercial licence is required before distribution; see the gate above.** Bundled third-party code inside JUCE (VST3 SDK, FreeType/HarfBuzz/SheenBidi, LV2 libraries, ...) has its own licences and must be audited before any redistribution. | Plugin only (`SAWBLADE_BUILD_PLUGIN=ON`): AudioProcessor, parameters (APVTS), editor, VST3 / AU / Standalone wrappers. Modules used: juce_audio_processors, juce_gui_basics, juce_data_structures, juce_events, juce_audio_utils (Standalone), juce_audio_plugin_client, and their dependencies. Not used by `core/`, `cli/` or `match/`. |
| SHA-256 | Written for Sawblade (`core/src/sha256.cpp`, FIPS 180-4; nothing vendored) | n/a (project code) | Capture `sha256` verification (T3). Verified against the FIPS test vectors in `tests/test_preset.cpp`. |
| Oversampler4x / AdaaClipper / pedal.hm / pedal.ts | Written for Sawblade (`core/src/oversampler.cpp`, `adaa_clipper.cpp`, `pedal_*.cpp`; nothing vendored) | n/a (project code) | Modeled pedal blocks (phase 7). Kaiser-windowed half-band FIR design (Kaiser 1974 / Oppenheim-Schafer) and second-order antiderivative anti-aliasing after Parker, Esqueda, Bilbao, "Reducing the aliasing of nonlinear waveshaping using continuous-time convolution" (DAFx 2016): published algorithms, reimplemented, no third-party code or new dependency. |
| dr_wav (mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` | Public domain or MIT-0 (choice) | WAV read/write (`core/src/wav_io.cpp`; implementation TU `core/src/dr_wav_impl.cpp`). |
| dr_flac (mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` (same checkout as dr_wav) | Public domain or MIT-0 (choice) | FLAC decoding for stem files (`readAudioFile`, `core/src/wav_io.cpp`; implementation TU `core/src/dr_flac_impl.cpp`, built in the third-party `sawblade_dr_wav` library with warnings off). |
| pybind11 | v3.0.4, `d03662f0984f652b60e7ddce53d3868002275197` (pybind/pybind11) | BSD-3-Clause | Python bindings (`bindings/python`, module `sawblade_core`). Fetched only with `-DSAWBLADE_BUILD_PYTHON=ON` (off by default); headers exposed as `SYSTEM`. |

## Development tools (external, never linked or distributed)

| Name | Version | License | Use |
|------|---------|---------|-----|
| pluginval (Tracktion) | v1.0.4 | GPL-3.0 (check the repository's `LICENSE`) | Dev tool, not linked or shipped. Optional VST3 validation: build it yourself and pass `-DSAWBLADE_PLUGINVAL_EXECUTABLE=...` (see `docs/PLUGIN.md`). Not fetched by our CMake. |
| Pillow + NumPy | Pillow 12.3.0, NumPy 2.4.6 (what the committed icon PNGs were rendered with; not pinned in the repo) | HPND (Pillow), BSD-3-Clause (NumPy) | Dev tools for `design/render/app_icon.py` (the app icon) and its `--check` ctest. Not linked, not shipped; the test is skipped without them. |

## Python (match/)

Pinned in `match/pyproject.toml`; installed with pip into `match/.venv` (not vendored).

| Name | Version | License | Use |
|------|---------|---------|-----|
| httpx | 0.28.1 | BSD-3-Clause | TONE3000 API client |
| numpy | 2.4.6 | BSD-3-Clause (wheel also bundles 0BSD, MIT, Zlib, CC0-1.0 components) | Quality filter percentiles; matching engine |
| scipy | 1.17.1 | BSD-3-Clause | Matching engine (later phases) |
| soundfile | 0.14.0 | BSD-3-Clause (links libsndfile, LGPL-2.1+, dynamically via the wheel) | WAV I/O |
| matplotlib | 3.11.2 | Matplotlib License (PSF-based, BSD-compatible) | Tone-check plots (later phases) |
| pytest | 9.1.1 | MIT | Tests only |
| respx | 0.23.1 | BSD-3-Clause | Tests only (fake HTTP API) |

### Optional: `match[separation]` (calibration method 1, `sawblade-calibrate`)

Install only on a developer machine (`pip install -e 'match[separation]' -c match/constraints-separation.txt`). Nothing
here is bundled with, or required by, the plugin.

| Name | Version | License | Use |
|------|---------|---------|-----|
| demucs | 4.0.1 | MIT (facebookresearch/demucs) | Stem separation of the reference mixes (htdemucs "other" stem ~ guitars). Pulls dora-search, julius, lameenc, openunmix, einops, omegaconf, ... (see the constraints file). |
| torch | 2.5.1 | BSD-3-Clause (wheel bundles NVIDIA CUDA libraries under their own licenses; run on CPU) | Demucs inference |
| torchaudio | 2.5.1 | BSD-2-Clause | Demucs dependency |
| htdemucs weights (`955717e8-8726e21a.th`) | htdemucs | Released with the Demucs repository (MIT); a separate weights licence is not stated - confirm before any redistribution | Downloaded at first use by `demucs.pretrained.get_model` from `https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/` into `~/.cache/torch/hub/checkpoints/` (calibration). `sawblade-models fetch` caches the official checkpoints itself, under `<models dir>/checkpoints/hub/checkpoints/` (sha256-pinned), and exports the ONNX files from them. Not committed, not redistributed. |

### Optional: `match[export]` (NAM export, `sawblade-export`)

Developer-machine tool, never bundled with the plugin: `pip install -e 'match[export]' -c match/constraints-export.txt`
(the constraints file pins the transitive set; torch stays at the separation pin 2.5.1, CPU only).

| Name | Version | License | Use |
|------|---------|---------|-----|
| neural-amp-modeler (sdatkinson) | 0.13.0 | MIT | The NAM trainer: `Dataset`, `LightningModule`, WaveNet export (`.nam`). Driven programmatically on Sawblade's own training signal (see `match/sawblade_match/export/train.py`); its standard `v3_0_0.wav` input is **not** used or fetched. `tkinter` (imported by `nam.train.core` for a GUI dialog) is stubbed on headless machines. |
| pytorch-lightning | 2.6.1 | Apache-2.0 | Training loop (NAM dependency) |
| librosa, wavio, pydantic, tensorboard, transformers, sounddevice, scikit-learn, numba | see constraints file | ISC / BSD / MIT / Apache-2.0 | Imported or required by NAM 0.13.0 (most are not exercised by Sawblade's path; `sounddevice` is not imported) |

The training signal is generated by Sawblade (`match/sawblade_match/export/signal.py`; no third-party audio, nothing to licence or fetch).
Exported `.nam` files and IRs are derived from TONE3000 captures: for the user's personal use only; sharing needs permission from the creators and TONE3000. Exports containing `cc-by-nc*` captures are additionally marked NON-COMMERCIAL (they carry attribution + that note in their metadata).

Transitive dependencies are not listed; audit them before any binary redistribution.

## Test fixtures

| File | Source | License | Use |
|------|--------|---------|-----|
| `tests/fixtures/nam/wavenet.nam`, `lstm.nam` | NeuralAmpModelerCore `example_models/` @ `0b3d3c9…` (unmodified copies), Copyright (c) 2023 Steven Atkinson | MIT (`tests/fixtures/nam/LICENSE-NeuralAmpModelerCore`, `NOTICE`) | NamBlock tests (T2) |
| `tests/fixtures/nam/linear_*.nam` | Hand-written for Sawblade | MIT | Linear-architecture NamBlock tests |
| `tests/fixtures/nam/linear_neg1_at_23.nam`, `linear_delay_300.nam` | Hand-written for Sawblade | MIT | Auto-align tests (T3): a Linear model with a single -1 at tap 23, and +1 at tap 300 |
| `tests/fixtures/ir/impulse.wav` | Generated (64-sample float32 unit impulse) | MIT | Identity cab for chain tests (T3) |
| `tests/fixtures/presets/*.json` | Hand-written for Sawblade | MIT | Preset parser / chain tests (T3); `schema_example.json` is the example from `docs/PRESET_SCHEMA.md` |
| `tests/fixtures/di_riff.wav`, `tests/fixtures/ir/ir_a.wav`, `ir_b.wav` | Generated by `tests/tools/make_fixtures.cpp` (`scripts/regen_fixtures.sh`) | MIT | Golden renders and CLI tests (T4) |
| `tests/fixtures/presets/golden_*.json`, `tests/golden/*.wav` | Hand-written presets and renders of them (`SAWBLADE_UPDATE_GOLDEN=1`) | MIT | Golden tests (T4) |

## Separator spike (`SAWBLADE_BUILD_SEPARATOR_SPIKE`)

Phase 5.0 spike (`spikes/separator/`, spec `docs/specs/phase5_0_separator_spike.md`). Off by default; with
the option OFF nothing below is fetched, built or linked. Nothing here is in `core/`, `cli/`, the plugin or
the tests. Measured results and the "what does this mean for 5.1" notes are in `spikes/separator/RESULTS.md`.

### Code

| Name | Version / pin | License | Use |
|------|---------------|---------|-----|
| demucs.cpp (sevagh/demucs.cpp) | `f1206e9adeea103aef4a636b9e62297cf1f8e34e` (2024-12-01) | MIT, "Copyright (c) 2023 Sevag H" (read from the repo's `LICENSE` file) | C++ port of Demucs v3/v4 inference (Eigen, CPU). `src/*.cpp` built into `demucs_cpp_lib` (our `-w`, `-O3`, optional `-march=native`, OpenMP); `cli-apps/threaded_inference.hpp` included by the spike driver. FetchContent with `GIT_SUBMODULES ""`: its submodules (eigen, demucs, libnyquist, googletest) are not fetched. Its CLI apps, tests and libnyquist are not built. |
| Eigen | 3.4.0 (the project's existing pin, see above) | MPL-2.0 | demucs.cpp's own submodule is Eigen `dd8c71e62852b2fe429edb6682ac91fd1c578a26` (a post-3.4 commit). We use the project's 3.4.0 instead; it builds and runs. See RESULTS.md for the null test against Python, which is the check that this substitution is harmless. |
| Demucs (facebookresearch/demucs) | `demucs==4.0.1` from PyPI (dev venv only: reference runs, ggml conversion script needs `demucs.pretrained.get_model`) | MIT, "Copyright (c) Meta Platforms, Inc. and affiliates." (`LICENSE` in the repo; README: "Demucs is released under the MIT license as found in the LICENSE file."). The repo README states it is no longer maintained by its author (fork: adefossez/demucs). | Python reference + the model architecture that demucs.cpp re-implements. Never shipped. |
| torch 2.5.1, torchaudio 2.5.1, numpy, scipy, soundfile, museval 0.4.1, musdb 0.4.3, stempeg | pinned by `match/constraints-separation.txt` where listed there; museval / musdb / stempeg are evaluation-only | torch BSD-3-Clause (wheel bundles NVIDIA CUDA libs under their own licences; CPU only here), others BSD / MIT (museval and musdb: MIT per the sigsep repos' `LICENSE`) | Spike venv (`~/.venvs/sawblade-demucs`, outside the repo). Never shipped. |

### Phase 5.1a additions (engine bake-off; `spikes/separator/`, spec `docs/specs/phase5_1a_separator_engine.md`)

All behind OFF-by-default options (`SAWBLADE_SEPARATOR_BLAS`, `SAWBLADE_BUILD_SEPARATOR_ONNX`, both only
meaningful with `SAWBLADE_BUILD_SEPARATOR_SPIKE=ON`). With the spike option OFF none of this is fetched, built or linked.

| Name | Version / pin | License | Use |
|------|---------------|---------|-----|
| demucs.cpp patch (`spikes/separator/patches/demucscpp-f1206e9a-sawblade.patch`) | against `f1206e9a` (above) | Our changes to MIT code; the patched files keep the upstream MIT notice | Shift fixed 0, Python-compatible context padding, cancel flag, FTZ/DAZ, no stdout/stderr. Applied by CMake (`patches/apply_patch.cmake`) to the FetchContent checkout, so the diff is reviewable in our repo. |
| OpenBLAS | Ubuntu 24.04 apt `libopenblas-dev` / `libopenblas0-openmp` / `libopenblas0-pthread` `0.3.26+ds-1ubuntu0.1` | BSD-3-Clause ("Copyright 2011-2023 The OpenBLAS Project", plus UT Austin 2009-2010 and others; from the package's `/usr/share/doc/libopenblas0-pthread/copyright`) | `EIGEN_USE_BLAS` GEMM backend for candidate (a), spike option `SAWBLADE_SEPARATOR_BLAS`. A system package, dynamically linked, not bundled. The OpenMP variant is linked by explicit path so its workers are libgomp threads. macOS would use Accelerate instead (system framework). |
| ONNX Runtime (microsoft/onnxruntime) | `1.30.0` (git `f2c39fe2f838cf35ce7da92824f5a5e3ee6e88a7`), official tarball `https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-linux-x64-1.30.0.tgz`, sha256 `a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd` (11,306,877 bytes), pinned by `URL_HASH` in `spikes/separator/CMakeLists.txt` | MIT, "Copyright (c) Microsoft Corporation" (the tarball's `LICENSE`). The tarball also ships `ThirdPartyNotices.txt` (6,369 lines: protobuf, Eigen, Microsoft GSL, HowardHinnant/date, google/re2 and others, each under its own licence); it must accompany any redistributed binary. | Candidate (b): CPU execution provider, `libonnxruntime.so` loaded by `separator_onnx`. The official macOS arm64 tarball of the same version is the one that ships the CoreML EP (not built or tested here). |
| PFFFT | the project's existing pin (above) | BSD-style | The STFT/iSTFT of `separator_onnx` (4096-point real FFT). No new FFT library. |
| `onnx` 1.23.1, `onnxruntime` 1.30.0 (Python wheels) | exact pins in the venv (`pip install onnx==1.23.1 onnxruntime==1.30.0`) | `onnx`: Apache-2.0 (`License-Expression` in its wheel metadata). `onnxruntime`: MIT (wheel metadata). | Export checking and ORT-vs-torch verification in `scripts/export_onnx.py`. Dev venv only, never shipped. |
| sevagh/demucs.onnx | not used | n/a | Nothing was read or copied from it; `scripts/export_onnx.py` is our own wrapper around demucs 4.0.1's `HTDemucs.forward`. |

The exported `*.onnx` files (spike: `~/.cache/sawblade/separator/onnx/`; product: the models dir, `~/.local/share/sawblade/models/` or `~/Library/Application Support/Sawblade/models/`, written by `sawblade-models fetch`) are derivative weights: same treatment as the
ggml files (never committed, never redistributed; `.gitignore` covers `*.onnx`, `*.ort`, `*.tgz`).

### Model weights (downloaded, never committed, never redistributed)

| File | Source | Licence statement found |
|------|--------|--------------------------|
| `htdemucs` = `955717e8-8726e21a.th` (84,141,911 bytes, sha256 `8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4`) | `https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th` (listed in the repo's `demucs/remote/files.txt`) | **No separate weights licence is stated.** The repo `LICENSE` is MIT and the README says "Demucs is released under the MIT license as found in the LICENSE file" (`README.md`, section "License"). The README, `docs/training.md` and the checkpoint host say nothing else about the weights (looked in: `README.md`, `LICENSE`, `docs/training.md`, `demucs/remote/*.yaml`). Whether MIT covers the trained weights is therefore an inference, not a statement. |
| `htdemucs_6s` = `5c90dfd2-34c22ccb.th` (54,996,327 bytes, sha256 `34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd`) | `https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/5c90dfd2-34c22ccb.th` (`demucs/remote/htdemucs_6s.yaml`: `models: ['5c90dfd2']`) | Same as above: not stated separately. README calls it "an experimental 6 sources model, that adds a `guitar` and `piano` source. Quick testing seems to show okay quality for `guitar`, but a lot of bleeding and artifacts for the `piano` source." |

Training data (README, quoted): htdemucs "has been trained on the MUSDB HQ dataset + an extra training dataset
of 800 songs"; model list: "`htdemucs`: first version of Hybrid Transformer Demucs. Trained on MusDB + 800
songs. Default model"; "`htdemucs_6s`: 6 sources version of `htdemucs`, with `piano` and `guitar` being added
as sources" (what data supplied the guitar and piano stems is **not stated** in the README or the docs we
read). MUSDB18 / MUSDB18-HQ is distributed on Zenodo on request and, per the SigSep dataset page
(`datasets/musdb.html`, retrieved via the `sigsep/sigsep.github.io` repo), "the tracks can only be used for
academic purposes"; individual track licences (CC BY-NC-SA for the MedleyDB tracks, etc.) are in SigSep's
`content/datasets/assets/tracklist.csv`. The 800 extra songs are not described further in the repo.

Constraint for Sawblade: Sawblade is personal and non-commercial (CLAUDE.md), so using these weights for the
user's own play-along and matching reference is consistent with every statement above. Because the weights'
own licence is not stated and they were trained on non-commercial / academic-only data, **a commercial or
redistributed product would need this re-cleared** (and the 6-stem model's guitar/piano data source is
unknown). The weights are therefore always fetched by the user from the official host (as
`fetch_weights.sh` does), never bundled in a repo, installer or preset, and any feature built on them should
be documented as depending on a non-redistributable download. The converted ggml files are derivative
weights and get the same treatment.

### Test material (never committed)

| Name | Terms |
|------|-------|
| "Music Delta - 80s Rock" 6.8 s MUSDB18-7 sample clip (from `sigsep/sigsep-mus-eval` at `2716132b2f4125d4174b99d01fb5ed2e9de17adb`, `tests/data/MUSDB18-7-SAMPLE/train/`) | The museval repo is MIT; that does not cover the audio. SigSep's `tracklist.csv`: source MedleyDB, licence **CC BY-NC-SA** (MedleyDB, Bittner et al.). MUSDB18 overall: academic use only. Personal evaluation only; not redistributed, not committed. |
| Synthetic 36 s mixture (`scripts/make_material.py`) | Generated locally by our own script; no third-party audio. |

## On-device separation (phase 5.1b, `SAWBLADE_WITH_SEPARATOR`)

| Name | Version | License | Use |
|------|---------|---------|-----|
| ONNX Runtime (microsoft/onnxruntime) | `1.30.0`, official tarballs pinned by `URL_HASH` in `cmake/Dependencies.cmake`: Linux x64 `onnxruntime-linux-x64-1.30.0.tgz` sha256 `a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd`; macOS arm64 `onnxruntime-osx-arm64-1.30.0.tgz` sha256 `6ebb5062a934537c352937821f9fe9718e7de1a2db1122a93dd363ffd53a7012` (no universal2 build of 1.30.0 exists; Intel Macs are not supported by the separator) | MIT. The tarball's `ThirdPartyNotices.txt` (protobuf, Eigen, GSL, re2, ...) must accompany any redistributed plugin that bundles the shared library. | CPU execution provider, shared library copied next to the plugin binary / into `Contents/Frameworks`. Fetched at configure time only when `SAWBLADE_WITH_SEPARATOR=ON` (default ON with the plugin). |
| dr_mp3 (mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` (same checkout as dr_wav) | Public domain or MIT-0 (choice) | MP3 decoding (`readAudioFile`; implementation TU `core/src/dr_mp3_impl.cpp`, third-party library with warnings off). |
| PFFFT | the existing pin | BSD-style | STFT / iSTFT in `core/src/separator.cpp`. |
| htdemucs / htdemucs_6s ONNX | exported on the user's machine by `sawblade-models fetch` into the models dir; pinned sha256 `79189af3...427d` (htdemucs) and `d23996ba...6132` (htdemucs_6s) | Weights licence: **none stated** (repository is MIT; trained on MUSDB18-HQ, academic use only), as the 5.0/5.1a spike found. Fine for personal non-commercial use; confirm before any redistribution. | Never committed, never bundled, never redistributed. |

The stem cache (`<data dir>/stems/`) holds the user's own separated songs as 32-bit float WAV; it is never committed.
Test audio for the separator tests is generated at test time (the mp3 fixture with the system `ffmpeg`, skipped when absent).
