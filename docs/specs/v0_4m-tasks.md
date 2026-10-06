# v0.4M — task specs (lead decisions for `v0_4m-matcher_feel.md`)

Phase spec: `docs/specs/v0_4m-matcher_feel.md`. This file pins the definitions, files and tolerances the phase spec leaves
open. Owner: match-engineer. Reviewer on every task. Where this file and the phase spec disagree, the phase spec wins and the
implementer raises it under "Decisions / questions for lead".

## Boundaries (parallel work)

- v0.4A edits `match/sawblade_match/calibrate/` (pedal-fit, metrics). **Do not edit `calibrate/` or `tonecheck/`** in this
  phase. Reuse their functions (`tonecheck.analysis.detect_onsets`, `gap_regions`, ...) by import only. If a tonecheck
  change seems needed, copy the logic into a matcher module and note it.
- Never touch `plugin/`. C++ (`core/`, `bindings/`) only through dsp-engineer and only if a renderer hook is unavoidable
  (none is expected: the gate, `pedal.ts`, `eq` blocks and post-EQ already exist in the preset schema).
- The local container cannot install Python packages (pypi.org blocked). **CI's `python` job is the test of record.** Write
  the tests so that they pass first time; the lead pushes and reads CI.

## Task A: feel features and loss terms

New module `match/sawblade_match/matcher/feel.py` (pure numpy/scipy, 48 kHz, no tonecheck edits). `loss.py` gains a
`feel` term built from it. All features are computed on the same excerpt and active mask as the existing loss.

### A.1 Low-end tightness (60-250 Hz)
- Band: 4th-order Butterworth band-pass 60-250 Hz (sos, zero-phase not needed). Envelope: RMS over 5 ms frames, hop
  2.5 ms, in dB.
- Notes: DI onsets (`detect_onsets`, the excerpt timeline). Note window = onset to the next onset, capped at 400 ms. A note
  counts if its window is >= 80 ms and its low-band peak (first 30 ms) is within 30 dB of the loudest note's peak.
- Per note: `t12` = ms from the peak until the envelope first falls 12 dB below the peak (censored at the window end);
  `sustainDb` = mean power in [peak + 40 ms, peak + 120 ms] (clipped to the window) re peak power, in dB.
- **Chugs** = notes whose *DI* low-band `t12` <= 150 ms (palm-muted). Use chugs if >= 3, else all counting notes; record
  which (`noteSet: "chugs"|"all"`) and the counts. Fewer than 3 notes: term dropped and recorded (as `decay` does today).
- Matched pair: per-note differences on the same onsets. `tight = median(asym(t12_out - t12_ref)) / 20 ms +
  median(asym(sustain_out - sustain_ref)) / 3 dB`, where `asym(d) = d` for d > 0 (floppier than the reference) and
  `0.5 * |d|` for d <= 0.

### A.2 Fizz (per frame, compared as distributions)
- Frames: 2048-pt Hann, hop 1024, frames >= 80 % active.
- Per frame: `hfRatioDb` = 10 log10(E[5-12 kHz] / E[1-4 kHz]); `hfFlat` = spectral flatness 5-10 kHz; `hfMod` = coefficient
  of variation of the 5-12 kHz Hilbert envelope (after a 1 kHz low-pass on the envelope) within the frame.
- Distance per feature = 1-D Wasserstein W1 estimated as the mean |quantile difference| at q = 0.05, 0.10, ..., 0.95.
- `fizz = W1(hfRatioDb) / 1.5 dB + W1(hfFlat) / 0.03 + W1(hfMod) / 0.1`.
- Off when the reference HF is not usable: the full-mix fallback basis (`hf_limit_hz` set) or a matched channel limited by
  `stft_fmax < 12 kHz`. **Check and report** which basis the user's NTM run takes (`--matched mono` on a clean amp track, no
  stems): the fizz term must be ON there. If the current code treats that file as a mix, add the smallest fix (e.g. a
  `--ref-clean` flag or automatic detection) and document it.

### A.3 Polish
- `flux`: per-frame mean |delta dB| between consecutive 1024-pt log-magnitude frames (hop 512), bins 300 Hz-8 kHz, active
  frames; W1 vs the reference / 0.5 dB.
- `crest`: per 400 ms active window (hop 200 ms), peak/RMS in dB; W1 / 1.5 dB.
- `floor`: inter-note level = output power in the DI gap regions (`gap_regions` on the DI) re the output's active power (dB),
  same for the reference. One-sided: `max(0, d) / 6 dB + 0.25 * max(0, -d) / 6 dB`, d = out - ref. Dropped (recorded) if the
  excerpt has < 100 ms of gaps.
- `polish = flux + crest + floor`.

### A.4 Loss integration
- `feel = W_TIGHT * tight + W_FIZZ * fizz + W_POLISH * polish`, initial weights 0.5 / 0.5 / 0.25 (tuned in D.1; documented in
  the `loss.py` docstring and README). Added to `total`. `LossResult` gains `feel` (weighted) and `feelTerms` (every
  sub-term, raw and normalised, the note counts and noteSet). `result.json` reports them for best, alts and the starter.
- Without a matched pair (soft targets): same features from the reference's guitar-dominant excerpt, onsets detected on the
  reference itself for tightness, all comparisons as W1 of the per-note / per-frame distributions, every weight x 0.5.
- Stage 1's cross-spectral pair x pair blend screen stays LTAS-only (no render). The feel term enters everywhere the full
  loss is evaluated (re-score, cab sweep, stage 2, finals).
