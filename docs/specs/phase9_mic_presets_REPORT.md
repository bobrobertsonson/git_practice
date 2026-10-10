# Phase 9 report: cab mic page + preset browser

Specs: `docs/specs/phase9a_mic_page.md`, `docs/specs/phase9b_preset_browser.md`.
Status: **both accepted by the lead after reviewer ACCEPT.** 9a needed two review rounds, 9b
two, and the Python part one.

Artifact: **Sawblade Mic + Presets**, https://claude.ai/artifact/XZCHM7QnaX9GCMse9rnTEj
(1x, 1280x800: the rig, the mic page with one mic, the mic page with BLEND 2 MICS, and the
preset browser). It is private until shared. The IR pack in the mic shots is a synthetic 14-IR
test pack. The preset carrying the NON-COMMERCIAL tag in the browser shot is a synthetic test
preset; real captures show their real licences.

## Commits
| Spec | Commits |
|---|---|
| Specs | `ae927ae`, `82a3666` (9b §3.3 added from user feedback) |
| 9a | `8fe9284` (core irMix), `ae7a7e0` (assets), `5446a48` (plugin), `ef34dc8`, `d4e54da` (docs), `7e60fc7` (review fixes) |
| 9a/9b Python | `b41a0b4`, `2c4c893` |
| 9b | `f752f03` (core category + capture-cache fallback), `3b1ce69` (plugin), `99760bc` (docs), `267b44d`, `a808e75` (review fixes) |

## What was built
### 9a: cab mic placement page and the `irMix` cab mode
- **Core: the `irMix` cab mode.**
  - Preset form: `{ "mode": "irMix", "irA", "irB", "mix" }`.
  - Each IR is loaded as `shared` loads one. The two are then summed into `(1−mix)·hA + mix·hB`
    with no re-normalisation, in `core/src/ir_mix.cpp`.
  - The sum runs on one convolver, with the same place and latency as `shared`, so the mode is
    live-compatible and the no-cab export is exact.
  - Strict keys and the writer round-trip are covered, and `docs/PRESET_SCHEMA.md` is updated.
- **Python.**
  - The export plan treats `irMix` as live-compatible.
  - New `sawblade-t3k pack <toneId>` downloads an IR tone's models and writes a manifest.
  - `--progress-json` is added to `pack` and `resolve`.
  - Every `sawblade-t3k` command exits 4 when the user is not logged in.
- **Plugin, `plugin/src/mic/`.**
  - **Name parser:** a tolerant parser turns IR model names into speaker, slot, mic, mic type,
    distance, position, numeric position and cab size, with "unknown" where a field can't be
    read.
  - **IR packs:** a pack comes from a `pack` manifest or a local WAV folder. Dots are laid out
    from geometry sidecars computed from the render scripts.
  - **Snapping:** a drag snaps to the nearest dot. Ties go to the lower model, and the mic and
    distance are kept when the dot has them.
  - **The page:** double-click the cab to open it. It shows the grille-off 4x12 or 2x12 render
    (rendered with bpy 4.2.0 and a new `--no-mic` flag), the pack card, the field readouts, the
    IR response, A/B and NEXT POSITION.
  - **BLEND 2 MICS:** switches the cab to `irMix`. The mix fader submits at most every 150 ms,
    and the latest submission wins.
  - **Swaps:** every swap goes through `processor.loadPreset()`, which is the normal off-thread
    loader with the 30 ms equal-power crossfade. There is no new audio-thread code.
- **`T3kTool`** (shared with 9b).
  - Runs `sawblade-t3k` as a child process from a settings path. The settings live in
    `<appdata>/sawblade/settings.json`, written read-modify-write.
  - Shows streaming progress, a not-logged-in message on exit 4, and LOCATE… for a missing
    executable.
  - Cancel kills the child.

