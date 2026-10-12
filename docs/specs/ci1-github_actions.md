# CI 1: GitHub Actions

Branch: `claude/sawblade-ci`. Base: `origin/claude/sawblade-plugin-setup-7k0b8q` @ `c80f669`.

## Problem

Many feature branches merge into `claude/sawblade-plugin-setup-7k0b8q`; nothing builds them
before the user pulls on their Mac, so clang-only diagnostics, macOS-only failures and broken
tests reach the user first. Add CI that catches these on every push / PR.

## Task

One workflow file `.github/workflows/ci.yml` (name `CI`) with four jobs.

Common:
- Triggers: `push` and `pull_request`, both filtered to branches `claude/**`; plus `workflow_dispatch`.
- `concurrency: { group: ${{ github.workflow }}-${{ github.ref }}, cancel-in-progress: true }`.
- `permissions: contents: read`. No secrets of any kind are referenced; in particular no
  `TONE3000_*` variable is set anywhere.
- Every third-party action pinned to a **full commit SHA** with a `# vX.Y.Z` comment (resolve the
  SHA with `git ls-remote https://github.com/<owner>/<action> refs/tags/<tag>`; for annotated
  tags use the peeled `^{}` SHA).
- `timeout-minutes` on every job.
- FetchContent cache: configure with `-DFETCHCONTENT_BASE_DIR=${{ github.workspace }}/.deps`
  and cache that directory with `actions/cache`, keyed on OS + job + `hashFiles('cmake/Dependencies.cmake')`.
- ccache (`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`), `CCACHE_DIR`
  inside the workspace, cached with `actions/cache` (key with `github.run_id`, `restore-keys` prefix
  per OS + job), `ccache -s` printed at the end. Cap size (`CCACHE_MAXSIZE`, ~1–2G).
- Ninja generator, Release, `cmake --build build --parallel`, `ctest --test-dir build --output-on-failure`.
- pluginval: download the **prebuilt** release binary of Tracktion/pluginval `v1.0.4`
  (`pluginval_Linux.zip` / `pluginval_macOS.zip`), do not build it. Run it as its own step
  (do NOT set `SAWBLADE_PLUGINVAL_EXECUTABLE`, so it doesn't also run inside ctest):
  `pluginval --strictness-level 10 --validate-in-process --timeout-ms 300000 --validate <plugin>`.

Jobs:
1. **linux-gcc** — `ubuntu-24.04`, GCC (default). apt: the JUCE packages listed in
   `docs/PLUGIN.md` + `ninja-build ccache xvfb`. Install xvfb **before** configuring (the editor tests
   register `xvfb-run` at configure time). `-DSAWBLADE_BUILD_PLUGIN=ON`. ctest, then pluginval on
   `build/plugin/SawbladePlugin_artefacts/Release/VST3/Sawblade.vst3` under `xvfb-run -a`.
2. **linux-clang-werror** — same as 1 with `CC=clang CXX=clang++`, plugin ON, ctest; no pluginval.
   `-Werror` comes from the existing `sawblade_warnings` target (our targets only); do **not** add a
   global `-Werror` to `CMAKE_CXX_FLAGS` (it would hit JUCE/deps). This job exists to catch clang-only
   diagnostics (e.g. `-Wunused-private-field`, see `mac1-clang_werror.md`).
3. **macos-arm64** — `macos-15` (arm64, Apple clang). `brew install ninja ccache` (skip what's present).
   Plugin ON; build everything (AU, VST3, Standalone). ctest. Then copy
   `.../Release/AU/Sawblade.component` to `~/Library/Audio/Plug-Ins/Components/`, refresh the AU
   registry (`killall -9 AudioComponentRegistrar || true`), run `auval -v aufx Swb1 Swbl` (fail the
   step on non-zero exit). Then pluginval level 10 on the installed `.component` and on the VST3.
4. **python** — `ubuntu-24.04`, `actions/setup-python` 3.11. `python -m venv match/.venv`,
   `match/.venv/bin/pip install -e 'match[dev]'` (no `separation`/`export` extras). Configure with
   `-DSAWBLADE_BUILD_PYTHON=ON -DSAWBLADE_BUILD_TESTS=OFF -DPython_EXECUTABLE=$PWD/match/.venv/bin/python`,
   build. Then `match/.venv/bin/pytest match -rs` with `SAWBLADE_CORE_DIR=$PWD/build/python` and
   `SAWBLADE_TONERENDER=$PWD/build/cli/tonerender` (check how the tests locate the module/binary and
   set whatever they read). Tests gated on captures, demucs or training stay skipped; `-rs` lists them.
   Cache pip (`setup-python` `cache: pip`, `cache-dependency-path: match/pyproject.toml`) + ccache + deps.

Docs:
- `docs/CI.md` (short): what each job checks, how to reproduce each locally (the exact commands),
  how caching works, how to bump pinned actions/pluginval, and that no secrets are used.
- README: status badge at the top for `ci.yml` on `claude/sawblade-plugin-setup-7k0b8q`
  (`https://github.com/bobrobertsonson/git_practice/actions/workflows/ci.yml/badge.svg?branch=claude%2Fsawblade-plugin-setup-7k0b8q`).

Out of scope: no source changes. If a job fails because of a real code problem, report it under
"Decisions / questions for lead" with the failing output; do not fix code in this task.

## Acceptance

- `actionlint` (download the release binary into the scratchpad, pinned version) reports nothing
  on `ci.yml`; YAML parses.
- Local dry-run of the linux-gcc and python job commands in this container (same configure/build/
  ctest/pytest commands, pluginval under xvfb if the download works) passes, or each failure is
  explained.
- No action referenced by tag/branch only; no `secrets.` reference; no `TONE3000` env.
- After push, all four jobs green on GitHub Actions (lead verifies via the API).
