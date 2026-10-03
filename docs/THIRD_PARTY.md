# Third-party dependencies

All dependencies are fetched with CMake FetchContent (`cmake/Dependencies.cmake`) and pinned to
exact commits. Headers are exposed as `SYSTEM`; third-party code is compiled without our
`-Werror` warning set.

| Name | Version / pin | License | Use |
|------|---------------|---------|-----|
| NeuralAmpModelerCore | `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` (sdatkinson) | MIT | `NAM/*.cpp` built into static lib `nam_core` with `NAM_SAMPLE_FLOAT` and `NAM_ENABLE_A2_FAST`; its tools and submodules are not built or fetched. Used by `NamBlock` (T2). |
| Eigen | 3.4.0, `3147391d946bb4b6c68edd901f2add6ac1f31f8c` (gitlab.com/libeigen/eigen) | MPL-2.0 | Header-only dependency of NAM core. We use the gitlab tag, not NAM core's Eigen submodule (which is not initialised). |
| nlohmann/json | v3.11.3, `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` | MIT | Preset parsing (T3) and NAM model JSON. NAM core includes it as `"json.hpp"`, so `single_include/nlohmann` is also on the include path. NAM core vendors 3.12.0; it builds fine against 3.11.3. |
| Catch2 | v3.7.1, `9827c148c397289df17d3967692619a198032a24` | BSL-1.0 | Unit/golden tests. Tests only. |
| PFFFT (marton78/pffft) | `aa16fd3db58de4ba5dae8b0438440bb9da46b6fa` (v1.1.0-58) | BSD-style (FFTPACK/Pommier; see `LICENSE.txt` in the repo) | FFT for the partitioned convolver (T2). Float-only C sources (SIMD auto-selected by the library) `pffft.c` + `pffft_common.c` only. |
| SHA-256 | Written for Sawblade (`core/src/sha256.cpp`, FIPS 180-4; nothing vendored) | n/a (project code) | Capture `sha256` verification (T3). Verified against the FIPS test vectors in `tests/test_preset.cpp`. |
| dr_wav (mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` | Public domain or MIT-0 (choice) | WAV read/write (`core/src/wav_io.cpp`; implementation TU `core/src/dr_wav_impl.cpp`). |

## Test fixtures

| File | Source | License | Use |
|------|--------|---------|-----|
| `tests/fixtures/nam/wavenet.nam`, `lstm.nam` | NeuralAmpModelerCore `example_models/` @ `0b3d3c9…` (unmodified copies), Copyright (c) 2023 Steven Atkinson | MIT (`tests/fixtures/nam/LICENSE-NeuralAmpModelerCore`, `NOTICE`) | NamBlock tests (T2) |
| `tests/fixtures/nam/linear_*.nam` | Hand-written for Sawblade | MIT | Linear-architecture NamBlock tests |
| `tests/fixtures/nam/linear_neg1_at_23.nam`, `linear_delay_300.nam` | Hand-written for Sawblade | MIT | Auto-align tests (T3): a Linear model with a single -1 at tap 23, and +1 at tap 300 |
| `tests/fixtures/ir/impulse.wav` | Generated (64-sample float32 unit impulse) | MIT | Identity cab for chain tests (T3) |
| `tests/fixtures/presets/*.json` | Hand-written for Sawblade | MIT | Preset parser / chain tests (T3); `schema_example.json` is the example from `docs/PRESET_SCHEMA.md` |