### 9b: preset browser, factory presets, A/B compare
- **Core.**
  - **Category:** a new optional top-level `category` field, set in all 11 factory presets (the
    only change to those files).
  - **Capture-cache fallback (from user feedback, `barbaric_v4.json`).** When a TONE3000
    capture's file is missing, every core loader looks it up in the capture cache:
    - The cache is `$SAWBLADE_CACHE_DIR`, else `~/.cache/sawblade/captures`, at
      `<id>/<modelId>.nam|.wav`.
    - The sha256 is verified.
    - Ids must be plain tokens, so path traversal is rejected.
    - When the capture isn't cached either, the error says "run: sawblade-t3k resolve <preset>".
- **Plugin, `plugin/src/presets/`.**
  - **Library:** Factory banks (Classic / Styles / Matched) and a User bank, read-only for
    factory presets. Categories with counts, AND search, and save / save as / rename / delete,
    where delete moves the file to the OS trash.
  - **Resolve-on-load:** runs `sawblade-t3k resolve` with progress, and reuses a fresh resolved
    file without starting a child process.
  - **A/B compare:** two slots on the existing top-bar button.
  - **Prev / next:** ‹ › step through the filtered list.
  - **Info panel:** every capture with its creator, licence, URL and a NON-COMMERCIAL tag.
- **Edits to shared files:** `PluginEditor.*` (a few dozen lines of wiring), `skin/RigView.*` (+8),
  `plugin/CMakeLists.txt`. `PluginProcessor.*` is untouched.

## Tests and gates (as last verified by the reviewer)
- **Release ctest:** 299/299 pass, including the editor tests under xvfb and pluginval v1.0.4
  at strictness 10 on the VST3 (`SUCCESS`).
- **Clang `-Werror` build:** clean, and its ctest passes 298/298 (pluginval is not registered in
  that tree).
- **ASan/UBSan Debug** passed the core tests for irMix, the cache fallback, categories, captures
  and presets. The full ASan suite was stopped at 175 of 188 for time, with no failures up to
  that point.
- **pytest:** 294 passed and 13 skipped (the usual opt-in tests and missing optional
  dependencies). The `irMix` fold-equals-mix test ran against a built core.
- **Spec acceptance tests:**
  - the parser table (28 real-world formats, including every spec case);
  - the dot merge and snap rules;
  - `irMix` render equivalence within 1e-6, mix 0 bit-identical to `shared`, block-size
    invariance and zero allocations;
  - preset save/load round-trip including parameter values;
  - the A/B swap (one engine build per switch);
  - the resolve flow with a fake executable;
  - the info panel and NC tag.

## Lead decisions and known gaps
- **Not logged in:** exit 4 means not logged in. A missing `TONE3000_CLIENT_ID` is a
  configuration error and exits 1.
- **Packs:** `pack` only accepts IR tones. Its model ordering has only been checked against the
  mocked API, never the live one.
- **Mic overlay:** the open-cab renders are made without the baked-in mic (`--no-mic`), and the
  page draws its own.
- **Saved user presets** store absolute capture paths, as plugin state does. They are complete,
  but they only work on this machine.
- **A/B during a load:** pressing A/B while a load is still building waits up to 3 s on the
  message thread so the stale preset isn't stored. The reviewer found no deadlock. A later
  change could switch to the pending preset instead of waiting.
- **Closing the mic page** joins the `T3kTool` worker on the message thread. After the kill this
  is quick, but the worst case is 10 s.
- **Factory preset folder:** it is a settings path, defaulting to `<repo>/presets` through a
  compile definition. Presets are not embedded in the binary.
- **Old licence text:** `match/sawblade_match/t3k/licenses.py` still says "Sawblade is
  commercial". That predates this phase and contradicts CLAUDE.md, so it should be fixed in a
  tidy-up.
- **Stray process kill:** early in 9a the implementer ran a broad `pkill -f "sh -c"`, which may
  have killed a shell command in another session on this machine. No effect was seen in this
  session's work.
- **No TONE3000 files committed:** no capture or IR files from TONE3000 are committed. Tests
  synthesise IRs and presets in temp dirs.
