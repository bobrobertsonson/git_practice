# Phase 6a.1 report: quick-then-thorough MATCH

Spec: `docs/specs/phase6a_1_quick_then_thorough.md`. It follows 6a; see `phase6a_record_match_plugin_REPORT.md`.
Status: **accepted by the lead after reviewer ACCEPT (round 2).** No audio is committed.

Artifact: **Sawblade Record + Match**, https://claude.ai/artifact/A4zf2qnJ9uZwypuu2r43si. Version 2 adds two
shots: "preview, refining" and "refined result ready". The page is private until shared. The shots come from the
fake matcher, so their labels and figures are test data.

Commits:
- `b8964c8`: spec.
- `0daf754`: runner, glue, fake child, headless tests.
- `4faa07b`: match screen, top-bar MATCH, editor tests, docs.
- `01f8a74`: review fixes.

## Process
- The main lead's session sent the behaviour decisions on 2026-10-04 after 6a was accepted. The lead wrote them
  into the spec and changed nothing else.
- The task loop ran as CLAUDE.md requires: spec → dsp-engineer → reviewer.
  - **Round 1.** The reviewer gave ACCEPT with seven should-fix items. The lead sent items 1–6 back anyway,
    because two of them could cause harm. Pruning could delete a foreign `*-match` folder in a shared jobs dir.
    `sameChain` could throw from the UI timer on a malformed preset, which would end the host.
  - **Round 2.** All six were fixed, and the reviewer gave ACCEPT with no must-fix items.
- 6b had not landed. No pushed branch has `--quick`, `--thorough` or `--progress-json`, and `match/.venv` does
  not exist in this container. So all runner tests use the fake child, which understands both passes and can
  hide them from `--help`.

## What was built (plugin/)
- **Match session.** A MATCH is a quick job plus an optional thorough job. Each job's `job.json` stores:
  - `pass`: quick or thorough;
  - `pair`: the other job's folder;
  - a `request` object (DI, reference and offset), so a refine can start from the quick job alone.
- **Two passes.**
  - The cached `--help` probe detects `--quick` and `--thorough`, and both must be listed. Otherwise MATCH falls
    back to one run, as in 6a, with no PREVIEW badge and no refine.
  - The quick results are badged PREVIEW.
  - When the quick job succeeds, the thorough job starts from the quick job's monitor thread. It uses the same
    `--di`, `--ref`, `--ref-channel`, `--offset-ms` and `--pool`. It starts only if AUTO-REFINE is on (a
    `PropertiesFile` key, default on).
  - While it runs, the results header shows REFINING… n % with a thin bar.
- **Refined arrival.**
  - Nothing is loaded and the engine is never rebuilt. A test asserts this through the engine build counter.
  - A REFINED section with a "refined result ready" badge appears above the PREVIEW section.
  - APPLY REFINED BEST auditions and then applies the best refined result through the loader.
  - **Auto-promote** changes only the badge. It happens when the applied quick candidate is the "same chain" as
    the thorough best (rules below).
  - A thorough pass that fails or is cancelled leaves a one-line note on the results header.
- **"Same chain"** is the pure function `sameChain(json, json)` in `MatchGlue.h`:
  - Structure, flags and strings must be equal. Name, notes, block ids and `playAlong` are ignored.
  - A capture compares by TONE3000 source ids when both sides have them, otherwise by file basename.
  - Keys ending in `Db` must be within 0.5 dB. `blend` must be within 0.01. All other numbers must be within a
    relative 1e-3.
  - Malformed input means "different". The function never throws.
- **Cancel.**
  - Cancel during the quick pass cancels everything, and no refine starts.
  - During the refine the button reads CANCEL REFINE and cancels only the refine.
  - These cancel a running or pending refine: a new MATCH, choosing another take for MATCH, and renaming or
    deleting the selected take.
- **Re-attach.**
  - The pair is rebuilt from `pair`.
  - A finished quick job with no thorough partner starts its refine once, when auto-refine is on.
  - A running thorough job is adopted with the 6a pid identity check and is never restarted.
- **Pruning on launch.**
  - Pruning runs once on a runner-owned thread, which is joined in the destructor.
  - It only considers real directories under `<jobs>/` named `YYYYMMDD-HHMMSS…-match` whose `job.json` has
    `kind: "match"`.
  - Symlinks are skipped and never followed. Each folder is handled in its own try/catch.
  - Folders are grouped by take, and the 5 most recently matched takes are kept.
  - A group with a live member is skipped, and so is a "starting" job less than 5 minutes old.
  - Takes, export jobs and `inputs/` are never touched.
