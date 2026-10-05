# Phase 6a report: record and match in the plugin

Spec: `docs/specs/phase6a_record_match_plugin.md`, with a "Lead refinements" section added in `a433f63`.
Status: **accepted by the lead after reviewer ACCEPT (round 3).** No audio or captures are committed. Tests
write their audio and fake tools into temp dirs.

Artifact: **Sawblade Record + Match**, https://claude.ai/artifact/A4zf2qnJ9uZwypuu2r43si (private until
shared). It shows the Standalone app at 1x, 1280x800, in four states:
- REC armed, with a take list;
- match progress, at stage 2, 60 %;
- the result list, with A/B active;
- Export NAM, as an extra shot.

The song title, capture names and error figures in the shots are test labels from a synthetic song and a
fake matcher.

Commits:
- `a433f63`: spec refinements.
- `3dd2de2`: recorder, runner, glue, tests.
- `8309ffd`: UI, docs, editor tests.
- `94cef77`: round 2. Process-group cancel, file log and re-attach, export checks, audition invalidation.
- `112fa42`: round 3. Pid identity on re-attach, spawn hygiene, candidate cache.

## Spec refinements (lead)
These were needed because `sawblade-match` as it is today cannot run the spec exactly as written.
- `--pool` is required by the matcher. It is a second setting, defaulting to
  `~/.cache/sawblade/captures/pool_manifest.json`, and a missing pool file gets a Locate… button.
- `--stems-dir` is a stem cache keyed by the reference's file name, not a song folder.
  - So the runner passes `--ref <stem> --ref-channel mid`.
  - The stem is chosen in this order: `guitar`/`guitars`, then `other`, then the mix, then the first audio file.
  - `--stems-dir` is not passed.
- `--offset-ms` = the sidecar's stem sample index ÷ stem rate × 1000. This uses the matcher's sign: the DI
  starts that far into the song.
- `--progress-json` is passed only if the tool's `--help` lists it. Otherwise the runner parses log lines.
  Today's matcher has no `--progress-json` (6b adds it), so the log fallback is the live path until 6b lands.

## What was built (plugin/)
- **`TakeRecorder`**
  - **Audio thread:** it taps the processor input before the gate, as the mono sum. The audio thread
    copies into a preallocated ring (≥ 2 s) and pushes into a 64-slot SPSC event queue.
  - **Writer thread:** it writes 32-bit float mono WAV to the takes dir (`~/.local/share/sawblade/takes`;
    on macOS `~/Library/Application Support/Sawblade/takes`).
  - **Overruns:** they are counted, never blocked on, and padded with zeros so the take stays on the DI
    timeline. The count is shown in the panel and stored in the sidecar.
  - **Sidecar JSON:** it records the rate, the length, overruns, dropped samples, and the playAlong
    stem-sample index at the take's first sample.
- **Play-along panel:** it has a new record band (+112 px) with these controls:
  - REC and STOP;
  - a take list with rename and delete;
  - USE FOR MATCH;
  - MATCH and EXPORT NAM. In plugin mode these two say "open the Standalone app"; REC works in both modes.
- **`JobRunner`**
  - **Ownership:** it is owned by the processor, so jobs survive the panel and the editor closing.
  - **Launch:** `posix_spawn` starts the tool in its own process group, with closed descriptors and default
    signals. stdout and stderr go to `<job>/log.txt`.
  - **Job files:** `job.json` holds the kind, state, pid, pgid, spawn time and command line. It is the
    source of truth.
  - **Monitor:** a monitor thread parses progress, which is either `progress.json` or log lines.
  - **Cancel:** SIGTERM to the group, then SIGKILL to the group after 2.5 s.
  - **Re-attach:** a new runner, including one after an app restart, re-attaches to the newest job of each
    kind. Before it treats a job as alive or signals it, it verifies the pgid and the process start time.
    A foreign pid makes the job "interrupted", and that pid is never signalled.
  - **Settings:** the executables and the pool are kept in a `PropertiesFile`, not in the plugin state.
    The state bytes are unchanged.
- **`MatchScreen`**: the FullMatch-style overlay.
  - It shows the reference, the DI take, the tools with Locate…, and the progress (stage, message, ETA,
    best error).
  - Cancel.
  - The result list shows the best result and the alternatives, with error dB (`loss`) and topology.
  - **Audition** loads a result through the `EngineLoader`. **A/B** switches back to the previous preset.
    **Apply** keeps the result, and **Revert** goes back to the previous one.
- **Export mode** runs `sawblade-export`.
  - It offers mode (nocab or withcab) and size (feather, lite or standard), with `--device auto`.
  - It also passes the selected take as `--di` and `--out <job>/export`.
  - Progress comes from `checkpoint/progress.json`, and Reveal opens the result folder.
  - **Source preset:** the source is the applied or auditioned candidate's resolved file, if it is still
    what is loaded. Otherwise the current preset is written with absolute capture paths.
  - Export is blocked with a message if a capture has no resolved path.
