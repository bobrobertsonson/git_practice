# Sawblade

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

## Render with `tonerender`

```
./build/cli/tonerender --preset presets/chainsaw_body.json --in my_di.wav --out out.wav --report out.json
```

```
tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256] [--report R.json] [--normalize-peak dBFS]
```

- Input: mono WAV (16/24/32-bit PCM or float; stereo uses the left channel, with a warning).
  Models must match the input's sample rate (NAM models are usually 48 kHz); IRs are resampled.
- Output: float32 mono WAV at the input rate, same length as the input, advanced by the chain's
  reported processing latency (`latencySamples`). The auto/manual alignment delay is part of
  the tone and stays in the audio; it is reported as `alignDelay`.
- Report (`--report`): preset name, rate, block size, latency per path and total,
  compensation and alignment delays, the resolved alignment (`delaySamplesB`, `invertB`,
  `peakCorrelation`), `liveCompatible`, export exactness, input/output peak and RMS (dBFS),
  render time and `realTimeFactor` (render time / audio duration; below 1 is faster than real
  time), warnings, and TONE3000 attribution (title, creator, license) for every capture that
  has a `source`.
- Exit codes: `0` ok, `2` usage error, `3` preset error (invalid preset, including values only
  invalid at the render rate, such as an EQ band at or above 0.49 x fs or a NAM sample-rate
  mismatch), `4` I/O or model load error (missing file, unreadable WAV, bad `.nam`, hash mismatch).
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
