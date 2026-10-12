# Spec: Phase 11 — Settings, first-run flow, About box, app icon, ToolRunner

Lead spec. Branch `claude/sawblade-p11-settings`. Implementers: `dsp-engineer` (everything under
`plugin/`, `design/render/app_icon.py`, CMake, docs) and `match-engineer` (one small change in
`match/`, section 8). Reviewer audits before acceptance.

## Why

The plugin cannot be used without a terminal today. The user has been bitten by three things:
being silently logged out of TONE3000, presets that reference captures that are not on disk, and
having to find the right `sawblade-t3k` / `sawblade-match` executables. Three parallel sessions
(6a record+match, 8 capture browser, 9 presets) each assume "a settings field with the executable
path". This phase builds that once: a settings store, a Settings panel with a TONE3000 login flow, a
first-run checklist, an About box with attribution, an app icon, and one shared child-process
helper (`ToolRunner`) the other sessions adopt.

Constraints (CLAUDE.md): nothing here touches the audio thread. New code goes in new files under
`plugin/src/settings/` and `plugin/src/about/`; edits to `PluginEditor.*` / `PluginProcessor.*`
are minimal (others edit them in parallel) and listed in section 9. No scope creep: anything not
asked for below goes in the report as a proposal.

## 1. Settings store — `plugin/src/settings/Settings.{h,cpp}`

Namespace `sawblade::plugin::settings`. JUCE-free except `juce::File` for the special-location
lookups (keep the parsing in nlohmann::json so the headless tests need no message loop).

### File location

| platform | file |
|---|---|
| macOS | `~/Library/Application Support/Sawblade/settings.json` |
| Linux | `~/.config/sawblade/settings.json` (`$XDG_CONFIG_HOME/sawblade/settings.json` if `XDG_CONFIG_HOME` is set) |
| any | override: `SAWBLADE_SETTINGS_FILE=<path>` (tests, portable setups) |

`Paths::settingsFile(const Env&)` returns it. `Paths` also exposes the sibling locations the
Python tools use, so the plugin and `sawblade-t3k` agree without talking:

| what | default | env override |
|---|---|---|
| TONE3000 token file | `~/.config/sawblade/t3k_tokens.json` (all platforms; this is what `match/sawblade_match/t3k/auth.py` uses) | `SAWBLADE_T3K_TOKEN_FILE` |
| capture cache | `~/.cache/sawblade/captures` | `SAWBLADE_CACHE_DIR` |
| takes | macOS `~/Library/Application Support/Sawblade/takes`, Linux `~/.local/share/sawblade/takes` (6a spec) | — |

### Schema (`settings.json`, version 1)

```json
{
  "version": 1,
  "matchVenvDir": "/Users/me/sawblade/match/.venv",   // optional; absent = auto-detect
  "captureCacheDir": "/Volumes/big/captures",          // optional; absent = env / default
  "tone3000ClientId": "t3k_pub_…",                     // optional; the PUBLISHABLE key only
  "separationModel": "htdemucs_6s",                    // "htdemucs_6s" (default) | "htdemucs" | "htdemucs_ft"
  "takesDir": "/Users/me/Music/takes",                 // optional; absent = default above
  "theme": "dark",                                     // only "dark" exists; stored for later
  "uiScale": 1.0,                                      // 0.5 .. 2.0; initial editor size = design size x uiScale
  "firstRunCompleted": true
}
```

Rules:
- Unknown top-level keys are preserved on a round trip (parallel sessions may add keys).
- Writes are atomic (write `settings.json.tmp` then rename) and the file is created with mode
  0600 and its directory with 0700 on POSIX.
- A malformed file never crashes: `load()` returns defaults plus an `error` string the panel shows;
  the next `save()` overwrites it.
- Values are validated on set: `uiScale` clamped to [0.5, 2.0]; `separationModel` must be one of
  the three names; `theme` must be `"dark"`; paths are stored as written (no expansion).

### The secret-key rule

`Settings::setTone3000ClientId(std::string) -> Result` **refuses** any value that contains
`t3k_cs_` (anywhere, any case) with the message:

> That is a TONE3000 *secret* key (t3k_cs_…). Sawblade never stores it. Use the *publishable* key
> (t3k_pub_…) from tone3000.com › Settings › API keys.