- **Docs:** `docs/PLUGIN.md`, "Record + Match" section.

## Tests
- **Full ctest:** Release, `-DSAWBLADE_BUILD_PLUGIN=ON`, `-Werror`, zero warnings: **278/278 passed**. That
  is 238 before plus 40 new. The implementer and the reviewer each ran it at `112fa42`.
- **Repeat runs:** the `[record]`/`[match]`/runner tests (52 cases) passed 3 out of 3 times for the
  reviewer.
- **clang -Werror:** `CC=clang CXX=clang++`, Release. `sawblade_plugin_tests`, `sawblade_editor_tests`, the
  VST3 and the Standalone built with 0 warnings. Both test suites pass under clang. The build dir was
  deleted afterwards.
- **pluginval** v1.0.4, strictness 10, VST3, xvfb-run: **SUCCESS**. The implementer ran it at `112fa42`.
- **Coverage:**
  - The recorder makes zero allocations and takes zero locks in processBlock while recording, including at
    take start with a running StemPlayer in Standalone and host-follow modes.
  - Overrun counting is tested with a stalled writer.
  - The WAV is bit-exact for a synthetic input across mixed block sizes.
  - The sidecar offset is checked against a running StemPlayer.
  - Recording leaves the tone state bytes unchanged.
  - The runner is tested against a fake Python `sawblade-match`/`sawblade-export`:
    - progress in both modes;
    - the result list;
    - a missing executable or pool;
    - a large-output tool that does not stall;
    - cancel kills the whole group, including a forked grandchild, with and without the tool ignoring
      SIGTERM;
    - re-attach and cancel after re-attach;
    - an unrelated live process in `job.json` is never signalled and the job reads "interrupted".
  - Audition, A/B, apply and revert go through the loader. The applied candidate is invalidated by a
    parameter or preset change.
  - The export source round-trips through `loadPresetFile` with NAM and IR captures. Export is blocked for
    unresolved captures.
  - Plugin-mode gating.
  - Editor: the panel's controls, and the four screenshot states.
- **Not run:**
  - the real `sawblade-match` and `sawblade-export`, because `match/.venv` does not exist in this container;
  - ASan/UBSan;
  - any macOS build. The macOS-only code is the `_NSGetEnviron` call, `sysctl KERN_PROC_PID` and
    `POSIX_SPAWN_CLOEXEC_DEFAULT`.

## Reviewer verdicts
1. **Round 1, ACCEPT.** The reviewer listed should-fix items. The lead sent six of them back anyway, because
   the spec says "Cancel kills the child cleanly" and "the job dir is the source of truth":
   - Cancel signalled only the tool's pid, not its process group.
   - A job did not survive an app quit, because its output went through a pipe.
   - The take-start path had no allocation assertion.
   - The applied candidate went stale after a parameter edit.
   - The export source had no round-trip test.
   - The take-list rescan ran on the message thread every 2 s.
2. **Round 2, REVISE.** One must-fix: pid reuse on re-attach could leave a job showing "Running" or signal
   an unrelated process group. The should-fix items were descriptor inheritance into the child and the
   cost of the candidate check. All of these were fixed in `112fa42`.
3. **Round 3, ACCEPT.** No must-fix. Optional items:
   - document that a large wall-clock step can make a live job look foreign. That case fails safe: the job
     is reported "interrupted" and is never signalled.
   - confirm the macOS paths on the Mac build.

## Accepted deviations
- The MATCH and EXPORT NAM buttons that start jobs are in the play-along panel. The existing top-bar MATCH,
  EXPORT NAM and A/B placeholders stay disabled.
- The panel is 112 px taller and covers more of the rig.
- `job.json`, `log.txt` and the extra sidecar fields (`droppedSamples`, `createdUtc` in ms) go beyond what
  the spec asks. They support re-attach and take sorting.
- The runner is POSIX-only. On Windows it reports an error, which does not matter for now because AU and
  VST3 builds come first.

## Open questions for the main lead
1. **First real run.**
   - Do a first real MATCH and EXPORT on the Mac against `match/.venv`.
   - Use a Mac clang `-Werror` build to confirm the macOS-only code paths.
   - After 6b lands, confirm `--help` lists `--progress-json` and that its field names match
     `{stage, fraction, etaSeconds, bestErrorDb, message}`.
2. **Top-bar buttons.** Should the top-bar MATCH, EXPORT NAM and A/B placeholders open the same screens and
   drive the same A/B, or be removed?
3. **Loop wraps.** A loop wrap during a take makes the single song offset wrong. This is documented but not
   detected. Options are to stop the take at the wrap, store per-segment offsets, or warn.
4. **Housekeeping.** Takes and job folders are never deleted. Do you want a retention rule or a "clear old
   jobs" action?
5. **Export DI.** The exporter is given the selected take as `--di`. Its default is a testdata file. Is the
   user's own take the right training/validation DI?
6. **Fresh Linux images** need the X11, freetype, ALSA and GL dev packages before JUCE configures. The
   implementer installed them with apt here.
