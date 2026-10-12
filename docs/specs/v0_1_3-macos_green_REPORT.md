# v0.1.3 — macOS arm64 CI green: REPORT

Spec: `docs/specs/v0_1_3-macos_green.md`. Branch `claude/sawblade-v0_1_3-macos-green` (from
`claude/sawblade-plugin-setup-7k0b8q` @ aaf9c3d). Implementer: dsp-engineer (Sonnet); auditor: reviewer (Sonnet).

## CI evidence

| Run | Head | macos-arm64 ctest | auval / pluginval (mac) | linux-gcc + pluginval 10 | clang -Werror | python |
|---|---|---|---|---|---|---|
| 61 (37342304118), baseline | 257e4b6 | 45 / 694 failed | skipped | green | green | green |
| 68 (37350475001) | ef4334e | 1 / 695 failed (level-match golden, 1.53e-7) | skipped | green | green | green |
| 69 (37354616347) | 8dadbd8 | 0 / 695 failed (green) | auval pass; pluginval AU + VST3 level 10 pass | green | green | green |

Each fix below names the run-61 tests that failed before it and the run where they pass.

## Task A — 39 "Subprocess aborted" (commit 26f7403)

Symptom (run 61): test bodies pass, then the process dies at exit with
`libc++abi: terminating due to uncaught exception of type std::__1::system_error: mutex lock failed: Invalid argument`.

Root cause: `Settings::~Settings()` (plugin/src/settings/Settings.cpp, via `applyCacheEnv()`) locks the core
cache-override mutex (core/src/preset.cpp). Both were namespace/static objects in different TUs: the shared
`Settings` (`gShared`, a static `unique_ptr`) and `gCacheOverrideMutex`. Their destruction order is link-order
dependent. On macOS the mutex died first, and libc++ throws when a destroyed mutex is locked; glibc's mutex
destructor is trivial, so Linux never showed it. The 39 tests are exactly those that create `Settings::shared()`:
`MatchSettings::matchExecutable()` evaluates `defaultMatchExecutable()` eagerly (plugin JobRunner.cpp).

Fix (product code, the same path a DAW unloading the plugin takes): both singletons are deliberately leaked
function-local heap state, `CacheOverrideState` in core/src/preset.cpp:410-415 and `SharedState` in
plugin/src/settings/Settings.cpp:187-192. `resetSharedForTests()` still destroys the instance. This is one bounded
allocation per process on the message thread, never in `process()`.

Separate bug, test 447 "T3kTool: destroying the tool while a child runs cancels it" (SIGABRT inside the case):
plugin/tests/test_mic.cpp declared `Run r` (mutex + cv) after `T3kTool tool`, so the tool's thread called
`onDone` into a destroyed `r` during destruction. `Run r` now comes first in all 7 such tests. Hardening:
ToolRunner notifies its cv under the lock, because a waiter may free the Job as soon as it sees `done_`.

There is no Linux-failing regression test: glibc tolerates the destroyed mutex. The evidence is the 39 tests,
aborting in run 61 and passing in run 68.

Residual hazards (reviewer, not reachable today): function-local `static std::mutex` in
core/src/model_store.cpp:48 (`cachedSha256`), and the function-static `Graveyard` in
plugin/src/browser/PreviewWorker.cpp, which joins threads at exit.

## Task B — 6 plain failures (commits 7157cef, 8dadbd8)

Root cause: no `-ffp-contract` setting anywhere. Apple clang on arm64 contracts `a*b+c` into FMA, while gcc
on x86-64 does not, so float results differed by ulps (4c19736 had already hit this once).

Fix: top-level CMakeLists.txt:46-53 adds `add_compile_options(-ffp-contract=off)` (gcc and clang, option
`SAWBLADE_FP_CONTRACT_OFF`, default ON) before `Dependencies.cmake`. It applies to our targets, the JUCE modules
compiled into the plugin, NAM core, pffft and the bindings. Per-platform determinism is unchanged, and the Linux
goldens stay bit-identical. Golden mismatches now print the count, the first index and max |diff|
(`SAWBLADE_REQUIRE_SAME_SAMPLES`, tests/test_util.h).

| run-61 failure | measured before | result |
|---|---|---|
| test_presets.cpp:222 round trip | 0.99999904632568359 vs 1.0 | passes in run 68, no test change |
| test_presets.cpp:299/323/326 A/B | 2.00000095, -1.00000048 | passes in run 68, no test change |
| Chain live params: mute ramps to exact silence | — | passes in run 68, no test change |
| v1 modeled presets vs phase 7 goldens | (hm_chainsaw) | passes in run 68, exact, no test change |
| rig editor: screenshots (test_editor.cpp:1222) | knob 0.5 vs 0.0 | passes in run 68 (see below) |
| Level match: legacy-shaped preset renders the golden exactly | — | 1.53e-7 in run 68 → tolerance below |

