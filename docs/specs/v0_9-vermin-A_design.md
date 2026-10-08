# v0.9 Task A/B — VERMIN circuit design (lead)

Companion to `docs/specs/v0_9-vermin.md`. This page fixes the constants, units and test definitions
so the implementation and the review check against the same numbers. Where this page and the parent
spec disagree, ask the lead.

## Units and files

- Internal signal unit: **volts**. Float 1.0 at the block input = 1 V at the pedal input. All circuit
  constants below are in SI.
- Files: `core/include/sawblade/pedal_rat.h`, `core/src/pedal_rat.cpp`; params struct and JSON parse in
  `pedal_params.h/.cpp` next to `TsParams`; registry entry `pedal.rat` in `block_registry.cpp`
  (`namTrainable = true`). All circuit constants live in one table (`RatVoicing`), as `HmVoicing` does,
  so a later capture fit changes one place.
- Reuse: `OnePole`, `ShortDelay`, `GainRamp`, `osPadding`, `dbToGain`, `pedalLevelDb` (VOLUME uses the same
  level mapping as the other pedals), `Oversampler4x`, `AdaaClipper`, `PedalImplConfig`. Do not change
  the shared `ClipType` enum: VERMIN's clip set differs (it has `none`, and its `asymmetric` is a 1+2
  diode string), so it gets its own small enum.
- Live parameters (`setLiveParams`): DIST, FILTER, VOLUME, TIGHT, MIX, CLIP, RUETZ, with the same
  20 ms ramps / smoothing the other modelled pedals use for continuous controls. If live params cost
  more than a small follow-up, ship static params and say so in the report (Task E needs them).

## Signal chain (base rate fs, oversampled rate 4 fs)

1. **Input HPF** 20 Hz, first order (base rate).
2. **TIGHT**: first-order HPF on the shared mapping `20·10^(t/10)` Hz (20..200 Hz, as the other pedals;
   revised from 20..250 Hz for shared semantics); off (0) = bypassed.
3. Upsample ×4.
4. **Op-amp gain stage** (below): GBW + slew + rails, at 4 fs.
5. **Diode clipper** (CLIP), `AdaaClipper` at 4 fs.
6. Downsample.
7. **FILTER** RC low-pass, **output coupling HPF** 10 Hz, **VOLUME**, **MIX** (clean path delayed by the
   pedal latency), base rate.

## Op-amp gain stage (the defining feature)

Non-inverting LM308-class stage. Feedback network:

- `Zf = Rd ‖ Cf`, with `Rd` the DIST pot (100 kΩ, audio taper) and `Cf = 100 pF`.
- `Zg = (R1 + 1/(s C1)) ‖ (R2 + 1/(s C2))`, with `R1 = 47 Ω, C1 = 2.2 µF` (corner 1.54 kHz) and
  `R2 = 560 Ω, C2 = 4.7 µF` (corner 60.5 Hz).
- **RUETZ** on removes the `R2/C2` leg (`Zg = R1 + 1/(s C1)`).
- Ideal closed-loop gain `G(s) = 1 + Zf/Zg`. Max ≈ 1 + 100k/(47‖560) ≈ 2305 (67 dB) at DIST 10.

DIST taper: `Rd = 100 kΩ · (a^t − 1)/(a − 1)` with `t = dist/10`, `a = 81` (≈ 10 % at mid travel),
plus a 0 Ω floor (gain 1 at DIST 0, which is circuit-true).

**Op-amp model** — single-pole op-amp with slew limit and rails, solved with the feedback network
(not a fixed low-pass, not a post-hoc filter):

- Open loop `A(s) = ωt / s`, `ωt = 2π · GBW`, `GBW = 1.0 MHz` (LM308 with 30 pF compensation).
  The closed-loop small-signal response is then `H(s) = A / (1 + A β(s))` with
  `β(s) = Zg / (Zg + Zf)`; its HF pole moves down as DIST rises. This `H(s)` (not the ideal `G(s)`)
  is the analytic reference for the Task B frequency-response test.
- The integrator state is the op-amp output `vo`. Per oversampled sample, solve the discretised
  network + integrator **implicitly** (trapezoidal / bilinear; `ωt·T ≈ 33` at 192 kHz, so explicit
  Euler is unstable). Then clamp the step: `|vo[n] − vo[n−1]| ≤ SR · T`, `SR = 0.3 V/µs`. When the
  clamp acts, the network states advance with the clamped `vo` (no hidden unclamped state).
- Rails: `|vo| ≤ Vrail = 3.8 V` (9 V supply, LM308 swing), as a smooth saturation near the rail with
  integrator anti-windup (the integrator does not keep integrating past the rail). In CLIP `none` this
  is the only clipping.
- Expose the stage as its own class (e.g. `RatOpAmpStage`, header-visible) so Task B can test it
  directly at 4 fs. The implementer documents the discretisation in the header comment.

## Diode clipper (after a 1 kΩ series resistor and the 4.7 µF coupling cap: model the cap as a
first-order HPF at 1/(2π·1k·4.7µ) ≈ 34 Hz)

