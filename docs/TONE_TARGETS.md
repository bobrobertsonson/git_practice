# Tone targets

North star: **Gatecreeper-style** death-metal rhythm tone — an HM-2-style "chainsaw" path
blended with a thick, tight high-gain "body" path. This doc defines what we aim for and how
we will measure it. Numbers marked *(hypothesis)* are starting points to be confirmed against
reference-song analysis in the matching phase; treat them as priors, not facts.

## Character in words

- **Saw path:** buzzy, serrated, aggressive upper-mid "rasp"; huge low-end bloom when used
  alone; loose and fizzy on its own. Classic recipe: HM-2-style pedal with both EQ controls
  high into a low/medium-gain amp, letting the pedal do most of the distortion.
- **Body path:** modern tight high-gain amp (often boosted with a mid-hump, low-cut boost):
  focused lows, clear palm-mute chug, solid low-mids, less fizz.
- **Blend:** body supplies tightness, attack definition and low-end control; saw supplies the
  characteristic upper-mid buzz. The blend should stay tight on fast palm mutes and D-beat
  strumming and not smear when chords ring.

## Spectral targets (long-term average spectrum, post-cab, 1/3-octave)

| Region | Target *(hypothesis)* | Why |
|---|---|---|
| < 60 Hz | rolled off (≥ 12 dB below 100 Hz band) | avoid flub; leaves room for bass |
| 80–150 Hz | strong, controlled "thump" | weight of palm mutes |
| 200–400 Hz | controlled, not scooped out | body; mud lives here if too strong |
| 500–800 Hz | level with 1–2 kHz (±2.5 dB) — **calibrated v2**: the original has a broad mid plateau, not a dip | HM-2 mid push |
| 1–2 kHz | prominent peak / plateau | HM-2-style "chainsaw" rasp |
| 3–5 kHz | present but not harsh | pick attack, articulation |
| > 7 kHz | ≥ 10.5 dB below the saw band (**calibrated v2**) | removes fizz |

Calibration v2 (2026-10-03): measured on the original's side channel; see
`docs/tone_targets.json` → `calibration`. Other rows remain priors.

## Dynamic / feel targets

- Tight gate on the DI: silence between hits, no chatter; fast release suitable for D-beat.
- Palm-mute transients: body path should dominate the first ~10–20 ms of the attack
  *(hypothesis)*; saw path dominates the sustain.
- Highly compressed by the amps, but no long-release bus compression (keeps it trainable).
- Paths phase-aligned: blend must not lose low end vs. either path alone (align/polarity).

## Blend defaults *(hypothesis)*

- Saw : Body around 45 : 55 after level matching.
- Shared cab IR (live-compatible) is the default.

## Measurements the matcher will use (phase 3+)

- 1/3-octave LTAS difference vs. reference (isolated or stem-separated guitars), A-weighted
  error in 80 Hz – 8 kHz.
- Spectral flatness / "buzz" metric in 1–3 kHz.
- Low-end tightness: decay time of 80–150 Hz energy after palm-mute onsets.
- Crest factor and short-term loudness range.
- Noise floor between notes (gate behavior) — measured but **never** trained into NAM.
