# Sawblade

JUCE guitar plugin (AU/VST3 now, AAX later) that builds **blended high-gain chains** from
TONE3000 NAM captures, matches them to a reference song, and exports the result as a
trainable NAM model for live loader pedals.

North-star tone: Gatecreeper-style — an HM-2-style "chainsaw" path blended with a thick,
tight high-gain "body" path. See `docs/TONE_TARGETS.md`.

## Roles and model routing

- **Lead (main session, Opus):** architecture, DSP/tone decisions, task specs, acceptance.
  The lead does not write implementation code.
- **Subagents** (`.claude/agents/`, all `model: sonnet`):
  - `dsp-engineer` — C++20 / CMake / JUCE 8: `core/`, `plugin/`, `cli/`, C++ tests.
  - `match-engineer` — Python matching engine in `match/` (later phases).
  - `reviewer` — audits every change; returns `ACCEPT` or `REVISE` + fix list.

### Task loop (never skip the reviewer)

1. Lead writes a spec in `docs/specs/<phase>-<task>.md` with acceptance tests.
2. Implementer builds it, runs the full test suite, commits on the working branch.
3. Reviewer audits the diff against the spec + the rules below → `ACCEPT` / `REVISE`.
4. On `REVISE`, implementer fixes the list; go to 3. Lead accepts after reviewer `ACCEPT`.

## Repository layout

```
core/      C++ DSP library (no JUCE GUI deps). Namespace `sawblade`.
plugin/    JUCE plugin (phase 2+).
cli/       `tonerender` offline renderer.
match/     Python matching engine; binds core via pybind11 (later phase).
tests/     Catch2 unit + golden tests; fixtures in tests/fixtures, goldens in tests/golden.
docs/      Specs, schema, tone targets.
third_party/  Vendored files only when FetchContent is impossible (record license).
```

## Signal graph

```
DI → input gain → gate (keyed on DI) → split
   Path A "Saw":  pre-EQ → [blocks: pedal NAM → low/med-gain amp NAM] → path EQ → level
   Path B "Body": pre-EQ → [blocks: boost NAM (optional) → high-gain amp NAM] → path EQ → level
→ latency compensation → auto polarity + delay align → blend
→ cab IR(s) (shared, or per-path before the sum) → post EQ → bus comp → output gain
```

Each path's middle section is a **modular chain of typed blocks** built through a block
registry (`nam`, `eq` in phase 1). Future modeled recreations of real pedals are new block
types; every type declares its latency and whether it is NAM-trainable.

## Roadmap (beyond phase 1)

- Phase 2: JUCE plugin (AU/VST3) on top of core; the user designs the UI — engineering builds
  to their design; mockups are reference only.
- Modular pedal chain: modeled recreations of real pedals as block types (DSP models, not
  captures); UI names use generic descriptors, not trademarks, unless licensed.
- Matching engine (`match/`), NAM export, AAX.

## Hard rules

- **Audio thread:** no heap allocation, no locks, no I/O, no exceptions, no logging inside
  `process()`. All allocation happens in `prepare()` / load. Tests enforce zero allocations
  during `process()` with an allocation-counting harness.
- **Model/IR swaps** are prepared on a background thread and handed to the audio thread
  lock-free; old objects are released off the audio thread.
- **Latency:** every block reports its latency in samples; paths are compensated to the
  longest path; total latency is reported to the host / render report.
- **Presets** are versioned JSON (`docs/PRESET_SCHEMA.md`); plugin state *is* the preset.
  Every capture stores its TONE3000 license + creator.
- **NAM export rules:** keep gate, reverb, delay, modulation and long-release compression
  out of anything that will be trained into a NAM model. "Live-compatible blend" = both paths
  share one cab IR (no-cab export is exact). "Studio blend" = per-path IRs; only the with-cab
  export is exact, and the UI must say so.
- Determinism: rendering the same preset + input must be bit-identical across runs and
  independent of processing block size (within float tolerance documented per test).
- No scope creep: implement what the spec asks; propose extras in the report instead.

## Capture licensing (TONE3000)

- Every capture keeps its `license`. Never use `cc-by-nc*` (non-commercial) captures; Sawblade
  is commercial. Licenses seen: `t3k`, `cc-by`, `cc-by-sa`, `cc-by-nc`, `cc-by-nc-sa`,
  `cc-by-nd`, `cc-by-nc-nd`, `cco`.
- TONE3000 Terms: no redistribution of tones, and no commercial distribution of content
  accessed through the platform without written permission from the content owner **and**
  TONE3000. So: never commit or bundle capture files; presets reference TONE3000 tone ids
  and fetch through the API. **Exported NAM models trained from TONE3000 captures are for
  the user's own use only** until the commercial agreement explicitly covers derived exports.
- Never use or store the `t3k_cs_…` secret key; only the publishable key + user OAuth tokens.

## Build & test

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/cli/tonerender --preset <preset.json> --in <di.wav> --out <out.wav> [--report r.json]
```

## Conventions

- C++20, warnings as errors on our targets (`-Wall -Wextra -Wpedantic`), no warnings from deps.
- Audio samples are `float`; coefficient math in `double`. NAM is built with `NAM_SAMPLE_FLOAT`.
- Third-party deps pinned to exact commits/tags via FetchContent; record license in
  `docs/THIRD_PARTY.md`.
- Commits: small, descriptive, one task per commit series.
