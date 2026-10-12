# Phase 7 report: modeled pedal blocks (`pedal.hm`, `pedal.ts`)

**Status: accepted by the lead.** The reviewer returned ACCEPT with no must-fix items. Release
ctest passes 200/200, and so does Debug under ASan/UBSan. Spec:
`docs/specs/phase7_modeled_pedals.md`, amended in `aaab414`.

Plots (PNG): `docs/reports/phase7/`. They are also published on the Artifact page
"Sawblade Modeled Pedals":
https://claude.ai/artifact/H6EvMnKEbq7ojVKCcQn1aK

## What was built
- **`Oversampler4x`** (`core/.../oversampler.*`): a 2x·2x linear-phase polyphase half-band
  cascade, Kaiser-designed in `prepare()`, with 87 taps and then 27 taps.
  - Round-trip ripple: 0.00014 dB over 0–0.4167·fs.
  - Stopband: 110.4 dB (stage 1) and 105.6 dB (stage 2).
  - Image and decimation alias: −110.8 dB.
  - The filter edges are fractions of fs, so one design serves 44.1–192 kHz.
- **`AdaaClipper`** (`adaa_clipper.*`): a piecewise-polynomial soft clipper,
  `u − u³/(3k²)` inside ±k and `±2k/3` beyond, with per-sign knees for asymmetry.
  - It uses 2nd-order antiderivative anti-aliasing (Parker/Esqueda/Bilbao 2016) in double,
    with ε = 1e-5 and exact limit fallbacks.
  - The reviewer checked F1/F2 continuity by hand and by test.
- **`pedal.hm`**, "Swedish chainsaw distortion". The signal path:
  1. Input buffer.
  2. 4x oversampling.
  3. Pre-filtered op-amp gain (6–46 dB).
  4. Diode clip.
  5. Interstage LPF and +20 dB.
  6. Diode clip.
  7. 4th-order 6.5 kHz LPF.
  8. Downsampling.
  9. "Color mix" dual-gyrator EQ as fitted RBJ biquads: Low 100 Hz, High 1.0 + 1.5 kHz, fixed
     4.8 kHz presence peak and 9 kHz roll-off.
  10. Level.

  Params: `level`, `low`, `high`, `distortion` (0–10).
- **`pedal.ts`**, "green overdrive". The signal path:
  1. Input buffer.
  2. 4x oversampling.
  3. A gained branch: HPF at 720 Hz, then a gain-dependent feedback LPF, gain `Rd/4.7k`.
  4. An asymmetric ADAA soft clip (k+ 0.45 / k− 0.30), summed with the clean signal, as in
     the feedback-loop topology.
  5. Downsampling.
  6. Tone LPF, 723 Hz–7.2 kHz.
  7. DC HPF.
  8. Level.

  Params: `drive`, `tone`, `level` (0–10).
- Both are registered in `BlockRegistry` with `namTrainable = true`.
  - Each reports **50 samples of latency** at every rate: a 198-sample oversampled round
    trip, plus ADAA, plus a pad, totalling 200 at 4·fs.
  - The Chain compensates this latency.
- Schema: `"modelVersion": 1` plus a `"params"` object, documented in
  `docs/PRESET_SCHEMA.md`. Unknown keys, out-of-range values and other model versions are
  PresetErrors.
- Example presets, which render with repo files only:
  - `presets/modeled/hm_chainsaw.json`: HM with everything at 10.
  - `presets/modeled/ts_boost.json`: TS at drive 2 into HM.
- Dev tool: `tests/tools/pedal_fr` writes the small-signal FR as CSV (the source of the plots).

