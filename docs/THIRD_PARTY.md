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
| dr_wav (mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` | Public domain or MIT-0 (choice) | WAV read/write (`core/src/wav_io.cpp`; implementation TU `core/src/dr_wav_impl.cpp`). |

## Development tools (external, never linked or distributed)

| Name | Version | License | Use |
|------|---------|---------|-----|
| pluginval (Tracktion) | v1.0.4 | GPL-3.0 (check the repository's `LICENSE`) | Optional VST3 validation: build it yourself and pass `-DSAWBLADE_PLUGINVAL_EXECUTABLE=...` (see `docs/PLUGIN.md`). Not fetched by our CMake. |

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
