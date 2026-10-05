# Spec 8: capture browser in the plugin (Standalone first)

Reference: `design/mockups/FullBrowse.dc.html` (layout reference only; the user owns the UI design).
The user selects a pedal, amp or cab in the rig and clicks **BROWSE CAPTURES** (already in the
inspector, currently disabled). A full-editor overlay lists TONE3000 captures for that slot; the user
can **PREVIEW** a candidate (a built-in DI riff rendered through the current rig with the candidate
swapped in) and **USE** it (swap the block's model through the normal loader).

Two parts, two implementers:
- **8a (match-engineer)**: `sawblade-t3k` gains `fetch`, `models`, `list` and `--json` error output.
- **8b (dsp-engineer)**: `plugin/src/browser/` — the child-process client, the browser UI, preview
  render + playback, the swap. Uses only the JSON contract below, never imports Python.

## Licence rule for this phase

`licenses.py` still refuses `cc-by-nc*` (the switch in `licence_noncommercial.md` is queued, not
done). This phase keeps that: `fetch` refuses nc, `search`/`list` keep marking nc as not passing.
The plugin shows what the CLI says; it has **no licence logic of its own** besides displaying the
`license` string. The mockup's "Sawblade is a commercial product" text is outdated: the footer note
reads "CC BY-NC captures are not enabled in this build." If CC BY-NC is enabled later (needs the
user's approval), only Python changes.

## 8a. CLI additions (`match/sawblade_match/t3k/cli.py` + small helpers)

All new commands take `--json` and `--cache-dir`. With `--json`, stdout carries exactly one JSON
document and nothing else (notes and logs go to stderr). Errors with `--json`: stdout is
`{"error": "<message>", "code": "<code>"}` and exit status 1. Codes: `license` (licence refused),
`auth` (not logged in / token refresh failed / TONE3000_CLIENT_ID missing), `not_found` (tone or
model id unknown), `network` (httpx error), `error` (anything else that is a T3KError). Add
`--json` to `whoami` (`{"id":..,"username":..,"display_name":..}`) and to `search` errors.
Without `--json` the existing text behaviour is unchanged.

1. `sawblade-t3k models TONE_ID --json` (read-only, no download): the candidate models
   `list_candidates` picks (A2 then A1; IRs as today):
   `{"tone_id": int, "architecture": str, "models": [{"model_id": int, "name": str, "size": str|null}]}`.
   Use whatever name/size fields `types.Model` already has; document the mapping.
2. `sawblade-t3k fetch TONE_ID [--model MODEL_ID] --json`: looks up the tone, `check_license`
   (same policy as `pull`: nc and unknown licences refused → code `license`, nothing downloaded),
   picks `--model` (must be one of `models`' list, else `not_found`) or the first candidate,
   downloads through `ensure_capture` into the cache (a cache hit downloads nothing), prints
   ```
   {"tone_id": int, "model_id": int, "path": "<absolute local path>", "sha256": "<hex>",
    "kind": "nam" | "ir", "gear": str,
    "source": {"provider": "tone3000", "id": "<tone id>", "modelId": "<model id>", "url": str,
               "title": str, "creator": str, "license": str}}
   ```
   `source` uses exactly the preset `CaptureSource` member names (`docs/PRESET_SCHEMA.md`), so the
   plugin copies it verbatim.
3. `sawblade-t3k list --source favorites|pool [--query Q] [--gear amp pedal ir] [--limit N] --json`:
   same record shape and filter verdict as `search --json` (reuse `assess`). `favorites` uses
   `list_favorited` (with `query` passed through); `pool` reads `<cache>/pool_manifest.json` (no
   network; missing manifest → empty list, not an error) and maps its entries to the same record
   shape (fields the manifest lacks → `null`).
4. Never print tokens with `--json`. `login` is unchanged except: add `--json-events`, which prints
   one JSON line `{"event":"device_code","verification_uri":..,"verification_uri_complete":..,
   "user_code":..,"expires_in":..}` when the code is known, then `{"event":"logged_in"}` on success,
   and does **not** print the refresh token (it is still saved by `TokenStore`; the plugin never sees it).

Tests (pytest, fully offline, fake `httpx` transport as the existing tests do): each command's JSON
shape; `fetch` refuses `cc-by-nc` and an unknown licence with code `license` and no file written;
`fetch` cache hit makes no download request; `--model` not in the list → `not_found`; `list
--source pool` with and without a manifest; `login --json-events` never prints the refresh token;
existing tests unchanged and green.

## 8b. Plugin (`plugin/src/browser/`)

New files only, except the minimal hooks listed under "Edits to existing files".

### T3kClient (child processes)
- `T3kRunner`: runs `sawblade-t3k` as a `juce::ChildProcess` on **its own worker thread** (one
  thread, a FIFO of jobs; a newer search supersedes a queued older one). Never on the audio thread,
  never blocking the message thread. Results are delivered to the message thread asynchronously
  (`MessageManager::callAsync` guarded by a `SafePointer` / weak flag so a closed browser never gets
  a callback). Per-job timeout (search/list/models 30 s, fetch 120 s, login = code expiry); timeout
  kills the child. Destructor kills running children and joins.
- The executable path is an injectable setting (`BrowserSettings`, stored with
  `juce::PropertiesFile` in the user app-data folder as `Sawblade/browser.settings`; **not** plugin
  state). Default: `<repo>/match/.venv/bin/sawblade-t3k`, with `<repo>` = compile definition
  `SAWBLADE_REPO_DIR` (= `PROJECT_SOURCE_DIR`). The browser has a small settings row (path field +
  "Reset") to change it.
- Parsing (`T3kJson.{h,cpp}`, pure, no JUCE GUI): search/list records, models, fetch result, error
  object, login events → plain structs. Malformed JSON / missing required fields → a parse error
  string, never a crash or exception escaping to JUCE.
- Login state: on opening the browser run `whoami --json`. Exit ≠ 0 → show a "Log in to TONE3000"
  view with a LOG IN button that runs `login --json-events`, shows the URL + code (selectable text,
  "open in browser" button) and waits; `logged_in` → retry the pending query. Any non-JSON stdout
  from login is discarded, never displayed or logged.

### Browser UI (`CaptureBrowser` overlay, like `PlayAlongPanel`)
- Header: "‹ RIG" (close), title "CAPTURES", "FOR · <SLOT NAME> SLOT", search field.
- Left filters: **SOURCE** FAVORITES / SEARCH / POOL (default FAVORITES; SEARCH runs `search QUERY`,
  needs a non-empty query); **GEAR** PEDAL / AMP / CAB IR, preselected from the piece (pedals →
  pedal, amps → amp, cab → ir) and changeable; **QUALITY** "PASSES FILTER ONLY" toggle, on by
  default (hides records with `passes == false` client-side). Footer note from "Licence rule" above.
- Cards grid (scrollable): title, `@creator · <license>`, `♥ fav · ↓ dl`, tags (`A2`, models count,
  sizes, status/flags), PREVIEW and USE buttons. **The licence text is shown on every card** and in
  the SELECTED panel, also for records that fail the filter.
- SELECTED panel: title, creator · licence · date · models count, the model list (from `models`,
  loaded when a card is selected; first model selected by default), status line (fetching / rendering
  / error text), PREVIEW and "USE IN <SLOT>" buttons. The mockup's spectrum drawing is optional; skip it.
- States: loading, empty ("No captures"), error (CLI error message + code), not logged in.

### Slot ↔ preset block mapping (`SlotTarget`)
Pieces map to the current preset (`processor.currentPreset()`):
- SawPedal / BodyPedal → path `a` / `b`: the first `nam` block with `slot` `pedal` or `boost`; if no
  block has a slot and the path has ≥ 2 `nam` blocks, the first one.
- SawAmp / BodyAmp → path `a` / `b`: the first `nam` block with `slot` `amp`; if none has a slot, the
  last `nam` block.
- Cab → `cab.ir` in shared mode; in per-path mode the SELECTED panel shows two buttons
  "USE IN SAW CAB" (`irA`) / "USE IN BODY CAB" (`irB`).
- No matching block → USE and PREVIEW disabled with the reason ("this preset has no pedal in the saw
  path"). Adding blocks is out of scope.

### Swap (USE)
`fetch TONE --model M --json` on the runner, then on the message thread: copy the current preset,
replace the target capture with `{file = path, resolvedPath = path, sha256, source = <fetch source>}`
(nam block params are rebuilt as a new `NamBlockParams` copy — `params` is `shared_ptr<const>`),
keep every other field and the block's gains, then `processor.loadPreset(newPreset)` — the normal
loader path (background build + lock-free swap). The preset then saves source id / modelId /
licence / creator / title / url as today. Kind check: an `ir` result can only go to the cab, `nam`
only to blocks (mismatch → error, no swap). Load failures surface through `status().error` as today.

### Preview
- Asset: `plugin/assets/preview_riff.wav`, 6.0 s, 48 kHz, mono, 24-bit or float, generated by a
  committed deterministic script `design/render/make_preview_riff.py` (seeded; Karplus-Strong or
  similar plucked low-string synthesis: palm-muted chugs + a few open power chords, DI level ≈
  −12 dBFS peak). No third-party audio. Added to `juce_add_binary_data` and the assets budget check.
- On PREVIEW: fetch (as USE) → build the candidate preset (same substitution) → on a **preview worker
  thread** (not the runner, not the loader, not the audio thread) run `sawblade::renderPreset` with
  the riff, `outputRate` = the processor's current host rate (48 kHz if not prepared), normalized
  to the existing render normalization → hand the buffer to the processor's `PreviewPlayer`.
- `PreviewPlayer` (`plugin/src/browser/PreviewPlayer.{h,cpp}`, JUCE-free): an immutable float buffer
  handed to the audio thread through a `SwapSlot` (core/swap_slot.h); retired buffers freed off the
  audio thread (same ownership pattern as `EngineRef`). `process(float* const* out, int ch, int n)`
  on the audio thread: while a preview plays, the output is the preview (dual-mono) instead of the
  rig, with 10 ms linear fades in/out, then returns to the rig. `stop()` from any non-audio thread
  (command via atomic). No allocation, locks, I/O on the audio thread — covered by the existing
  alloc/lock counting harness. A new PREVIEW or closing the browser stops the old one.
- Preview latency: none added to the host-reported latency (it is a UI audition, not the signal path).

### Edits to existing files (keep minimal — other sessions edit these)
- `PluginProcessor.h/.cpp`: a `PreviewPlayer` member + accessor, `prepare` call in `prepareToPlay`,
  one `process` call at the end of `processBlock`, and a thread-safe `hostRate()` getter if needed.
  Nothing else.
- `PluginEditor.cpp`: enable BROWSE CAPTURES; on click open the `CaptureBrowser` overlay for the
  selected piece (added like `panel_`). Nothing else.
- `plugin/CMakeLists.txt`: new sources, the asset, `SAWBLADE_REPO_DIR`, new test files.

## Acceptance tests

Catch2 (`plugin/tests/test_browser*.cpp`; UI ones in the editor test executable under xvfb):
1. **Fake child process**: `plugin/tests/fake_t3k.py` (or a shell script) set as the executable path;
   it answers `whoami/search/list/models/fetch/login` from canned JSON per argv, can be told (env var)
   to fail with an error object, to sleep (timeout test) or to print garbage. Tests: search →
   records; list favorites/pool; models; fetch → result; error object → error state with message;
   timeout kills the child and reports a timeout; garbage → parse error, no crash; the message thread
   is never blocked (callbacks arrive via the message loop).
2. **JSON parsing** unit tests (pure): every shape above, missing fields, wrong types, extra fields
   ignored, unicode titles.
3. **Swap through the loader**: with a real preset from `presets/` or `tests/fixtures/presets` and
   `tests/fixtures/nam/*.nam` as the "fetched" file, USE on each piece kind (pedal, amp, shared cab
   with `tests/fixtures/ir/*.wav`, per-path cab A and B) → after `waitForLoader`, `currentPreset()` has
   the new file and the full `source` (provider, id, modelId, url, title, creator, license), all other
   blocks unchanged, `status().error` empty; the saved state JSON round-trips it. Kind mismatch and
   missing block → no swap, error shown.
4. **Licence shown**: render the browser with fake records (including a `cc-by-nc` one that fails the
   filter, with QUALITY off) → every card's licence label text equals the record's `license`; the
   SELECTED panel shows it; a `license` error from fetch is displayed. Save a screenshot
   `build/screenshots/capture_browser.png` (1280 × 800, populated with fake data).
5. **Preview**: render with a fixture preset + fixture nam returns 6 s at host rate, finite, non-silent;
   `PreviewPlayer` swaps in on the audio thread with zero allocations and zero locks (existing harness),
   output equals the preview buffer after the fade-in, returns to the rig after the end, `stop()` works,
   block-size independent within 1e-6.
6. **Login**: whoami fails → login view; `login --json-events` fake shows URL + code; `logged_in` →
   the query runs; a fake that prints a token-looking line in plain text never reaches any label.
7. Riff asset: generator is deterministic (two runs → identical bytes); asset is 6.0 s ± 1 sample,
   48 kHz, mono, peak between −15 and −6 dBFS.

Build gates (all must pass, results pasted in the report):
- Full `ctest` (Release, `-DSAWBLADE_BUILD_PLUGIN=ON`), zero warnings.
- A clang build with `-Werror` (`CC=clang CXX=clang++`, see `docs/specs/mac1-clang_werror.md`).
- pluginval v1.0.4 strictness 10 on the VST3 (`SAWBLADE_PLUGINVAL_EXECUTABLE`, docs/PLUGIN.md),
  under xvfb-run: SUCCESS.
- `pytest` in `match/` green.
- Standalone smoke: launch under xvfb with the fake CLI, open the browser, search, preview, USE.

## Out of scope
Adding/removing blocks, editing pedal knobs, the matcher, AU/AAX-specific work, caching search
results across sessions, artwork thumbnails, the spectrum drawing, any licence policy change.

## Never
Commit captures (cached `.nam`/IR files); log or display tokens; run child processes, file I/O or
renders on the audio thread; block the message thread waiting for a child.
