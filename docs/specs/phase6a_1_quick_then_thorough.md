# Phase 6a.1: quick-then-thorough MATCH

Follow-up to 6a (`docs/specs/phase6a_record_match_plugin.md`, report `..._REPORT.md`). The behaviour decisions
come from the main lead (2026-10-04). The matcher flags `--quick`, `--thorough`, `--progress-json` and
`--listen` belong to phase 6b. They are not in this branch yet, so the tests use the fake child.

## Plugin (dsp-engineer)
1. **Two-pass MATCH.**
   - MATCH starts `sawblade-match … --quick` first. Its candidates fill the results list with a
     **PREVIEW** badge.
   - When the quick job succeeds, the runner automatically starts `--thorough` in the background on the same
     take, reference and offset, with its own job dir. Settings has an **auto-refine** toggle (default on); with
     it off, the thorough job is not started.
   - While the thorough job runs, a thin progress bar sits on the results header, labelled **REFINING…**.
   - If the tool's `--help` does not list `--quick`/`--thorough`, the runner falls back to today's
     single run. That run has no PREVIEW badge and no refine.
2. **Thorough result arrives.**
   - Audio is never interrupted, and nothing is loaded automatically.
   - The list gains a **REFINED** section at the top with a "refined result ready" badge.
   - Any audition or apply of a quick candidate stays as it is until the user clicks a refined candidate or
     the single **APPLY REFINED BEST** button.
   - **Auto-promote:** this happens when the applied quick candidate is the same chain as the thorough best:
     - the same topology;
     - the same captures, in the same slots;
     - every level and gain parameter within 0.5 dB.

     It changes only the badge and does no load. Write the "same chain" comparison as a pure function and
     test it.
3. **Cancel.**
   - During quick, Cancel cancels everything, and no refine starts.
   - During refine, Cancel cancels only the thorough job, and the quick results stay.
   - Starting a new MATCH, or switching USE FOR MATCH to another take, cancels a running refine.
4. **Job folders.**
   - On launch, keep the match job folders of the 5 most recent takes; this covers quick and thorough.
   - Prune older match job folders. Never prune a running job, and never touch takes or export jobs.
   - Document this in `docs/PLUGIN.md`.
5. **Top bar.** Enable the top-bar MATCH button: it opens the play-along panel's record + match area. In
   plugin mode it gives the same "open the Standalone app" message as the panel's MATCH. The EXPORT NAM and
   A/B buttons in the top bar stay disabled until phase 12.
6. **Re-attach.** Both jobs re-attach after the panel or the app closes. A quick job that re-attaches as
   finished, with no thorough job yet and auto-refine on, starts the refine once. A thorough job that is
   running is re-attached and is not restarted.

## Acceptance
- **Tests with the fake child.** The fake understands `--quick` and `--thorough`, and can also hide them from
  `--help`. The tests cover:
  - the argv assertions (`--quick`, then `--thorough`, with the same `--di`, `--ref`, `--offset-ms` and
    `--pool`);
  - the PREVIEW and REFINED states;
  - auto-refine on and off;
  - cancel during quick (no thorough starts) and during refine (quick results kept);
  - a new MATCH cancelling the refine;
  - auto-promote, and its negative case (a level 0.6 dB off, or a different capture);
  - APPLY REFINED BEST through the loader;
  - zero audio interruption, meaning no engine load while the refined result arrives;
  - pruning: 7 takes' worth of jobs leave 5, and a running job is kept;
  - the top-bar MATCH button, in Standalone and plugin mode;
  - the fallback to a single run.
- The full ctest passes with zero warnings, and the clang `-Werror` build of the plugin and editor tests
  passes. pluginval at strictness 10 on the VST3 still reports SUCCESS.
- **Screenshots**, added to the "Sawblade Record + Match" artifact:
  - PREVIEW results with REFINING… in progress;
  - the REFINED section with APPLY REFINED BEST.
