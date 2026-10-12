# Phase 5.2 report: play-along in the plugin

Spec: `docs/specs/phase5_2_playalong_plugin.md`.
Status: **accepted by the lead after reviewer ACCEPT (round 2).** No audio is committed; tests
synthesise stems into temp dirs.

Artifact: **Sawblade Play-Along**, https://claude.ai/artifact/RUKy1Sr5769k6gaZB1hoDp
(1x, 1280x800, panel closed and open; private until shared). The open shot is Standalone mode
with a synthetic song, a loop set and count-in on. The song title in that shot is a test label;
the audio is synthetic.

Commits: `3bcb683` (core + CLI), `7ce700b` (plugin, panel, state, docs), `6686b9e` (review fixes).

## Lead decisions given to the implementer (refining choices the spec left open)
- No PLAY ALONG deck render exists in `design/render/`, so the panel uses the existing skin
  style. It is an overlay on the bottom 170 px, toggled by a PLAY ALONG top-bar button, closed
  by default, and the open/closed state is not saved.
- Offset: the player's playhead p plays stem sample p − offset. The CLI flag, the panel and the
  saved `offsetMs` use the matcher's sign (the DI starts `offset` into the song, so positive
  means the stems lead), and they negate it before calling the player.
- Message → audio control goes through a preallocated lock-free SPSC queue (256 entries),
  drained at the top of each audio chunk.
- Suggested level = clamp(rig loudness estimate − `backingLoudnessLufs`, −40, +6) dB. The rig
  reference is −18 LUFS until rig signal has been seen. It is applied once on a user load and
  never on a state restore.

## What was built
- **Core:**
  - `OtherRole` for `loadStemDirectory`. The default, Guitar, treats a 4-stem `other` as the
    guitar stem; a real `guitar`/`guitars` file always wins.
  - `StemSet::otherMappedToGuitar`.
  - `StemPlayer::setStartOffsetSamples` is atomic and is applied at stopped-state adoption.
  - The preset parser accepts an object-valued `playAlong`, ignores it and never writes it back.
- **CLI:** `--backing-offset-ms`, `--other-role guitar|other`.
- **Python:** `load_stems(..., other_role="guitar")` and `StemSet.other_mapped_to_guitar`.
- **Plugin:**
  - `PlayAlong` (queue, play-along loader thread, SwapSlot hand-off, `collectGarbage()` off the
    audio thread, K-weighted rig loudness estimator, overflow resync).
  - `PlayAlongPanel` with these controls: LOAD SONG (folder picker plus folder drag-and-drop),
    play/pause, position and seek bar, loop A/B and on/off, count-in with BPM,
    MUTE/GHOST/FULL, KEEP KEYS, backing level, offset, and SYNC TO HOST in plugin mode.
  - `setRigLatencySamples` is called on every engine publish and in `prepareToPlay`.
  - The plugin defaults to backing off and follows the host transport when sync is on.
    Standalone free-runs.
- **State:** the optional `playAlong` object is written only once settings differ from their
  defaults, so untouched sessions save byte-identical state. A non-object `playAlong` is
  stripped plugin-side so the tone state still loads. A missing, empty or undecodable folder
  shows a message in the panel and never throws.
- **Docs:** `docs/PLUGIN.md` (play-along section), `docs/PRESET_SCHEMA.md` (`playAlong`, non-tone
  UI state, ignored by the matcher and export).

## Tests
- Release, `-DSAWBLADE_BUILD_PLUGIN=ON`, `-Werror`, zero warnings: **ctest 238/238 passed**
  (reviewer, clean build at `6686b9e`).
- **pluginval** v1.0.4, strictness 10, VST3, xvfb-run: `SUCCESS`.
- The `[playalong]` tests (18 cases) passed 5 out of 5 repeated runs.
- **Core and CLI:**
  - role mapping (all three cases);
  - sample-exact offset alignment;
  - a CLI render with an offset equals a render of the shifted stems;
  - the parser ignores `playAlong`.
- **Plugin:**
  - Zero allocations and zero locks in `processBlock` with the backing audibly playing,
    including count-in, a loop wrap, a non-user set swap that is adopted, and mixed block
    sizes, in Standalone and host-follow modes.
  - Host-follow offset is sample-exact at +100 ms and −50 ms.
  - Queue overflow followed by resync, both manual and from the loader tick.
  - With sync off, plugin mode is silent.
  - A user load pauses Standalone playback.
  - A non-object `playAlong` leaves the tone state intact.
  - A user load applies the suggested level once; a restore keeps the saved level.
- **Editor:** the panel exists, its controls are bound, the status-priority order holds, and
  the screenshot test captures the panel closed and open.
- **ASan/UBSan** (Debug, core + CLI): 181/181 clean (round 1; round 2 changed no core or CLI
  code).
- **Python build:** the bindings tests pass, including the new `other_role` test (reviewer
  round 1, implementer round 2).

## Reviewer verdicts
1. **Round 1, REVISE.**
   - Must fix: the zero-allocation test did not prove the backing was playing, because the
     Standalone user reload paused it. Also, this report was missing (lead's item).
   - Should fix: queue overflow could leave the UI and the audio diverged; the Python
     `load_stems` default changed silently; missing tests for sync off and the Standalone pause;
     panel status priority; a stale editor comment; a non-object `playAlong` rejected the whole
     state.
   - All of these were fixed in `6686b9e`.
2. **Round 2, ACCEPT.** No must-fix items. Non-blocking notes:
   - The resync is deferred while a long song loads.
   - Play, pause and seek are not replayed on a resync (documented in `docs/PLUGIN.md`).
   - The resync test's 2 s polling timeout could be raised for loaded CI machines.
   - `setStateInformation` parses the state JSON twice on the message thread. This is harmless.

## Accepted deviations
- A dedicated play-along loader thread instead of the `EngineLoader` thread. A song load can
  take tens of seconds and must not block `prepareToPlay`. It uses the same pattern: latest
  request wins, SwapSlot hand-off.
- A message-side mutex serialises queue producers. The audio thread never takes it.
- An offset change takes effect only while the transport is stopped, which follows from the
  core's adopt-while-stopped rule.
- The panel's status LED is painted in code rather than an `LedIndicator`, because an existing
  editor test counts the `LedIndicator`s.
- The shared processor test harness moved to `plugin/tests/processor_harness.h`.
- The top-bar preset button and latency chip were narrowed to make room for PLAY ALONG. The
  open panel covers the lower part of the inspector.
- No spec text was changed.

## Open questions for the main lead
1. **Offset sign in the UI.** The panel uses the matcher's sign (positive means the stems
   lead). A host-timeline use, such as "the song starts 4 s into the project", needs a negative
   value. Do you want a friendlier label, or the opposite sign in the panel?
2. **Song change while the host plays.** In plugin mode a newly loaded song is adopted only
   when the host stops, following the core rule. In Standalone a user load pauses playback so
   the new song is adopted. Is that acceptable?
3. **KEEP KEYS** reloads the folder from disk, because the role mapping is applied at load
   time. This is slow for long songs. An alternative is to keep `other` and `guitar` separate
   in memory and map the role at mix time.
4. **Memory and load time** (5.1 open questions 1 and 2) still apply: whole songs are held in
   memory as float32 at the host rate.
5. **Panel look.** The panel follows the existing skin style. If a PLAY ALONG deck render is
   made in `design/render/`, the panel can be reskinned to match it.
6. **Python default.** `load_stems` now defaults to other-as-guitar, matching the CLI.
   `match/` code that loads a 4-stem folder for the matcher gets the guitar stem from `other`,
   which matches the lead decision. Confirm this is what the matcher wants.
