# v0.3.1 — polish from the v0.3 hand test and the hotfix reviews

Source: the user's v0.3 Logic hand test (2026-10-07: "level match works but needs to be more instant", "OUTPUT needs a
readout or typing in", A/B "how to set each?") plus nits the reviewer left open on v0.3.0.1 and the main-branch CI
history. Owners: dsp-engineer (plugin, core, C++ tests, scripts), match-engineer (Tasks F, G), reviewer on every task.

**Scheduling:** start only after v0.4M and v0.6 have merged into the working branch. Both touch `plugin/src` and the
export/level code; starting earlier guarantees merge conflicts. Base the branch on the working branch after those
merges.

## Task A — level match feels instant

Today the LEVEL MATCH trim follows a rig change through the 10 Hz `levelTick()` (`plugin/src/PluginProcessor.h:128`,
`:248`), and a new rig's trim must be measured before it applies, so the user hears a jump and then a correction.

- When a rig is loaded or a capture is swapped, apply the trim that is already known for it at once: the stored trim of
  a preset, or a pre-measured trim cached by rig hash (the existing `autoTrimHash`). Measure only on a cache miss, and
  keep the last applied trim (not 0 dB) until the new measurement arrives.
- Smooth trim changes over ≤ 20 ms (no zipper noise); no allocation, locks or I/O on the audio thread (CLAUDE.md).
- Tests: known-rig swap applies the cached trim in the same block the rig becomes active; cache miss holds the previous
  trim then ramps to the measured value; zero allocations in `process()` during both.

## Task B — knob value readout and type-in

- Every rig knob and the OUTPUT / INPUT gain controls show their value with units on hover and while dragging (dB, %,
  Hz, ms as the parameter declares), and accept a typed value on double-click (Enter applies, Esc cancels, out-of-range
  clamps, bad text reverts). Values typed in go through the same undoable parameter path as a drag.
- Follow the user's existing UI design; no layout changes beyond the readout itself.
- Tests: text → value round-trip per unit type; clamp; invalid text; undo restores the previous value; host
  automation sees one gesture.

## Task C — A/B slot names

- The A/B compare shows which preset or rig is in each slot (name, truncated with a tooltip for the full name) and a
  one-line "how": click A or B to hear it, the edit applies to the slot shown as active, COPY A→B / B→A.
- Tests: names follow loads and copies; a slot that is empty says so.

## Task D — scripts and test robustness

1. `scripts/mac_update.sh` step 3 judges auval by its last line (`tail -n 1`, around line 106). auval's last line is a
   row of dashes, so a pass is reported as a warning. Grep the full output for `AU VALIDATION SUCCEEDED` instead and
   print the failing section when absent.
2. Settings/test cache fragility: the first `Settings::shared()` creation clears `setCaptureCacheRootOverride`, which
   broke 10 tests in CI run 200. Make the override survive `Settings::shared()` creation (or make tests that set it
   create Settings first through one fixture helper) so test order cannot matter. Add a test that sets the override,
   then creates `Settings::shared()`, and checks the override is still in force.
3. The v0.3.0.1 reviewer flagged the placement of `#include <pthread.h>` in `plugin/tests/test_settings.cpp:21`,
   whose only use is near line 1271. Move the include beside that code (or into a small helper that owns the
   thread-attribute setup), guarded for platforms where pthreads are not the threading API.

4. macOS screenshot-test flake (CI run 290 on the v0.7 branch, attempt 1): plugin tests "pedal face and the CIRCUIT
   switch" and "pedal drawer" failed with `nonBackgroundFraction` 0.0 (a blank render), then passed on rerun. Same
   family as the integration-step-10 race fixed in 8205a3f (bounds read before the face was placed). Find the root
   cause (render before layout / async image load / first-paint timing) and make the tests wait on the real
   condition, not a sleep. Add a stress run (the two tests x50 on macOS CI or locally) to show it no longer flakes.

## Task E — credential filter tightening (`plugin/src/settings/ToolEnv.cpp`)

- `looksLikeCredential` misses `KEY=value` pairs whose value is letters only and shorter than 20 characters. For `=`
  (not `:`), treat a value of 8+ characters with no spaces as credential-shaped. Keep the `t3k_cs_` rule and
  `safeToolLine` / `lastSafeLine` behaviour otherwise unchanged; keep it regex-free and bounded (no recursion, no
  unbounded backtracking).
- Boundary tests: a 4 KB line (no crash, bounded time), 16 vs 17 leading whitespace characters, values of 7 vs 8
  characters after `=`, and the real CLI messages that must still show (e.g. `TONE3000_CLIENT_ID is not set`).

## Task F — token store robustness (match-engineer)

- `TokenStore.load()` (`match/sawblade_match/t3k/auth.py:78`) catches `FileNotFoundError, KeyError, ValueError` but a
  token file whose JSON root is a list (or a string, number or null) raises `TypeError`. Treat any non-object root, and
  any wrong-typed field, as "no session" with the same message as a missing file.
- Tests: list, string, number and null roots; `expires_at` as a non-numeric string; all return `None`, none raise.

## Task G — matched presets with local IRs load anywhere on the user's machine (match-engineer + dsp-engineer)

Found 2026-10-08: the user copied a matcher `best.preset.json` into the User bank and it did not load. `portable()`
(`match/sawblade_match/matcher/run.py:240`) rewrites provider `local` captures to `local-irs/<title>.wav` relative to the
preset; core `locateCapture()` (`core/src/preset.cpp`) falls back to the capture cache only for TONE3000 captures. So any
match that picks a user IR is unloadable outside its run folder, and `*.resolved.json` is skipped by the preset scan.

- **Matcher:** when it writes a preset, copy each chosen local IR into `<captureCacheRoot>/local/<sha256[:16]>.wav`
  (the user's own machine cache; never committed or bundled). The preset keeps `source: {provider: "local", id:
  <sha16>, title, path: <original absolute path>}` and `sha256`.
- **Core:** `locateCapture()` falls back, for provider `local`, to `<captureCacheRoot>/local/<source.id>.wav`, then to
  `source.path`. sha256 is verified as today. One loader serves tonerender, the plugin and the bindings.
- **`sawblade-t3k resolve`:** repairs older presets by copying a missing local IR into the cache from `source.path` or
  from the IR index (sha256 lookup); reports any IR it cannot find, never silently.
- Tests: a local-IR preset copied to another directory loads (core + pybind); sha mismatch is a clear error; the cache
  file is written once; resolve repairs a pre-fix preset.
- User check: a matched preset copied into the User bank loads in Logic.

## Acceptance

All tasks reviewer-ACCEPTed; full CI green on one head (gcc, clang -Werror, macOS + auval + pluginval 10, python);
REPORT `docs/specs/v0_3_1-polish_REPORT.md` with the user's Logic re-check of Tasks A–C (level match on capture swap
and preset load, typing an OUTPUT value, reading the A/B slots).