The refused value is not stored, not written, not logged. Leading/trailing whitespace is trimmed.
A value that does not start with `t3k_pub_` is accepted with a warning in the result (`warning`
string: "does not look like a publishable key (t3k_pub_…)") so a future key format still works.
Empty clears the key. `Settings::load()` also drops a `t3k_cs_` value found in an existing file
(and reports it in `error`).

### API

```cpp
struct Env {                                   // injectable for tests; Env::system() reads the process
  std::function<std::optional<std::string>(std::string_view name)> getenv;
  std::filesystem::path home;                  // ~
  std::filesystem::path executable;            // the running binary (juce::File::currentExecutableFile)
  std::filesystem::path sourceDir;             // compile-time SAWBLADE_SOURCE_DIR ("" when unknown)
  std::function<bool(const std::filesystem::path&)> exists;
  bool isMac;
};

class Settings {
 public:
  explicit Settings(std::filesystem::path file, Env env = Env::system());   // does not touch disk
  static Settings& shared();                   // the process-wide instance at Paths::settingsFile(Env::system()), load()ed once

  bool fileExists() const;
  bool isFirstRun() const;                     // no file existed when this instance first load()ed
  void markFirstRunCompleted();                // sets firstRunCompleted = true and save()s
  std::string load();                          // "" or error text
  std::string save();                          // "" or error text

  // typed getters: the stored value (optional) and the effective value (with auto-detect / env / default)
  std::optional<std::filesystem::path> matchVenvDir() const;        std::optional<std::filesystem::path> effectiveMatchVenvDir() const;
  std::optional<std::filesystem::path> captureCacheDir() const;     std::filesystem::path effectiveCaptureCacheDir() const;
  std::string tone3000ClientId() const;                             std::string effectiveTone3000ClientId() const;  // stored, else TONE3000_CLIENT_ID env (never a t3k_cs_ value)
  std::string separationModel() const;         // default "htdemucs_6s"
  std::optional<std::filesystem::path> takesDir() const;            std::filesystem::path effectiveTakesDir() const;
  std::string theme() const;  double uiScale() const;
  // setters: set + save(); return Result{ok, error, warning}
  Result setMatchVenvDir(std::optional<std::filesystem::path>);  Result setCaptureCacheDir(std::optional<std::filesystem::path>);
  Result setTone3000ClientId(std::string);  Result setSeparationModel(std::string);  Result setTakesDir(std::optional<std::filesystem::path>);
  Result setTheme(std::string);  Result setUiScale(double);

  std::filesystem::path tokenFile() const;     // Paths::tokenFile(env)
  std::filesystem::path toolPath(std::string_view tool) const;  // <effective venv>/bin/<tool> ("" if no venv)
  void addListener(Listener*); void removeListener(Listener*);  // settingsChanged() on the calling thread after every save
};
```

### Auto-detect order for the match venv (`detectMatchVenv(const Env&)`)

A candidate is **valid** when `<dir>/bin/sawblade-t3k` exists (`exists` predicate). First valid wins:

1. `SAWBLADE_MATCH_VENV` env var.
2. Build tree: when `env.sourceDir` is non-empty and `env.executable` lies inside `env.sourceDir`
   (the plugin runs from `build-plugin/`, `build-mac/`, … inside the checkout):
   `<sourceDir>/match/.venv`.
3. `~/sawblade/match/.venv` (the `docs/MAC.md` layout).
4. unset (`std::nullopt`).

`SAWBLADE_SOURCE_DIR` is a compile definition on the plugin sources (`"${PROJECT_SOURCE_DIR}"`).
When the stored `matchVenvDir` is set it wins over detection, even when invalid (the panel then
shows the "not found" light; the user asked for it).

## 2. `ToolRunner` — `plugin/src/settings/ToolRunner.{h,cpp}`

One child-process helper for every feature that runs a `sawblade-*` tool (6a, 8, 9 adopt it later).

