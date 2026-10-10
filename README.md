# Sawblade

[![CI](https://github.com/bobrobertsonson/git_practice/actions/workflows/ci.yml/badge.svg?branch=claude%2Fsawblade-plugin-setup-7k0b8q)](https://github.com/bobrobertsonson/git_practice/actions/workflows/ci.yml?query=branch%3Aclaude%2Fsawblade-plugin-setup-7k0b8q)

Sawblade is a guitar plugin project (AU/VST3 first, AAX later) that builds **blended high-gain
chains** from [TONE3000](https://www.tone3000.com) NAM captures, matches them to a reference
song, and exports the result as a trainable NAM model for live loader pedals. The north-star
tone is Gatecreeper-style: an HM-2-style "chainsaw" path blended with a thick, tight high-gain
"body" path (`docs/TONE_TARGETS.md`).

Phase 1 (this repository state) is the JUCE-free C++20 core library plus `tonerender`, an offline
command-line renderer: DI in, tone out, driven by the same preset the plugin will use.

```
DI -> input gain -> gate (keyed on DI) -> split
   Path A "Saw":  pre-EQ -> [blocks: pedal NAM -> amp NAM] -> path EQ -> level
   Path B "Body": pre-EQ -> [blocks: boost NAM -> amp NAM] -> path EQ -> level
-> latency compensation -> auto polarity + delay align -> blend
-> cab IR(s) (shared, or per-path) -> post EQ -> bus comp -> output gain
```

## Layout

| Path | Contents |
|---|---|
| `core/` | DSP library `sawblade_core` (namespace `sawblade`, no JUCE): EQ, gate, NAM block, convolver, chain, preset model, offline renderer |
| `cli/` | `tonerender` |
| `plugin/` | JUCE plugin (optional, `SAWBLADE_BUILD_PLUGIN`): engine, processor, parameters, editor, headless tests |
| `tests/` | Catch2 tests, fixtures (`tests/fixtures`), golden renders (`tests/golden`), fixture generator (`tests/tools`) |
| `presets/` | Starter factory presets (captures are not in the repo, see `presets/README.md`) |
| `docs/` | Preset schema (`PRESET_SCHEMA.md`), specs, third-party licenses (`THIRD_PARTY.md`) |

## Build

Requires CMake >= 3.24, Ninja, a C++20 compiler and network access for the first configure
(dependencies are pinned and fetched with FetchContent).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Sawblade targets build with `-Wall -Wextra -Wpedantic -Werror`; third-party code does not.
Pass `-DSAWBLADE_BUILD_TESTS=OFF` to skip the tests.

## Test

```
ctest --test-dir build --output-on-failure
```

This runs the unit tests, the allocation-counting real-time safety tests, the golden renders
and the `tonerender` CLI tests (which run the built binary).

## Plugin (optional, JUCE 8)

The VST3 / Standalone plugin (AU on macOS) is built with `-DSAWBLADE_BUILD_PLUGIN=ON` (default OFF;
fetches JUCE). **JUCE 8 is AGPLv3 or commercial: a JUCE commercial licence is required before the
plugin is distributed** (`docs/THIRD_PARTY.md`). Build, threading model, latency accounting, the
real-time resampler and the headless tests are described in `docs/PLUGIN.md`.

```
cmake -S . -B build-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON
cmake --build build-plugin && ctest --test-dir build-plugin --output-on-failure
```

## Render with `tonerender`

```
# runnable on a fresh checkout (in-repo fixtures):
./build/cli/tonerender --preset tests/fixtures/presets/golden_shared.json \
    --in tests/fixtures/di_riff.wav --out out.wav --report out.json

# factory presets need their TONE3000 captures first (see presets/README.md and match/README.md):
./build/cli/tonerender --preset presets/chainsaw_body.json --in my_di.wav --out out.wav --report out.json
```

```
tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256] [--report R.json] [--normalize-peak dBFS]
           [--render-rate auto|HZ] [--out-rate input|render]
```

- Input: mono WAV (16/24/32-bit PCM or float; stereo uses the left channel, with a warning).
  `--render-rate auto` (default) renders at the NAM models' training rate (usually 48 kHz; all
  non-bypassed NAM blocks must agree, else exit 3 naming them; with none, the input rate). The
  input is resampled to it with a Kaiser-windowed sinc (passband flat within 0.05 dB to 20 kHz at
  44.1 <-> 48 kHz, stopband >= 90 dB, linear phase, no delay) and, unless `--out-rate render`,
  back to the input rate. Signals are treated as zero outside their extent; a DI that starts or
  ends mid-note gets the filter's step response at the edges. Equal rates involve no resampling. IRs are resampled at load.
- Output: float32 mono WAV at the input rate, same length as the input, advanced by the chain's
  reported processing latency (`latencySamples`). The auto/manual alignment delay is part of
  the tone and stays in the audio; it is reported as `alignDelay`.
- Report (`--report`): preset name, `inputRate`/`renderRate`/`outputRate` (latencies are in samples
  at the render rate), block size, latency per path and total,
  compensation and alignment delays, the resolved alignment (`delaySamplesB`, `invertB`,
  `peakCorrelation`), `liveCompatible`, export exactness, input/output peak and RMS (dBFS),
  render time and `realTimeFactor` (render time / audio duration; below 1 is faster than real
  time), warnings, and TONE3000 attribution (title, creator, license) for every capture that
  has a `source`.
- Exit codes: `0` ok, `2` usage error, `3` preset error (invalid preset, including values only
  invalid at the render rate, such as an EQ band at or above 0.49 x fs or a NAM sample-rate
  mismatch with a forced `--render-rate`), `4` I/O or model load error (missing file, unreadable WAV, bad `.nam`, hash mismatch).
  Errors go to stderr.

Rendering is deterministic: the same preset and input give bit-identical output, independent of
`--block` within float tolerance.

## Golden renders and fixtures

`tests/golden/*.wav` are renders of `tests/fixtures/presets/golden_*.json` on
`tests/fixtures/di_riff.wav`, using only in-repo fixtures. The golden tests (and the CLI tests)
require a max absolute difference <= 1e-4 and equal length.

After an intentional DSP change, regenerate the goldens and review the diff:

```
SAWBLADE_UPDATE_GOLDEN=1 ./build/tests/sawblade_tests "[golden]"
```

The audio fixtures (`di_riff.wav`, `ir/ir_a.wav`, `ir/ir_b.wav`) are produced by a deterministic
generator (`tests/tools/make_fixtures.cpp`); regenerate them with

```
scripts/regen_fixtures.sh            # fixtures only
scripts/regen_fixtures.sh --goldens  # fixtures, then rewrite the goldens
```

## Presets

A preset is one versioned JSON document (`docs/PRESET_SCHEMA.md`); plugin state *is* the preset.
Unknown keys are rejected. Every capture carries its TONE3000 license and creator.

## Licenses

Sawblade's third-party dependencies and test fixtures and their licenses are listed in
`docs/THIRD_PARTY.md`.