- **Top bar.** MATCH is enabled and opens the play-along panel's record + match area. In plugin mode it shows the
  "open the Standalone app" note. EXPORT NAM and A/B in the top bar stay disabled until phase 12.
- **Docs.** `docs/PLUGIN.md` describes the two-pass MATCH, AUTO-REFINE, re-attach and pruning.

## Tests
- **Full ctest:** Release, `-DSAWBLADE_BUILD_PLUGIN=ON`, `-Werror`, zero warnings: **304/304 passed**. That is 278
  from 6a plus 26 new. The implementer and the reviewer each ran it at `01f8a74`.
- **Repeat runs:** the new `[twopass]`, `[prune]`, `[samechain]`, `[topbar]` and take tests passed 3 out of 3,
  for both the implementer and the reviewer.
- **clang -Werror:** `CC=clang CXX=clang++`. The plugin and editor tests, the VST3 and the Standalone built with
  0 warnings, and the plugin and editor tests pass under clang (reviewer: 123/123). The build dir was deleted.
- **pluginval** v1.0.4, strictness 10, VST3, xvfb-run: **SUCCESS**. The implementer ran it at `01f8a74`.
- **Coverage:**
  - argv: `--quick`, then `--thorough`, with identical inputs and different `--out`/`--progress-json`.
  - PREVIEW and REFINED states, and AUTO-REFINE on and off.
  - Cancel during quick and during refine; a new MATCH, a take switch, and a rename or delete cancelling the
    refine.
  - Auto-promote at 0.4 dB (same) and 0.6 dB (different), and with a different capture.
  - Malformed captures in `sameChain`.
  - APPLY REFINED BEST through the loader, and no engine build when the refined result arrives.
  - Pruning: 7 takes leave 5, a running job is kept, and these are kept: a foreign `x-match` folder, a symlinked
    match folder and its target, and a wrong-kind `job.json`.
  - The top bar in Standalone and plugin mode, and the single-run fallback.
- **Not run:** the real matcher (6b has not landed), ASan/UBSan (no audio-thread or DSP change), and any macOS
  build.

## Reviewer verdicts
1. **Round 1, ACCEPT.** Seven should-fix items. The lead required items 1–6:
   - prune checks (`kind`, name shape, symlinks, foreign folders);
   - make `sameChain` total;
   - cancel the refine on take rename or delete;
   - a note when the refine fails or is cancelled;
   - cancel a pending refine on a take switch;
   - remap `lastGoodRow`.

   Item 7, more race and audition tests, was not required.
2. **Round 2, ACCEPT.** No must-fix items. Non-blocking notes:
   - a prune comment says a folder whose `job.json` has odd field types is "left alone", but it is actually
     pruned when its group is old, as the test asserts;
   - a hard-coded x coordinate (478) for the REFINING label;
   - a failed take rename has already cancelled the refine.

   None of these affects behaviour.

## Accepted deviations and decisions
- **Two-pass needs both flags.** A tool that lists only `--quick` gets a single run, so a PREVIEW that can never
  be refined is never shown.
- **Auto-promote** looks only at an *applied* quick candidate that is still the loaded preset. An auditioned
  one does not count.
- **Pruning age.** "Last 5 takes" means the 5 most recently *matched* takes, ranked by their newest job folder,
  not by recording time.
- **One cancel button.** It cancels whatever is live, and its label switches to CANCEL REFINE.
- **Top-bar MATCH** opens the panel's record + match area, not the match screen directly. Opening the screen
  directly would be a one-line change.

## Open questions for the main lead
1. **Top-bar MATCH in Standalone.** Should it open the match screen directly (one line), instead of the panel?
2. **Old quick jobs.** Reopening the screen on an old finished quick job with no thorough partner starts a refine
   when auto-refine is on, as the spec words it. Should there be an age limit?
3. **Two instances.** If the plugin and the Standalone are open together, both could re-attach and start the
   same refine. This is an edge case and is not handled.
4. **After 6b lands.** Do a first real run on the Mac and confirm:
   - `--help` lists `--quick`, `--thorough` and `--progress-json`;
   - the progress fields match `{stage, fraction, etaSeconds, bestErrorDb, message}`;
   - quick and thorough both write `result.json` in the 6a shape (`best`/`alternatives`, `loss`, `topology`).
5. **Nits** from round 2 (the prune comment, the hard-coded label coordinate) can ride the next plugin change.
