# CI

Workflow: `.github/workflows/ci.yml` (name `CI`). Runs on `push` and `pull_request` for branches
`claude/**`, and on manual `workflow_dispatch`. A newer run on the same ref cancels the older one.
Permissions are `contents: read`. **No secrets are used**: no `secrets.` reference and no
`TONE3000_*` variable is set anywhere, so tests that need TONE3000 access stay skipped.

## Jobs

| Job | Runner | Checks |
|---|---|---|
| `linux-gcc` | ubuntu-24.04, GCC | Full build with the plugin ON, `ctest`, then pluginval level 10 on the VST3 under `xvfb-run -a`. |
| `linux-clang-werror` | ubuntu-24.04, clang | Same build with `CC=clang CXX=clang++`, `ctest`; no pluginval. Catches clang-only diagnostics (e.g. `-Wunused-private-field`). `-Werror` comes from the `sawblade_warnings` target (our code only); there is deliberately no global `-Werror`, which would hit JUCE and the deps. |
| `macos-arm64` | macos-15 (Apple clang) | Full build (AU, VST3, Standalone), `ctest`, installs the AU into `~/Library/Audio/Plug-Ins/Components`, `auval -v aufx Swb1 Swbl`, then pluginval level 10 on the AU and the VST3. |
| `python` | ubuntu-24.04, Python 3.11 | Builds the `sawblade_core` module and `tonerender`, runs `pytest match -rs`. Tests gated on captures, demucs or training stay skipped (`-rs` lists them). |

pluginval is not run inside ctest in CI (`SAWBLADE_PLUGINVAL_EXECUTABLE` is not set); it is its own
workflow step, so a failure is easy to spot.

## Reproduce locally

Linux packages (Debian/Ubuntu): the JUCE list from `docs/PLUGIN.md` plus tools:

```
sudo apt-get install -y libasound2-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev libcurl4-openssl-dev \
  ninja-build ccache xvfb          # install xvfb BEFORE configuring (editor tests register xvfb-run then)
```

linux-gcc (use `CC=clang CXX=clang++` in front of `cmake -S` for linux-clang-werror, and skip pluginval):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
curl -fsSL -o pluginval_Linux.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip
unzip -o pluginval_Linux.zip -d pluginval
xvfb-run -a pluginval/pluginval --strictness-level 10 --validate-in-process --timeout-ms 300000 \
  --validate build/plugin/SawbladePlugin_artefacts/Release/VST3/Sawblade.vst3
```

macos-arm64:

```
brew install ninja ccache
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON
cmake --build build --parallel && ctest --test-dir build --output-on-failure
cp -R build/plugin/SawbladePlugin_artefacts/Release/AU/Sawblade.component ~/Library/Audio/Plug-Ins/Components/
killall -9 AudioComponentRegistrar || true
auval -v aufx Swb1 Swbl
curl -fsSL -o pluginval_macOS.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_macOS.zip
unzip -q -o pluginval_macOS.zip -d pluginval
pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --validate-in-process --timeout-ms 300000 \
  --validate ~/Library/Audio/Plug-Ins/Components/Sawblade.component
```

python:

```
python3.11 -m venv match/.venv
match/.venv/bin/pip install -e 'match[dev]'
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PYTHON=ON -DSAWBLADE_BUILD_TESTS=OFF \
  -DPython_EXECUTABLE=$PWD/match/.venv/bin/python
cmake --build build --parallel
SAWBLADE_CORE_DIR=$PWD/build/python SAWBLADE_TONERENDER=$PWD/build/cli/tonerender match/.venv/bin/pytest match -rs
```

Lint the workflow with `actionlint` (pinned release, e.g. v1.7.9): `actionlint .github/workflows/ci.yml`.

## Caching

- **FetchContent**: every job configures with `-DFETCHCONTENT_BASE_DIR=$GITHUB_WORKSPACE/.deps`; that
  directory is cached with `actions/cache`, keyed on OS + job + `hashFiles('cmake/Dependencies.cmake')`.
  Changing a pin in `cmake/Dependencies.cmake` (which also holds the JUCE pin) invalidates it.
- **ccache**: compiler launchers are set at configure time; `CCACHE_DIR` is `$GITHUB_WORKSPACE/.ccache`,
  capped by `CCACHE_MAXSIZE=1500M`. The cache key ends in `github.run_id` (always a fresh save) with a
  `restore-keys` prefix per OS + job, so each run starts from the most recent cache of the same job.
  `ccache -s` is printed at the end of each job.
- **pip** (python job): `actions/setup-python` with `cache: pip`, keyed on `match/pyproject.toml`.

## Bumping pins

- **Actions** are pinned to full commit SHAs with a `# vX.Y.Z` comment. To bump: pick the new tag, then
  `git ls-remote https://github.com/<owner>/<action> "refs/tags/<tag>" "refs/tags/<tag>^{}"` and use the
  `^{}` (peeled) SHA if the tag is annotated, otherwise the plain one. Update every use of the action
  (`grep -n 'actions/cache@' .github/workflows/ci.yml`) and the comment.
- **pluginval**: change `PLUGINVAL_VERSION` in the workflow `env`, and the two `sha256` values in the
  download steps (`sha256sum pluginval_Linux.zip`, `shasum -a 256 pluginval_macOS.zip`). Also update the URL
  in this file.
