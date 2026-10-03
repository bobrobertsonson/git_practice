# Phase 2A spec — calibrate tone targets from real references

Owner: match-engineer → reviewer. Lead approves the final numbers.

## Goal
Replace the hypothesis values in `docs/tone_targets.json` with values measured from guitar-
dominant audio of the north-star (Gatecreeper original) and the cover mix, without changing
the rule *shapes* unless the data contradicts them (lead decides).

## Inputs (git-ignored, see docs/TEST_MATERIAL.md)
- `testdata/reference/barbaric_pleasures_original.mp3` (north star; vocals/bass/drums/master)
- `testdata/reference/barbaric_pleasures_cover_mix.mp3` (cover mix of the DIs)
- `testdata/gatecreeper_cover/Guitar_L.wav`, `Guitar_R.wav` (DIs; define guitar activity
  for the cover — offsets ≈ +190 ms L / +175 ms R)

## Guitar isolation — two methods, both implemented, results compared
1. **Stem separation** (preferred): Demucs (`htdemucs`, MIT) via pip, CPU. The "other" stem
   ≈ guitars for this instrumentation (vocals separated out). Optional dependency group
   `match[separation]`; cache stems under `testdata/stems/` (git-ignored). If the model weights
   can't be downloaded, method 1 reports "unavailable" and the tool continues with method 2.
2. **Section selection**: frames where guitar dominates.
   - Cover: frames where either DI is active and the mix's 2–5 kHz band correlates with the
     rendered-DI envelope; exclude frames with strong kick/snare transients
     (spectral-flux spikes in 50–120 Hz / 180–250 Hz) — document the heuristic.
   - Original: frames with low vocal likelihood (no 300 Hz–3 kHz harmonic peaks with
     vibrato-like pitch continuity) and steady guitar texture; if this is not reliable,
     restrict to user-provided time ranges (`--sections 0:12,95:110`).

## Output — `sawblade-calibrate`
- For each reference × method: 1/3-oct LTAS (same analysis as tonecheck), group levels,
  every rule's measured value, buzz, lowDecayDbPerMs.
- A proposed `tone_targets.json` v2 written to a new file (never overwrite the committed one):
  thresholds set so the **original's guitars pass every rule with ≈ the rule's tolerance as
  margin**; rules the data contradicts are listed separately with evidence (not silently
  changed).
- `calibration_report.md` + PNG: LTAS of original vs cover (both methods), current vs
  proposed thresholds, and how the smoke render scores under both.

## Tests
Synthetic mixes (known "guitar" spectrum + synthetic drum hits + a synthetic vocal tone):
section selection excludes drum/vocal frames; the recovered guitar LTAS is within ±1 dB of
the known one; proposal logic places thresholds as specified. Demucs path is skipped in CI
when unavailable.
