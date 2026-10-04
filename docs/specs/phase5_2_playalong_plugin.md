# Phase 5.2: play-along in the plugin (from pre-separated stems)

This builds on 5.1 (`StemPlayer`, `StemSet`, loudness metadata) and the 2.5 skin. The
on-device separator is still a spike (5.0), so 5.2 loads a **folder of already-separated
stems**. Plugging the separator in comes later and only changes how the folder gets filled.

## Core (dsp-engineer)
1. **Stem role mapping** for `loadStemDirectory`:
   - `OtherRole::Guitar` (default) treats a 4-stem `other` as the guitar stem.
   - `OtherRole::Other` keeps it as other.
   - A real `guitar`/`guitars` file always wins: when one exists, `other` stays other.
2. **Start offset:** `StemPlayer::setStartOffsetSamples(int64)`, set off the audio thread and
   applied at the next stopped-state adoption. The playhead maps to `stem time + offset`.
   Negative values mean the stems lead.
3. **CLI:** add `tonerender --backing-offset-ms <ms>` and `--other-role guitar|other`.
4. **Tests:** role mapping (all three cases), offset alignment to the sample, and a render with
   an offset matching a shifted fixture.

## Plugin (dsp-engineer)
1. **Play-along panel**, placed under the pedalboard or as an overlay toggled by a
   PLAY ALONG button in the top bar. It follows the hardware look of the "PLAY ALONG" deck
   render if `design/render/` has it by then, otherwise the existing skin style. Controls:
   - LOAD SONG: pick a folder of stems; drag-and-drop a folder also works;
   - play/pause, a position display and a seek bar;
   - loop A/B (set from the current position) and loop on/off;
   - count-in on/off with BPM;
   - guitar stem MUTE / GHOST / FULL;
   - backing level;
   - "keep keys" (other role).
2. **Threading:**
   - Loading happens on the existing loader thread.
   - The StemSet is handed over through SwapSlot, and the old set is freed off the audio
     thread.
   - `processBlock` mixes the StemPlayer output after the rig, with the backing delayed by the
     rig latency, as the core already does.
3. **Standalone vs plugin:** in the plugin the backing is off by default and follows the host
   transport when enabled. In Standalone it plays free-run.
4. **State:**
   - The stem folder path, offset, loop points, levels and modes go into plugin state as an
     optional `playAlong` object.
   - Add that object to `docs/PRESET_SCHEMA.md` as non-tone UI state, ignored by the matcher
     and export.
   - Missing files on load show a clear message and never throw.
5. **Suggested level:** use the StemSet loudness (5.1b) to suggest a starting backing level
   relative to the rig's output loudness. It is a one-time default and is never automatic
   gain.

## Acceptance
- **Tests:**
  - core and CLI tests above;
  - editor tests: the panel exists, controls are bound, and the screenshot test includes the
    panel;
  - zero allocations in `processBlock` with the backing playing;
  - pluginval at level 10 still passes.
- **Screenshots:** publish 1x screenshots of the panel (closed and open) as an Artifact titled
  "Sawblade Play-Along", and record the URL in docs/specs/phase5_2_playalong_plugin_REPORT.md.
- **Never commit audio.** Tests use synthetic stems.
