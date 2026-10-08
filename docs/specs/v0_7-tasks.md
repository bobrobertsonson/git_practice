# v0.7 — task specs, Tasks A–B (lead decisions for `v0_7-genre_benchmark.md`)

Phase spec: `docs/specs/v0_7-genre_benchmark.md` (authoritative version: `origin/claude/sawblade-plugin-setup-7k0b8q`,
a76468f). This file pins the files, schema, scoring and thresholds the phase spec leaves open. Owner: match-engineer.
Reviewer on every task. Where this file and the phase spec disagree, the phase spec wins and the implementer raises it
under "Decisions / questions for lead". Task C (the user's baseline run) is out of scope here.

## Boundaries

- Branch `claude/sawblade-v0_7-benchmark` (from the v0.4M head). v0.4M and v0.8 are in flight on other branches: put
  new code in **new files**: `match/sawblade_match/bench/` (package), `match/tests/test_bench*.py`, `docs/benchmark/`.
  The only edits allowed outside them: one `sawblade-bench` line in `match/pyproject.toml` `[project.scripts]`, and, only
  if scoring a section is impossible without it, an optional keyword argument on an existing matcher function whose
  default keeps today's behaviour exactly (say which in the report). No edits to `calibrate/`, `tonecheck/`, `plugin/`,
  `core/`, `bindings/`.
- **Reuse, don't re-implement scoring.** The A-weighted error is `tonecheck.cli.compare_to_reference` of two
  `tonecheck.analysis.analyze` results exactly as `pathcheck._Analyses.error` computes it (the run's `aWeightedErrorDb`);
  the LTAS loss and feel terms are `pathcheck.feel_and_ltas` (`reference.build_target` + `feel.evaluate`); the raw feel
  deltas are `known_answer.feel_deltas`; guardrails are `run._guardrails`; the blend reference is `refsum.blend_refs`
  (polarity `auto` unless the manifest says otherwise); per-path renders are `pathcheck.single_preset`; the match itself
  is `run.run_match(Config(...))`; listening levels use `loudness.integrated_lufs`. Import them; if a private helper must be
  reused, import it (note it) rather than copy it.
- **No audio in git, ever.** Tests make their audio in `tmp_path` from the committed fixture captures. The manifest holds
  relative paths only. Third-party multitracks are personal-evaluation-only (CLAUDE.md, `docs/TEST_SOURCES.md`).

## Task A — `docs/benchmark/BENCHMARK.md` + `docs/benchmark/cases.json`

### A.1 Manifest schema (`"schema": "sawblade.bench.cases", "version": 1`)

Top level: `schema`, `version`, `rootHint` (`"/Users/notsch/Desktop/NailTheMix"`; informational, the runner takes
`--root`), `defaults` (below), `cases` (list), `gaps` (list of `{style, note}`).

Each case:

| key | meaning |
|---|---|
| `id` | unique, `[a-z0-9_]+` |
| `tier` | `1` (known-answer pair, scored strictly) or `2` (reference only + stand-in DI, informative) |
| `kind` | `single` (one amp track), `blend` (reference = sum of tracks), `pathcheck` (scored from a parent blend case's result; never runs the matcher) |
| `style` | free text, e.g. `"HM-2 death metal, two-amp blend"` |
| `session` | session folder relative to the root |
| `di` | `{path, channel}`; path relative to the root; `channel` `"left"|"right"|"mono"` (default `left` = first column, as `run_match`) |
| `reference` | `single`: `{path, channel}`; `blend`: `{tracks: [{path, role: "a"|"b", gainDb}], polarity}` (`gainDb` default 0 = unity faders, `polarity` default `auto`, both as `refsum`); `pathcheck`: `{parent, path: "a"|"b"}` |
| `offsetMs` | coarse DI→reference offset, `null` = searched (the matcher's `offset.resolve_offset`); NTM exports of one session start together, so `0.0` |
| `fit` | `[startS, endS]` in DI seconds, or `null` = automatic (A.2) |
| `heldOut` | `[startS, endS]` in DI seconds, disjoint from `fit`, or `null` = automatic |
| `transfer` | optional list of other-take pairs (the R side): `{di, reference}` in the same shapes; scored, not fitted |
| `topology` | `auto` (default), `single` or `blend`: passed to `Config.topology` |
| `counts` | `true` for the cases in the total; `false` for path checks and tier 2 |
| `confirmed` | `false` when a file name is inferred rather than taken from the user's listing (the runner prints it) |
| `notes` | free text (e.g. Jinjer: stem, may include processing) |

`defaults`: `{"diChannel": "left", "offsetMs": 0.0, "topology": "auto", "heldOutMaxS": 30.0}`.

### A.2 Sections

The fit section is all the matcher sees: the runner writes the DI and the reference cropped to `fit` (offset applied) to
`<out>/<case>/fit/` and runs the matcher on those files only. The held-out section is never seen by the fit. Automatic
sections (when `null`): on the DI-reference overlap, find the active region (first to last 100 ms frame within 40 dB of
the loudest frame); `fit` = its first half, `heldOut` = the densest `heldOutMaxS` seconds of its second half (same frame-
energy rule as `excerpt.select_excerpt`, reused). The resolved sections are written to `scores.json` so the user can pin
them in the manifest later. Deterministic.

### A.3 Case list

From the phase spec's lead decision table, verbatim: Tier 1 `bloodbath_blend` (**primary**, `kind: blend`, DI 17, tracks
18 HM2 = role a, 19 UBR = role b, `counts: true`), `bloodbath_hm2` / `bloodbath_ubr` (`kind: pathcheck`, parent
`bloodbath_blend`, path a / b, `counts: false`), `bloodbath_mz`, `immortal_disfig`, `veil_of_maya`, `sylosis_57`,
`sylosis_reamp`, `haunted`, `jinjer` (`kind: single`, reference `channel: "left"` for the Jinjer stem). Transfer (R side)
where the spec lists it: Bloodbath 20 DI / 21 HM2 / 22 UBR (blend of 21+22), Immortal Disfigurement 21/23, Veil of Maya
26/28, Sylosis 30 → 33 (for `sylosis_57`) and 30 → 44 (for `sylosis_reamp`), Jinjer 25 → 23 right channel. Tier 2: every
reference-only entry of the spec, `kind: single`, `counts: false`, `di` = a stand-in DI (DI-only sessions Reflections /
Cognizance / Trees on Mars; pick one per case, record which), `offsetMs: null`, `fit`/`heldOut` in **reference** seconds
(tier 2 has no aligned DI: the matcher runs unmatched with `--ref-section`-equivalent `sections` on the fit section).

Bloodbath file names are known (`NailtheMix_March2023_Bloodbath_44k24b/17 GTR RHY L DI.wav`, 18–22 likewise, see
`scripts/run_v04m_validation.sh`): `confirmed: true`. Every other session folder and file name not literally in the
listing is inferred → `confirmed: false`. `gaps`: black metal, doom/sludge (fuzz), crust/hardcore, plus "clean-ish rhythm
control: no DI pair (Nothing More is tier 2 only)". Counted cases: 8 (blend + 7 single), within the spec's 8–10.

### A.4 `BENCHMARK.md`

What the benchmark is for, the tiers, the case table (id, style, kind, DI, reference, transfer, counts), the gaps, the
section rule, the score definitions (B.3), the regression thresholds (B.4), the runtime budget (B.6), the audio /
licence rule, and the user's command lines (`run`, `compare`). Short; the manifest is the source of truth.

Tests: `test_bench_manifest.py` loads the committed manifest with the runner's loader: schema valid, ids unique, every
`pathcheck` parent exists and is `blend`, `counts` set as above, ≥ 1 counted case per style the spec lists as covered,
gaps contain the three uncovered styles, no absolute paths, no audio files anywhere under `docs/benchmark/`.

## Task B — `sawblade-bench run` / `sawblade-bench compare`

### B.1 Package and entry point

`match/sawblade_match/bench/`: `manifest.py` (load + validate), `sections.py`, `score.py`, `runner.py`, `compare.py`,
`cli.py` (`main(argv)` with subcommands). `pyproject`: `sawblade-bench = "sawblade_match.bench.cli:main"`. A Python API
`run_bench(cases, root, out, *, pool, plan=None, quick=False, seed=0, threads=4, ir_library=None, log=None) -> dict` so
tests inject the fixture pool and a small `Plan`; the CLI builds the pool exactly like `sawblade-match` (`--pool`, `--ir-dir`
repeatable, `--no-ir-dirs`; same defaults).

### B.2 `run`

`sawblade-bench run --root DIR [--manifest PATH] [--cases ID,ID…] [--tier 1|2|all] [--quick|--thorough] [--seed N]
[--threads N] [--pool …] [--ir-dir …] [--check-only] --out DIR`

- Default manifest `docs/benchmark/cases.json` of the repo; default `--tier 1`; default `--thorough` (the matcher's default).
- Preflight before any matching: every file of the selected cases exists and is readable; within a pair the sample rates
  agree. `--check-only` prints the table (case, file, ok/missing, rate, length, `confirmed`) and exits 0 if all present,
  2 otherwise. Without it, missing cases get `status: "missing"` and are skipped; the rest run.
- Per running case: resolve offset and sections → crop the fit files → `run_match` (matched `mono` reference, the case's
  topology, seed, quick/thorough, `write_audio=False`, `refine_offsets=True`) → render the winning preset over the full DI
  with the core engine → score the held-out section (B.3) → path checks of a blend case (both paths, its `pathcheck`
  children) → transfer pairs (render the same preset on the transfer DI, score the transfer reference over its own
  automatic held-out section; offset searched as `pathcheck` does for a foreign DI) → `listen/` pair.
- `listen/<case>/ref.wav`, `render.wav`: the held-out section, time-aligned, both BS.1770 loudness-matched to the reference
  (gain recorded), float WAV. Written into `--out` (the user's disk), never into the repo.
- Outputs: `<out>/scores.json`, `<out>/scores.md` (one row per case + the total), the per-case matcher run directories,
  and stdout ending in the one-line total: `bench total: <n> of <m> counted cases ran, mean held-out A-wt <x> dB,
  median <y> dB, within 0.5 dB: <k>, guardrail fails <g>`.
- Exit: 0 when it ran (even with missing cases), 2 bad arguments / unreadable manifest.

### B.3 Scores (`"schema": "sawblade.bench.scores", "version": 1`)

Top: `manifest` (path + sha256), `gitSha` (or null), `args`, `seed`, `threads`, `mode`, `cases` (by id), `total`, `timings`.
Per case: `status` (`ok|missing|error`, error text), `sections` (resolved fit / held-out, `source: manifest|auto`),
`offset` (ms + source), `chosen` (topology, capture names per slot, cab/IR), `fitAWeightedErrorDb` (the run's
`after[0].aWeightedErrorDb`), `heldOut`: `aWeightedErrorDb`, `ltasLossDb`, `feel` (the weighted `tight`/`fizz`/`polish`/
`total` from `feel.evaluate` and the raw deltas of `known_answer.feel_deltas`: `t12Ms`, `sustainDb`, `hfRatioDb`, `hfFlat`,
`fluxDb`, `floorDb`; `null` with the reason where the reference can't support a term), `guardrails` (`run._guardrails`
on the held-out render), `pathchecks` (per path: the same held-out numbers vs its isolated track, plus the blend ratio
`chosenDb` / `refDb` / `diffDb` as `pathcheck`), `transfer` (list, same held-out numbers), `listen` (paths + gain dB),
`runtimeS`. Tier 2: `heldOut.aWeightedErrorDb` only (feel `null`, reason "unmatched reference").
`total`: over `counts: true` cases with `status: ok`: `n`, `m`, mean / median held-out A-wt, `within05` (≤ 0.5 dB, the
known-answer tolerance), guardrail fail count. Timings are the only non-deterministic fields; they live under `timings` /
`runtimeS` and `compare` ignores them.

### B.4 `compare A.json B.json`

`sawblade-bench compare A.json B.json [--json out.json] [--threshold KEY=V …]` (A = baseline, B = candidate).
Per case present and `ok` in both: delta B − A of held-out A-wt and of each raw feel delta (all are "lower is better"
distances to the reference). **Regression thresholds (lead, stated):**

| metric | regression if B − A > |
|---|---|
| held-out `aWeightedErrorDb` | 0.30 dB |
| `t12Ms` | 10 ms |
| `sustainDb` | 1.5 dB |
| `hfRatioDb` | 1.0 dB |
| `hfFlat` | 0.02 |
| `fluxDb` | 0.3 dB |
| `floorDb` | 3.0 dB |

Rationale: the feel thresholds are the D.1 known-answer tolerances (`known_answer.FEEL_TOLERANCES`): moving a term by a
whole tolerance is a change worth a listen. A-wt: D.1's seed-to-seed spread on one case was 0.20–0.46 dB, but the benchmark
fixes the seed, so 0.30 dB is the lead's provisional noise bar; revisit it once Task C repeats a case with a second seed.
A case is
**worse** if any metric regresses, **better** if A-wt improves by > 0.30 dB and nothing regresses, otherwise **same**.
Path checks and transfer are compared and flagged the same way; tier 2 is shown, never flagged. Missing / errored on
either side → "not compared". Guardrails: newly failing ones are listed, not flagged. Output: a markdown table + the line
`compare: better on N, worse on M, same on K, not compared on J (thresholds: …)`. Exit 1 if any case is flagged worse,
0 otherwise, 2 unreadable input / schema mismatch. Thresholds live in one dict in `compare.py`, printed in the output and
`BENCHMARK.md`; `--threshold` overrides are echoed.

### B.5 Determinism

`run_match` gets the CLI seed for every case (default 0); no other randomness (sections, polarity, offsets are
deterministic). Same inputs + seed + `--threads` → `scores.json` identical except `timings` / `runtimeS`.

### B.6 Runtime budget (state it in `BENCHMARK.md` and the report)

`--quick` ≤ 5 min per case on 4 cores (the matcher's own quick budget; scoring + path checks + transfer + listen add
≤ 30 s per case). Tier 1 quick: ≤ 45 min for 8 counted cases; thorough: measured by the user in Task C. CI: the new
tests together ≤ 4 min on the `python` job.

### B.7 Tests (synthetic, `tmp_path`, fixture captures; `pytest.importorskip("sawblade_core")` where the core is needed)

1. **Single-path known answer:** a hidden single-path chain (`test_matcher.hidden(pool, "single")`-style, fixture pool)
   rendered by the core over a synthetic DI ≥ 12 s (`known_answer.gap_di` or the fixture riff, looped/seeded); a temp
   root with `di.wav` + `amp.wav`; a temp manifest with explicit `fit` [0, 6] and `heldOut` [6, 12]. `run_bench` with a
   small plan: held-out A-wt ≤ 0.5 dB (the existing known-answer tolerance), chosen topology `single`.
2. **Blend known answer through refsum:** a hidden blend (linear blend law, no bus comp) rendered per path with
   `pathcheck.single_preset` into `a.wav` / `b.wav`; first assert `refsum.blend_refs(a, b)` ≈ the full hidden render;
   the manifest's blend case + two `pathcheck` cases. Held-out blend A-wt ≤ 0.5 dB; both path checks and the ratio are
   computed and finite (report their values; assert ≤ 0.5 dB only if the recovered captures equal the hidden ones).
3. **Held-out isolation:** the matcher only receives the cropped fit files (spy on `run_match`'s `Config`: DI/ref
   durations = the fit section; no sample of the held-out window inside them).
4. **Determinism:** two runs, same seed → `scores.json` equal except timings.
5. **compare:** hand-built score files: a 0.31 dB A-wt worsening and an 11 ms `t12` worsening are flagged (exit 1), a
   0.29 dB one is not; better/same/not-compared counts; `--threshold` override; schema mismatch → exit 2.
6. **Preflight / CLI:** `--check-only` with a missing file exits 2 and names it; a missing case is skipped with
   `status: missing` and the total says `n of m`; `main(["run", ...])` and `main(["compare", ...])` smoke.
7. Manifest test (A).

Use the plan sizes of `test_known_answer_feel.small_plan` / `test_matcher` as a starting point; keep the suite inside B.6.

## Report

`docs/specs/v0_7-genre_benchmark_REPORT.md` (lead writes after ACCEPT + green CI): case list, thresholds, runtime,
synthetic results, reviewer verdicts, the policy (a matcher change that regresses any case beyond threshold needs the
lead's explicit sign-off with the user's listening verdict), and a "Baseline — pending Task C" section. The implementer
puts measured numbers and proposals (extras) in their hand-off message, not in new code.
