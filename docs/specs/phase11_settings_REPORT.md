# Phase 11 report: settings, first-run flow, About box, app icon, ToolRunner

Lead report. Spec: `docs/specs/phase11_settings.md`. Branch `claude/sawblade-p11-settings`,
head `d404995`. Screenshot artifact: https://claude.ai/artifact/4MyefN9V5XxtR3ym25syZ8

## What landed

| item | where | state |
|---|---|---|
| Settings store (JSON, typed API, atomic 0600 writes, unknown keys kept, read-modify-write) | `plugin/src/settings/Settings.{h,cpp}` | done |
| Match-venv auto-detect (env var, build-tree `match/.venv`, `~/sawblade/match/.venv`, unset) | `Settings.cpp` `detectMatchVenv` | done, tested in every order |
| Secret-key refusal (`t3k_cs_` anywhere, any case: refused, never stored, dropped on load, redacted in tool output) | `Settings.cpp`, `ToolRunner.cpp` | done |
| `ToolRunner` (resolve executable, env via `/usr/bin/env`, line streaming, cancel, timeout, JSON result, message-thread or worker callbacks) | `plugin/src/settings/ToolRunner.{h,cpp}` | done |
| Settings panel (gear in the top bar; Browse/Auto/Default, Test, Log in to TONE3000 with big copyable code + Open, cache stats, separation model, takes dir, theme/scale, About) | `plugin/src/settings/SettingsPanel.{h,cpp}`, `LoginFlow.h` | done |
| First-run checklist (tools / logged in / captures, status lights, LOCATE / LOG IN / FETCH CAPTURES, once per process, DONE writes the file) | `SettingsPanel.cpp` | done |
| About box (version · git hash · build date, JUCE AGPLv3 note, captures-in-this-preset with creator / licence / nc tag / TONE3000 link / on-disk dot, full THIRD_PARTY.md) | `plugin/src/about/*`, `BuildInfo.h.in` | done |
| App icon (Pillow, deterministic, 16..1024 px, `--check`), `ICON_BIG` / `ICON_SMALL` in CMake | `design/render/app_icon.py`, `plugin/assets/icon/` | done |
| `sawblade-t3k login --json` / `whoami --json` (never prints the refresh token) | `match/sawblade_match/t3k/cli.py` | done, reviewer ACCEPT |
| Docs | `docs/PLUGIN.md` "Settings, ToolRunner, first run, About" | done |

## Merge with the working branch

The task branched before p8 (capture browser), p10 (rig editor), 7b (pedals), p9 (mic / presets),
6a (record + match) and 5.1b (separator) merged into
`claude/sawblade-plugin-setup-7k0b8q`. Those sessions had each built their own executable-path
setting, exactly the duplication phase 11 was meant to prevent. The merge (`9e07de4`) unified them
with the smallest edits:

- **One settings file.** `Settings` now lives at `<appdata>/settings.json`, the file the preset
  browser (`presets/T3kTool`) already used; both preserve each other's keys, saves are atomic,
  0600 and read-modify-write. Linux: `~/.local/share/sawblade/settings.json` (XDG honoured),
  macOS `~/Library/Application Support/Sawblade/settings.json`; overrides
  `SAWBLADE_SETTINGS_FILE` > `SAWBLADE_APPDATA` > `SAWBLADE_DATA_DIR`. The spec's `~/.config`
  location was dropped for this. The TONE3000 token file stays where the Python side keeps it
  (`~/.config/sawblade/t3k_tokens.json`).
- **One `appDataDir()`.** Upstream had two definitions (inline in `AppPaths.h`, out-of-line in
  `T3kTool.cpp`, different env vars: an ODR violation that decided which settings file a feature
  used by link order). `AppPaths.h` is now the single definition honouring both env vars, with a
  test that every resolver agrees.
- **One tool path.** `MatchSettings::defaultMatchExecutable/defaultExportExecutable`,
  `BrowserSettings::defaultExecutable` and `settings::defaultT3kExecutable` fall back to
  `Settings::shared().toolPath(...)` when a venv is set or detected; the explicit per-feature
  overrides those features already had still win. `T3kTool::setT3kExecutable` now keeps the file
  0600.
- **One login protocol.** Upstream's capture browser already ran `login --json-events` and
  `whoami --json` with a global `{"error", "code"}` wrapper and exit 4 for "not logged in".
  Ours became an alias: `--json` = `--json-events`; `logged_in` and `whoami` gained
  `username / display_name / id / token_file` (best effort on login); errors keep upstream's shape.
  Two upstream tests that already failed on upstream (expecting exit 1 where the CLI returns 4)
  were aligned with the documented exit code. `T3kClient` ignores the extra keys (asserted).
