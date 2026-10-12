# v0.1.3 — macOS arm64 CI green (+ build stamp, Eigen mirror)

Source: HANDOFF open items (a)–(c). Owner: dsp-engineer (C++/CMake/CI), match-engineer only if a
root cause is in `match/`; reviewer audits each task.

Why first: the user plays on an Apple Silicon Mac. On `macos-arm64` CI (run 61, head 257e4b6) 45 of
694 tests fail, so auval + macOS pluginval never run, and the failing areas (background jobs,
preset round trip) are features the user will touch next (record + match, NAM export, presets).

## Task A — the 39 "Subprocess aborted" tests

runner, two-pass, export, T3kTool, match, housekeeping. 39 tests, one symptom: treat as one root
cause until proven otherwise. Find it from the CI log of a failing test (abort message, signal,
backtrace if any); candidates (unverified): a test helper that spawns a child (python venv / fake
tool) via a Linux-only path or `/proc`; an assertion in a shared runner fixture; macOS
`posix_spawn`/`fork` in a multithreaded process; temp-dir path length (`/var/folders/...`).
Fix the product code if the product is wrong on macOS; fix the test if only the test is.

## Task B — the 6 plain failures

1. two render golden tests; 2. preset save/load round trip; 3. A/B compare;
4. "Chain live params: mute ramps to exact silence"; 5. "rig editor: screenshots".

For goldens / exact-silence: the arm64 FMA contraction issue from `4c19736` is the first suspect.
Rule (CLAUDE.md): determinism is per-platform bit-identical; cross-platform comparisons use a
documented float tolerance per test. Do not loosen a tolerance without showing the measured
difference and why it is FP-contraction noise and not a DSP difference (report numbers). A real
audible difference is a bug to fix, not a tolerance to widen. Never regenerate goldens on macOS to
make them pass. Screenshot test: if it is pixel-exact against Linux fonts, make it platform-aware
the way the project already does elsewhere, or skip-on-mac **only** with a reason in the test and
the report (this is the only test that may be skipped, and only if it is purely cosmetic).

## Task C — build stamp that cannot go stale

`BuildInfo.h` is generated at configure time; `scripts/mac_update.sh` reconfigures only when
`build.ninja` is missing, so the About page can show an old commit. Regenerate it every build
(custom target / `configure_file` at build time, no-op rebuild when unchanged so incremental builds
stay fast), and show `version · short sha · dirty flag` in the About page and the Standalone window
title. Test: the generated header changes when HEAD changes without reconfiguring.

## Task D — Eigen from the GitHub mirror

Fetch Eigen at the same pinned commit (3147391d, 3.4.0) from a GitHub mirror instead of gitlab.com
(gitlab outages broke CI configure; cloud sessions cannot reach gitlab). Same for NAM core's Eigen
submodule if FetchContent pulls it. Record the mirror + licence in `docs/THIRD_PARTY.md`.

## Acceptance

- `macos-arm64` CI: Test step green, then **auval and pluginval steps run and pass** on the final
  commit. linux-gcc (ctest + pluginval 10), clang `-Werror`, python stay green.
- Each fix has a test that failed before on macOS (CI evidence: run id + test name).
- No test deleted or disabled; the only permitted skip is Task B.5 under the rule above.
- Report `docs/specs/v0_1_3-macos_green_REPORT.md`: root cause per group with file:line, measured
  numbers for any tolerance change, reviewer verdicts, CI run ids, final sha.
