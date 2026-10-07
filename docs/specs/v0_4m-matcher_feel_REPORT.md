# v0.4M — matcher matches the feel: report

Spec: `docs/specs/v0_4m-matcher_feel.md` (Tasks A–E, B2–B4). Lead-pinned definitions: `docs/specs/v0_4m-tasks.md`.
Branch: `claude/sawblade-v0_4m-matcher-feel`. CI (GitHub Actions) is the validation of record; the local container could
not install scipy/pytest (pypi blocked), so engineers also ran the render tests against a local core build with a scipy
shim — those numbers are labelled "shim" below and are indicative only.

Status: **Tasks A, B, B2.1 (core), B3, C, D.1 and E accepted and green on CI.** B2.1 (matcher half), B2.3 and B4 are in
the final review/merge round (see "Open"). Validation on the real Bloodbath audio is the user's (Mac commands below).

## What changed, in one paragraph per suspect

1. **The reference basis was wrong for clean amp tracks (likely the main cause of "fizzy").** In the Bloodbath runs,
   `--matched mono` on a clean amp WAV with no stems fell back to the full-mix basis: LTAS above 4.5 kHz was only a
   one-sided ceiling, the STFT term stopped at 4.5 kHz and no texture term ran — the matcher never fitted the top end.
   Now `--matched mono` implies a clean reference (two-sided LTAS to 8 kHz, fizz terms on); `--ref-clean/--ref-mix`
   override it. The run log says which basis it used.
2. **Feel terms (Task A).** Tightness (60-250 Hz per-note t12 + sustain, palm-muted chugs from the DI), fizz (5-12 kHz
   ratio, flatness, HF envelope modulation, compared as distributions), polish (spectral flux, crest, inter-note floor).
   Off, with a recorded reason, wherever the reference can't support them (full mixes; floor needs a clean track).
3. **Search space (Task B, B2, B4).** Tight boost (modeled `pedal.ts`) in front of the amp; post-cab HP/LP with 12/24
   dB/oct; every cab swept, plus an analytic screen of the user's IR library (B3, thousands of IRs in ~1 s) and two-IR
   blends; gate threshold/hold/release/range matched to the reference's inter-note floor; studio-processing detection
   with an optional fast bus comp; pre-EQ before the drive.
4. **Honest A/B (Task C).** `listen/ref.wav` and `listen/render.wav` are BS.1770 loudness-matched, time-aligned, same
   30 s section, float WAV.
5. **Export notes (Task E).** Every NAM export lists what is not in the model (gate, cab IR for no-cab, post EQ, bus
   comp) with hardware settings and where it goes around the loader pedal.

## Feature definitions and weights (as shipped)

See the `feel.py` and `loss.py` docstrings for the exact definitions. Weights: tight 0.25, fizz 0.25, polish 0.125, each
normalised sub-term Huber-softened (delta = one normaliser: 20 ms t12, 3 dB sustain, 1.5 dB hfRatio W1, 0.03 flatness
W1, 0.1 HF-mod W1, 0.5 dB flux, 1.5 dB crest, 6 dB floor). Stage 2's first linear block is LTAS-only; feel enters from
the gain block on. Rationale: the first-pass 0.5/0.5/0.25 without Huber broke the blend known-answer CI test (0.753 vs
0.5 dB) because noise-level texture differences pulled the search off the spectral fit on a wrong combo; with the fix CI
is green. D.1 compared three weight sets (0.125/0.25/0.5 scales) on the synthetic and 6b fixtures: differences were
within search noise, so the weights stay until the user's A/B says otherwise.

## Synthetic results (D.1, shim, seed 1 asserted in CI)

Hidden chain = fixture amp + cab + `pedal.ts` boost + post HP/LP 24 dB/oct + gate above the DI floor; DI with synthetic
gaps at a −70 dBFS floor. Tolerances: A-weighted ≤ 0.5 dB, |Δt12| ≤ 10 ms, |Δsustain| ≤ 1.5 dB, hfRatio W1 ≤ 1.0 dB,
hfFlat W1 ≤ 0.02, flux W1 ≤ 0.3 dB, floor ≤ 3 dB.

| seed | A-wt dB | Δt12 ms | Δsus dB | hfRatio | hfFlat | flux | floor dB | pass |
|---|---|---|---|---|---|---|---|---|
| 1 | 0.224 | 0.33 | 0.29 | 0.34 | 0.008 | 0.072 | 0.41 | yes |
| 2 | 0.457 | 11.67 | 0.98 | 0.30 | 0.002 | 0.055 | 0.01 | no (t12) |
| 3 | 0.198 | 0.85 | 0.55 | 0.17 | 0.018 | 0.014 | 0.05 | yes |

Known limitation (accepted): seed 2 picks a slightly over-strong HPF (124 Hz/12 dB vs 86 Hz/24 dB) — the tightness term
is one-sided by design ("floppy" costs double) and A-weighting barely sees < 125 Hz. Real-audio check: the user's A/B.

Suspect contribution on the synthetic case (each suspect switched off with `--ablate`):

| variant | A-wt dB | Δt12 ms | Δsus dB | hfRatio | floor dB |
|---|---|---|---|---|---|
| full | 0.49 | 2.6 | 1.33 | 0.81 | 0.13 |
| no feel | 0.44 | 0.0 | 0.94 | 0.61 | 0.01 |
| no boost | 2.36 | 49.2 | 6.99 | 2.47 | 7.25 |
| no filters | 0.50 | 3.5 | 1.31 | 0.56 | 0.13 |

(Pre-fix table; the boost is the only suspect that matters on a chain built from the same captures. The feel term can't
show its value when the reference is a render of the same kind of chain — that's what the Bloodbath runs test.)

## Reviewer verdicts

| Task | Verdict | CI |
|---|---|---|
| A feel terms | ACCEPT after 3 REVISE rounds (full-mix gating, STFT cache, Huber + staging after the CI failure) | green (run 166) |
| C listening | ACCEPT (+2 should-fixes applied) | green |
| E export notes | ACCEPT after 1 REVISE (gate default, no non-schema keys) | green |
| B search space | ACCEPT after adaptations (export accepts modeled pedals; gate sweep honours clean-reference rule); combined-code fix (post.hp as post-CMA grid) | green (run 170) |
| B2.1 core hook | ACCEPT (gcc + clang 356/356) | green (run 170) |
| B3 IR library | ACCEPT after 1 REVISE (concurrent-write race on duplicate IRs; relative paths) | green (run 172) |
| D.1 + search fixes | ACCEPT (joint HP/LP × slope with re-polish, boost level fixed, gate tolerance, pedal Occam) | green (run 185) |
| B2.1 matcher, B2.3, B4 | REVISE (pre-EQ placement) — in progress | — |

## Open

- B2.1 matcher half / B2.3 / B4: pre-EQ moved after stage 2 on the refined winner; merge with B3 + D.1; re-review; CI.
- Bright-DI widening threshold (`diTilt > −1.5 dB/oct`) fires on the fixture DI; rebase on the user's DIs (printed by
  every run).
- TONE3000 `gears` value for IR tones ("cab" vs "ir") unverified against the live API (one constant, `IR_GEAR`).
- Follow-ups noted, not done: `plan.py` gate-default mismatch (`gate: {}` is enabled in the core); plugin IR preview
  does not model irMix offset/invert; `_refine_native` uses the left channel for a stereo `--matched mono` reference.

## Mac validation commands

(written at READY-FOR-USER-VALIDATION; see the final section of this report)

## User results

(pending)