```cpp
struct ToolRequest {
  std::string tool;                                    // "sawblade-t3k", "sawblade-match", …
  std::vector<std::string> args;
  std::map<std::string, std::string> env;              // added to the child's environment
  std::optional<std::filesystem::path> executable;     // bypass resolution (tests, "Locate…")
  std::chrono::milliseconds timeout{0};                // 0 = none
  bool callbacksOnMessageThread = true;                // false: callbacks on the worker thread (headless tests)
};

struct ToolResult {
  enum class Outcome { Ok, NonZeroExit, StartFailed, Cancelled, TimedOut };
  Outcome outcome;  int exitCode = -1;
  std::string error;                                   // human text for StartFailed / TimedOut
  std::vector<std::string> lines;                      // every complete line, stdout and stderr merged, redacted
  std::string text;                                    // lines joined with '\n'
  std::optional<nlohmann::json> json;                  // see "JSON result"
  std::filesystem::path executable;                    // what was run
};

class ToolRunner {
 public:
  explicit ToolRunner(Settings& settings);
  // Resolution: request.executable, else settings.toolPath(tool) if it exists, else the first match
  // on PATH, else StartFailed with: "<tool> not found. Set the match venv in Settings (gear icon)."
  std::filesystem::path resolve(std::string_view tool, std::string* error) const;

  class Job {
   public:
    void cancel();                                     // kills the child; result.outcome = Cancelled
    bool isRunning() const;
    bool wait(std::chrono::milliseconds);              // tests / synchronous callers
    const ToolResult& result() const;                  // valid once !isRunning()
  };
  std::shared_ptr<Job> run(ToolRequest,
                           std::function<void(const std::string& line)> onLine,
                           std::function<void(const ToolResult&)> onDone);
};
```

Behaviour:
- Runs on its own worker thread (`juce::Thread` or `std::thread`), using `juce::ChildProcess`
  with stdout and stderr merged (JUCE cannot separate them). Lines are split on `\n` as they
  arrive; `onLine` fires per complete line; the remainder is flushed at exit. `\r` is stripped.
- Environment: `juce::ChildProcess` inherits the parent environment and takes no env table, so
  on POSIX the command is `/usr/bin/env KEY=VALUE … <exe> <args…>`. ToolRunner always adds
  `PYTHONUNBUFFERED=1`, `TONE3000_CLIENT_ID=<effective client id>` (only when non-empty),
  `SAWBLADE_CACHE_DIR=<effective cache dir>`, then `request.env` (which wins).
- **Redaction**: before a line is stored or delivered, every match of `t3k_cs_[A-Za-z0-9_-]+` is
  replaced by `t3k_cs_[redacted]`. (The login flow uses `--json`, which never prints the refresh
  token, see section 8; redaction is a second net.)
- JSON result: `json` is the last line that parses as a JSON object or array; if none, the whole
  `text` if it parses; else `nullopt`. Never throws.
- `cancel()` from any thread; the worker joins within 2 s in tests. Destroying the `ToolRunner`
  cancels and joins every job it started. A `Job` outlives the runner safely (shared_ptr).
- Callbacks: with `callbacksOnMessageThread` they are posted with
  `juce::MessageManager::callAsync` and dropped if the runner is gone; otherwise they run on the
  worker thread. `onDone` is called exactly once, after the last `onLine`.
- Never used from the audio thread; nothing here is real-time.

## 3. Settings panel — `plugin/src/settings/SettingsPanel.{h,cpp}`

An overlay like `PlayAlongPanel`, docked over the rig area (0, kTopBar) to (kRigW, kDesignHeight),
closed by default, toggled by a **gear button** in the top bar (title "Settings", tooltip
"Settings: tool paths, TONE3000 login, cache"). Esc and a × button close it. Open/closed is UI
state, never saved. It reads and writes `Settings::shared()` (tests construct the panel with a
`Settings&` of their own). Skin palette and `SawbladeLookAndFeel` fonts; plain widgets.

Sections, top to bottom (scrollable `juce::Viewport` if it does not fit):

1. **Checklist** (section 4) — shown expanded on first run, collapsed to one status line otherwise
   ("Setup: 3/3 ok" or "Setup: 1 problem") with a SHOW button.
2. **Tools**
   - Match venv: path field, Browse… (directory chooser), Auto button (clears the stored value;
     field shows the detected value greyed with "(auto)"), status light + text: "found:
     sawblade-t3k, sawblade-match" / "not found: no bin/sawblade-t3k in …".
   - Test button: runs `sawblade-t3k --help` through ToolRunner; shows "ok (exit 0, 120 ms)" or the
     error.
