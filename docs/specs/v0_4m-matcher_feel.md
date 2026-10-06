# v0.4M — matcher: match the feel, not just the average tone (tightness, fizz, polish)

Source: first known-answer run (2026-10-06), Bloodbath "Zombie Inferno" NTM session, left rhythm DI matched
to the real HM-2 amp track of the same take (`--matched mono --offset-ms 0 --quick`, user's Mac):

- Result: A-weighted error 2.72 dB (unweighted 3.01, mean diff −1.62 dB); chosen single path HM-2W Custom →
  1986 JCM800 2204 → Marshall V30/G12T75 IR; blend lost (1.93 vs 1.66 screen loss).
- Guardrails failed: `gap_noise` −10.8 dB (rule ≤ −60), `fizz_texture` (flatness 5–10 kHz at ceiling).
  `lowTightnessMs` 102, crest 13.8 dB.
- **User's listening verdict:** "the rendered one is much less 'pro' sounding. The real one is more refined,
  less floppy, less fizzy overall."

So the current loss finds the average spectrum but not the feel. Owner: match-engineer (dsp-engineer only for
a renderer hook); reviewer on every task. The NTM audio stays on the user's Mac (never in git or the cloud):
build with synthetic and existing fixtures here; validation runs are done by the user with commands you hand
the lead, and the lead reports the numbers back.

## Task A — loss terms for feel, computed against the reference (not generic thresholds)

When the reference is a matched pair (`--matched`), add these as loss terms comparing render vs reference on
the aligned signal (weights documented and tuned in Task D):
1. **Low-end tightness:** per-note envelope decay and release in 60–250 Hz after palm-muted chugs (onsets from the
   DI), plus low-band sustain ratio. "Floppy" = slower decay / more sustain than the reference.
2. **Fizz:** 5–12 kHz energy relative to 1–4 kHz, spectral flatness 5–10 kHz, and HF envelope modulation (fizz
   is noise-like); measured per frame and compared as distributions, not averages.
3. **Polish / refinement:** short-term spectral flux and crest/compression vs the reference (proxy for a smoother,
   more controlled top and low-mid), and inter-note noise floor vs the reference's (the gate).
Without a matched pair, the same features enter as soft targets from the reference's guitar-dominant sections.

## Task B — a search space that can actually produce a tight, polished tone

- **Tightening boost:** single-path candidates also try a tight boost (TS-style capture or modeled `pedal.ts`,
  drive low, level high) in front of the amp; record whether it won.
- **Gate:** the gate threshold comes from the DI noise floor and is matched to the reference's inter-note floor
  (fixes `gap_noise`).
- **Post-cab filters:** HPF 60–140 Hz and LPF 6–11 kHz (12–24 dB/oct) in the search; real productions cut both.
- **Cab/IR breadth:** IR choice dominates fizz: sweep all cached IRs (and mic positions) for the top candidates,
  not only the screen winner's family.

## Task B2 — all four suspects are in scope (user, 2026-10-06: "need to do all 4")

After the user A/B'd both Bloodbath paths ("the real one just sounds better, more refined, more perfect"), the
four suspects are each a deliverable, not a hypothesis:
1. **Cab/IR:** besides the sweep above, allow a **two-IR blend** (two mics or cabs, level + phase-aligned) as pros
   do, and report which IR/mic choice won and by how much it moved the feel terms.
2. **Tightening and filtering:** the boost and HPF/LPF above, mandatory candidates, with their contribution
   reported.
3. **Feel scoring:** Task A.
4. **Studio processing:** detect it in the reference (dynamics + spectrum shape no capture + IR chain reaches),
   and when present, let the search use the chain's existing post EQ and bus comp (signal graph: post EQ → bus
   comp) to reproduce it. NAM export rule (CLAUDE.md): long-release compression stays out of trainable exports;
   a fast bus comp used here must be flagged in the preset so export can exclude or warn.
Report each suspect's measured contribution (feel terms + A-weighted error with it on vs off) on the synthetic
known answer and, after the user runs them, on Bloodbath HM2 and UBR.

## Task C — honest level for listening

Every listening render the matcher writes is loudness-matched to the reference (BS.1770 integrated, same
section), and the report says by how much; louder sounds "better", so A/B must be level-matched. Write a
`listen/` folder with `ref.wav`, `render.wav`, both level-matched and time-aligned, 30 s from the same section.

## Task D — validation loop with the user

1. Synthetic known answer here: render a DI through a hidden chain incl. tight boost + HPF/LPF + gate; the
   matcher must recover a chain whose new feel terms are within stated tolerances (and the old LTAS error).
2. Hand the lead exact Mac commands for: Bloodbath L DI → HM2 AMP and → UBR AMP (thorough), and R side as a
   held-out check. Report before/after: A-weighted error, each feel term, guardrails, chosen chain.
3. Success = the user's A/B (level-matched `listen/` files) no longer hears "floppy / fizzy / less pro", or names
   what is still off. Numbers alone do not close this phase.

## Acceptance

Unit tests for each feature (known signals: a slow vs fast decaying chug, white vs shaped HF noise); synthetic
recovery test; matcher still passes all existing tests and the 6b known-answer fixture within its tolerance;
`--quick` stays ≤ 5 min on 4 cores; full suite green (Python job + C++ jobs if touched). Report
`docs/specs/v0_4m-matcher_feel_REPORT.md` with reviewer verdicts, the feature definitions and weights, the
synthetic results, the Mac commands, and the user's results once run.
