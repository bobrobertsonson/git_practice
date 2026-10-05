# v0.2.1 — MATCH inside the DAW + import a prerecorded DI

Source: user request (2026-10-05): "I want to be able to match to a prerecorded DI inside of Logic too."
Runs **after v0.2 merges** (both touch the editor). Owner: dsp-engineer (plugin); match-engineer only if
the matcher's offset search must change (Task B). Reviewer audits each task.

Today: MATCH is gated to the Standalone app (`SawbladeProcessor::matchEnabled()` =
`playAlong().standalone()`), and its DI can only be a take recorded in PLAY ALONG (`planMatch` →
`selectedTake`). The gate has no technical reason left: EXPORT NAM already runs a child process
(`sawblade-export`) from the same `JobRunner` in a host, and jobs survive the editor closing.

## Task A — MATCH in plugin mode

- Remove the Standalone gate: MATCH (top bar, play-along band, MatchScreen) works in a host exactly as in
  the Standalone app. Delete the "MATCH runs in the Standalone app" notices and update the tests that
  assert them (`test_editor.cpp` "MATCH runs in the Standalone app") to assert the new behaviour.
- Same `JobRunner` rules as export: the child runs out of process; nothing on the audio thread; the job
  folder is the source of truth; closing the editor or the host's project leaves a recoverable job.
- Several instances in one project: each instance's job is its own folder; two instances may run
  matches at once; a result is applied only to the instance that started it (test with two processors,
  like the v0.1.2 two-instance tests).
- Applying a result in a host is one undoable state change and marks the host project dirty
  (`updateHostDisplay` / parameter notifications as preset loads already do).
- pluginval and auval must not start a job (no tool configured in those runs: assert).

## Task B — import a prerecorded DI as a take

- **IMPORT DI…** button in the take band, plus drag-and-drop of an audio file onto the take list
  (WAV / AIFF / FLAC; a drop onto the song area keeps loading a song as today). Works in Standalone and host.
- The file is copied (never moved or referenced in place) into the takes folder as a normal take: mono
  WAV at the file's own rate, plus the sidecar. Stereo: a choice "left / right / sum" in the import dialog,
  default left (the matcher's `--di` is the left / mono guitar). Clipped or silent files are rejected with
  a one-line reason. Sidecar field `imported: { source: <original file name>, channel }`.
- **Where the DI sits in the song:** the import dialog asks "DI starts at [m:ss.mmm] in the song",
  default 0:00.000 with the hint "a DI bounced from the start of the song: leave 0:00". It is stored as
  the take's offset and passed as `--offset-ms`; the matcher already refines within ±3 s.
  A **"don't know"** checkbox passes no offset; then the matcher must find it over the whole song —
  match-engineer: extend `matcher/offset.py` to a whole-song coarse search when `--offset-ms` is absent
  and the DI is shorter than the reference (envelope xcorr, decimated; report the found offset and its
  confidence in the result; below a confidence threshold the job fails with "could not place the DI in
  the song: enter where it starts").
- USE FOR MATCH, RENAME, DELETE work on imported takes like recorded ones; the list marks them `IMPORTED`.

## Task C — doc: the record-in-Logic path

Document in `docs/PLUGIN.md` (and the in-app tooltip) the zero-import path that already exists: load the
song in PLAY ALONG with "follow host", put Sawblade on the DI track, play the DI region and REC — the take
carries its song position automatically. One test proves the offset of a host-follow take equals the host
playhead at the take's first sample (if an existing test already proves it, cite it instead).

## Task D — the MATCH screen holds its own inputs

User (2026-10-05): "I couldn't see any place to load or record for match." Today the song (LOAD SONG)
and the DI (REC → USE FOR MATCH) live only in the PLAY ALONG panel; the MATCH screen just says to go
there. Fix:

- MatchScreen section "1 · REFERENCE SONG" gets **LOAD SONG…** (+ drop), same code path as the
  play-along panel, with the separation progress shown in place.
- Section "2 · YOUR DI" gets **REC / STOP**, **IMPORT DI…** (Task B) and a take picker (the take list,
  newest first, selected = the match DI). No USE FOR MATCH round trip: picking a take here selects it.
- **START MATCH** is enabled only when both are set; when not, the button's caption says what is
  missing ("load a song first" / "record or import a DI").
- The play-along panel keeps its controls (same state, no duplication of logic: both views drive the
  same `PlayAlong` / `TakeRecorder` / `MatchSettings`).
- Mouse-driven editor test: from a fresh state, a user can load a song, import a DI and start a match
  without leaving the MATCH screen.

## Acceptance

- New tests: MATCH enabled in plugin mode (editor, mouse-driven); two-instance job isolation; import of
  mono / stereo (each channel choice) / 44.1k / 48k / 24-bit / float files → take WAV + sidecar exact;
  rejection of silent and clipped files; offset passed as `--offset-ms`; whole-song offset search on a
  fixture (DI cut from a known position of a synthetic reference, found within 10 ms; a non-matching DI
  fails with the message above).
- Full suite green: gcc + clang `-Werror`, ctest, Python, pluginval 10, macOS (auval + pluginval).
- Report `docs/specs/v0_2_1-match_in_host_REPORT.md` with reviewer verdicts and screenshots of the take band.
