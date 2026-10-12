# v0.7 — genre benchmark: the matcher is judged on many heavy tones, not one

Source: user decision 2026-10-07 ("4a: yes"). Today the matcher's only real known answer is Bloodbath "Zombie Inferno"
(HM-2 + Überschall). CLAUDE.md requires that Sawblade work for ANY heavy tone; a change that improves Bloodbath can
silently worsen thrash. Owner: match-engineer; reviewer on every task.

**Scheduling (lead, 2026-10-08):** Tasks A–B (definition, runner, synthetic tests) start now, branched from the v0.4M
head, in new files (`match/sawblade_match/bench/`, `docs/benchmark/`) so they do not collide with v0.4M or v0.8. Task C
(the user's baseline run) waits until v0.8 calibration has merged, because the baseline numbers depend on it. The audio stays on the user's Mac (never in git
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
| **bloodbath_blend** (primary) | Bloodbath | 17 GTR RHY L DI | **18 HM2 AMP + 19 UBR AMP** (summed, see blend cases) | HM-2 death metal, two-amp blend |
| bloodbath_hm2 (path check) | Bloodbath | 17 GTR RHY L DI | 18 GTR RHY L HM2 AMP | saw path alone |
| bloodbath_ubr (path check) | Bloodbath | 17 GTR RHY L DI | 19 GTR RHY L UBR AMP | body path alone, non-HM-2 |
| bloodbath_mz | Bloodbath | 23 GTR RHY C DI | 24 GTR RHY C MZ AMP | death metal, third amp |
| immortal_disfig | Immortal Disfigurement | 20 GTR DI L | 22 GTR REAMP L | deathcore / slam |
| veil_of_maya | Veil of Maya | 25 GTR L DI_1 | 27 GTR L_1 | djent / metalcore |
| sylosis_57 | Sylosis | 27 GUITAR DI A left main | 31 SCOTT 57 A left main | thrash (mic'd amp) |
| sylosis_reamp | Sylosis | 27 GUITAR DI A left main | 43 JOSH REAMP A Left main 1 | thrash (second engineer's reamp) |
| haunted | The Haunted | 25-GT 1 DI | 26-GT 1 | Swedish thrash |
| jinjer | Jinjer | 24 Gtr Rtm L DI | 23 Gtr Rtm STEM (left channel) | groove / prog metalcore (stem: may include processing; weaker) |

**Blend cases (user, 2026-10-07):** "the Bloodbath mix as well as many others are not just a single amp sound but a
blend on the album … judging based just on the HM2 recording isn't what we want in total. On these sort of albums there
are usually 2 tracks per side — HM2 and the body blend amp." So:

- Wherever a session has two or more amp tracks of the **same DI take** that are blended on the record, the case's
  reference is the **blend** (the sum of those tracks), and it is the case that counts toward the total. Default sum is
  unity gain after checking sample alignment; when the engineer's fader levels are known (session file, user), the
  manifest records them and the sum uses them. The single-amp tracks become **path checks**: the matcher's blend result
  renders each path alone and is scored against the matching isolated track (saw ↔ HM-2 track, body ↔ body track),
  plus the chosen blend ratio against the reference ratio. That is a stronger known answer than either single track.
- Single-path cases (one amp track per DI) stay single-path; CLAUDE.md requires both kinds to be supported.
- The lead re-checks the other Tier-1 sessions for same-take multi-amp tracks with the user (e.g. Sylosis 31 SCOTT 57 vs
  43 JOSH REAMP: two engineers' alternatives or a blend on the record?) and converts them to blend cases where the record
  blends them.

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