- Cost: report the time per `evaluate()` call on a 6 s excerpt before and after. The target is at most 1.5x.
- Unit tests (synthetic, no captures): slow vs fast decaying low chug, so the floppy one scores worse and `t12` is within
  5 ms of the analytic value; white vs shaped (harmonic, low-passed) HF content, so the fizz features order correctly and an
  identical pair scores 0; a gated vs ungated gap, so `floor` orders correctly; gain invariance (+6 dB = same feel); the
  term is dropped with < 3 notes.

## Task B: search space

- **Tight boost** (single-path candidates): for every single combo re-scored in stage 1 (and refined in stage 2), also
  score a variant with the modeled `pedal.ts` (slot `boost`, `modelVersion` 1) directly in front of the amp (after any
  pedal). Its params enter stage 2's gain group: `drive` 0-3, `level` 6-10, `tone` 3-8 (preset defaults 1/8/5). The boost
  variant competes as its own candidate. Occam: the boost costs like one extra block (prefer no boost within 0.1 dB).
  `result.json` records `tightBoost: {tried, won, params}`. No capture of a TS is required (any `drive` capture already
  competes through `single2`).
- **Gate matched to the reference:** after stage 2, on the final chain, sweep threshold = DI floor + {4, 8, 12, 16, 20} dB
  x release {80, 150, 250} ms. Pick the one minimising `floor` (A.3) subject to the LTAS error rising <= 0.05 dB and the
  tightness term not getting worse. Without a matched pair, use the reference's own inter-note floor (soft). Record the
  sweep. Also **diagnose the Bloodbath `gap_noise` −10.8 dB** on a synthetic case: is the gate not closing (threshold),
  not reached (gap detection), or is it the post-gate chain (NAM noise, cab tail)? Fix what is the matcher's to fix and
  report the rest.
- **Post-cab filters** (post EQ, after the shared cab): `post.hp` 60-140 Hz and `post.lp2` 6-11 kHz, slope 12 or 24 dB/oct
  (24 = two cascaded biquads, Butterworth Qs), slope a discrete param per filter (continuous [0, 1] thresholded at 0.5).
  Neutral defaults (hp 60 Hz/12 dB, existing `post.lp` stays). They do not count toward the EQ regulariser. Both must
  survive the preset round trip (`eq` band types `highPass`/`lowPass` already exist).
- **Cab/IR breadth:** for the top 3 candidates per topology (after stage 2), sweep **every** cab in the pool (all IRs and
  mic positions) with the full loss, re-using the memoised NAM output (IR swap = linear stage only). Then re-run the
  last linear CMA-ES block on the winner if the cab changed. Record the sweep (n cabs, best/worst, whether the cab changed).
- `--quick` must stay <= 5 min on 4 cores for the real pool: give the Mac command that measures it; locally show the added
  cost per candidate in timings.

## Task C: honest level for listening

- BS.1770-4 integrated loudness (K-weighting, 400 ms blocks, 75 % overlap, -70 LUFS absolute + -10 LU relative gate) in
  `match/sawblade_match/matcher/loudness.py` (numpy/scipy). Unit tests, within 0.1 LU: a mono 997 Hz sine at -20 dBFS
  peak = -23.0 LUFS; EBU Tech 3341 case 1 (stereo 1 kHz sine, -23 dBFS peak each channel) = -23.0 LUFS; a gated case (tone
  plus -80 dBFS silence segments) is unaffected by the silence. No new dependency.
- With `--listen` and a reference: write `listen/ref.wav` and `listen/render.wav` (float WAV, 48 kHz, mono for a matched mono
  pair), 30 s from the same section (the guitar-dominant 30 s, or the whole thing if shorter), time-aligned with the found
  offset, the render loudness-matched to the reference (integrated LUFS over that same section). Also `listen/before.wav`
  (the starter) when it exists. No peak normalisation, float so nothing clips; report true-peak (4x oversampled) of each.
- Every other listening file the matcher writes is loudness-matched to the reference the same way (no peak normalisation).
- `result.json -> listening`: `{section: [s0, s1], lufsRef, lufsRenderRaw, gainDb, lufsBefore?, gainBeforeDb?, offsetMs,
  truePeakDb: {...}}`; the run log prints "render was X dB louder/quieter than the reference before matching".

## Task D.1: synthetic known answer (CI)

- Extend `known_answer.py` and the tests: hidden chain = fixture captures + `pedal.ts` boost + post HPF/LPF (24 dB/oct) +
  a gate with threshold above the DI floor; the DI fixture gets inter-note gaps with a realistic noise floor (synthetic
  noise added in the test, -70 dBFS).
- Pass (all on the whole section): A-weighted LTAS error <= 0.5 dB (unchanged); tightness median |delta t12| <= 10 ms and
  |delta sustain| <= 1.5 dB; fizz W1(hfRatioDb) <= 1.0 dB, W1(hfFlat) <= 0.02; flux W1 <= 0.3 dB; floor |d| <= 3 dB.
  Report the same numbers with the feel term off (weights 0) to show what it buys; that run does not have to pass.
- Weights: run the synthetic case (and the 6b fixture) for at least 3 weight sets, pick the set, document why. Real tuning
  comes from the user's run (D.2/D.3).
- Existing tests incl. the 6b known-answer fixtures stay within their tolerances.

## Report

`docs/specs/v0_4m-matcher_feel_REPORT.md`: reviewer verdicts per task, feature definitions + weights, synthetic results,
timings, the Mac commands (lead writes those), user results when back.
