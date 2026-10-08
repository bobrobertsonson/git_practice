# v0.7 — genre benchmark: report (Tasks A–B; baseline pending Task C)

Spec: `docs/specs/v0_7-genre_benchmark.md` (a76468f). Lead-pinned definitions: `docs/specs/v0_7-tasks.md`.
Branch: `claude/sawblade-v0_7-benchmark`, from the v0.4M head ff991ee. CI of record: run 287 on e60a9b5. All jobs are
green: python, linux-gcc + pluginval, linux-clang -Werror, macOS arm64 + auval + pluginval.

Status: **Tasks A and B are accepted by the reviewer. Task C (the user's baseline run) waits for v0.8 calibration.**

## What exists

- `docs/benchmark/cases.json`: the manifest (schema `sawblade.bench.cases` v1). It holds relative paths only, no audio.
  `docs/benchmark/BENCHMARK.md` is the human description and the user's commands.
- `sawblade-bench run`: one run of the matcher over the cases (package `match/sawblade_match/bench/`).
  - For each case, the matcher sees only the fit section, which the runner crops into `<out>/<case>/fit/`.
  - The winning preset is rendered on the held-out section and scored there.
  - For a blend case, each path is checked alone, and the R-side transfer pair is scored.
  - It writes `scores.json`, `scores.md` and level-matched `listen/<case>/{ref,render}.wav` (in `--out`, never the repo), and prints a one-line total.
  - `--check-only` checks that the files exist before any matching.
- `sawblade-bench compare A.json B.json`: per-case deltas, a regression flag, then
  `compare: better on N, worse on M, same on K, not compared on J`. Exit 1 if any case is flagged worse.
- Scoring reuses the matcher's code and does not re-implement it:
  - `pathcheck._Analyses.error`: the run's `aWeightedErrorDb`;
  - `pathcheck.feel_and_ltas` and `known_answer.feel_deltas`;
  - `run._guardrails`;
  - `refsum.blend_refs`, polarity auto;
  - `pathcheck.single_preset`.
  No existing matcher function was changed. The only edit to an existing file is the `sawblade-bench` line in `match/pyproject.toml`.

## Case list

8 cases count toward the total:

| id | style | kind |
|---|---|---|
| **bloodbath_blend** (primary) | HM-2 death metal, blend of DI 17 → 18 HM2 + 19 UBR | blend, topology auto |
| bloodbath_mz | death metal, third amp | single |
| immortal_disfig | deathcore / slam | single |
| veil_of_maya | djent / metalcore | single |
| sylosis_57 | thrash (mic'd amp) | single |
| sylosis_reamp | thrash (second engineer's reamp) | single |
| haunted | Swedish thrash | single |
| jinjer | groove / prog metalcore (stem, left channel; weaker) | single |

These cases are run and scored but do not count toward the total:

- `bloodbath_blend_forced`: the same files with `topology: blend`, added by the main lead on 2026-10-08. It gives a per-path answer on every run.
- `bloodbath_hm2` and `bloodbath_ubr`: path checks, parented to the forced-blend case.
- Tier 2, 12 reference-only cases, each with a stand-in DI: they are shown in `compare` and never flagged.

**Not covered: black metal, doom/sludge (fuzz), crust/hardcore, and a clean-ish rhythm control.** Nothing More, the clean-ish control, is tier 2 only, and there is no DI pair for it. Until those gaps are filled, the benchmark does not speak for those styles.

**File names:** only the Bloodbath files 17–22 are `confirmed: true`. Every other session folder and file name is inferred from the listing, so the user must run `--check-only` before the baseline.

## Sections, determinism, runtime

- **Sections:** `fit` and `heldOut` can be pinned per case. When they are `null`:
  - fit = the first half of the active DI-reference overlap;
  - held-out = the densest 30 s of the second half.
  The resolved sections are written to `scores.json`.
- **Determinism:** the same inputs, seed and `--threads` give an identical `scores.json`, apart from `timings` and `runtimeS`. This is tested, as the main lead asked.
- **Runtime budget:**
  - `--quick` takes ≤ 5 min per case on 4 cores (the matcher's budget), plus ≤ 30 s per case for scoring, path checks, transfer and listen.
  - A tier 1 quick run is ≤ 45 min.
  - Thorough runtime will be measured by the user in Task C.
  - CI: the new tests take 160 s locally, against a 4-minute budget. The CI python job took about 34 min in run 287, against its 45-minute limit.

## Regression thresholds (`compare`, B − A, lower is better)

| metric | flagged if B − A > |
|---|---|
| held-out A-weighted error | 0.30 dB (provisional) |
| t12 | 10 ms |
| sustain | 1.5 dB |
| hfRatio | 1.0 dB |
| hfFlat | 0.02 |
| flux | 0.3 dB |
| floor | 3.0 dB |

- The feel thresholds are the D.1 known-answer tolerances.
- A case is "better" only if its A-weighted error improves by more than 0.30 dB and nothing regresses.
- Path checks and transfer pairs are flagged the same way.
- Newly failing guardrails are listed, not flagged.
- The 0.30 dB value was accepted by the main lead and is to be revisited after a second-seed repeat in Task C.

**Policy:** a matcher change that regresses any case beyond its threshold needs the lead's explicit sign-off, together with the user's listening verdict.

## Synthetic results (CI, fixture captures)

The DI is the fixture riff, looped to 13 s. The fit section is 0–6 s and the held-out section 6–12 s; seed 5.

| case | fit A-wt dB | held-out A-wt dB | tolerance |
|---|---|---|---|
| single-path hidden chain (found as single) | 0.191 | 0.185 | ≤ 0.5 |
| blend hidden chain through refsum | 0.292 | 0.288 | ≤ 0.5 |

- **Single-path feel:** the held-out feel deltas of the single case are all well inside the D.1 tolerances: t12 2.9 ms, sustain 0.63 dB, hfRatio 0.26 dB, hfFlat 0.001, flux 0.004 dB, floor 0.07 dB.
- **Blend path checks:** the matcher reached the blend sum with different amp captures, so each path alone is far off: A 6.12 dB, B 3.19 dB, ratio diff −7.9 dB.
  - The test now raises a visible warning when this happens, instead of quietly turning off its strict per-path check.
  - An oracle test feeds the hidden preset in as the winner. It scores < 0.05 dB on the blend, on both paths and on the ratio, so the harness is correct.
  - **The blend's non-uniqueness belongs to the matcher, and it is exactly what the forced-blend Bloodbath path checks will show on real audio.**

## Reviewer verdicts

| commit | verdict |
|---|---|
| f82bcf7 (Tasks A+B) | ACCEPT with no must-fix. These departures from the spec were judged acceptable: the final-offset rule, the `skipped` status, path-check children carrying the parent's numbers, extra kwargs on `run_bench`, tier 2 scoring, compare counting only tier 1, and importing private helpers |
| e60a9b5 (should-fixes 2, 3) | ACCEPT |

**Deviation worth knowing:** the held-out DI offset is not `pathcheck.stored_offset`.

- `stored_offset` takes the run's final full-length refinement with no acceptance rule; on the synthetic case it was −204 samples off.
- `bench.runner.final_offset` uses that refinement only when its peak is accepted (`fineAccepted`, or envelope ratio ≥ 2.0, the rule in `run.py`). Otherwise it uses the starter offset, and failing that 0.
- `scores.json` records which source was used, in `offset.refinementSource`.

## Open (non-blocking)

1. **CI margin:** the new tests have about 1.5× headroom (160 s against 240 s). The python job's total is 34 of 45 min.
2. **Offset rule on real audio:** `final_offset` has only been checked on synthetic data.
   - In the first real `scores.json`, check `offset.refinementSource` and `refinementSamples48`.
   - Look especially at Jinjer (a stem) and the reamp cases.
3. **Compare table:** it does not show the blend-ratio `diffDb`, which is in the compare JSON only.
4. **Base drift:** this branch is based on v0.4M ff991ee, and v0.4M has since moved to bfad9ae (reporting-only changes). There are no shared files, so a merge is expected to be clean.
5. **Questions for Task C:**
   - Is a `--tier all` run acceptable once `--check-only` has confirmed the inferred names?
   - The stand-in DI choice for each tier 2 case.

## Proposals (not implemented)

- Print the topology margin between the forced blend and auto in `scores.md`.
- `compare --only-counted`.
- `run --resume`, which skips cases already in `scores.json` (useful for the long thorough run).
- An optional second seed, to calibrate the A-weighted threshold.
- Make the matcher's blend search report the non-uniqueness, for example per-path confidence, since the synthetic blend shows the sum can match while the paths do not.

## Baseline — pending Task C

This section waits for v0.8 calibration to merge. The lead then hands the user the exact Mac commands:

1. `--check-only`;
2. a tier 1 quick run, then a thorough run;
3. a repeat with a second seed.

The baseline table (case, held-out A-weighted error, feel deltas, guardrails, chosen chain) and the user's listening notes go here. No audio is committed.
