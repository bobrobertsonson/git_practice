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

## Lead implementation decisions (2026-10-04, binding for implementer and reviewer)

These resolve the ambiguities in the sections above. Where they conflict with an earlier line,
these win.

### Probe and measurement

- **One probe run, two segments.** `Chain::resolveAlignment()` keeps its recipe and result.
  A new `Chain::resolveLevelMatch()` (not RT-safe, needs `prepare()`) renders the 1.5 s guitar
  segment through both paths at the blend point (same tap as alignment, gate bypassed, current
  latency compensation, *alignment applied* as resolved/stored so the sum is coherent), with
  each path's `levelDb` from the preset included (the test "6 dB `levelDb` offset → trims
  cancel it" requires this). Both resolvers run inside `prepare()`; the level-match pass runs
  whenever **both paths are enabled**, regardless of `levelMatch.mode` and `blendLaw`, so a
  live law toggle in the plugin always has a make-up curve to use. With one path enabled (single
  topologies) trims and make-up are all 0 and the pass is skipped.
- **Guitar segment** (`chain.cpp`, documented in a comment): 1.5 s, seed 2, Karplus–Strong
  plucks; 8 hits at 0.1875 s spacing, notes cycling A1 55 Hz, A1, D2 73.42 Hz, A1, A1, E2
  82.41 Hz, A1, D2; excitation = 1 period of seeded uniform noise; loop filter = two-tap
  average with loss factor chosen so the string decays by 60 dB in about 0.25 s (palm mute);
  generated in `double` at the chain's rate, then scaled so the segment's peak is −12 dBFS.
  Expose `kLevelProbeSeconds = 1.5`, `kLevelProbePeakDbfs = -12`, `kLevelProbeSeed = 2`,
  `kLevelProbeHits = 8` on `Chain`.
- **Loudness**: `integratedLoudnessLufs` (mono: pass the same buffer as L and R, or add a mono
  overload) over the full guitar segment of each path's output (`lufsA`, `lufsB`). If either is
  `nullopt` (gated silence) → trims 0, make-up 0, warning "level match: path X is silent".
- **Trims**: `trimA = max(0, lufsA_louder − lufsA)` etc.: the louder path gets 0, the quieter
  the positive difference, clamped to +18 dB. `sumLufs` = loudness of the aligned,
  polarity-corrected linear sum at `b = 0.5` after trims (report only).
- **Make-up curve**: for `b ∈ {0, .25, .5, .75, 1}` form the equal-power sum
  `cos(πb/2)·A' + sin(πb/2)·B'` of the *trimmed* (per the effective mode: resolved trims for
  `auto`, stored for `manual`, 0 for `off`) aligned path outputs in memory (no re-render), measure
  LUFS `L(b)`, and set `makeupDb[i] = Lref − L(b)` with `Lref = (L(0) + L(1)) / 2`, each clamped
  to ±12 dB. Between the five points `m(b)` is linear in dB.

### Signal path

- **Trims** are folded into each path's existing `level` gain target
  (`levelDb + trim + mute`), so no new stage and no latency. `LiveParams.levelDbA/B` remain the
  taste offset on top; changing them never changes the trims.
- **Blend law** becomes a *live* control: `LiveParams.blendLaw` (`Linear | ConstantLoudness`),
  default from the preset. Weights at the target `b` are `(1−b, b)` or `(cos, sin)`; the make-up
  linear gain at the target is `10^(m(b)/20)` (0 dB for linear). All three are ramped linearly
  over `kLiveRampMs` exactly like today's `blendA_/blendB_`, so there is no per-sample `exp`
  or interpolation on the audio thread. Polarity still rides on the B weight.
- **Headroom**: the sum is multiplied by 0.5 (exact in float) and the output gain by 2. The bus
  compressor's `thresholdDb` is referred to the *pre-headroom* level (internally −6 dB) so every
  existing preset keeps its compressor behaviour; goldens must stay bit-identical (they do not
  use the compressor; if one does, document the float tolerance). State this in
  `docs/PRESET_SCHEMA.md`.

### Schema, info, report, CLI, bindings

- Preset: `LevelMatch { mode (Auto|Manual|Off), trimADb, trimBDb }` and
  `BlendLaw { Linear, ConstantLoudness }`; `levelMatch.mode` defaults to `off`, `blendLaw` to
  `linear`; the writer emits both keys always (round trip) — but a preset *read* without them
  must render bit-identically. Trims parse in [0, 18].
- `ChainInfo` gains `levelMatchMode`, `trimDb[2]` (in effect), `lufs[2]`, `sumLufs`,
  `makeupDb[5]`, `blendLaw`. `AlignResult` is unchanged.
- Render report JSON: `"levelMatch": { "mode", "trimADb", "trimBDb", "lufsA", "lufsB",
  "sumLufs" }` and `"blend": { "value", "law", "makeupDb": [5] }`. `tonerender --report` writes
  the JSON; the console summary adds one line `level match: A +x.x dB, B +y.y dB; make-up
  [..]`.
- pybind: `sawblade_core.level_match(preset, sample_rate, base_dir=None, cache=None) -> dict`
  with `trimADb, trimBDb, lufsA, lufsB, sumLufs, makeupDb (list[5]), delaySamplesB, invertB`
  — build the chain, `prepare()`, return the info; no audio rendered. The matcher uses this.

### Plugin

- `blendLaw` toggle is a live edit (snapshot), no engine rebuild. MATCH LEVELS runs the probe
  on the loader thread like RE-MEASURE and writes the result back as `levelMatch.mode =
  manual` with the measured trims (same convention as RE-MEASURE → `align.manual`). A preset
  loaded in `auto` stays `auto` and the read-outs show the resolved trims from `ChainInfo`.
- Read-out under each LEVEL knob: `"+4.2 dB auto"` / `"+4.2 dB manual"` / `"0.0 dB off"` plus
  the user offset when non-zero: `"+4.2 dB auto · +1.0 dB"`.
- "Default CONSTANT + auto for new rigs": when the rig editor switches a preset to the BLEND
  topology and the preset carries neither key, set `auto` + `constantLoudness`. Presets that
  carry the keys keep them.

### Matcher

- The matcher mixes paths in numpy; keep that. After captures are chosen, call
  `sawblade_core.level_match` on the candidate preset (blend 0.5, align as the matcher resolved
  it), apply the trims to the cached per-path renders, then fit the *linear* blend `b_lin` as
  today and convert to the constant-loudness position with the same A:B ratio:
  `b_cl = (2/π)·atan(b_lin / (1 − b_lin))` (clamped). Emit `levelMatch: { mode: "manual",
  trimADb, trimBDb }`, `blend: b_cl`, `blendLaw: "constantLoudness"`. The Occam comparison
  (`OCCAM_DB`) runs between the level-matched candidates. The export report prints the trims
  and make-up from the render report.
