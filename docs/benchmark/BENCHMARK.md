# Sawblade genre benchmark

The matcher is judged on many heavy tones, not one. A change that improves one case (Bloodbath, HM-2) can silently worsen
another (thrash). `sawblade-bench run` scores the matcher on a fixed set of cases; `sawblade-bench compare` shows, for two runs,
which cases got better, worse or stayed the same. The manifest `docs/benchmark/cases.json` is the source of truth for the case
list; this file explains it. Phase spec: `docs/specs/v0_7-genre_benchmark.md`, task spec: `docs/specs/v0_7-tasks.md`.

## Tiers

- **Tier 1: known-answer pairs.** A DI and the recorded amp track(s) of the same take. The reference is the real answer, so the
  score is strict and the counted cases make up the total. A `blend` case's reference is the sum of two amp tracks of the same
  DI (the album tone is usually a blend); a `single` case has one amp track.
- **Tier 2: reference only.** An amp track without a DI of its own take. The matcher runs unmatched against it with a stand-in DI
  (a DI-only session). Informative, never pass/fail, not counted.

`pathcheck` cases are not matcher runs: they score the per-path renders of a blend case (path A alone against the isolated A
track, path B alone against the isolated B track). The parent of the Bloodbath path checks is `bloodbath_blend_forced`, the same
files as `bloodbath_blend` with the topology forced to blend, so the per-path answer exists on every run whatever `auto` picks.
`bloodbath_blend` (topology `auto`) also reports its own path checks whenever it picks a blend.

## Cases

