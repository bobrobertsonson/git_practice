**Blocked on user:** (0) this Mac is not logged in to GitHub, so pushes fail until `gh auth login` + `gh auth setup-git`; (1) choose a fix for the one macOS ctest failure (below; DSP-adjacent, not applied); (2) the factory presets need the user's capture files in `presets/captures/` before they make sound.

# Mac 1 build report

| | |
|---|---|
| Main-branch commit built | `d310518` (`origin/claude/sawblade-plugin-setup-7k0b8q`; previous run: `4f725fe`, same results) |
| macOS | 26.6 (25G72) |
| Xcode | none; Command Line Tools only (Apple clang 21.0.0, clang-2100.0.123.102). Full Xcode not needed |
| Chip | Apple M5 (arm64) |
| Build | `cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON`, CMake 4.4.4, Ninja. Clean, 0 compiler warnings (only `ranlib: ... has no symbols` notes for JUCE's empty ARA/LV2 units) |
| Artefacts | AU `Sawblade.component`, VST3 `Sawblade.vst3`, Standalone `Sawblade.app` (ad-hoc signed) |

## ctest

`ctest --test-dir build-mac --output-on-failure`: **150 tests, 148 passed, 1 failed, 1 skipped.**

- Skipped (expected): `plugin:test harness: LockGuard sees mutex acquisitions`, which runs on Linux only (`--wrap` linker flags).
- Failed: `plugin:Processor: starts as a zero-latency pass-through (Init preset)` (`plugin/tests/test_processor.cpp:164`, `CHECK(y == x)`). The output is the input times 1.00000012f (one float ulp hot) on 989 of 1000 samples. Latency is correctly 0.

Root cause (dsp-engineer diagnosis, reproduced in a scratch program; no repo change):
- `levelA`/`levelB` (range -24..+12 dB, default 0) are stored by APVTS as `convertTo0to1(0) = 0.666666687f`.
- Reading the value back, `convertFrom0to1` computes `start + (end - start) * p`.
- On arm64, Apple clang defaults to `-ffp-contract=on`, which fuses that into an FMA. The result is +7.15e-7 dB instead of 0.
- Linux/x86-64 has no FMA by default, so it gets exactly 0 there.
- `Chain::setLiveParams` sees a change from 0 and ramps both path levels to `pow(10, 7.15e-7/20) = 1.00000012f`.
- Confirmed: a scratch build with `-ffp-contract=off` passes the whole plugin test binary (23 passed, 1 skipped).

This is a real, tiny defect: every fresh instance on an FMA platform applies a spurious 7e-7 dB level ramp. It is not a build or packaging problem. Both real fixes change how output values are computed, so **neither was applied**. Options for the lead/user:
1. `plugin/src/PluginProcessor.cpp:69`: snap parameter reads with the existing `round4` (as preset export already does). Recommended: one line, platform-independent, also covers automation.
2. Top-level `CMakeLists.txt`: `add_compile_options(-ffp-contract=off)` for all targets including JUCE and NAM. A global numeric change that may cost a little speed and hides the round-trip issue.
3. Loosen the test to a 1e-6 margin. Not recommended; it masks the drift.

## pluginval (v1.0.4, `~/tools/pluginval.app`, `--strictness-level 10 --validate-in-process`)

Plugins were copied to `~/Library/Audio/Plug-Ins/Components/Sawblade.component` and `~/Library/Audio/Plug-Ins/VST3/Sawblade.vst3` first.

- VST3: **SUCCESS**
- AU: **SUCCESS** (one informational line: "Current program is -1", because the plugin exposes no host programs)

## auval

`auval -v aufx Swb1 Swbl`: **AU VALIDATION SUCCEEDED.** (`auval -a`, the full component scan, hung on this machine and was killed. Validating the plugin directly by type/subtype/manufacturer works.)

## Fixes made

None. The build and packaging needed no changes on macOS.

## Play through the Standalone

```
open build-mac/plugin/SawbladePlugin_artefacts/Release/Standalone/Sawblade.app
```

1. In the app, click **Options → Audio/MIDI Settings**. Pick your interface as input and output, and enable the guitar input channel. Turn off "Mute audio input" if it is shown, and use headphones or monitors, not the Mac's speakers, to avoid feedback. On first launch macOS asks for microphone (audio input) access; allow it.
2. The app starts on the **Init** preset (clean pass-through).
3. Click the preset-load button and choose **`presets/chainsaw_body.json`**: the default north-star blend (Saw 0.45 / Body 0.55, shared V30 cab, live-compatible). Try `swedeath_saw.json` for more chainsaw and `tight_body.json` for tighter palm mutes.

**Before step 3 makes sound:** the factory presets point at local capture files that are not in the repo (TONE3000 terms). `presets/captures/` is empty on this Mac. Put these in it (see `presets/README.md` for what to pick on TONE3000): `saw_pedal_hm2_maxed.nam`, `saw_amp_lowgain.nam`, `body_boost_ts_tight.nam`, `body_amp_highgain.nam`, `cab_4x12_v30.wav`. Otherwise the load reports missing files and the app stays on its current preset.
