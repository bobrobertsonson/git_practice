# v0.1.2 — live controls do nothing on the user's Mac (bug)

Source: user report 2026-10-05 after playing v0.1 (HANDOFF item 1). Owner: dsp-engineer; reviewer audits.

## Report

On the Mac build of v0.1 (Apple Silicon, built with `scripts/mac_update.sh`), "the plugin works but
it's not tweakable at all — just one sound, none of the EQ stuff works, blends or anything". A
three-step check, each step failed (no audible change):

1. Load `presets/modeled/chainsaw/classic_buzzsaw.json`, turn the pedal knobs.
2. Load `presets/matched/bolt_thrower_v1.json` (two paths), sweep BLEND 0 → 1.
3. Same preset, rig editor → EQ tab, drag a node on the graph.

Unknown yet (asked): Standalone vs Logic; whether the preset name / sound changed on load at all.

## Hypotheses (unverified — reproduce before fixing)

H1. Preset loads fail or are silently ignored, so the engine stays on INIT (clean pass-through,
    no EQ bands, identical paths) and every control has nothing to act on.
H2. Loads work but live parameter changes never reach the engine: `readParams()` →
    `Engine::setParams()` → `chain_->setLiveParams()` (`plugin/src/PluginProcessor.cpp:88`,
    `plugin/src/Engine.cpp:88`). Note the Mac-only change `4c19736` (snap parameters to the 1e-4
    grid; baseline built from the snapped preset) touched exactly this path.
H3. The editor's controls are not attached to the host parameters (UI moves, parameter does not),
    or the rig editor's live edits go to a preset copy the engine never sees.
H4. The Mac build is stale (an old artefact installed over the new one).

## Task

1. Write headless tests, at the level the user acts, that fail today if any of H1–H3 holds:
   - load each of the three presets through the same flow the preset browser uses, then render
     the fixture DI (`tests/fixtures/di_riff.wav`) through the processor; assert the render differs
     from INIT;
   - move each control through the editor component (pedal knob, BLEND, an EQ-graph node drag),
     not by setting the parameter directly; render again; assert the output changes by more than
     a stated threshold (RMS or spectral difference, documented in the test);
   - the same with the processor driven the way Logic drives it (parameter changes via
     `setValueNotifyingHost` from another thread between blocks, blocks of 512 at 44.1 kHz, which
     also exercises the 44.1 → 48 kHz resampling path).
   `bolt_thrower_v1` needs TONE3000 captures: use the test fakes / stand-in NAM fixtures the
   existing tests use for capture-backed presets; never commit real captures.
2. If the tests pass on Linux gcc and clang, the bug is platform- or build-specific: add the tests
   to CI (the macOS arm64 job runs ctest), check that job's result, and add a version/commit
   readout to the About page and the standalone window title if not already there so the user can
   confirm what build is running (H4).
3. Fix the root cause. Report it with file:line.

## Acceptance

- New tests fail on the pre-fix code (reviewer checks with a revert) and pass after.
- Full suite green: gcc + clang `-Werror`, ctest, Python suite, pluginval level 10 (VST3), and
  the CI macOS arm64 job green on the final commit.
- Audio-thread rules unchanged (no allocation/locks in `process()`; allocation harness green).
- Report `docs/specs/v0_1_2-live_controls_REPORT.md`: root cause, which hypothesis, reviewer
  verdict, test counts, final sha.
