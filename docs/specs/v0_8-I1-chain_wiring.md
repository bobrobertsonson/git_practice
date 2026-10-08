# v0.8 I1 — calibrated chain wiring (core)

Parent: `docs/specs/v0_8-input_calibration.md` (Task B). Builds on B1 (`core/include/sawblade/calibration.h`, accepted
run 266). Audit table: `docs/specs/v0_8-input_calibration_REPORT.md` A3 (row numbers below refer to it).
Owner: dsp-engineer. Reviewer: reviewer. Scope: `core/`, `tests/`, and `cli/` only where the render report is written.
Plugin, preset schema, matcher and export are I2–I4. Do not touch them.

## Decisions (lead)

1. **Opt-in at the core level for now.** `Chain` gets a calibration setting, off by default. With it off, every
   existing test and golden must be bit-identical. I4 decides the preset default and the migration of old presets.
   The reason: matched presets were tuned without calibration. Switching it on silently would change their sound.
2. **Calibration gain is in addition to intent.** With calibration on, a NAM block's input gain becomes
   `planned gainInDb + block.inputGainDb`. The block's own `inputGainDb` (preset/matcher, row 4) and amp GAIN
   (row 8) stay as deliberate offsets on top. INPUT (row 1) stays the user's offset.
3. **Hops between NAM blocks are planned, not loudness-based.** With calibration on, for a block that feeds another
   NAM block in the same path:
   - `normalizeLoudness` (row 6) is not applied;
   - the capture-swap make-up (row 7) is not applied;
   - the next block's planned gain carries the hop.

   `outputGainDb` (row 5) stays as an intent offset. Only the **last** NAM block of a path keeps `normalizeLoudness`
   and swap make-up, so swapping an amp changes tone and feel but not monitoring level (output side).
4. **Gain ladder (row 9):** each rung is its own capture, with its own planned gain from its own metadata. A rung swap
   uses the rung's levels.
5. **Modelled DSP pedals (row 11)** are `LevelKind::Neutral`. They model gain relative to their input, and their LEVEL
   knob is a deliberate user offset. `NominalOutput` stays unused until a block needs it. Document this in the block
   registry.
6. **Metadata source:** NAM's `HasInputLevel` / `GetInputLevel` / `HasOutputLevel` / `GetOutputLevel` on the loaded
   DSP. Always check `Has*`, because `Get*` returns 0 when the field is absent. Gear kind comes from the existing
   `gear_type`.
7. **Threading:** the plan is computed at prepare/load/swap, off the audio thread. Linear gains reach the audio thread
   through the existing swap mechanism. A change of planned gain is smoothed (reuse the existing gain smoother and its
   time constant). `process()` makes zero allocations; the allocation harness must cover the calibration-on path.
8. **Render report:** add a `calibration` object with:
   - `enabled`, `deviceDbu`, `deviceAssumed` (true when +12 was used);
   - per path and block: `gainInDb`, `inputMissing`, `outputMissing`, `captureInputDbu`, `captureOutputDbu`;
   - `anyUncalibrated`.

   This field is additive, so it causes no schema break. Write it through the CLI/report path that already exists.

## API sketch (refine as needed)

`ChainCalibration { bool enabled = false; calibration::DeviceCalibration device; calibration::CalibrationDefaults defaults; }`
set at prepare, or through a swap-safe setter. `NamBlock` exposes `BlockLevelInfo levelInfo() const`.

## Acceptance (tests)

1. **Off is identical:** with calibration off, the full existing suite and goldens are unchanged, bit for bit.
2. **Single amp, on:** the effective input gain into the amp equals `device − captureInputDbu + block.inputGainDb`.
   Use the fixture `tests/fixtures/nam/wavenet.nam` (18.3 dBu) and `lstm.nam` (12.3 dBu). Measure by rendering, not
   only by reading the plan.
3. **Amp swap, on:** swap wavenet ↔ lstm with no user action.
   - The planned input gain changes by exactly the 6.0 dB metadata difference.
   - INPUT and block intent are preserved.
   - The output stays within ±0.5 dB of the level-match target.
   - The gain is applied exactly once: assert no make-up is added on top of the planned gain for a block that
     feeds a NAM, and that the last-block make-up and normalisation behave as defined above.
4. **Pedal→amp hop, on:** the amp's input equals `pedal.outputDbu − amp.inputDbu` (+ intents).
   - Swapping the pedal changes the amp's drive only by the metadata difference, not by a loudness make-up.
5. **Missing metadata:** a capture without `input_level_dbu` is neutral (today's behaviour) and is flagged in the
   report. An uncalibrated device uses +12 and sets `deviceAssumed`.
6. **Gain ladder:** a rung swap between captures with different `input_level_dbu` changes planned gain by the
   difference.
7. **Real-time:** zero allocations in `process()` with calibration on, including during a swap. The result is
   block-size independent within the existing tolerance.
8. **Synthetic stand-in for the user's matched chain.** Mark this in the test and in the REPORT as
   "SYNTHETIC — redo on the user's L_ubr_quick preset". The chain is a pedal NAM → amp NAM built from fixtures. Report,
   before (calibration off) and after (on):
   - each block's effective input level relative to its capture's `input_level_dbu`;
   - the dynamics-sweep slope (dB out per dB in) over an 18 dB input sweep;
   - plainly, whether calibration moved the amp drive by ≥3 dB or by less.

   Write the numbers into `docs/specs/v0_8-input_calibration_REPORT.md` under "I1".
9. **Gate check:** with the DI noise floor at −49.5 dBFS (synthetic noise) and the gate settings as v0.4M Task H
   derives them on the merged code, calibration on does not open the gate on noise. Report the gate's open fraction
   on a noise-only segment, off vs on. The gate is keyed on the DI before any calibration gain, so this should be
   unchanged. Verify it rather than assume.
10. Full ctest and pytest green; warnings-as-errors clean.

## Out of scope

- Preset fields and defaults (I4).
- Plugin Settings and device step, and the live-gate floor seed stored with the device calibration (I2).
- Drift check (I3).
- Matcher `--input-dbu` and export metadata (I4).
