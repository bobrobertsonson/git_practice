# v0.2 — tweakable capture presets (amp-head knobs, gain steps, suggested body path)

Source: user feedback on v0.1 (2026-10-05): matched / style presets are "one sound" — single
path, capture blocks have no knobs. User decisions (2026-10-05): controls **on the amp heads**;
GAIN = **drive + gain steps**; switching to BLEND fills path B with a **suggested body path**.
The user designs the UI: this spec fixes behaviour and controls only. Visuals follow the existing
skin (`docs/specs/phase2_5_skin.md`, `knob_amp` filmstrip); anything new that needs art is
proposed in the report, not invented.

Owner: dsp-engineer (core + plugin), match-engineer for Task C's pool query only. Reviewer audits
every task. Hard rules in CLAUDE.md apply in full (audio thread, swaps, latency, determinism,
versioned presets, NAM export rules).

## Task A — per-path amp controls (core + preset schema + host parameters)

Each path gets an **amp control set**, applied around that path's amp block (the block
`rig::ampIndex` identifies; a path without one has no amp controls):

| Control | Range | Default | Acts as |
|---|---|---|---|
| GAIN | 0–10 | 5 | input drive into the amp block: 5 = 0 dB, 0 = −12 dB, 10 = +12 dB (smoothed) + gain steps (Task B) |
| BASS | 0–10 | 5 | low shelf 100 Hz, ±12 dB, 5 = flat |
| MID | 0–10 | 5 | peak 650 Hz, Q 0.7, ±12 dB, 5 = flat |
| TREBLE | 0–10 | 5 | high shelf 3 kHz, ±12 dB, 5 = flat |
| PRESENCE | 0–10 | 5 | high shelf 5.5 kHz, ±9 dB, 5 = flat |
| LEVEL | 0–10 | 5 | output after the tone stack: 5 = 0 dB, ±12 dB |

- Tone stack sits after the amp block, before the path EQ; coefficients in `double`, smoothed,
  no allocation in `process()`. Linear and time-invariant at fixed knobs → NAM-trainable (state it
  in the block registry).
- **Neutral defaults are exact:** a preset with no `ampControls` (all existing presets) loads at
  defaults and renders **bit-identical** to before this phase (golden test on every committed
  preset).
- Preset schema: optional `paths.<a|b>.ampControls { gain, bass, mid, treble, presence, level,
  gainStep? }`; bump the schema version, keep reading old versions, document in
  `docs/PRESET_SCHEMA.md`. Plugin state round-trips them.
- Host parameters: 12 new continuous params with stable ids (`ampA_gain` … `ampB_level`),
  automatable, snapped to the 1e-4 state grid like the others.

## Task B — gain steps (capture packs with several gain settings)

- A capture block may carry an ordered **gain ladder**: the other models of the same TONE3000 tone
  that are the same amp at different gain settings, each with its gain value. Source of truth:
  the T3K pack's model list, parsed by the existing T3K client (match-engineer: a function that
  returns the ladder for a tone id, from model titles/metadata; unit-tested on recorded API
  fixtures; returns "no ladder" when ambiguous — never guess).
- GAIN then selects the nearest ladder rung (capture swap) and uses drive only for the remainder
  between rungs. Swaps follow the hard rule: load on a background thread, hand over lock-free,
  **equal-power crossfade ≤ 30 ms**, release the old model off the audio thread. Hysteresis so a
  knob resting near a boundary does not flip-flop. Missing rung captures are fetched through the
  existing resolve flow; until present, GAIN is drive-only and the UI says so.
- No ladder → drive only (Task A behaviour). Latency must not change across rungs (assert).
- The saved preset stores the chosen rung (`gainStep` = model id) so a render is deterministic.

## Task C — BLEND fills path B with a suggested body path

When `setTopology(Blend)` turns on an **empty** path B (no blocks), fill it with:
modeled TS boost (`pedal.ts`, drive 0, tone 5, level 8) → a high-gain amp capture chosen by a pure,
tested rule from the user's capture pool (`pool_sources.json` / cache): prefer a different amp
family from path A, high-gain slot, already cached first; then level-match with the existing 10.1
logic so the blend knob is constant-loudness. Path B with existing blocks is left untouched (current
behaviour). No pool / nothing cached → path B gets the TS + the first high-gain amp from
`presets/CAPTURE_SHORTLIST.md` and the resolve flow fetches it. Undoable as one edit.

## Task D — UI wiring (existing skin, no new art)

- Each amp head on the rig page shows its six knobs (`knob_amp` filmstrip) bound to the Task A
  params; the head of a path with no amp block shows its knobs disabled with a one-line reason.
- A capture block shows, where it is shown, that it is a capture (fixed tone) — the user must never
  see a knob that does nothing (v0.1 confusion).
- GAIN shows the active rung ("GAIN 7 · capture: Gain 6") when a ladder exists.
- Path B empty and BLEND off: the body amp head reads "BODY PATH OFF — turn up BLEND to add one".
- Screenshot the rig page at defaults and with knobs moved (CI artefact, as earlier phases did).

## Acceptance

- New unit tests: tone-stack responses (each control's gain at its frequency within 0.1 dB), neutral
  default bit-identity on every committed preset, schema round trip, param ids stable, gain-ladder
  parser on fixtures, rung swap allocation-free on the audio thread with a crossfade and no click
  (peak step < −60 dBFS on a sine), hysteresis, Task C selection rule, editor knobs move params
  (mouse-driven, like `test_live_controls.cpp`).
- Full suite green: gcc + clang `-Werror`, ctest, Python, pluginval 10 (VST3). macOS: no new
  failures versus the v0.1.3 baseline.
- Report `docs/specs/v0_2-tweakable_presets_REPORT.md` with reviewer verdicts, screenshots, and
  proposals (art needed, controls the user may want next).