## Acceptance results (measured)
| # | Test | Result |
|---|---|---|
| 1 | HM EQ section vs. biquad table (8 freqs × 3 settings) | within ±0.5 dB |
| 1 | HM 10/10/10, re \|H(400)\| | low peak +10.75 dB @ 102 Hz (≥ 8); mid peak +15.86 dB @ 1089 Hz (≥ 12); presence local max @ 4.58 kHz, +1.26 dB over 3.5 kHz (≥ 1); 12 kHz −65 dB under mid peak |
| 1 | HM low = high = 0 | 100 Hz −10.3 dB, 1250 Hz −9.8 dB re 400 Hz (≤ −6) |
| 1 | HM knob isolation (absolute) | low 0→10: +30.0 dB @ 100 Hz, +0.49 dB @ 1250 Hz. high 0→10: +34.2 dB @ 1250 Hz, +0.30 dB @ 100 Hz |
| 1 | TS drive 10, tone 0 | peak 710 Hz; 100 Hz −11.4 dB; 5 kHz −13.8 dB |
| 1 | TS tone span @ 4 kHz (drive 5) | +14.1 dB (≥ 12) |
| 1 | TS drive 0 | 100 Hz is 13.5 dB below 1 kHz (≥ 10) |
| 2 | THD monotonic, −20 and −40 dBFS | passes; span at −40 dBFS: HM 30 dB, TS 38.6 dB; TS H2 at drive 10 = −43.8 dBc |
| 3 | Aliasing, 5 kHz −6 dBFS, max gain | **HM −93.8 dB, TS −91.6 dB** (< −80). OS only −36/−39; ADAA only −59/−54; neither −24/−26 (asserted > −80, so the test is sensitive) |
| 4 | Latency reported = measured | 50 = 50 at 44.1/48/96/192 kHz; Chain compensation verified |
| 5 | Zero allocation | 0 allocations in standalone and Chain tests |
| 6/7 | Block size 1/7/64/512/4096; repeat render | bit-identical (tolerance 0) |
| 8 | Preset round-trip and errors | exact; all error cases raise PresetError |
| 9 | Example presets, existing goldens | render; goldens unchanged |
| 10 | `-Werror`, ASan/UBSan | clean |

## Spec amendment made during the task
The lead's THD span criterion (≥ 6 dB at −20 dBFS) was wrong for these topologies.
- At −20 dBFS both pedals already sit near saturation at knob 0: the HM span is 1.4 dB and
  the TS span is 3.6 dB.
- The span is now asserted at −40 dBFS, a realistic single-note DI level. Monotonicity is
  still asserted at both levels.
- Knob isolation is measured as an absolute change, because the 100 Hz gyrator skirt moves
  the 400 Hz reference by about 4 dB.
- The models were not changed to fit the tests.

## Validation: no real-unit comparison in this session
Real HM-2 NAM captures exist only in `~/.cache/sawblade` on the main machine, so none were
available here. Validation rests on two things:
- the published descriptions of the HM-2 "color mix" response, which are encoded in the
  targets: Low near 100 Hz, High a broad 1–1.5 kHz boost, about +15–20 dB over the
  300–500 Hz trough when maxed, an upper-mid presence peak and a steep top roll-off;
- the TS feedback-network corner frequencies (720 Hz HPF, 723 Hz tone RC, a 51 pF cap
  against the drive pot).

The lead derived analytic responses from the stage tables before implementation, and the
measured results land on them; for example, the TS drive-0 bass cut was predicted at about
−13 dB and measured at −13.5 dB. **Nothing here proves the models match a real HM-2 or TS by
ear or by capture.**

## Lead notes and follow-ups (proposed, not done)
1. **Fit against captures (main machine).**
   - Fit the HM EQ table, the clip knees and the top-end roll-off to the HM-2 NAM captures
     with small-signal sweeps plus THD-vs-level.
   - The modeled HM rolls off steeply: about −30 dB at 9 kHz relative to 400 Hz (see the
     plot). It may be darker than a real unit, so check it first.
   - Bump `modelVersion` if the sound changes.
2. **Gain staging.** HM saturates almost fully even at distortion 0 for a hot input, because
   of the fixed +20 dB interstage. A real HM-2's low settings are cleaner. Revisit this
   during fitting.
3. **Latency of the shipping pedals.** Reported equals measured is proven on the
   flat-filter variant, which is exact by construction. The IIR group delay of the voicing
   filters is deliberately not counted, as with the existing EQ block.
4. **Reviewer non-blocking notes.**
   - The ADAA ε is absolute; check its precision at |x| of about 200 during fitting.
   - There is no denormal guard in the double-state one-poles. The cost is performance
     only.
   - There is no debug assert for `n ≤ maxBlock`.
5. **Plugin.** Parameter smoothing and realtime knob changes are needed before these
   blocks get live UI knobs. Today, parameter changes go through the Chain rebuild/swap.
   That is for the separate UI session.
6. **Python side.** `match/` and `bindings/` were untouched by design. The matcher cannot
   search these knobs yet; exposing them is a natural next task.

## Commits
- Spec: `92cb895`, then `aaab414` (amendment).
- Implementation:
  - `66ae935`: core.
  - `32cf226`: tests and `pedal_fr`.
  - `e009d88`: presets and docs.
  - `02c695f`: comment tidy.
  - `81c16ed`: THD per the amendment.
- Plots, report and the schema wording fix: the commits that follow.
