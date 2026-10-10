# Phase 3.3 spec — generalize to any heavy tone

User requirement: Sawblade must match **any heavy tone**, not just HM-2 death metal.

## A. Matcher search space (match-engineer)
- Remove title-based slot filters (e.g. "HM-2" for path A). Slots are typed by **gear class**,
  derived from TONE3000 `gear` plus a small, documented classifier over title/tags/makes:
  `drive` (OD/boost/TS), `distortion` (HM-2, RAT, DS-1, MT-2, …), `fuzz` (Muff, Tone Bender…),
  `preamp/amp-in-a-box`, `amp_low`, `amp_high`, `cab`. Unknown → offered to every pedal slot.
- Topologies searched (each a preset shape), chosen per reference by the search itself:
  1. **single path**: [pedal?] → amp → cab (most heavy tones)
  2. **single path, two pedals**: [pedal1] → [pedal2] → amp → cab (e.g. boost into fuzz)
  3. **blend** (current): A [pedal?] → amp, B [pedal?] → amp, shared cab
  Prefer the simplest topology within 0.1 dB loss (Occam), report all three.
- Pedal slots allow "none". Amp slots accept any amp class (path roles are labels, not filters).
- Budget scaling: with a bigger pool, pre-screen captures individually (each capture's
  1/3-oct response to the DI excerpt, cheap) and keep the top-N per class before pair search;
  document N and its effect on recall (validate against the full search on the current pool).

## B. Tone targets become profiles (match-engineer; lead approves numbers)
- `docs/tone_targets.json` v2 stays as profile `swedish_death_hm2`. Add a profile schema:
  `profiles/<id>.json` (rules + tolerances + provenance). Rules are **guardrails**, the
  reference-derived LTAS is the target.
- **Reference-derived default**: when matching a reference, derive the profile from the
  reference's isolated guitars (stems → side → sections) with loosen-only tolerances, so any
  style works without hand-written rules. Hand-written profiles are optional presets.
- Seed profiles (calibrated when the user supplies references): `swedish_death_hm2`,
  `modern_metalcore`, `doom_fuzz`, `thrash`, `black_metal`, `djent`.

## C. Capture pool (lead + match-engineer)
- Pool policy unchanged (newer + well reviewed + commercial-OK licences), but coverage
  targets per gear class so every heavy style has candidates: ≥ 4 each of drive, distortion,
  fuzz, amp_low, amp_high (incl. 5150/6505, Recto, Uberschall/Diezel/ENGL, Orange/Sunn-style
  doom amps, JCM800/Plexi), cab IRs (V30, Greenback, G12T-75, open-back 2x12).
- `sawblade-t3k pull --class fuzz …` support; a coverage report.

## D. Validation set (needs user material)
- ≥ 3 more DI + finished-mix pairs from different heavy styles (user has non-HM-2 DIs), each
  run through (a) known-answer and (b) DI → mix. Acceptance: every pair improves A-weighted
  error ≥ 4× vs the starter and lands ≤ 2 dB; Gatecreeper results don't regress.
- Plus north-star references (song files) per style for profile calibration.