3. **TONE3000**
   - Client id field (publishable key). On commit → `setTone3000ClientId`; a refusal shows the
     message in red under the field and restores the field to the stored value; a warning shows in
     amber. The field never echoes a `t3k_cs_` value.
   - Status line: "Token file present (…/t3k_tokens.json)" / "Not logged in" from
     `Settings::tokenFile()` existence (offline, immediate).
   - **Test** button: runs `sawblade-t3k whoami --json`; shows "Logged in as @user (display name)"
     or the error text (e.g. "TONE3000_CLIENT_ID is not set…", "not logged in: run login").
   - **Log in to TONE3000** button: runs `sawblade-t3k login --json`. On the `device_code` event
     the panel shows a login box: the user code in a big mono font (≥ 36 px), the verification
     URL, buttons COPY CODE, COPY URL (system clipboard), OPEN (`juce::URL::launchInDefaultBrowser`
     on `verification_uri_complete` if present else `verification_uri`), CANCEL (kills the job),
     and "Waiting for approval… (expires in N s)" counting down. The CLI polls TONE3000 itself; on
     the `logged_in` event the box shows "Logged in as @user", the status line refreshes and the
     box closes after 3 s. On `error` / non-zero exit the message is shown in red with a RETRY
     button.
4. **Captures**
   - Cache dir: path field, Browse…, Default button; text "N captures on disk (M MB)" counted
     off the message thread (count `*.nam` + `*.wav` recursively, once per open and after a fix).
   - Open folder button (`juce::File::revealToUser`).
5. **Separation**: combo with the three model names (default htdemucs_6s); a one-line note
   "htdemucs_6s has a guitar stem; htdemucs (4 stems) maps other→guitar".
6. **Recording**: takes dir field, Browse…, Default.
7. **Appearance**: theme combo (only "Dark", disabled, tooltip "more themes later"); UI scale
   combo 75 % / 100 % / 125 % / 150 % / 200 % → `uiScale`; note "applies when the window is next
   opened".
8. Footer: "Settings file: <path>" (click reveals it), **About Sawblade…** button (section 5).

Every control has a title (accessibility test) and the same `configure()` look as the editor's
buttons. The panel polls nothing on a timer except the login countdown and the whoami/test jobs'
completion (both via ToolRunner callbacks).

## 4. First-run flow

`Settings::shared().isFirstRun()` is true when no settings file existed at first load. The
editor's constructor opens the Settings panel with the checklist expanded **once per process**
(a process-wide `std::atomic<bool>` so a DAW with 12 instances shows it once; tests reset it
through `SettingsPanel::resetFirstRunShownForTests()`). Closing the panel, or pressing its DONE
button, calls `markFirstRunCompleted()` (which writes the file, so it never shows again).

Checklist rows, each with a status light (`LedIndicator`-style dot: green ok / amber warning /
red problem / grey unknown), one line of text, and one fix button:

| step | ok when | text | fix button |
|---|---|---|---|
| Tools found | `effectiveMatchVenvDir()` set and `bin/sawblade-t3k` + `bin/sawblade-match` exist | "sawblade-t3k, sawblade-match in <venv>" / "match venv not found" | LOCATE… (= Browse for the venv dir) |
| Logged in | token file exists (offline check); after a Test / login, the whoami result | "token file present" / "logged in as @user" / "not logged in" | LOG IN (= the login flow) |
| Captures cached | every tone3000 capture of the current preset has a `resolvedPath` that exists on disk, **and** the cache dir exists | "all N captures of this preset on disk" / "K of N captures missing" / "no cache dir yet" | FETCH CAPTURES |

FETCH CAPTURES: writes `processor.currentPreset()` to `<cache dir>/_resolve/<presetName>.json`
(`toJson`), runs `sawblade-t3k resolve <that> -o <cache dir>/_resolve/<presetName>.resolved.json`
through ToolRunner (streaming lines into a small log area), and on exit 0 calls
`processor.loadPresetFile(resolved)`. Errors show in red in the row. This is the whole fix for "a
preset that references captures not on disk" and uses only existing processor API.

