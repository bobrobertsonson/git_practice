# v0.1.2 — live controls do nothing on the Mac: REPORT

Spec: `docs/specs/v0_1_2-live_controls.md`. Reviewer verdict: **ACCEPT** (commit 045e6f6).

## Root cause: user setup, not a plugin bug

User facts forwarded by the lead during the phase:
- The host was Logic (AU).
- Loading a preset changed both the name and the sound, so H1 was out.
- The controls later worked with no code change.
- The user had **two Sawblade instances on two Logic tracks**. They were tweaking one instance while hearing the other, because Logic routes live input monitoring to the selected track. Switching tracks made the controls work.

How each hypothesis stands:

| Hypothesis | Result |
|---|---|
| H1 (loads ignored) | Ruled out by the user ("the sound changed on load") and by test 1. |
| H2 (params never reach the engine) | Does not reproduce. Tests 2–5 drive real controls, plus host-style changes from another thread at 44.1 kHz / 512, and every one changes the output. |
| H3 (editor not attached) | Does not reproduce. Tests 2–4 move the knobs, BLEND and EQ nodes with mouse events. They check that the host parameter follows the control and that the render changes. |
| H4 (stale build) | Not pursued after the user facts. See proposals. |
| H5 (restored old state) / H6 (editor opened before the first engine swap) | Dropped on the lead's update before any testing. |
| Shared static state between instances (the only remaining plugin-side explanation for the two-track setup) | Ruled out by test 6. |

No product code changed.

## Tests added

All are in `plugin/tests/test_live_controls.cpp`, tagged `[editor][live]`, in the `sawblade_editor_tests` executable. CI runs them under xvfb, and the macOS job runs ctest. Together they take about 5 s.

**Metric:** relDiff = RMS(a−b) / max(RMS(a), RMS(b)) over 1 s of `tests/fixtures/di_riff.wav`, after a 0.3 s warm-up.
- "Changed" means relDiff > 0.05.
- "Unchanged" means relDiff < 1e-3.

Capture-backed presets use the repo's identity-NAM and impulse-IR stand-ins. These are copied into an isolated capture cache under the TONE3000 ids; no real captures are committed.

| # | Test | Measured |
|---|---|---|
| 1 | Each preset loads through `PresetLoadFlow` (the browser's load path) and differs from INIT | buzzsaw 0.996, bolt_thrower 1.017 |
| 2 | Pedal knobs on `classic_buzzsaw`, all six dragged on the pedal face | 0.42 – 0.99 |
| 3 | BLEND on `bolt_thrower_v1`, dragged on the main panel | 1.06 |
| 4 | EQ-graph node drag in the rig editor (Post, Path A, Path B) | 0.36 / 0.23 / 0.088 |
| 5 | Logic-style: `setValueNotifyingHost` from another thread between 512-sample blocks at 44.1 kHz | blend 1.06, pedal 0.42 |
| 6 | Two instances with editors in one process, controls moved on A then on B (`[isolation]`) | other instance < 1e-3; its live params and preset name are unchanged |

The spec's "fails on pre-fix code" criterion does not apply because there is no fix. The reviewer instead checked that each test would fail if its failure mode were present:
- A detached control or a parameter that never reaches the engine would give relDiff ≈ 0.
- Shared engine, parameter or preset state would break test 6.

## Validation

| Check | Result |
|---|---|
| gcc Release with `-Werror` | Built clean. |
| ctest | **684/684 passed.** 2 skipped, both env-gated: the htdemucs separator and the app-icon test (no Pillow). |
| Reviewer re-run of the new tests | 6/6 passed. |
| clang | Not run here. CI's `linux-clang-werror` job covers it. |
| Python suite | Not run; no Python changed. |
| pluginval | Not run; no plugin code changed. |
| CI run 37337777150 (sha 3eb464c) | linux-gcc (ctest + pluginval VST3 level 10), linux-clang-werror and python are green. |
| CI macos-arm64 | All 6 new `editor:live controls` tests **pass on Apple Silicon**. The job is **red**: 45/683 fail. These are exactly the same 45 tests that fail on the base branch at f9ac2ca (run 37333081170, 45/677). |

The 45 macOS failures are pre-existing and are not caused by this phase:
- 39 tests in `plugin:runner`, `plugin:two-pass`, `plugin:export`, `plugin:T3kTool` and `plugin:housekeeping` end in "Subprocess aborted".
- 3 render-golden tests fail: `Chain live params: mute ramps`, `Level match: legacy preset golden` and `v1 modeled presets ... phase 7 goldens`.
- 2 preset tests fail: `plugin:save / load round trip` and `plugin:A/B compare`.
- `editor:rig editor: screenshots` fails.

So the spec's criterion "macOS arm64 job green on the final commit" is **not met**, and it cannot be met from this phase. Because the Test step fails, the job's auval and pluginval steps are skipped. This needs its own Mac-CI task; see proposal 3.

Local build workarounds (environment only, no repo change):
- gitlab is blocked, so Eigen and NAM core came in via `FETCHCONTENT_SOURCE_DIR_*` overrides.
- X11 extension and ALSA headers are missing, so JUCE was built with those features off.

## Reviewer non-blocking notes (left as is)

- **Test 5:** the thread is joined before each render. That covers delivery between blocks, as named, but not a concurrent race. A TSan-style variant is a possible later task.
- **Test 5 comment:** it claims the swept window ends like the pure-1 render, but only the difference from 0 is asserted.
- **BLEND test:** it uses identity NAM stand-ins, so it catches a dead or detached BLEND but not a NAM-blending bug.

## Proposals (not done)

1. **Build-identity readout (H4 hardening).** Add the version and git commit to the About page and the Standalone window title, regenerated on every build. The implementer found that a configure-time-only header goes stale under `scripts/mac_update.sh`, which reconfigures only when `build.ninja` is missing. Estimate: about 30 lines of CMake plus the title.
2. **Multi-instance UX.** Show the track/instance name in the editor header, or a small "monitoring?" hint, so two open Sawblade windows can be told apart in Logic.
3. **macOS CI repair (new task, high priority).** The base branch's macos-arm64 job fails 45 tests (list above). The goldens and the save/load round-trip are the most worrying for the user's Mac build, because they suggest Apple arm64 renders differ from the Linux goldens. Triage these first, then the subprocess-runner aborts.
