# Phase 10.1: path level matching and a constant-loudness blend

Blending two rigs only means something if the two paths are level-matched *before* the blend
and the blend knob does not change the output loudness. Today each path has a manual
`levelDb`, the matcher fits those as free parameters, and the blend is a linear crossfade.
That is fine for a preset the matcher produced, and wrong the moment a player turns BLEND or
swaps a capture: a scooped chainsaw path and a dense high-gain body path differ by 3–6 dB of
perceived level at equal peak, and because both paths carry the same aligned DI they sum
coherently, so a linear 50/50 can sit anywhere between the louder path and +6 dB.

Owner: `dsp-engineer`. Builds on phase 10 (rig editor) and the auto-align probe in
`docs/PRESET_SCHEMA.md` ("Alignment"). Reviewer audits; lead accepts.

## Core

1. **Level-match probe.** Extend the existing deterministic probe run (the auto-align probe:
   1.0 s, seed 1, white noise −18 dBFS, 80 Hz–5 kHz) with a second, guitar-shaped segment: a
   1.5 s synthesized palm-mute burst — Karplus–Strong plucks at 55/73/82 Hz (A1/D2/E2), 8 hits,
   peak −12 dBFS, seed 2, generated in code (no audio fixture). Both segments pass through
   each path *at the blend point* (same tap as alignment: post path-EQ, gate bypassed,
   per-path IRs included in `perPath` mode).
2. **Measurement.** BS.1770 integrated loudness (`loudness.h`) of each path's probe output
   over the guitar-shaped segment only (the noise segment is for alignment). Also measure the
   loudness of the aligned, polarity-corrected sum at `blend = 0.5` after trims.
3. **Trims.** `trimADb`, `trimBDb` such that both paths read the same LUFS, normalised so the
   louder path's trim is 0 and the other is positive, clamped to +18 dB. Applied as part of
   each path's existing output gain (no new gain stage, no extra latency). The player's
   `levelDb` stays on top as a taste offset.
4. **Blend law.** New `blend.law`:
   - `linear` (today's behaviour, default for presets that do not set it): `(1−b)·A + b·B`.
   - `constantLoudness`: equal-power crossfade `cos(πb/2)·A + sin(πb/2)·B`, followed by a
     make-up gain `m(b)` so that the measured probe loudness of the output is the same at
     every `b`. `m(b)` is computed from the probe at `b ∈ {0, 0.25, 0.5, 0.75, 1}` and
     linearly interpolated in dB; it lives in the Chain as five floats, recomputed whenever
     the probe runs, and is smoothed like the blend itself.
5. **When the probe runs.** Exactly when auto-align runs today (preset load / capture swap, on
   the background loader, before the swap is handed to the audio thread), plus the rig editor's
   RE-MEASURE. Results are deterministic for a given preset and are written to the render
   report (`levelMatch.trimADb/trimBDb`, `blend.makeupDb[5]`).
6. **Headroom.** The sum node gets 6 dB of fixed headroom (−6 dB into post-EQ / bus comp,
   +6 dB at the output gain) so matched paths cannot clip the float path's downstream
   non-linearities (bus comp detector). Document it; it is bit-transparent for linear stages.
7. **Schema (v1, additive):**
   ```jsonc
   "levelMatch": { "mode": "auto" | "manual" | "off", "trimADb": 0.0, "trimBDb": 0.0 },
   "blend": 0.5,                      // unchanged scalar stays valid
   "blendLaw": "linear" | "constantLoudness"
   ```
   Defaults when absent: `levelMatch.mode = off`, `blendLaw = linear` — so every existing
   preset, golden and export renders bit-identically. New presets written by the plugin and
   by the matcher set `auto` + `constantLoudness`.
8. **Export.** The trained signal includes the trims and make-up at the preset's `blend`; the
   export report prints them. Nothing else changes in export.
9. **CLI.** `tonerender` prints the measured trims and make-up curve in `--report`.

## Plugin (rig editor)

- A **MATCH LEVELS** button on the blend strip (near BLEND and RE-MEASURE) runs the probe and
  shows the two trims as small read-outs under each path's LEVEL knob ("+4.2 dB auto").
- A **blend law toggle** (LINEAR / CONSTANT) next to BLEND; default CONSTANT for new rigs.
- Turning a path's LEVEL knob never alters the auto trim; the read-out shows auto trim and
  user offset separately.
- State: `levelMatch` and `blendLaw` round-trip through plugin state like every other preset
  field.

## Matcher (match-engineer, small)

- Fit `blend` *after* auto trims (the Python side runs the same probe through pybind or
  `tonerender --report` and reads the trims), and record `blendLaw: constantLoudness` in
  emitted presets. Occam's "single path unless the blend buys ≥ 0.1 dB" comparison is then
  made between level-matched candidates. No change to the loss.

## Tests

- Probe determinism: identical trims/make-up across two runs and across block sizes 64/512.
- Two paths that are copies of each other with a 6 dB `levelDb` offset → trims cancel it
  within 0.1 dB; the constant-loudness output is within ±0.3 LU across `b ∈ {0, .25, .5, .75, 1}`.
- A legacy preset without the new keys renders bit-identically to the pre-10.1 golden.
- Zero allocations in `process()` with `constantLoudness` active; latency unchanged.
- Editor test: MATCH LEVELS updates the read-outs; state round-trips.

## Acceptance

- All tests above green; ctest and pluginval (where available) green; clang -Werror clean.
- `docs/PRESET_SCHEMA.md` updated (Blend section rewritten; `levelMatch`, `blendLaw`).
- Report `docs/specs/phase10_1_path_level_match_REPORT.md` with the measured trims for
  `presets/matched/barbaric_v4.json` and one style preset, and a plot of output loudness vs
  blend for both laws.
- Reviewer ACCEPT.
