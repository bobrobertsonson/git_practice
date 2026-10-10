# Phase 3.5: gate that's quiet in the gaps without chopping tails (core)

## Why
- The user found the first gate too strong because it chopped the stops. They picked the
  "medium" setting from the A/B.
- With the matcher's gate (floor +4 dB, hold 40 ms, release 150 ms, range -50 dB), the
  tonecheck `gap_noise` rule still fails on every render, by about +3 dB. Hiss and buzz
  survive in the short gaps between riffs.
- A hard gate cannot satisfy both at once. The fix is an expander mode with a filtered
  key, so the gate opens and closes on the picking rather than on low-string rumble.

## Changes (dsp-engineer, core only; no matcher changes in this task)
1. `Gate` gets `mode: "gate" | "expander"`, default `"gate"`. Existing presets must render
   bit-identical.
   - In expander mode, below the close threshold the gain follows a downward expansion
     `ratio` (1.5 to 10, default 4), limited by `rangeDb`.
   - Hysteresis and hold behave as in gate mode.
2. `keyHighPassHz` (0 = off, else 40-400 Hz, 12 dB/oct) filters the key signal only.
   - The audio path is not filtered.
   - It is allocation-free in `process()`, with coefficients set in `prepare()` or
     parameter update.
3. `releaseMs` applies per state as now. Add an optional `releaseCurve: "linear-db"`, a dB
   ramp at constant dB per ms, alongside the existing one-pole.
   - Reason: the one-pole approach to -50 dB is slow at the end and leaves an audible noise
     tail.
4. Preset schema:
   - Add the new fields to `docs/PRESET_SCHEMA.md` with defaults that preserve current
     behaviour.
   - The reader validates their ranges; a bad value is a preset error (exit code 3).
5. Bindings expose the new fields; the Python preset dicts pass them through.

## Acceptance
- Unit tests:
  - expander gain law against an analytic reference (static curve within 0.1 dB);
  - key HPF magnitude at 3 points;
  - linear-dB release slope within 5%;
  - the zero-allocation harness passes;
  - block-size independence;
  - old presets bit-identical (golden).
- Lead render check: the `tonerender` CLI accepts the new fields. Render the cover DI (left)
  with the v3 preset in three ways:
  - (a) the current gate;
  - (b) expander with ratio 4, key HPF 120 Hz, linear-dB release 150 ms;
  - (c) the same as (b) with ratio 8.
  Report `gap_noise` and `lowDecayDbPerMs` from tonecheck for each. Put the WAVs (and
  MP3s at 192 kbps) in the report; the lead does the listening A/B.
- The full C++ and Python test suites pass.