| id | tier | style | kind | DI | reference | transfer (R side) | counts |
|---|---|---|---|---|---|---|---|
| `bloodbath_blend` | 1 | HM-2 death metal, two-amp blend | blend | 17 GTR RHY L DI | 18 GTR RHY L HM2 AMP + 19 GTR RHY L UBR AMP (sum) | 20 GTR RHY R DI / 21 GTR RHY R HM2 AMP + 22 GTR RHY R UBR AMP (sum) | yes |
| `bloodbath_blend_forced` | 1 | HM-2 death metal, two-amp blend (blend forced) | blend | 17 GTR RHY L DI | 18 GTR RHY L HM2 AMP + 19 GTR RHY L UBR AMP (sum) | 20 GTR RHY R DI / 21 GTR RHY R HM2 AMP + 22 GTR RHY R UBR AMP (sum) | no |
| `bloodbath_hm2` | 1 | HM-2 death metal, saw path alone | pathcheck | (parent's) | path a of `bloodbath_blend_forced` | - | no |
| `bloodbath_ubr` | 1 | death metal, body path alone (non-HM-2) | pathcheck | (parent's) | path b of `bloodbath_blend_forced` | - | no |
| `bloodbath_mz` † | 1 | death metal, third amp (non-HM-2) | single | 23 GTR RHY C DI | 24 GTR RHY C MZ AMP | - | yes |
| `immortal_disfig` † | 1 | deathcore / slam | single | 20 GTR DI L | 22 GTR REAMP L | 21 GTR DI R / 23 GTR REAMP R | yes |
| `veil_of_maya` † | 1 | djent / metalcore | single | 25 GTR L DI_1 | 27 GTR L_1 | 26 GTR R DI_1 / 28 GTR R_1 | yes |
| `sylosis_57` † | 1 | thrash (mic'd amp) | single | 27 GUITAR DI A left main | 31 SCOTT 57 A left main | 30 GUITAR DI A right main / 33 SCOTT 57 A right main | yes |
| `sylosis_reamp` † | 1 | thrash (second engineer's reamp) | single | 27 GUITAR DI A left main | 43 JOSH REAMP A Left main 1 | 30 GUITAR DI A right main / 44 JOSH REAMP A Right main 1 | yes |
| `haunted` † | 1 | Swedish thrash | single | 25-GT 1 DI | 26-GT 1 | - | yes |
| `jinjer` † | 1 | groove / prog metalcore | single | 24 Gtr Rtm L DI | 23 Gtr Rtm STEM | 25 Gtr Rtm R DI / 23 Gtr Rtm STEM (right) | yes |
| `atg_gt1` † | 2 | melodic death metal (Swedish) | single | GTR DI | 21-GT 1 | - | no |
| `atg_gt1_hm` † | 2 | HM-2 variant, melodic death metal | single | GTR DI | 22-GT 1 HM | - | no |
| `knocked_loose` † | 2 | metallic hardcore | single | GTR DI | 064 RHY L | - | no |
| `gojira` † | 2 | groove / progressive death metal | single | GTR DI | 18 Gtr 1 Mic | - | no |
| `meshuggah` † | 2 | djent (extended-range) | single | GTR DI | 15. RHYTHMGUITAR L | - | no |
| `allagaeon` † | 2 | technical melodic death metal | single | GTR DI | 16 Rhy Gtr 1 | - | no |
| `opeth` † | 2 | progressive death metal | single | GTR DI | 26 Rhythm 1 Left | - | no |
| `daath` † | 2 | technical death metal | single | GTR DI | G RHY 1 L_01 | - | no |
| `slamadeus` † | 2 | slam / brutal death metal | single | GTR DI | Guitar Left | - | no |
| `face_yourself` † | 2 | metalcore | single | GTR DI | 07_rhy_01 L | - | no |
| `vesta` † | 2 | modern progressive metal | single | GTR DI | 09 GTR - RHY a | - | no |
| `nothing_more` † | 2 | clean-ish rhythm control (less saturated) | single | GTR DI | 16 RTM Gtr 1 | - | no |

`†` = the file names are inferred, not taken from your listing (`confirmed: false`); `--check-only` prints them. The Bloodbath
files 17-22 are known (`scripts/run_v04m_validation.sh`). Counted cases: 8 (the Bloodbath blend and seven single-amp cases).
Tier 2 stand-in DIs: Cognizance, Reflections or Trees on Mars (recorded in each case's notes); their file names are guesses
until you confirm them.

### Gaps (not covered yet)

- **black metal**: no source yet; ask the user for a multitrack
- **doom / sludge / stoner (fuzz)**: no source yet; ask the user for a multitrack
- **crust / hardcore**: no tier 1 source yet (Knocked Loose is tier 2, reference only)
- **clean-ish rhythm control**: no DI pair: Nothing More is tier 2 only

Until these are filled the benchmark says nothing about them.

## Sections: what the matcher sees and what is scored

For every tier 1 case the runner crops the DI and the reference (offset applied) to the **fit** section and writes them to
`<out>/<case>/fit/`; the matcher runs on those files only. The **held-out** section never reaches the fit. Sections are
`[startS, endS]` in DI seconds (tier 2: reference seconds) and are pinned in the manifest or chosen automatically: on the usable
DI-reference overlap, the active region is the first to the last 100 ms frame within 40 dB of the loudest; `fit` is its first
half and `heldOut` the densest `heldOutMaxS` (30 s) of its second half (the matcher's own frame-energy rule,
`excerpt.select_excerpt`). The resolved sections are written to `scores.json`, so you can pin them later. Deterministic.

## Scores (`scores.json`, schema `sawblade.bench.scores` v1)

Per case: the chosen topology and captures; `fitAWeightedErrorDb` (the run's own number on the fit section); and, on the
held-out section, the full-length render of the winning preset cut to the held-out window and compared with the reference:

- `aWeightedErrorDb`: the tonecheck A-weighted LTAS error, as the matcher reports it (`pathcheck._Analyses.error`);
- `ltasLossDb`: the matcher loss's LTAS term with the level offset removed (`pathcheck.feel_and_ltas`);
- `feel`: the weighted `tight` / `fizz` / `polish` / `total` terms of `feel.evaluate` and the raw deltas of
  `known_answer.feel_deltas` (`t12Ms`, `sustainDb`, `hfRatioDb`, `hfFlat`, `fluxDb`, `floorDb`); `null` where the reference
  cannot support a term (a stem or full mix drops some);
- `guardrails`: the derived profile's rules on the held-out render (`run._guardrails`);
- `pathchecks` (blend winners): the same numbers for path A and path B alone against the isolated tracks, plus the blend ratio
  (`chosenDb` = LUFS A alone - LUFS B alone, `refDb` = the same on the tracks at their faders, `diffDb`);
- `transfer`: the same numbers for the other-take pairs (the R side), scored and never fitted: the winning preset is rendered on
  the transfer DI, the offset searched as `pathcheck` does for a foreign DI, the held-out section the automatic one of that pair.

All distances are "lower is better". `listen/<case>/ref.wav` and `render.wav` are the held-out section, time-aligned, the render
gained to the reference's BS.1770 loudness (the gain is recorded), 48 kHz float WAV, written into `--out` only. Timings
(`timings`, `runtimeS`) are the only non-deterministic fields.

## Regression thresholds (`compare`, B minus A)

| metric | regression if B - A is above |
|---|---|
| held-out `aWeightedErrorDb` | 0.30 dB |
| `t12Ms` | 10 ms |
| `sustainDb` | 1.5 dB |
| `hfRatioDb` | 1.0 dB |
| `hfFlat` | 0.02 |
| `fluxDb` | 0.3 dB |
| `floorDb` | 3.0 dB |

The feel thresholds are the known-answer tolerances (`known_answer.FEEL_TOLERANCES`): a term that moves by a whole tolerance is
worth a listen. The A-weighted bar is provisional (the lead's noise estimate; the benchmark fixes the seed): revisit it once a
case is repeated with a second seed. A case is **worse** if any metric (its held-out numbers, its path checks, its transfer
pairs) regresses, **better** if the A-weighted error improves by more than 0.30 dB and nothing regresses, otherwise **same**.
Tier 2 is shown, never flagged. A case missing or failed on either side is "not compared". Newly failing guardrails are listed.
The thresholds live in one dict (`bench/compare.py`) and are printed in every comparison; `--threshold KEY=V` overrides and is echoed.

Policy: a matcher change that regresses any case beyond a threshold needs the lead's explicit sign-off with your listening verdict.

## Runtime budget

`--quick`: at most 5 minutes per case on 4 cores (the matcher's own quick budget) plus at most 30 s per case for scoring, path
checks, transfer pairs and the listen files; tier 1 quick (8 counted cases and the forced blend): about 45-50 minutes. `--thorough`
(default): measured by you in Task C. The tests of the harness together take under 4 minutes.

## Audio and licences

No audio is ever committed: the manifest holds relative paths only, the tests make their audio in a temp directory from the
committed fixture captures, and `listen/` and all run directories are written under your `--out`. The multitracks are third-party
material used for personal evaluation only (`docs/TEST_SOURCES.md`, CLAUDE.md); never redistribute them.

## Commands

```
# 1. check the files (names marked with a dagger are guesses until this says ok)
sawblade-bench run --root <NailTheMix folder> --check-only --out /tmp/bench

# 2. run tier 1 (add --quick for the fast preset, --tier all for tier 2 as well, --cases bloodbath_blend,jinjer for some)
sawblade-bench run --root <NailTheMix folder> --pool <pool_manifest.json> \
    --ir-dir <your IR folder> --quick --seed 0 --out <work dir>/bench/baseline

# 3. after a matcher change, run again into another folder and compare (exit 1 if any case is worse)
sawblade-bench compare <work dir>/bench/baseline/scores.json <work dir>/bench/candidate/scores.json
```

`run` exits 0 when it ran (missing cases are skipped and counted as `n of m` in the last line), 2 on bad arguments or an unreadable
manifest. `--check-only` exits 2 if any file is missing. `compare` exits 1 if any case is worse, 2 on unreadable input or a
schema mismatch. Use the same `--seed`, `--threads` and `--quick`/`--thorough` for both runs: the same inputs give identical scores.