**Screenshot test (B.5):** this was not cosmetic and was not skipped. The test pumped the message loop with
`runDispatchLoop()` + `callAsync(stopDispatchLoop)`. On macOS that is `[NSApp run]`, which does not return for the
queued stop, so the blend knob never received its attachment update and stayed at 0.5. The test now pumps with
`runDispatchLoopUntil(20)`, the idiom every other editor test uses. The assertion is unchanged, and the test now
REQUIREs that at least one blend knob exists, so it cannot pass vacuously.

**The one tolerance change (tests/test_level_match.cpp:~304).**
- Measured: max |diff| = 1.53e-7 on Apple arm64 (run 68). It was reproduced at 1.23e-7 on Linux x86 with clang
  `-mfma -mavx2`.
- Cause: the golden preset uses an LSTM NAM, which runs through Eigen. Eigen's NEON `pmadd` uses explicit
  `vfmaq_f32` (Eigen/src/Core/arch/NEON/PacketMath.h:1083), and intrinsics ignore `-ffp-contract=off`. This is
  FP-contraction noise, not a DSP difference.
- Rule: exact (`== 0.0`) stays on Linux x86-64, the golden platform. Elsewhere `maxDiff <= 1e-6`, which is 6.5x the
  measured noise and 100x tighter than the existing cross-platform `kGoldenTol = 1e-4` (tests/test_render.cpp:33)
  on the same golden file. A 0.01 dB trim is about 1.2e-3 relative, and the test also hard-checks
  `levelMeasured == false` and zero trim/makeup, so a wrongly applied probe still fails it. maxDiff is printed on
  every run.
- Not done: undefining `__ARM_FEATURE_FMA` for nam_core. That would trade NAM arm64 speed for bit-exactness, and
  undefining a builtin macro in a dependency is fragile.

## Task C — build stamp (commit 71cc059, merged ef05f59; docs ef4334e)

- `cmake/GenBuildInfo.cmake` renders `BuildInfo.h` to a temp file and installs it with `copy_if_different`. It is
  run by the always-run `SawbladeBuildInfo` target (plugin/CMakeLists.txt, `BYPRODUCTS` set), so a no-op rebuild
  compiles nothing and a new HEAD regenerates the header without a reconfigure. `scripts/mac_update.sh` therefore
  always gets a fresh stamp.
- Without git, the stamp fields read `unknown`.
- About page: `Sawblade <version> · <sha> · clean|dirty · built <date>`. The Standalone window title is
  `Sawblade - <version> · <sha> · clean|dirty`.
- Test `build_stamp` (tests/test_build_stamp.cmake): the sha changes on a new commit without a reconfigure; the
  same HEAD leaves the content and mtime untouched; a tracked edit reads dirty; no git reads unknown.
- Manual check on the Mac: the Standalone window title. It is untestable headless, because editor tests run with
  `wrapperType_Undefined`.

## Task D — Eigen from GitHub (commit 4b56a6b)

Eigen now comes from `https://github.com/eigen-mirror/eigen.git` at the unchanged pin
3147391d946bb4b6c68edd901f2add6ac1f31f8c (3.4.0), cmake/Dependencies.cmake:8-9. NAM core is fetched with
`GIT_SUBMODULES ""`, so its gitlab Eigen submodule is no longer cloned; neither submodule was used. The mirror and
MPL-2.0 are recorded in docs/THIRD_PARTY.md. Local configure in the cloud container now works. No `gitlab` URL
remains in CMake or CI.

## Reviewer verdicts

- Task C (71cc059): ACCEPT. Should-fix applied: docs/PLUGIN.md updated.
- Tasks D/A/B (4b56a6b, 26f7403, 7157cef): ACCEPT. Should-fix applied: non-vacuous blend-knob REQUIRE.
- Follow-up (8dadbd8): ACCEPT. Should-fix applied: this report documents the tolerance and both measurements.

## Proposals (not done, outside scope)

1. Split `BuildInfo.h` into a stable version header and a per-commit stamp, so that only AboutBox.cpp recompiles
   on a new commit (test_editor.cpp and test_integration.cpp currently do as well).
2. Remove the leftover `kGitHash` once nothing reads it.
3. Hardening: make `cachedSha256`'s static mutex and PreviewWorker's `Graveyard` teardown-safe the same way.
4. A CI check that `build_stamp` is registered (it silently skips when git is missing at configure).

## Final

All acceptance criteria are met on 8dadbd8, the last code commit (CI run 69). The macOS arm64 Test step is green, and auval plus pluginval AU and VST3 (level 10) ran and passed for the first time. linux-gcc (ctest + pluginval 10), clang -Werror and python are green. No test was deleted, disabled or skipped, and no golden was regenerated. Exactly one tolerance changed, with the measured numbers above. The commits after 8dadbd8 are docs-only (this report).

Manual follow-up on the user's Mac: confirm that the Standalone window title shows the stamp.
