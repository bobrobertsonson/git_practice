# Phase 6b: matcher under 5 minutes (and progress for the plugin)

Today a full match takes 20-35 min on 4 shared cores (v4: 36.5 min). Target: **best result in
≤ 5 min** on 4 cores with the current 77-capture pool, and no loss of quality: the final
A-weighted error is within 0.15 dB of today's full search on the original/v4 and cover/v3
references, and the stem-basis fizz acceptance still holds.

## Work (match-engineer)
1. **Profile first.** Report the time per stage: reference prep, excerpt, stage-1 pair
   renders, stage-2 CMA-ES, offset refinement, final renders, listening files.
2. **Levers.** Try these and keep the ones that pay:
   - render the pair product in parallel processes (ProcessPoolExecutor, `--jobs`);
   - shorter stage-1 excerpts with a two-pass screen (coarse 2 s, then 6 s on the top 10%);
   - cache NAM renders per (capture, gain) on the excerpt, so blends reuse path renders
     (verify this is already done, and extend it if not);
   - early-stop CMA-ES on plateau;
   - skip listening-file renders unless asked (`--listen`);
   - a blend-aware prescreen (pair scoring on a short excerpt) so the cap can stay on without
     losing blend partners. Report recall vs the full search as in 3.3.
3. **`--progress-json PATH`** writes `{stage, fraction, etaSeconds, bestErrorDb, message}`
   atomically at least once per second, for the plugin's progress bar (6a).
4. **`--quick`** preset = the fast settings above. The full search stays available as
   `--thorough`.

## Acceptance
- A timing table, before and after, for original/v4 and cover/v3 with the same seed.
- The quality deltas above.
- Full pytest passes (counts printed). Tests for progress-json and quick mode.
- Use `nice`. Don't run more than one full match at once, because the NAM training job may
  still be running.

## Amended acceptance (lead, after measurement)
`--quick` is preview quality: at most ~5 min CPU-equivalent on 4 cores, within 1 dB A-weighted of `--thorough` on both references, fizz metrics unchanged. The 0.15 dB target was not met; see `phase6b_matcher_speed_REPORT.md`. `--thorough` remains the default final match.
Listening files are written only with `--listen` in both modes (intentional default change); the R render is always made with `--di-r` for the clip guard.
