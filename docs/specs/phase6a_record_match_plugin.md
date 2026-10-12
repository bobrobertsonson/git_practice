# Phase 6a: record and match in the plugin (Standalone first)

The loop: play along to a song → Sawblade records the clean DI → MATCH runs the matcher on
that DI against the song → audition the results → apply one → keep playing.

## Plugin (dsp-engineer)
1. **DI recorder.**
   - It records the plugin's **input**, the clean DI before the gate.
   - It writes to a preallocated, lock-free ring that a writer thread drains to a 32-bit float
     WAV in `~/Library/Application Support/Sawblade/takes/` (on Linux,
     `~/.local/share/sawblade/takes/`).
   - Nothing is allocated and no I/O runs on the audio thread. Overruns are counted and
     reported, never blocked on.
   - **Song alignment:** when play-along is running, the take records the backing playhead
     position (stem sample index) at the take's first sample, in a JSON sidecar next to the
     WAV. That is the DI-to-song offset the matcher needs.
   - **Controls:** REC arm/stop in the play-along panel, a take list (rename/delete), and a
     "Use for MATCH" selector.
2. **Match job runner.**
   - MATCH opens the existing FullMatch-style screen (see design/mockups/FullMatch.dc.html):
     reference = the loaded song (its stems folder, guitar stem preferred), DI = the selected
     take.
   - It launches `sawblade-match` as a **child process** through `juce::ChildProcess`. Pass the
     path to the match venv's executable in a settings field, defaulting to
     `<repo>/match/.venv/bin/sawblade-match`. Pass `--di`, `--ref` or `--stems-dir`,
     `--offset-ms` from the take sidecar, `--out` a job dir, and `--progress-json`.
   - Progress is read from the job dir. If `--progress-json` doesn't exist yet, poll
     result.json/log lines; see 6b, which adds `--progress-json`.
   - Cancel kills the child cleanly.
   - **Results:** a list of the best + alternatives, with error dB and topology. "Audition"
     loads a candidate's resolved preset into the engine through the normal off-thread loader
     (A/B against the current one). "Apply" makes it the current preset.
3. **Export NAM button:** same runner for `sawblade-export` (mode/size choice, device auto),
   with progress and the result folder revealed in Finder.
4. **Robustness:**
   - The plugin never blocks the message thread on the child.
   - A missing executable gives a clear message and a "Locate…" button.
   - Jobs survive closing the panel; the job dir is the source of truth.

## Acceptance
- **Unit tests:**
  - the recorder ring (no alloc in process, overrun counting, WAV written bit-exact for a
    synthetic input, offset sidecar correct with a running StemPlayer);
  - the runner with a fake child (a script that writes progress and a result) covering
    progress parsing, cancel, and the result list;
  - audition/apply through the loader.
- pluginval at level 10 still passes, and all tests pass.
- **Screenshots** (artifact "Sawblade Record + Match"): the REC armed state, the match
  progress, the result list.
- **Plugin mode:** recording works in plugin mode too, but MATCH/EXPORT are Standalone-only
  for now; in the plugin they show "open the Standalone app".

## Lead refinements (6a implementation, 2026-10-04)
Only where the spec as written cannot run against today's `sawblade-match`:
- **`--pool` is required** by `sawblade-match`. A second settings field holds the pool
  manifest path, default `~/.cache/sawblade/captures/pool_manifest.json`; a missing file gets
  the same clear message + "Locate…" as a missing executable.
- **Reference.** `sawblade-match --stems-dir` is a calibrate stem *cache keyed by the
  reference's file name*, not a song folder, and `--ref` is required. So the runner passes
  `--ref <stem file>` with `--ref-channel mid`, choosing the song folder's guitar stem
  (`guitar`/`guitars`), else `other`, else the mix/first audio file (shown in the screen).
  `--stems-dir` is not passed.
- **Offset.** `--offset-ms` = sidecar stem sample index / stem rate × 1000 (the matcher's sign:
  the DI starts that far into the song). No sidecar (no play-along running) → no `--offset-ms`.
- **Progress.** If the executable's `--help` lists `--progress-json`, pass it and read
  `{stage, fraction, etaSeconds, bestErrorDb, message}`; otherwise parse the child's stdout/
  stderr log lines (stage names, last line as message, indeterminate fraction) and finish on
  `result.json`. The fake child in tests covers both modes.
- **Results** come from `result.json` (`best`/`alternatives`, `loss` shown as error dB,
  `topology`) and the `*.preset.resolved.json` files beside it.