- The gear button sits after RIG in the new top bar (preset button 170 px, latency chip 136 px).
- **Takes folder.** `TakeRecorder` resolves the Settings takes folder lazily when a take starts (an
  explicit `setTakesDir` still wins), so the panel control applies live and constructing a
  processor never touches the settings file.
- **Capture cache dir.** A stored `captureCacheDir` is applied through a new mutex-guarded core
  override (`sawblade::setCaptureCacheRootOverride`, consulted by `captureCacheRoot()`, load-time
  only) rather than by mutating the process environment, which would race with `getenv` on the
  loader thread inside a host. Child tools still get `SAWBLADE_CACHE_DIR` explicitly from
  `ToolRunner`.

## Verification

Full ctest 538 / 538 passed (1 env-gated htdemucs skip), run by the reviewer at d404995. pluginval `--strictness-level 10` on the VST3 passes. clang `-Werror` clean on our
targets. Match suite: 245 passed, 8 skipped (the two `sawblade_core`-bound files need the pybind
module, not built here). Acceptance tests 1-17 of the spec exist and pass
(`plugin/tests/test_settings.cpp`, 15 cases; phase 11 block of `plugin/tests/test_editor.cpp`;
`plugin: app icon is reproducible`). Screenshots viewed by the lead:
`build-plugin/screenshots/sawblade_{settings,firstrun,login,about}_1x.png`.

Reviewer: ACCEPT (round 3). Round 1 REVISE: takes-dir default ignored the app-data env vars, capture on-disk check ignored the TONE3000 cache fallback, load errors not shown, two upstream tests reading the real home. Round 2 REVISE: TakeRecorder read Settings in the processor constructor (real-home reads from every headless test, inert panel control) and the setenv-based cache-dir coherence raced with getenv on the loader thread. Both fixed as described above; the reviewer also verified under strace that the plugin test binary never opens the settings or token file in a fresh HOME.

## Decisions

- `token_file` is reported by `whoami --json` even when the file does not exist (a token seeded
  from `TONE3000_REFRESH_TOKEN`); the panel only uses it as a hint.
- `expires_in` is an integer (truncated).
- The "Logged in" checklist light is the offline token-file check; the Test button and the login
  flow give the authoritative answer. No timer polls TONE3000.
- BuildInfo's git hash is taken at configure time (so it reads `-dirty` in a working tree).
- Windows paths (`Scripts\`) are out of scope; `appDataDir()` lost T3kTool's `%APPDATA%` branch.

## Follow-ups (not built, proposals)

- `docs/THIRD_PARTY.md` still says "Sawblade is a commercial product"; the About box shows that
  text under a note saying the opposite. Queued as item 7 of `docs/specs/licence_noncommercial.md`.
- Regenerate `BuildInfo.h` on every build (a custom command) so the hash is never stale.
- Preset names longer than the 170 px button truncate; elide with a tooltip or reclaim width.
- The separation-model setting is stored but not consumed: its only consumer is the play-along's
  per-instance `fourStemModel` (saved host state). The combo's tooltip says so. Seeding new
  instances from the stored value is a small change in `PlayAlong`.
- The 6a / 8 / 9 code still carries its own child-process runners (`JobRunner`, `T3kRunner`,
  `T3kTool`); they now share the executable resolution, and can adopt `ToolRunner` for the process
  handling when they are next touched (`docs/PLUGIN.md` has the recipe).
- Settings are loaded once per process; a file edited by another process is not re-read until
  the next launch.

## Process

- Lead wrote the spec, delegated: match-engineer (section 8) and dsp-engineer (the rest) in
  parallel, each committing only its own paths on the shared branch.
- The match change went through two reviewer rounds (REVISE: catch-all exception handling in JSON
  mode and a flush-before-poll test; then ACCEPT).
- The dsp-engineer (sonnet) was cut off three times by the account's per-model rate limit. The
  lead checkpoint-committed the on-disk work each time (`04cdc36`, `0a98200`) and resumed the same
  agent with its context, so nothing was redone.
- The integration lead asked for the moved working branch to be merged here first. The lead
  started the merge; the two engineers resolved disjoint files concurrently (cli.py vs plugin/docs)
  without committing; the lead committed the merge (`9e07de4`) and pushed.
- Reviewer rounds on the plugin diff: REVISE, REVISE, ACCEPT; every must-fix and the promoted should-fix items are test-backed. Head accepted: d404995.