| CLIP | shape (`AdaaClipper::setShape(kPos, kNeg, order)`) | what it is |
|---|---|---|
| `silicon` (stock) | 0.55, 0.55, 1 | 1N914 pair |
| `led` | 1.7, 1.7, 2 | red LED pair |
| `asymmetric` | 0.55, 1.1, 1 | one diode one way, two the other |
| `none` | clipper bypassed (clean delay kept so latency is identical) | op-amp rails only |

## FILTER, VOLUME, MIX

- FILTER: `fc = 1/(2π (1.5 kΩ + Rf) · 3.3 nF)`, `Rf` = 100 kΩ **audio-taper** pot:
  `Rf = 100 kΩ · (a^t − 1)/(a − 1)`, `t = filter/10`, `a = 81`. FILTER 0 → 32.2 kHz
  (clamped to 0.45 fs), FILTER 5 → ≈ 4.2 kHz, FILTER 10 → 475 Hz. First-order LPF.
  *Revised 2026-10-08 (main lead):* the taper is a voicing choice made by feel (the literal reverse-log
  put noon at 527 Hz), not verified against a schematic; it is one named `RatVoicing` constant so a
  capture fit can revisit it.
- VOLUME 0..10 through `pedalLevelDb`. "Stock = VOLUME at unity": pick the stock VOLUME so that the
  stock pedal (DIST 5, FILTER 5, silicon) on a −12 dBFS-RMS DI riff (an existing test fixture) comes
  out at the same RMS as the input ± 1 dB; state the value in the header and PRESET_SCHEMA.
- MIX 0..100 %, clean = input after the input HPF, delayed by the pedal latency (as `pedal.hm`).

## Parameters (preset JSON, `pedal.rat`)

`distortion` 0–10 (5), `filter` 0–10 (5), `volume` 0–10 (stock from above), `tightness` 0–10 (0),
`clip` `"silicon" | "led" | "none" | "asymmetric"` ("silicon"), `mix` 0–100 (100), `ruetz` bool (false).
Missing keys take the stock value; out-of-range values are rejected at parse like the other pedals.
**Schema:** add the new block type to `docs/PRESET_SCHEMA.md` only; do not bump the schema version or
touch existing fields (v0.8 owns schema v5).

## Calibration (v0.8)

The base branch has no nominal-output-dBu field. Do not invent one. The report lists it as a merge-time
follow-up, with the measured stock output level in dBFS so the value can be filled in.

## Task B acceptance tests (Catch2, `tests/test_pedal_rat.cpp`)

1. **Linear FR** at DIST 0 / 5 / 10, small signal (input −60 dBFS sine sweep or impulse; the slew and
   clippers must stay inactive — assert it), `flatFilters` off, CLIP `none`, FILTER 0, TIGHT 0, MIX 100:
   measured response vs. the analytic chain (input HPF · H(s) · coupling HPF · FILTER LPF · out HPF ·
   volume) within ± 0.5 dB from 40 Hz to 0.4 fs, at 48 kHz and 44.1 kHz.
2. **Slew**: `RatOpAmpStage` alone at 4 · 48 kHz, DIST 10, 5 kHz sine at 1 V peak input: the output's
   maximum `|Δvo|/T` is within 10 % of 0.3 V/µs (and must actually reach the clamp), and THD of the
   stage output > a stated floor that a slew-free run (SR = ∞, test-only knob) does not reach — i.e. the
   test proves slew changes the waveform.
3. **GBW**: at DIST 10, small signal, the closed-loop −3 dB corner of the stage is within 10 % of the
   analytic `H(s)` corner, and it moves down from DIST 5 to DIST 10.
4. **FILTER sweep**: −3 dB corner (of the FILTER stage response, measured through the pedal with CLIP
   `none` at small signal, or on the stage alone) monotonic over 0..10 in steps of 1; at 0 / 5 / 10 within
   10 % of the RC formula (with the 0.45 fs clamp at 0 stated).
5. **CLIP modes** (DIST 7, 200 Hz sine, 0.1 V peak): `none` output peak > `silicon` by ≥ 6 dB; `led` peak
   > `silicon`; `asymmetric` 2nd harmonic ≥ 20 dB above `silicon`'s 2nd harmonic (relative to the
   fundamental).
6. **RUETZ**: small-signal gain at DIST 10 at 200 Hz and 100 Hz drops by the analytic amount
   (`|H_stock| / |H_ruetz|` from the formulas above, stated in the test) ± 0.5 dB; at 5 kHz it changes by
   < 0.5 dB.
7. **Real-time / determinism**: zero allocations in `process()` (allocation-counting harness);
   block-size independence (1, 7, 64, 512, 4096 vs. one shot, within the tolerance the other pedals use);
   latency reported equals the measured latency (flatFilters impulse method); bit-identical across runs.
8. **Aliasing**: same method and threshold as the existing pedal alias tests; worst case DIST 10,
   CLIP `none` and `silicon`; report the measured level. It must be ≤ the worst existing pedal's
   measured level. The alias test must also fail with oversampling off (sensitivity check).
9. **Registry / preset**: `pedal.rat` parses (all keys, defaults, rejects bad values), reports
   `namTrainable = true`, renders through `tonerender`.

## Report data (Task A/B part)

Measured FR plots at DIST 0/5/10 vs. analytic, alias level, CPU cost per instance (real-time factor at
48 kHz, Release, single core), stock output level (dBFS RMS on the fixture), slew test numbers.
