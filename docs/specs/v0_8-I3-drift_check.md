# v0.8 I3 — input-level drift check

Parent: `docs/specs/v0_8-input_calibration.md`. Builds on I2 (device record, `calibrationTick`, the gate's floor
publishing). The design is the main lead's (2026-10-07), recorded here.
Owner: dsp-engineer. Reviewer: reviewer.

Why: a device calibration is only valid at the interface gain it was made at. The user's earlier "strange compressor"
feel most likely came from a raised interface gain (main lead, 2026-10-08). If the gain knob moves after calibration,
every planned NAM drive is wrong by the same amount, and nothing tells the user.

## Behaviour

- **Statistic.** The p95 of short-term input peaks over *played* frames only, where played means the gate is open.
  Measure it on the DI, before INPUT and before any calibration gain, so neither the user's INPUT offset nor the
  calibration moves it.
  - Audio thread: per block, while the gate is open, accumulate peaks over 50 ms windows into a small fixed-size
    histogram (for example 0.5 dB bins from −80 to 0 dBFS).
  - Publish a lock-free summary: a fixed-size array snapshot or atomics. Use the same pattern as the I2 floor atomic
    or the existing meter ring, whichever is simplest.
  - No allocation, locks or I/O in `process()`.
- **Message thread** (`calibrationTick`, 10 Hz): turn the summary into a rolling p95 over the last 15 s of played
  frames.
- **Baseline.** Learn it in the first minutes of played audio after a device record is created or changed: at least
  60 s of played frames. Store it in the device record, next to the I2 learned gate floor. Settings only, written
  off the audio thread.
  - A record with no baseline yet learns one.
  - Re-picking a device clears the baseline, as it does the floor.
- **Trigger.** Raise the drift notice when |rolling p95 − baseline| ≥ 5 dB, sustained for ≥ 30 s of played time; clear it below 4 dB.
  Why not 6 dB and a 30 s window (lead decision, 2026-10-08): a literal ≥6 dB threshold is a coin flip for a true 6 dB change (p95 ±0.5 dB). A true ±6 dB change must be noticed within 60 s of played time, and dynamics within ±4 dB must never trigger it.
  Silence does not count toward the 30 s and does not reset it.
- **Notice.** One non-blocking notice: "Your input seems ~N dB hotter|quieter than when you calibrated — did the
  interface gain change? [Recalibrate] [Ignore]".
  - N is rounded to whole dB.
  - [Recalibrate] opens the Settings device step.
  - [Ignore] silences the notice until the baseline changes or the drift moves another ≥ 5 dB away from the level
    that was ignored.
  - Use the existing notice area and existing button components; the user designs the UI.
- **Never change any gain automatically.**
- **Gating.** Active only when calibrated input levels are on AND a device record exists. Otherwise do nothing:
  no statistic and no stored baseline.

## Tests

- **Core / statistic (local):**
  - A synthetic DI at +0, +6 and −6 dB relative to the baseline yields a p95 shift within ±0.5 dB of the true offset.
  - Frames with the gate closed are excluded. A loud noise burst while the gate is closed must not move the
    statistic.
  - Block-size independent.
  - `process()` allocates nothing.
- **Plugin (CI):**
  - A true +6 dB and a true −6 dB are each raised within 60 s of played time. +6 dB for 20 s does not. Dynamics within ±4 dB over minutes never raise it.
  - Playing dynamics within ±4 dB over minutes never raise it, for example alternating loud and soft passages.
  - [Ignore] silences the notice. A further 5 dB shift re-raises it.
  - [Recalibrate] opens the device step.
  - The baseline is stored in the device record, not in the preset or the plugin state.
  - No gain changes anywhere when the notice fires: an engine-params snapshot before and after must be equal.
  - With calibration off, nothing runs and output is bit-identical.
- Full ctest and pytest green; CI green.

## Out of scope

- Default flip, presets, matcher and export (I4).
- Auto-correcting the gain (explicitly never).
