# Phase 8 report: capture browser (lead)

Spec: `docs/specs/phase8_capture_browser.md`. Status: **accepted** (both parts reviewer ACCEPT).
Branch `claude/sawblade-p8-capture-browser`. Screenshot artifact: "Sawblade Capture Browser"
(https://claude.ai/artifact/Em3S98GnFFppiNpsTFwi4r, private).

## What shipped

- **8a, `sawblade-t3k` (match/)**: new commands `models TONE --json`, `fetch TONE [--model M] --json`
  (downloads into the cache and returns the path, sha256, kind, gear and a `source` object whose fields
  match the preset's `CaptureSource`), and `list --source favorites|pool --json` (same records as `search --json`).
  It also adds `whoami --json` and `login --json-events`, which prints the device code and URL as JSON and never prints tokens. With `--json`
  every error is `{"error", "code"}` with code `license|auth|not_found|network|error`; a final catch-all
  covers unexpected exceptions. Licence policy unchanged: `fetch` refuses `cc-by-nc*` and unknown
  licences before any download.
- **8b, plugin (`plugin/src/browser/`)**:
  - `T3kJson` parses the CLI output (pure functions).
  - `T3kRunner` runs one worker thread plus a watchdog for `juce::ChildProcess`. Timeouts are 30 s for queries, 120 s for fetch, and the code's expiry for login.
  - `T3kClient` wraps the typed commands. `BrowserSettings` holds the CLI path (a PropertiesFile, default `<repo>/match/.venv/bin/sawblade-t3k`).
  - `SlotTarget` maps a rig piece to a preset block or IR.
  - `PreviewRender`/`PreviewWorker` render off-thread through `renderPreset`. Closing the browser cancels a render and never blocks.
  - `PreviewPlayer` is a JUCE-free SwapSlot hand-over with 10 ms cross-fades.
  - `BrowserController` holds the state; `CaptureBrowser` is the overlay UI.
  - Preview riff: `design/render/make_preview_riff.py` generates `plugin/assets/preview_riff.wav` (Karplus-Strong, 6 s, 48 kHz, 24-bit, −12 dBFS peak, byte-deterministic). No third-party audio.
- Edits to shared files are minimal (28 lines):
  - `PluginProcessor.h/.cpp`: the player member, an accessor, a prepare call and one process call.
  - `PluginEditor.cpp`: BROWSE CAPTURES is enabled and opens the overlay.

## Gates

| Gate | Result |
|---|---|
| GCC Release, plugin ON, full ctest (xvfb) | 266/266 passed, 0 warnings (implementer). Reviewer's clean `build-review`: 265/265; it has no pluginval test registered. |
| clang `-Werror` (`build-clang`) | full build clean, ctest 265/265 |
| pluginval v1.0.4, strictness 10, VST3, xvfb | PASSED (implementer, then re-run by the lead after the review fixes: 13.1 s) |
| match pytest (without the two files that need the compiled core module) | 191 passed, 6 skipped |
| Standalone smoke (xvfb, fake CLI) | login view with the real CLI (no credentials → `auth`). With the fake CLI: cards, models, preview "playing", search "HM-2", USE swapped the preset, ‹ RIG closes. No audio device under xvfb, so the preview could not be heard; a processor test covers the audio path. |

The tests cover:
- a fake child process (`plugin/tests/fake_t3k.py`) for search, list, models, fetch, errors, timeout and garbage output;
- JSON parsing;
- a swap through the loader for every piece kind, with a state round-trip, checking that the other block parameters are unchanged;
- licence text on every card, including the "unknown licence" fallback;
- the preview player with zero allocations and zero locks, block-size independence and stop;
- login, where a token-like line never reaches a label;
- browser destruction during a render (returns in under 150 ms, and no preview starts afterwards).

## Review rounds

- 8a: ACCEPT. Follow-up (lead): a `--json` catch-all and three more tests.
- 8b round 1: REVISE. Closing the browser joined the preview thread and blocked the message thread. Fix: a shared `PreviewWorker` with a cancel flag, joined on a cleanup thread; leftover cleanup threads are joined at module unload. Search stripped leading dashes from the query. Fix: the query now goes after `--`. Three test or shutdown items were also fixed.
- 8b round 2: ACCEPT.

## Lead decisions

- Licence: the code still refuses NC (`licence_noncommercial.md` is queued), so the browser footer says
  "Non-commercial (CC BY-NC) captures can't be used yet". The plugin has no licence logic of its own.
- Per-path cab: PREVIEW puts the candidate IR in both irA and irB. USE offers SAW CAB / BODY CAB.
- Preview normalized to −3 dBFS peak. It adds no host latency.
- The login view appears only on CLI code `auth`/exit. Other failures show the error and the CLI-path row.
- `fetch` always contacts the API, even on a cache hit, to re-check the licence. So USE and PREVIEW need network.

## Follow-ups (not done, proposed)

1. When `licence_noncommercial.md` lands: change the refusal wording ("Sawblade is commercial") and the browser footer,
   and show a "non-commercial" tag on nc cards.
2. `search` maps gear `ir` → `cab` for the API `gears` parameter, while `list` filters on `ir` client-side.
   Verify against the real API with credentials.
3. Real-credential smoke test (login device flow, search, fetch) on the user's machine. Hear the preview
   in a real audio device / DAW.
4. A host unloading the plugin during a preview render waits for that render (bounded, about a second or two).
   Add a cancel hook in `renderPreset` if this ever matters.
