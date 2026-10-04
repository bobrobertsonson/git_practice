# Mac 1 fix: snap host parameters to the 1e-4 state grid

Branch: `claude/sawblade-mac-build`. Approved by the user (2026-10-03): "round the parameter".

## Problem

`plugin:Processor: starts as a zero-latency pass-through (Init preset)` fails on macOS arm64:
every output sample is the input × 1.00000012f.

Cause (diagnosed and reproduced):
- `levelA`/`levelB` (range -24..+12, default 0) are stored by APVTS as
  `convertTo0to1(0) = 0.666666687f`.
- `convertFrom0to1` computes `start + (end - start) * p`. Apple clang (`-ffp-contract=on`) fuses
  that into an FMA, giving **+7.15e-7 dB** instead of 0. x86-64 without FMA gives exactly 0.
- `SawbladeProcessor::readParams()` (`plugin/src/PluginProcessor.cpp:69`) passes that raw value to
  `Engine::setParams` → `Chain::setLiveParams`. That sees a change from the baseline 0 and ramps
  both path levels to `1.00000012f`.

The same class of mismatch exists after any preset load. The engine's baseline is built from the
double-precision clamped preset (`clampedToParams`, `PresetMapping.cpp`), while the audio thread
reads the float-quantised (and possibly FMA-perturbed) parameter. So a preset value such as
`levelDb: -3.27419` starts a tiny spurious ramp on every platform.

## Task

Make the values the audio thread reads and the values the engine baseline is built from **identical**,
by snapping both to the grid the saved state already uses (`round4`: 1e-4 units, `PluginProcessor.cpp:30`).

1. `readParams()`: return `round4(float param value)` for every parameter. Keep it real-time safe
   (no allocation, locks or I/O; `std::round` is fine).
2. The engine baseline for a loaded preset / restored state / Init must use the same snapped
   values. That is, the preset after `clampedToParams` (or wherever the baseline is formed) holds
   exactly what `readParams()` will return once the parameters have been written. Choose the
   smallest correct place (e.g. snap in `clampedToParams` via the float the parameter will store,
   then `round4`). Document the rule in one comment and in `docs/PLUGIN.md` ("Parameters (APVTS)
   and the preset").
3. The core DSP (`core/`) is unchanged: no changes to `Chain`, ramps, gains or any `process()` math.
4. Do not change compiler flags. Do not loosen the existing Init test.

## Acceptance tests

- The existing `Processor: starts as a zero-latency pass-through (Init preset)` passes on this Mac
  (bit-exact `y == x`).
- New test: load a preset whose parameter values are **not** float- or 1e-4-exact (e.g.
  `levelDb` -3.27419 on both paths, blend 0.333333, input gain 1.23456 dB, a post-EQ gain
  -2.71828), process a few blocks, and prove that no live-parameter change was applied after
  the load. Either expose a counter or check `readParams()` against the engine baseline, or compare
  against the bit-exact output of the same snapped preset rendered without parameter updates. The
  test must **fail on the pre-fix code on this Mac** (say in the report how you checked).
- The existing state round trip stays byte-stable, and the allocation and lock tests pass.
- Full `ctest`: everything passes except the Linux-only LockGuard skip.
