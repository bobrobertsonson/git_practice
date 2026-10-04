**Blocked on user:** (0) this Mac is not logged in to GitHub, so pushes fail until `gh auth login` + `gh auth setup-git`; nothing else. TONE3000 login done on this Mac; `barbaric_v4` resolved and ready to play.

# Mac 1 build report

| | |
|---|---|
| Main-branch commit built | `ee0291c` (`origin/claude/sawblade-plugin-setup-7k0b8q`) + fixes `205d03b`, `4c19736`. Earlier runs: `4f725fe`, `d310518`, `435838a` (that one needed the fix) |
| macOS | 26.6 (25G72) |
| Xcode | none; Command Line Tools only (Apple clang 21.0.0, clang-2100.0.123.102). Full Xcode not needed |
| Chip | Apple M5 (arm64) |
| Build | `cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON`, CMake 4.4.4, Ninja. At `435838a` the build **failed** under clang -Werror (fixed, see below); after the fix: clean, 0 compiler warnings (only `ranlib: ... has no symbols` notes for JUCE's empty ARA/LV2 units) |
| Artefacts | AU `Sawblade.component`, VST3 `Sawblade.vst3`, Standalone `Sawblade.app` (ad-hoc signed) |

## ctest

`ctest --test-dir build-mac --output-on-failure`: **188 tests, 187 passed, 0 failed, 1 skipped** (after `4c19736`).

- Skipped (expected): `plugin:test harness: LockGuard sees mutex acquisitions`, which runs on Linux only (`--wrap` linker flags).
- Fixed (`4c19736`, below): `plugin:Processor: starts as a zero-latency pass-through (Init preset)` used to fail on macOS with output = input × 1.00000012f. Cause: on arm64, Apple clang fuses JUCE's `convertFrom0to1` into an FMA, so the `levelA`/`levelB` default read back as +7.15e-7 dB instead of 0, and the engine ramped to it. x86-64 gets exactly 0.

## pluginval (v1.0.4, `~/tools/pluginval.app`, `--strictness-level 10 --validate-in-process`)

Plugins were copied to `~/Library/Audio/Plug-Ins/Components/Sawblade.component` and `~/Library/Audio/Plug-Ins/VST3/Sawblade.vst3` first.

- VST3: **SUCCESS**
- AU: **SUCCESS** (one informational line: "Current program is -1", because the plugin exposes no host programs)

## auval

`auval -v aufx Swb1 Swbl`: **AU VALIDATION SUCCEEDED.** (`auval -a`, the full component scan, hung on this machine and was killed. Validating the plugin directly by type/subtype/manufacturer works.)

## Fixes made

- `205d03b` **Fix clang -Werror build break on macOS** (spec `docs/specs/mac1-clang_werror.md`; dsp-engineer, reviewer **ACCEPT** with a clean `build-review` configure + build + ctest). StemPlayer (spec 5.1) broke the Apple clang build: `-Wunused-private-field` on `StemPlayer::master_` (`core/include/sawblade/stem_player.h:198`), then `-Wunused-const-variable` on `kD`/`kB`/`kV`/`kG` (`tests/test_stem_player.cpp:28-31`). GCC has neither warning, so Linux was green. All five symbols were unreferenced and were deleted (5 lines). No functional or DSP change, no flags weakened, no suppressions.
- `4c19736` **plugin: snap host parameters to the 1e-4 state grid** (spec `docs/specs/mac1-param_snap.md`; fix chosen by the user; dsp-engineer, reviewer **ACCEPT** with Release + ASan/UBSan builds, 188/188, and an independent pre-fix revert that reproduces both failures). `readParams()` returns `snapParam(float param)`, and the engine baseline is built from the clamped and snapped preset (`clampTo` → `snapParam`), so the audio thread and the baseline are bit-identical on every platform and no spurious ramp starts after Init, a load or a restore. New test: odd preset values leave the engine at its baseline. `core/` and compiler flags are unchanged. `getStateInformation` bytes are unchanged (state was already rounded to 1e-4). Reviewer's non-blocking notes, left for the lead: the `paramsFromPreset` comment in `PresetMapping.h` is now orphaned above `snapParam`; `engineParamState()`/`Engine::liveParams()/baseline()` are public test hooks; the Init fallback after a failed rate rebuild still ramps once to the user's parameter values (pre-existing, semantic, out of scope).
- Note for the lead: consider a macOS/clang CI job (or `-Wunused-private-field -Wunused-const-variable` checks) so this doesn't recur.
- Process note: this session started outside the repo, so the project's `.claude/agents` were not registered. The dsp-engineer and reviewer ran as general-purpose agents (model sonnet) following `.claude/agents/dsp-engineer.md` / `reviewer.md` verbatim.

## Play through the Standalone

```
open build-mac/plugin/SawbladePlugin_artefacts/Release/Standalone/Sawblade.app
```

1. In the app, click **Options → Audio/MIDI Settings**. Pick your interface as input and output, and enable the guitar input channel. Turn off "Mute audio input" if it is shown, and use headphones or monitors, not the Mac's speakers, to avoid feedback. On first launch macOS asks for microphone (audio input) access; allow it.
2. The app starts on the **Init** preset (clean pass-through).
3. Load **`presets/matched/barbaric_v4.json`** ("Barbaric Pleasures · matched v4"): the matcher's single-path tone, HM-2 (Boss HM-2 1985 MIJ TTSV10) into a JCM 800 2203, with a Mesa oversized 4x12 IR (shared cab). It references TONE3000 ids, so its captures can be fetched. Resolve it first, then load the **resolved** file with the preset-load button:

   ```
   export TONE3000_CLIENT_ID=t3k_pub_...       # publishable key
   match/.venv/bin/sawblade-t3k login          # device flow, approve in the browser
   match/.venv/bin/sawblade-t3k resolve presets/matched/barbaric_v4.json
   # -> presets/matched/barbaric_v4.resolved.json (absolute cache paths; git-ignored)
   ```

   All three captures are licensed `t3k`. The cache is `~/.cache/sawblade/captures/`.

The factory presets (`presets/chainsaw_body.json`, `swedeath_saw.json`, `tight_body.json`) are blends, but they only name local files, not TONE3000 ids. They need these files placed by hand in `presets/captures/` (see `presets/README.md`): `saw_pedal_hm2_maxed.nam`, `saw_amp_lowgain.nam`, `body_boost_ts_tight.nam`, `body_amp_highgain.nam`, `cab_4x12_v30.wav`. Without them a load reports missing files and the app stays on its current preset.
