# v0.4 — pedals: accuracy, captures on the board, a real pedalboard on the main page

Source: user decision 2026-10-06, after testing v0.2: pedals matter more than the amp knobs; "right now I want
accuracy, captures and models, and moving them around on the main page". Runs **after v0.3 merges** (it builds
on v0.3's undo and per-slot level match). Owners: match-engineer (Task A fitting), dsp-engineer (model fixes,
Tasks B–C), reviewer on every task. Hard rules in CLAUDE.md apply. No new art: generic faces use the existing
skin; propose art in the report. UI names use generic descriptors, not trademarks.

## Task A — modeled-pedal accuracy against real captures

State today (HANDOFF, 7.1 report): `pedal.hm` v1/v2 measured ~4 dB RMS LTAS shape error vs real HM-2 captures,
with the harmonic-profile term at ~40 dB on every model; v3 (7c) applied fixes but was **never re-fit**; `hmx`,
`eye`, `muff`, `ts` have no published accuracy numbers.

1. **Check the metric first.** A ~40 dB harmonic error on every model, free or constrained, suggests a measurement
   problem (level reference, floor, capture input level) as much as a model problem. Verify the harmonic term on a
   known answer (render `pedal.hm` itself, fit a capture *made from* `pedal.hm` → near-zero error) and fix the
   metric if it fails.
2. **Fit every modeled pedal** with `sawblade-calibrate pedal-fit` against the TONE3000 captures of its pedal family
   (labelled knob settings where available, `presets/CAPTURE_SHORTLIST.md` + search; record licence/creator,
   never commit captures). Publish `docs/reports/v0_4/accuracy.md`: per pedal and capture, free and
   knob-constrained LTAS / harmonic / dynamics errors, and a one-line verdict.
3. **Targets** (knob-constrained, i.e. the pedal's knobs set to the capture's labelled settings and only level
   free): LTAS shape ≤ 2.0 dB RMS on ≥ 75 % of labelled captures per pedal, and a harmonic error within 2× the
   capture-to-capture spread of the same pedal. A pedal that misses: root-cause and fix the DSP (knob law, EQ
   curve, clipping stage), re-fit, repeat; if still short, report the remaining gap and what would close it.
   Changing a model's sound changes presets that use it: re-render the committed modeled presets, report the
   level/LTAS change per preset, and bump the model version so old presets can keep the old sound
   (`model.version`; old versions stay loadable and bit-identical).

## Task B — pedal captures on the pedalboard

- Any TONE3000 **pedal** capture (NAM) can be placed on the pedalboard as a pedal, from the board's "+ PEDAL"
  slot: a picker with two tabs, MODELED (the five models) and CAPTURES (TONE3000 pedal captures; search, cached
  first, licence + creator shown, the existing resolve/fetch flow).
- A capture pedal has a generic face (existing skin): name, creator, licence tag, `CAPTURE · FIXED TONE`, a LEVEL
  knob and the bypass footswitch; no knob that does nothing. Where the pack has several knob settings, the
  pedal shows a setting selector (reuse the v0.2 ladder machinery where it fits; a plain list otherwise).
- Placement uses v0.3's per-slot level match, so a capture pedal drops in at matched loudness.
- NAM-trainable as today (a capture pedal is a `nam` block); the export rules are unchanged.

## Task C — a real pedalboard on the main page

- **Move:** drag a pedal left/right to reorder within its path, and drag between path A (SAW) and path B (BODY).
  Drop targets highlight; the cables redraw; latency compensation updates (no glitch: swap prepared off the audio
  thread as today).
- **Add / remove:** "+ PEDAL" slots open Task B's picker; remove by dragging a pedal off the board or a
  right-click / ctrl-click REMOVE. Bypass with the footswitch (existing).
- **Capacity:** at least 6 pedals per path, laid out so they stay readable at 1280×800 (shrink faces to a minimum
  width, then scroll the board horizontally).
- Every move/add/remove/bypass is one v0.3 undo step and is reflected immediately in the rig editor and vice versa.
- Tests (mouse-driven): reorder within a path; move A→B and B→A; add from both picker tabs; remove; bypass; undo
  of each; the rendered chain matches the board order (render test); latency report updates.

## Out of scope (propose in the report)

New pedal models (list the ones worth adding next, by style family in CLAUDE.md's scope: e.g. a Rat-style
distortion for grind/crust — see `docs/specs/phase7c_zone_rat.md` — an octave-fuzz, a noise-gate pedal on the
board), pedal art, amp-head art.

## Acceptance (phase)

All task tests; accuracy table published with targets met or gaps explained; full suite green (gcc + clang
`-Werror`, ctest, Python, pluginval 10 VST3, macOS auval + pluginval AU/VST3); v0.2/v0.3 tests still pass.
Report `docs/specs/v0_4-pedals_REPORT.md` with reviewer verdicts, the accuracy table, and a hand-test checklist
for the user (Logic, macOS).