The checklist re-evaluates when the panel opens, after each fix job, and when the preset changes
(the editor's existing timer tick calls `panel.refresh()` while visible, like the play-along panel).

## 5. About box — `plugin/src/about/AboutBox.{h,cpp}`, `plugin/src/about/BuildInfo.h.in`

Opened from the Settings footer (`AboutBox::show(processor, settings)`, a `juce::DialogWindow`
or an overlay over the whole editor; the overlay is preferred so it works in every host and in
the editor tests). Contents:

1. Icon (`icon_256.png` embedded) + "SAWBLADE" + version line: `Sawblade 0.1.0 · <git hash> ·
   built <date>`. `BuildInfo.h` is generated by CMake (`configure_file`) with `PROJECT_VERSION`,
   `git rev-parse --short HEAD` (+"-dirty" when the tree is dirty; "unknown" without git) and
   `string(TIMESTAMP … UTC)` at configure time.
2. Licence note (verbatim): "Built with JUCE 8 under the AGPLv3 for personal, non-commercial use.
   Sawblade is not sold. Giving a binary to anyone else requires publishing the source under the
   AGPLv3 or a JUCE licence (docs/THIRD_PARTY.md)."
3. **Captures in this preset**: one row per capture of `processor.currentPreset()` that has a
   `source` (walk `paths[*].blocks[*].model`, `cab.ir`, `cab.irA/irB`): slot ("Saw pedal",
   "Body amp", "Cab A", …), title (or file name), creator, licence (exactly as stored; empty →
   "licence: unknown"), a clickable TONE3000 link (`source.url`, else
   `https://www.tone3000.com/tones/<id>`) opened in the browser, and a dot: green if the file is
   on disk, red if missing. Non-commercial licences (`-nc`) add the tag "non-commercial". Captures
   without a `source` are listed with their file name and "local file, no TONE3000 metadata". The
   list is the CLAUDE.md attribution rule made visible.
4. **Third-party**: `docs/THIRD_PARTY.md` embedded through `juce_add_binary_data` and shown in a
   read-only, monospace, scrollable `juce::TextEditor` (plain text; tables stay as source). Put the
   file in the `SawbladeAssets` list.
5. CLOSE button.

## 6. App icon

`design/render/app_icon.py` (Pillow + numpy only, deterministic, no bpy, no fonts): the saw blade
motif from `pedal_b2.py` (16-tooth blade, concentric rings, stencil bridges) in saw orange
`#ff6a1a` with bone `#e8e1d2` ring highlights on the dark `#0b0a09` background, inside the macOS
rounded-square (corner radius 22.4 % of the side, 10 % transparent margin around the square so
Finder spacing matches Apple icons). Supersample 4× and downsample with Lanczos. Outputs
`plugin/assets/icon/icon_{16,32,64,128,256,512,1024}.png`; `--check` re-renders to a temp dir and
compares bytes with the committed files (the test below runs it). Commit the PNGs; total under
600 kB. Document the script in `design/render/README.md` (one table row + one line) and
`plugin/assets/README.md`.

CMake: `juce_add_plugin(... ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon_1024.png"
ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon_256.png")`. Embed `icon_256.png` in
`SawbladeAssets` for the About box. `check_assets.cmake`: count the budget with `GLOB_RECURSE`
so `icon/` is included.

## 7. Minimal edits to shared files

`PluginEditor.cpp` (keep each change to the fewest lines):
- top bar: a gear `juce::TextButton` (`"⚙"`, title "Settings") after PLAY ALONG. Make room by
  narrowing the preset button 240 → 214 and the latency chip 170 → 136 in `resized()`.
- construct `SettingsPanel` after `PlayAlongPanel` (on top of it), `setBounds` in `resized()`,
  `refresh()` from the existing timer tick while visible, first-run open in the constructor.
- `setSize(kDesignWidth * uiScale, kDesignHeight * uiScale)` in the `SawbladeEditor` constructor.
- `PluginEditor.h`: `setSettingsOpen(bool)`, `settingsOpen()`, `aboutOpen()` for the tests.

`plugin/CMakeLists.txt`: new sources, `SAWBLADE_SOURCE_DIR` define, `BuildInfo.h` generation,
icon args, `docs/THIRD_PARTY.md` + `icon_256.png` in `SawbladeAssets`, the new test file, and
`sawblade_plugin_tests` gets `SAWBLADE_TEST_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/tools"` for the fake
scripts. `PluginProcessor.*`: **no change** (everything needed is public already).

## 8. `match/`: `--json` for `sawblade-t3k login` and `whoami` (match-engineer)

`match/sawblade_match/t3k/cli.py`:
- `whoami --json`: prints exactly one line `{"username": …, "display_name": …, "id": …,
  "token_file": "<path>"}`; on error prints one line `{"error": "<message>"}` to stdout and exits 1
  (keep the plain-text behaviour without `--json`).
- `login --json`: one JSON object per line, flushed immediately:
  `{"event": "device_code", "user_code": …, "verification_uri": …, "verification_uri_complete": … | null, "expires_in": <seconds>}`
  then, after `poll_for_session`, `{"event": "logged_in", "username": …, "display_name": …, "id": …, "token_file": "<path>"}`
  (fetch the user through a `T3KClient` built from the new session). On failure
  `{"event": "error", "message": "<text>"}`, exit 1. **In `--json` mode the refresh token is never
  printed** (the plain-text mode keeps printing it for the container workflow).
- Tests (`match/tests`, respx): whoami json ok and error shapes; login json event sequence from
  a mocked device flow; the refresh token string does not appear anywhere in the captured stdout
  in `--json` mode. Full match suite stays green. `match/README.md`: one paragraph on `--json`.

## 9. Acceptance tests

Headless, `plugin/tests/test_settings.cpp` (ctest prefix `plugin: `), temp dirs only, no network:

1. **Round trip**: set every key, `save()`, load into a new instance → equal; an unknown key
   `"future": {"x": 1}` in the file survives save; file mode is 0600; the `.tmp` is gone.
2. **Defaults**: no file → defaults (`separationModel == "htdemucs_6s"`, `uiScale == 1.0`, no venv,
   cache = env or `~/.cache/sawblade/captures`, takes default per platform); a malformed file →
   defaults + non-empty `load()` error, and `save()` then produces a valid file.
3. **First run**: no file → `isFirstRun()`; `markFirstRunCompleted()` → file exists, a fresh
   instance on that file has `isFirstRun() == false`; a pre-existing file → false from the start.
4. **Auto-detect order** with an injected `Env` and temp dirs: (a) env var wins over a valid
   build-tree venv; (b) build-tree venv wins over `~/sawblade/match/.venv`; (c) build-tree venv is
   ignored when the executable is outside `sourceDir`; (d) an invalid candidate (dir without
   `bin/sawblade-t3k`) is skipped to the next; (e) nothing valid → `nullopt`; (f) a stored
   `matchVenvDir` wins over everything, even when invalid.
5. **Cache dir**: explicit > `SAWBLADE_CACHE_DIR` > default; `effectiveTakesDir()` default per
   platform (`isMac` both ways).
6. **Secret refusal**: `t3k_cs_abc`, ` T3K_CS_abc `, `xt3k_cs_y` are refused with the message
   above, nothing is stored or written (file content unchanged); `t3k_pub_abc` is stored;
   `other_key` is stored with a warning; empty clears; a file containing a `t3k_cs_` id loads
   with the key dropped and an error string.
7. **Tool path resolution**: venv `bin/<tool>` wins; else PATH (temp dir prepended to a fake PATH
   passed through `Env::getenv`); else error text contains "Settings".
8. **ToolRunner with fake scripts** in `plugin/tests/tools/` (committed `#!/bin/sh` scripts,
   executable bit set; `callbacksOnMessageThread = false`):
   - `lines.sh`: prints 5 lines with 50 ms sleeps → `onLine` receives all 5, in order, the first
     one before the job ends; `result.lines.size() == 5`; `Outcome::Ok`, exit 0.
   - `exit3.sh` → `NonZeroExit`, `exitCode == 3`.
   - `json.sh`: a log line, then a one-line JSON object → `result.json` is that object; a
     multi-line pretty JSON document as the whole output also parses.
   - `sleep.sh` (sleeps 30 s): `cancel()` after 200 ms → `onDone` within 2 s, `Cancelled`, and
     `kill -0 <pid>` fails afterwards (the script prints its `$$` first).
   - `env.sh`: echoes `TONE3000_CLIENT_ID`, `SAWBLADE_CACHE_DIR`, `PYTHONUNBUFFERED`, `EXTRA` →
     the values ToolRunner set (client id from settings, cache dir effective, `EXTRA` from
     `request.env`).
   - `secret.sh`: prints `token t3k_cs_SECRET123 end` → the delivered and stored line reads
     `token t3k_cs_[redacted] end`.
   - missing executable → `StartFailed`, error contains "Settings".
   - `timeout`: `sleep.sh` with `timeout = 300ms` → `TimedOut` within 2 s.
   - Destroying a `ToolRunner` with a running `sleep.sh` returns within 2 s.
9. **Login JSON parsing**: `LoginFlow` (the pure state machine behind the panel, in
   `SettingsPanel.cpp` or `LoginFlow.h`) fed the three event lines from section 8 produces
   `code`, `url`, `loggedInAs` as expected; a non-JSON line is ignored; `error` sets the message.

Editor tests (`plugin/tests/test_editor.cpp`, under xvfb; each test points `SAWBLADE_SETTINGS_FILE`
at a temp file before constructing the editor and calls `resetFirstRunShownForTests()`):

10. Gear button exists with title + tooltip; the panel is closed by default when a settings
    file exists; clicking opens and closes it; Esc closes it.
11. **First run**: no settings file → the panel is open with the checklist expanded and three
    rows; DONE closes it and the settings file now exists; a second editor in the same process
    does not open it; the existing accessibility test still passes with the new controls.
12. Typing `t3k_cs_x` in the client-id field and committing shows the refusal text and the
    stored id is unchanged; `t3k_pub_x` is stored.
13. Login view: with `matchVenvDir` pointing at a temp venv whose `bin/sawblade-t3k` is a fake
    script that prints the `device_code` line and sleeps, pressing LOG IN shows the code
    "ABCD-1234" and the URL; CANCEL ends the job. Screenshot `sawblade_login_1x.png`.
14. Checklist: with the fake venv (both tools present) the Tools row is green; with a token file
    created in a temp `SAWBLADE_T3K_TOKEN_FILE` the Logged-in row is green, without it red; a
    preset built in the test with one tone3000 capture whose file is missing turns the Captures
    row red with "1 of 1 captures missing".
15. About box: opens from the footer; version text contains the project version and a non-empty
    hash and date; the third-party text contains "JUCE" and "AGPLv3"; the captures list has one
    row per capture of a test preset with creator, licence, "non-commercial" for a `cc-by-nc`
    capture and a URL; CLOSE closes it. Screenshot `sawblade_about_1x.png`.
16. Icon: every `icon_<n>.png` decodes to an n×n image with transparent corners and an opaque
    centre; `design/render/app_icon.py --check` passes (ctest, needs python3 + Pillow; skipped
    with a message if Pillow is missing).
17. Screenshots `sawblade_settings_1x.png` (panel open, checklist collapsed) and
    `sawblade_firstrun_1x.png` (first-run checklist) under `build-plugin/screenshots/`.

Gates: full `ctest` (core + plugin + editor) green; clang `-Werror` build (`CC=clang CXX=clang++`)
clean; pluginval `--strictness-level 10` on the VST3 passes (the lead's background build puts
pluginval at `/home/user/pluginval/build/pluginval`; configure with
`-DSAWBLADE_PLUGINVAL_EXECUTABLE=` and run the `plugin: pluginval VST3` test). The existing zero-
allocation / lock tests stay green (nothing here touches `processBlock`).

## 10. Docs

- `docs/PLUGIN.md`: new section "Settings, ToolRunner, first run, About" — the file locations
  table, the keys, the auto-detect order, the `ToolRunner` API with a 15-line usage example
  (6a/8/9 adopt it: "resolve the tool, build a `ToolRequest`, stream lines, read `result.json`"),
  the redaction rule, the login event protocol, how the first-run flag works, and the About
  box contents. Update the "Editor" paragraph for the gear button and the top-bar widths.
- `docs/specs/phase11_settings_REPORT.md`: written by the lead after acceptance.

## 11. Out of scope (report as proposals, do not build)

Windows paths/`Scripts\`, a Standalone menu bar entry, theme switching, a login status poll on a
timer, markdown rendering of THIRD_PARTY.md, updating the "Sawblade is commercial" wording in
`docs/THIRD_PARTY.md` (queued in `licence_noncommercial.md` item 7).
