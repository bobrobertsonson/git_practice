# v0.3.1 — polish from the v0.3 hand test and the hotfix reviews

Source: the user's v0.3 Logic hand test (2026-10-07: "level match works but needs to be more instant", "OUTPUT needs a
readout or typing in", A/B "how to set each?") plus nits the reviewer left open on v0.3.0.1 and the main-branch CI
history. Owners: dsp-engineer (plugin, C++ tests, scripts), match-engineer (Task F), reviewer on every task.

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

## Acceptance

All tasks reviewer-ACCEPTed; full CI green on one head (gcc, clang -Werror, macOS + auval + pluginval 10, python);
REPORT `docs/specs/v0_3_1-polish_REPORT.md` with the user's Logic re-check of Tasks A–C (level match on capture swap
and preset load, typing an OUTPUT value, reading the A/B slots).
