# v0.7 — genre benchmark: the matcher is judged on many heavy tones, not one

Source: user decision 2026-10-07 ("4a: yes"). Today the matcher's only real known answer is Bloodbath "Zombie Inferno"
(HM-2 + Überschall). CLAUDE.md requires that Sawblade work for ANY heavy tone; a change that improves Bloodbath can
silently worsen thrash. Owner: match-engineer; reviewer on every task. The audio stays on the user's Mac (never in git
or the cloud); this phase builds the harness here with synthetic fixtures and the user runs it.

## Task A — benchmark definition

- `docs/benchmark/BENCHMARK.md` + a manifest `docs/benchmark/cases.json` (committed: names, styles, file *paths on
  the user's Mac* relative to a root they choose, offsets, which track is DI / which is the reference; no audio).
- Target 8–10 matched pairs (DI + the recorded amp track of the same take), at least one per style: HM-2 death metal,
  non-HM-2 death metal, thrash, black metal, doom/sludge (fuzz), metalcore/djent, crust/hardcore, plus one clean-ish
  rhythm control. Sources: the user's NTM sessions already on the Mac (Bloodbath, Immortal Disfigurement, Veil of
  Maya, Sylosis) and multitracks the user adds; the lead fills gaps by asking the user (`docs/TEST_SOURCES.md` terms).
- Each case has a held-out section (not used for fitting) for scoring.

### Lead decision: case list from the user's NTM library (listing 2026-10-07, `/Users/notsch/Desktop/NailTheMix`)

**Tier 1, known-answer pairs (DI + amp track of the same take; scored strictly):**

| case | session | DI | reference | style |
|---|---|---|---|---|
| bloodbath_hm2 | Bloodbath | 17 GTR RHY L DI | 18 GTR RHY L HM2 AMP | HM-2 death metal |
| bloodbath_ubr | Bloodbath | 17 GTR RHY L DI | 19 GTR RHY L UBR AMP | death metal, non-HM-2 |
| bloodbath_mz | Bloodbath | 23 GTR RHY C DI | 24 GTR RHY C MZ AMP | death metal, third amp |
| immortal_disfig | Immortal Disfigurement | 20 GTR DI L | 22 GTR REAMP L | deathcore / slam |
| veil_of_maya | Veil of Maya | 25 GTR L DI_1 | 27 GTR L_1 | djent / metalcore |
| sylosis_57 | Sylosis | 27 GUITAR DI A left main | 31 SCOTT 57 A left main | thrash (mic'd amp) |
| sylosis_reamp | Sylosis | 27 GUITAR DI A left main | 43 JOSH REAMP A Left main 1 | thrash (second engineer's reamp) |
| haunted | The Haunted | 25-GT 1 DI | 26-GT 1 | Swedish thrash |
| jinjer | Jinjer | 24 Gtr Rtm L DI | 23 Gtr Rtm STEM (left channel) | groove / prog metalcore (stem: may include processing; weaker) |

Held-out checks: the R side of each pair where it exists (Bloodbath 20/21/22, Immortal Disfigurement 21/23, Veil of
Maya 26/28, Sylosis 30/33 and 44, Jinjer 25).

**Tier 2, reference only (amp tracks without a DI of the same take; scored as unmatched references with a stand-in
DI, informative, not pass/fail):** At the Gates (`21-GT 1` and `22-GT 1 HM`, an HM-2 variant), Knocked Loose
(`064 RHY L`), Gojira (`18 Gtr 1 Mic`), Meshuggah (`15. RHYTHMGUITAR L`), Allagaeon (`16 Rhy Gtr 1`), Opeth
(`26 Rhythm 1 Left`), Daath (`G RHY 1 L_01`), Slamadeus (`Guitar Left`), Face Yourself (`07_rhy_01 L`), Vesta
(`09 GTR - RHY a`), Nothing More (`16 RTM Gtr 1`, the less-saturated control).

DI-only sessions (Reflections, Cognizance, Trees on Mars) are stand-in DIs for Tier 2, not cases.

**Gaps (no source yet): black metal, doom/sludge (fuzz), crust.** The lead asks the user for other multitracks;
until filled, the REPORT states the benchmark does not cover them.

## Task B — runner and score

- `sawblade-bench run --root DIR [--cases …] [--quick|--thorough] --out DIR`: runs the matcher on each case's fit
  section, renders the held-out section, scores it (A-weighted LTAS error, the v0.4M feel terms, guardrails), writes
  `scores.json` + a markdown table + level-matched `listen/` pairs per case, and a one-line total.
- `sawblade-bench compare A.json B.json`: per-case deltas with a regression flag (any case worse than a stated
  threshold on A-weighted error or any feel term), so every matcher phase reports "better on N, worse on M".
- Deterministic (seeded); runtime budget stated (`--quick` ≤ 5 min per case on 4 cores).
- Tests on synthetic cases (hidden chains rendered by tonerender) — the harness must recover known chains within the
  existing known-answer tolerances.

## Task C — baseline

The user runs it on the Mac (lead hands exact commands); the scores become the baseline every later matcher phase
(including v0.4M's validation) is compared against. Results summarised in the REPORT; audio never committed.

## Acceptance

Tasks A–B reviewer-ACCEPTed with tests; CI green; REPORT `docs/specs/v0_7-genre_benchmark_REPORT.md` with the case
list, the baseline table once run, and the policy: a matcher change that regresses any case beyond threshold needs the
lead's explicit sign-off with the user's listening verdict.
