# v0.8 I2 — device calibration in the plugin, calibrated measurements, live-gate floor seed

Parent: `docs/specs/v0_8-input_calibration.md`. Builds on I1 (`docs/specs/v0_8-I1-chain_wiring.md`).
Owner: dsp-engineer (core + plugin). Reviewer: reviewer.
Prerequisite for the phase end state: calibration ON by default in plugin and match/tonerender (main lead, 2026-10-08).
I2 does not flip any default; I4 does. I2 makes everything that measures levels calibration-aware first, so the flip
is safe.

Note: the plugin cannot be built in the lead's container (no X11 headers). CI is the build of record. Keep plugin
edits small and well tested by plugin/tests, which CI runs.

## Part 1 — core: calibration-aware measurements (hard prerequisite for the default flip)

- `measureReferenceLufs`, `computeAutoTrim` and `slotMakeupDb` (`core/src/auto_trim.cpp`, A3 rows 7 and 16) take a
  `ChainCalibration` (default: off). They measure the chain as it will actually play.
- With calibration on, `slotMakeupDb` for a block that feeds a NAM (I1 `feedsNam`) returns 0 and says so. The make-up
  would be dropped anyway, so computing it only wastes a render. Keep the result struct's existing fields and add a
  flag such as `skippedHop`.
- Level-match trims (A3 row 13) are measured with the same calibration the chain uses.
- Tests:
  - With calibration off, results are bit-identical to today.
  - With it on, the auto trim lands the calibrated chain at −18 LUFS ± the existing tolerance.
  - A swap of the last block keeps monitoring level within ±0.5 dB (the main lead's I1 addition, now through the
    real make-up path).
  - Make-up on a hop block is skipped.

## Part 2 — plugin: device calibration record (Settings, not preset)

- Record: `{ dbu, method: preset|manual|measured, model, gainAtMinimum, pad, air, date, liveGateFloorDbfs? }`.
  - It lives in the plugin's settings store, which already persists app settings. Plugin state is the preset and does
    NOT contain this record.
  - Storage writes happen off the audio thread only.
- Device step UI (Settings):
  - It says plainly: "Set your interface's instrument gain to minimum (note PAD/Air). Sawblade supplies all gain."
  - Presets, each with its source cited in a code comment and in docs/PLUGIN.md:
    - Focusrite Scarlett 4i4 3rd gen, Inst: +12.5 dBu, or +14 dBu with PAD (Focusrite user guide, at minimum gain).
      This is the user's interface.
    - Focusrite Scarlett 4i4 4th gen, Inst: +12 dBu.
  - "Enter dBu" (validated to the plausible range [−60, +60], with a practical warning outside [0, +24]).
  - "Measure", the guided path:
    - It is a stub that explains the procedure and stores `method: measured` when a value is entered.
    - A full guided measurement needs a reference signal from the user. Propose the design in the report and do not
      build it in I2.
  - Never compute the level from the Focusrite Control 2 readout. A code comment cites REPORT A1b for why.
- Notices:
  - While no record exists: a non-blocking "Interface not calibrated: assuming +12 dBu" notice. Show it in the main
    view where existing notices appear, and in Settings.
  - Blocks flagged uncalibrated in the I1 plan (missing capture metadata) show a small per-block "uncalibrated" mark.
    Reuse an existing badge style; the user designs the UI, so add no new visual language.
- Engine: the plugin passes the device record (or none) into `ChainCalibration`. Calibration stays OFF by default in
  I2, behind a Settings toggle "Calibrated input levels (beta)", default off. I4 flips it.
- Hop make-up: plugin swap code (`rig/Pedalboard.cpp`, `browser/BrowserController.cpp`) uses the Part 1
  `skippedHop` result and does not store a make-up for hop blocks when calibration is on.

## Part 3 — live-gate floor seed (parked spec, 2dda4a9)

- Store the learned live-gate floor in the device calibration record. Seed the live follower from it on prepare.
  - The key is the record itself; there is no device name.
  - Never seed from a preset or a matched reference DI.
  - Write it off the audio thread only: the audio thread publishes the learned value through an existing lock-free
    channel, and the message thread persists it with throttling.
- With no stored floor, use today's seed (−70 dBFS).
- Tests:
  - The seed is used on prepare.
  - Learning updates the stored value off the audio thread, with no allocation in process().
  - A preset load does not change the seed.

## Acceptance

- Full ctest and pytest green. Plugin tests (CI) green, including new tests for the device record round trip, the
  preset values, the notice when uncalibrated, the toggle default off, and the hop make-up skip.
- With calibration off, everything is bit-identical to before I2.
- docs/PLUGIN.md: the device step, the presets with sources, and the beta toggle.

## Out of scope

- Drift check (I3).
- Preset fields, default flip, matcher/tonerender `--device-dbu` and export metadata (I4).
