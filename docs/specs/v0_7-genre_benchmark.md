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
