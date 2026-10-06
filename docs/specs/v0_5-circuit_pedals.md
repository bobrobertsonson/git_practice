# v0.5 — circuit-level pedal models (with optional mods)

Source: user decision 2026-10-06: "we should build the pedals virtually based on their circuits and then add
mods optional." Runs after v0.4 (which delivers the movable pedalboard, capture pedals and a verified accuracy
harness). Owners: dsp-engineer (engine + models), match-engineer (validation against captures), reviewer on
every task. Hard rules in CLAUDE.md apply in full; UI names stay generic descriptors (no trademarks).

## Why, and what changes

Today's modeled pedals (`pedal.ts`, `pedal.hm` v3, `pedal.hmx`, `pedal.eye`, `pedal.muff`) are **behavioural**:
gain stages, filters and fitted clip knees tuned to measurements. They can match a capture at one setting
and drift at others, and a hardware mod has no natural place in them. A **circuit model** simulates the
schematic (component values, diodes, op-amp/transistor stages), so every knob and every mod (a component
swap) behaves like the real pedal by construction, and accuracy is checked against captures at many settings.

Costs to manage (state them in the report): more CPU (nonlinear solvers + oversampling), latency from
oversampling filters (must be reported per block, as every block does), schematic/tolerance uncertainty (a
circuit model is only as accurate as its schematic and component models, so captures stay the referee).

## Task A — circuit engine in `core/`

- Wave Digital Filter based (preferred: `chowdsp_wdf`, header-only; verify its licence is compatible
  and record it in `docs/THIRD_PARTY.md`, pinned by commit via FetchContent; if unsuitable, a small in-house WDF
  or DK-method nodal solver; justify the choice in the report).
- Nonlinear elements: diode pairs (Shockley, with series resistance; silicon / germanium / LED variants),
  op-amp stages (ideal + finite GBW / rail clip where the circuit needs it), BJT stages if a target circuit needs
  them. Iterative solves bounded (fixed max iterations, no allocation, deterministic).
- Oversampling (2× or 4×, per circuit) with the latency reported to the chain; aliasing budget ≤ −80 dB at
  the pedal's max drive on a 1 kHz / −6 dBFS sine (the 7c budget).
- Real-time rules: zero allocations in `process()` (allocation harness), bit-identical and block-size independent
  (within the documented tolerance), CPU budget ≤ 3 % of one Apple M1 core per circuit at 48 kHz/128 (measure
  on CI macOS runner and report).

## Task B — pilot: the TS-style overdrive as a circuit (`pedal.ts` v2)

The boost used by every BLEND fill. Published, well-analysed circuit: input buffer, op-amp clipping stage with
the diode pair in the feedback loop, tone stage, output. Knobs DRIVE / TONE / LEVEL map to the real pots (taper
included). **Validation:** against TONE3000 captures of the same pedal at labelled settings using v0.4's
accuracy harness; target knob-constrained LTAS ≤ 1.5 dB RMS and harmonic error within the capture-to-capture
spread, at ≥ 3 settings. Old `pedal.ts` v1 stays loadable (presets keep v1 unless upgraded; `model.version`).

## Task C — the chainsaw circuit (`pedal.hm` v4) with optional mods

Model the HM-2-style circuit stage by stage (see `docs/research/chainsaw_pedals.md` and the 7.1 report for the
measured behaviour of real units): gain/clipping stages and the active EQ (the gyrator bands behind the
LOW / HIGH knobs that make the chainsaw mid-scoop/boost). Knobs as the real pedal. **Mods** (optional, off by
default, each a named component change, saved in the preset):
- clipping diodes: stock / germanium / LED / asymmetric pair / none,
- gain: stock / more (gain resistor) / less,
- EQ voicing: stock / shifted band frequencies (the commonly modded caps) / wider bands,
- input: stock / tighter low end (input cap),
- second clip stage: stock / bypassed.
The mods appear in the pedal's drawer (existing AdvancedDrawer, no new art). Validation as Task B against the
HM-2 captures already used in 7.1, stock settings first; mods are checked for plausibility (direction and size
of the change), not against captures, unless captures of modded units are found (record them if so).

Presets: `pedal.hm` v3 stays loadable and bit-identical; new presets and the BLEND/match code use v4 once it
meets its target; a preset upgrade path (user-initiated) re-levels with v0.3's level match.

## Task D — then the rest of the family

`pedal.hmx`, `pedal.eye`, `pedal.muff` as circuits, in that order, same validation and versioning. Scope may
split into a v0.6 if the budget requires; the lead decides after Task C's report.

## Acceptance (phase)

Engine tests (allocation, determinism, block-size independence, aliasing, CPU); per-circuit accuracy tables vs
captures with targets met or gaps explained; old model versions bit-identical; full suite green (gcc + clang
`-Werror`, ctest, Python, pluginval 10 VST3, macOS auval + pluginval AU/VST3). Report
`docs/specs/v0_5-circuit_pedals_REPORT.md` with reviewer verdicts, CPU and latency per circuit, accuracy
tables, and a hand-test checklist (stock vs mods A/B in Logic, level-matched).
