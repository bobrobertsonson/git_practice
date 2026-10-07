# v0.6 Task A (core / plugin side): what the pinned NAM core plays, and where Sawblade assumes A1

Author: dsp-engineer, 2026-10-07. Audit only: no behaviour change, no dependency bump. `match/` is out of scope
(match-engineer: `docs/reports/v0_6/audit_match.md`; it was not present when this was written, so the
"A2 Full / A2 Lite" mapping below is derived from the core itself and upstream's own example files, and should be
cross-checked against the trainer audit).

Sources: NeuralAmpModelerCore commit `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` (cloned at that exact commit; tag
`v0.6.0`; `NAM/version.h:10-17` says 0.6.0). All `NAM/...:line` citations below are at that commit.

## 1. Verdict (short)

| Question | Answer |
|---|---|
| Does the pinned core load packed / slimmable / container A2 files? | Yes. Architectures `WaveNet`, `SlimmableContainer`, `LSTM`, `ConvNet`, `Linear`, `Sequential` are registered. |
| Does it play A2 Full and A2 Lite? | Yes, both. A2 Full = WaveNet, 23 layers, 8 channels; A2 Lite = same shape, 3 channels (`NAM/wavenet/a2_fast.h:1-5`). A packed `.nam` is a `SlimmableContainer` holding both (upstream `example_models/A2.nam`: Lite at `max_value` 0.5, Full at 1.0). |
| Which A2 variants take the fast path? | A2 Full (8 ch) and A2 Lite (3 ch) with the exact A2 shape; everything else is the generic WaveNet (correct, slower). Details in 1.4. |
| Is a dependency bump needed? | **No.** The pin is upstream `main` HEAD and the `v0.6.0` tag (0 commits behind `origin/main` when fetched 2026-10-07). No bump proposal. |
| Built here? | Yes: core, CLI and the whole test suite build with `-Werror` and zero warnings; `ctest` 371/371 pass (1 skipped by design). The plugin was not attempted (needs JUCE/X11). See section 4. |

### 1.1 How the core loads a `.nam`

- `get_dsp(path)` reads the JSON, requires the keys `version`, `architecture`, `config`, `weights`
  (`NAM/nam_file.cpp:31-38`; missing key = `NamFileValidationError`, which is a `std::runtime_error`-derived throw at load time).
  A `.wav` file path is also accepted and becomes a `Linear` model (`NAM/get_dsp.cpp:177-190`; new in this pin, unused by Sawblade).
- Version gate (`NAM/get_dsp.cpp:21-42`, constants `NAM/get_dsp.h:66-67`): must match `^\d+\.\d+\.\d+$`;
  earliest 0.5.0, latest fully supported 0.7.0. Below 0.5.0, or `major`/`minor` above 0.7 (e.g. 0.8.0, 1.0.0), throws
  ("unsupported version"). A newer patch of 0.7 (0.7.1) is `PARTIAL`: it loads and prints a warning to `std::cerr`
  (`get_dsp.cpp:125-130`, at load time, never in `process()`). Verified by probe (section 3.1): 0.4.0, 0.8.0, 1.0.0 throw; 0.7.1 plays.
- Dispatch is by the `architecture` string through `ConfigParserRegistry` (`get_dsp.cpp:280`). Registered strings:
  `Linear` (`linear.cpp:548`), `LSTM` (`lstm.cpp:203`), `ConvNet` (`convnet.cpp:360`), `SlimmableContainer` (`container.cpp:180`),
  `Sequential` (`sequential.cpp:241`), `WaveNet` (`wavenet/model.cpp:1334`). Registration is by static initialiser, which is
  why `core/CMakeLists.txt:55-58` links `nam_core` whole-archive (do not change that).
- Metadata read by the core: only `loudness`, `input_level_dbu`, `output_level_dbu` (`get_dsp.cpp:270-278`), taken from the
  **top-level** `metadata` object. For a container, the top-level metadata is the one applied; submodel metadata is not
  (`container.cpp:150-165` builds submodels with `get_dsp(model_json)`, so each submodel's own metadata is applied to the submodel
  only and never reaches Sawblade). Sawblade reads `loudness` plus `name`, `gear_type`, `modeled_by` from the same top-level object (`core/src/nam_block.cpp:47-50`).

### 1.2 Packed / slimmable: two different mechanisms, how a size is selected

1. **`SlimmableContainer`** (the packed file): `config.submodels = [{max_value, model: <full nested .nam spec>}, ...]`,
   sorted by ascending `max_value`, last `max_value >= 1.0`, all submodels the same sample rate
   (`container.cpp:19-50`, `150-169`). It is a `DSP(1,1)` that forwards `process()` to one active submodel (`container.cpp:52-56`).
   - Default active submodel = **the last one (full size)** (`container.cpp:48-49`).
   - Selection: `SetSlimmableSize(val)` picks the first submodel with `val < max_value`, else the last (`container.cpp:85-122`).
     So with `A2.nam`'s breakpoints `{0.5}`: `val < 0.5` = A2 Lite, `val >= 0.5` = A2 Full (`GetSlimmableSizeBreakpoints`, `container.cpp:124-133`).
   - **Not RT-safe**: `SetSlimmableSize` takes a mutex and calls `Reset()` on the new submodel (`container.cpp:111-118`); only the active submodel is
     reset/prewarmed (`container.cpp:71-83`). Must be called from a non-audio thread before the swap; `process()` itself only does an atomic load.
   - Each submodel is built through `get_dsp()`, so an A2-shaped WaveNet submodel gets the fast path (1.4). Verified: a container
     with only the Lite submodel runs at Lite cost (probe, 3.1).
2. **Single-file slimmable WaveNet** (`layers[i].slimmable = {method: "slice_channels_uniform", kwargs.allowed_channels: [...]}`,
   `wavenet/model.cpp:1291-1311`, `wavenet/slimmable.cpp:541-583`): slices channels out of one weight set. `SetSlimmableSize`
   builds a *new generic* `WaveNet` on the calling thread and stages it for `process()` to swap in with an atomic exchange
   (`slimmable.cpp:421-487`, `527-530`); allocation happens on the caller, not the audio thread. **It always builds the generic
   `wavenet::WaveNet` (`slimmable.cpp:421-449`), never the A2 fast path**, even at full size.
   Any other `slimmable.method` throws (`model.cpp:1305`, `slimmable.cpp:559`).

Sawblade today never calls `SetSlimmableSize` (grep: no hit in core/plugin/cli/bindings), so every container plays its **last (largest)
submodel**: A2 Full for an A2 container. There is no way for a user or preset to pick A2 Lite from a packed file.

### 1.3 Does the pin play every A2 variant the Anagram accepts?

The user's statement is KosmOS 1.16+ runs A2 Full, A2 Lite and A1. All three play in the pinned core:

| File type | Core path | Verified here |
|---|---|---|
| A2 Full, standalone `WaveNet`, 8 ch, 23 layers | `A2FastModel<8>` | yes: probe, null vs generic is not part of this audit (Task B) |
| A2 Lite, standalone `WaveNet`, 3 ch | `A2FastModel<3>` | yes |
| A2 packed (Lite + Full) `SlimmableContainer` | container, Full active | yes |
| A1 (WaveNet 16/8 "standard", sr absent, v0.5.x) | generic WaveNet | yes (`wavenet_a1_standard.nam`) |
| LSTM A1 | `LSTM` | yes (`lstm.nam`) |
| single-file slimmable WaveNet | `SlimmableWavenet` (generic) | yes (`slimmable_wavenet.nam`) |

Verified by loading the upstream example files at the pin through Sawblade's own `NamBlock` (scratchpad probe, 3.1), not just by reading code.
**Caveat for the match-engineer:** the example files are produced by upstream's tooling. The trainer pin (`neural-amp-modeler==0.13.0`)
`export_container` output must be loaded once through `NamBlock` to confirm the same (Task B fixtures do this). Risk points: the `version`
string it writes (must be within 0.5.0..0.7.x), `sample_rate` present at the container top level (`NamBlock::prepare` throws if it differs
from the host rate, `core/src/nam_block.cpp:130-132`), and the fast-path shape contract below.

### 1.4 `NAM_ENABLE_A2_FAST`: what it does and for which configs

Set in `cmake/Dependencies.cmake:45` (`target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)`).
It only affects `wavenet::create_config` (`NAM/wavenet/model.cpp:1320-1323`): if the config matches `is_a2_shape` and is not slimmable,
the parser returns `A2FastModel<3|8>` instead of the generic `WaveNet`. Order in `create_config`: slimmable check first (1317), then A2 fast.

`is_a2_shape` (`NAM/wavenet/a2_fast.cpp:832-978`) accepts only this exact shape; anything else silently falls back to the generic path:

- exactly 1 layer array; no post-stack `head`; no `condition_dsp` (851); `head_scale` numeric (857); `in_channels` 1
- `input_size == condition_size == 1`; `channels == bottleneck`; `channels` in {3, 8} (876)
- 23 layers; `kernel_sizes` = 6 x14, 15, 15, 6 x7; `dilations` = 1,3,7,17,41,101,239 | 1,3,7,17,41,101,239 | 1,13 | 1,3,7,17,41,101,239 (`a2_fast.h:35-41`)
- every `activation` = `LeakyReLU` slope 0.01; `gating_mode` all `none`; no `gated: true` (923); `secondary_activation` null
- `head1x1` inactive (936); `layer1x1` active, groups 1 (941); layer-array `head`: out_channels 1, kernel 16, dilation 1, bias true
- no FiLM anywhere; `groups_input == groups_input_mixin == 1` (971); no `slimmable` key (976)

Fallback (slow path) cases, all correct output but generic cost:
1. any A2-like config that differs in one of the above (other channel counts such as 4/5/6, different slope, gating, FiLM, extra layer array);
2. single-file slimmable WaveNet (1.2 item 2), at every size;
3. a `.nam` read with the macro off (not our build).
Measured here (one run, 4 s of audio, 64-sample blocks, Intel Xeon 2.1 GHz cloud VM, Release): A2 Lite fast 0.068 s vs the same weights forced
generic (slope 0.02) 0.310 s = 4.6x; A2 Full fast 0.348 s vs generic 0.353 s = no measurable gain on this x86 build. So the fast path
matters for Lite, and for Full only where upstream's benchmarks say so (macOS/arm, Task B CPU table). Indicative only; Task B owns the real numbers.

### 1.5 Latency and receptive field

- The core has **no latency API and no receptive-field getter** on `nam::DSP` (`NAM/dsp.h`: only `GetPrewarmSamples()`, line 105).
  NAM models are causal; the fast path and the generic path report no look-ahead. Sawblade therefore reports latency 0 for a NamBlock
  (`core/include/sawblade/nam_block.h:59-60`, `NamBlock::latencySamples()` is the Processor default), unchanged for A2: correct, since the
  trainer aligns capture latency out at training time. Verified: `NamBlock::latencySamples() == 0` for every A2 file probed.
- The receptive field that matters is the **prewarm length**: A2 = 1 + sum of per-layer lookback (kernel-1)*dilation + (head kernel 16 - 1)
  = **6347 samples** (132 ms at 48 kHz) for both Full and Lite (`a2_fast.cpp:183-190`; measured 6347 by probe); A1 standard = 4093 (generic
  `model.cpp:657-661`); LSTM example 24000. `NamBlock::prepare()/reset()` call `ResetAndPrewarm`, which runs that prewarm
  (`nam_block.cpp:133,144`), so prepare cost grows ~1.5x vs A1 standard and reset is not RT-safe (already documented at `nam_block.h:62-66`).
  A2 also caches the prewarm state (`a2_fast.cpp:342-403`, commit "Cache A2 prewarm state"), so repeat resets are cheaper.
- `A2FastModel::process` resizes buffers only if `num_frames > GetMaxBufferSize()` (`a2_fast.cpp:735`); `NamBlock::process` chunks to
  `maxBlock_` (`nam_block.cpp:150-151`), so that branch never runs. Probe: **0 allocations in `process()`** for A2 Full, A2 Lite, the container,
  single-file slimmable, and A1 standard; block sizes 512 vs 37 bit-identical for all A2 files (the container with an LSTM submodel differs by 4.5e-7,
  the known LSTM float behaviour at `core/src/chain.cpp:394` / `tests/test_level_match.cpp:308`).

### 1.6 Upstream history after the pin, bump proposal

`git rev-list --count 0b3d3c97..origin/main` = 0 (fetched 2026-10-07; origin/HEAD = main = tag `v0.6.0` = the pin). Only the upstream `dev`
branch is ahead ("Smooth parameters", unreleased). **No bump is proposed.** Relevant A2 work already inside the pin (newest first):
`Cache A2 prewarm state` (#319), `Fix A2 fast-path prewarm count` (#300), `Reject conditioned/gated configs in the A2 fast-path detector` (#301),
`Relax A2 head scale validation` (#273), `Fix LSTM real-time safety` (#299), slimmable breakpoint introspection (#298), `Sequential` model (#323),
variable-sample-rate models (#333; only `Linear` returns `SupportsArbitrarySampleRate()` true today, `linear.cpp:151`).
Licence of the pin: MIT (`LICENSE`, Copyright 2023 Steven Atkinson), already in `docs/THIRD_PARTY.md:19`.
Watch item for later: file version 0.8.0 would be rejected by this core; if a future trainer writes it, that is the trigger for a bump proposal.

## 2. A1 assumption inventory (core / plugin / cli / bindings / tests / docs)

`cli/` and `bindings/` contain no NAM architecture, size or latency logic (they go through `NamBlock` via `CaptureCache`): no findings.
`match/` is excluded (other engineer).

| file:line | assumption | what changes for A2 |
|---|---|---|
| `core/src/nam_block.cpp:43-44` | model must be mono 1-in/1-out | A2 Full/Lite and the container are 1/1 (`container.cpp:20`, `a2_fast.cpp:170`): no change |
| `core/src/nam_block.cpp:47-50` | reads `loudness`, `name`, `gear_type`, `modeled_by` from the top-level metadata | A2 container metadata is top-level too, but upstream's `A2.nam` carries only `date, loudness, gain, input_level_dbu` (no `name`/`gear_type`/`modeled_by`): `NamMetadata` strings come back empty. Fine for loading; matters for UI names and `ExportNotes` (`namName`). Trainer must write them at the container top level |
| `core/src/nam_block.cpp:104,111`, `nam_block.h:21` | loudness normalisation to -18 dB via `metadata.loudness` | unchanged semantics; `input_level_dbu`/`output_level_dbu` are never read by Sawblade, so an A2 file's dBu calibration is ignored (as for A1) |
| `core/src/nam_block.cpp:130-132` | throws if `expected_sample_rate != host rate` and not `SupportsArbitrarySampleRate()` (only `Linear`) | A2 files carry `sample_rate` 48000 at the top level: same behaviour as A1 with a rate. A1 files with no rate (-1) load at any rate unchecked. Unchanged |
| `core/src/nam_block.cpp:133,144` | `ResetAndPrewarm` at prepare/reset; deprecated shim in this core (`dsp.h:176-182` says it will be removed) | prewarm is 6347 samples for A2 vs 4093 for A1 standard (+55 % prepare time). `ResetAndPrewarm` still exists in v0.6.0 (`dsp.cpp:142`) but should move to `SetPrewarmOnReset(true)` + `Reset` before the next bump |
| `core/include/sawblade/nam_block.h:59-64` | latency 0; alloc-free "verified for WaveNet, LSTM and Linear" | latency 0 stays right for A2 (no latency API in core). The alloc/null claim is verified only for A1 fixtures; Task B adds A2 Full/Lite/container |
| `core/include/sawblade/nam_block.h:34-54`, `core/src/nam_block.cpp:56-82` | `NamModel` stores a parsed `dspData` and `NamBlock::load(model)` re-runs `get_dsp` | for a container this parses/instantiates **both** submodels on every load (and once more in `NamModel::load`, which discards the DSP). Memory = Lite + Full weights (~14k floats for A2.nam, small). Not a correctness issue |
| (absent) `core/` | nothing calls `SetSlimmableSize` or exposes the active size | A packed file always plays Full. Lite-from-container needs a new, non-RT call path (build/prepare off-thread, hand over via SwapSlot, per CLAUDE.md model-swap rule). Proposal for the lead, not done |
| `core/CMakeLists.txt:55-58`, `cmake/Dependencies.cmake:36-45` | `nam_core` linked whole-archive, A2_FAST on | required for A2 (container + WaveNet registration); keep |
| `core/src/chain.cpp:394` | comment: probe reset perturbs LSTM start-up ~1e-7 | for A2 the reset is deterministic (cached prewarm); the block-size independence probe is bit-exact for A2 Full/Lite. Determinism tests stay valid |
| `core/src/preset.cpp` / `docs/PRESET_SCHEMA.md:183` | schema table says nam block latency is "the model's" | code reports 0 (`nam_block.h:59`); wording only, same for A1 and A2 |
| `docs/PRESET_SCHEMA.md:359` | `source.modelId` "which model variant (size/architecture) of the tone" | still true; a preset records one model id, so an A2 tone with Full and Lite variants records one of them. No schema change needed unless a container-size choice is added |
| `plugin/src/browser/T3kJson.h:23,36`, `T3kJson.cpp:102-103,165` | parses `models_count`, `a2_models_count`, `a1_models_count`, and a free-text `architecture` per model list | already carries A2/A1 counts; `architecture` is stored but not interpreted. No Full/Lite distinction exists in the record |
| `plugin/src/browser/CaptureBrowser.cpp:33` | chip "A2" when `a2Count > 0`, then sizes from the tool | A2 is shown as one tag; no A2 Full vs A2 Lite, no A1-only marker (a tone with only A1 simply has no "A2" chip). If the pool tool starts reporting Full/Lite sizes they print through `r.sizes` (line 36) with no code change |
| `plugin/src/LadderFetch.h:24`, `plugin/src/PluginProcessor.cpp:303-310` | the gain ladder is fetched at size `"standard"`; a capture of another size "is not one of them" and gets no ladder (message at 308) | `standard` is an A1 size name. If the pool prefers A2 models (match/ side, `t3k/fetch.py`), the amp capture a user picked may be an A2 model that is not a "standard" rung, so the ladder is silently refused ("GAIN stays drive-only"). Needs the match side to define the ladder size for A2 (probably `a2`/`full`) and this constant to follow |
| `plugin/src/ExportPanel.cpp:266-268` | SIZE buttons FEATHER / LITE / STANDARD; tooltips "not judged against the acceptance limits" / "Standard-size model: judged against the acceptance limits" | these are the A1 size names. Task C: three-way A2 Full (default) / A2 Lite / A1, text and acceptance wording per size; the A1 sizes must stay reachable under A1 |
| `plugin/src/ExportPanel.cpp:384-386,588-589,653` | `size("feather"/"lite"/"standard")`, name mapping Feather/Lite/Standard | same; the per-size "last run N min" row (588-595) is keyed by these three strings |
| `plugin/src/ExportPanel.cpp:650` | "Training uses the standard NAM capture signal at 48 kHz" | A2 training signal/rate must match what the trainer uses; wording check in Task C |
| `plugin/src/ExportSettings.h:14`, `ExportSettings.cpp:23` | `size` is one of `{"feather","lite","standard"}`, default `"standard"` | the persisted setting rejects other strings; a new `arch` field (a2/a1) and size values (`full`, `lite`) need a new key plus a migration that maps the old value to A1. `docs/PRESET_SCHEMA.md:613` documents the same triple |
| `plugin/src/JobRunner.h:50,163`, `plugin/src/JobRunner.cpp:961` | passes `--size <feather|lite|standard>` to `sawblade-export`; wall time kept per size | needs `--arch`; the wall-time map (`MatchSettings::exportWallSeconds(size)`) keys must include the architecture or A2 Lite and A1 lite would share a timing |
| `plugin/src/ExportNotes.cpp:280-291` | the exported model is written as "NAM (name)" in a generic `loaderOrder`; only a generic loader | no A1-specific text; the `anagram` profile (Task C) extends it. No change needed for A2 itself |
| `tests/fixtures/nam/wavenet.nam`, `lstm.nam`, `linear_*.nam` (`NOTICE`, `THIRD_PARTY.md:84`) | every NAM fixture is A1 (v0.5.4) or Linear | no A2 fixture exists: nothing in the suite loads an A2 file today. Task B adds `tests/fixtures/a2/` (synthetic) |
| `tests/test_nam_block.cpp:107,128,141-142` | alloc-free, block-size and metadata tests loop over `wavenet.nam`, `lstm.nam`, `linear_*.nam` | add A2 Full, A2 Lite, A1 and a container to those loops (Task B) |
| `tests/test_resample.cpp:380` | "48 kHz WaveNet fixture" (A1) | unaffected |
| `plugin/tests/fake_tools.h:187` | fake exporter writes `{"version": "0.5.4", "architecture": "WaveNet", "config": {}, ...}` | still loads only as far as tests need; an A2 fake should write a container header for Task C panel tests |
| `plugin/tests/fake_tools.h:193`, `fake_t3k.py:45,110-113`, `test_browser_json.cpp:28,64` | `size == "standard"` is "judged"; fake tone data uses `a2_models_count`, `"architecture": "A2"`, size `"standard"` | fixture data only; update with Task C |
| `plugin/tests/test_editor.cpp:2764,2800,2827,2908,3161-3162`, `test_record_match.cpp:883` | export UI tests use sizes `feather`/`lite`/`standard` | change with the size/arch split |

Things that were checked and need **no** change for A2: block latency compensation and path alignment (`chain.cpp:364-375,498-500`: model latency is 0 and
the NamBlock contributes 0 to the path latency sum; no receptive-field math is used), gain ladder crossfades (they use `NamBlock`s), `CaptureCache` sharing (immutable `NamModel`), the
render report's latency fields, `tonerender`, the Python bindings, and the metadata `sawblade` block: **no reader of a `metadata.sawblade` block exists in
core/plugin/cli/bindings** (grep over all four returns only filesystem paths named "sawblade"); the block is produced and consumed on the match side.

## 3. Evidence

### 3.1 Probe (scratchpad, not committed)

A throwaway C++ program linked against the repo's `libnam_core.a` + `libsawblade_core.a` loaded each file through the real `NamBlock` at 48 kHz, 64-sample
blocks, and the `NamModel` path. Files: upstream `example_models/` at the pin, plus A2.nam's two submodels extracted as standalone files and version-edited copies.

```
file                       arch              ver    fast-shape  prewarm  loudness  latency  alloc in process()
A2.nam (container)         SlimmableContainer 0.7.0  -           6347     yes       0        0   (breakpoint 0.5 = Lite|Full; plays Full)
a2_lite (submodel 0)       WaveNet 3 ch       0.7.0  yes (3)     6347     yes       0        0
a2_full (submodel 1)       WaveNet 8 ch       0.7.0  yes (8)     6347     yes       0        0
slimmable_wavenet.nam      WaveNet slimmable  0.7.0  no          0        no        0        0   (generic)
slimmable_container.nam    container (LSTM+WaveNet x2) 0.7.0 -   4093     no        0        0
wavenet_a1_standard.nam    WaveNet 16/8       0.5.0  no          4093     no        0        0
lstm.nam                   LSTM               0.5.4  no          24000    yes       0        0
full copy, version 0.7.1   WaveNet 8 ch       0.7.1  yes         6347     yes       0        0   (stderr warning, plays)
full copy, 0.8.0 / 1.0.0 / 0.4.0                                                            load throws "unsupported version"
```

Indicative cost, 48 kHz, 64-sample blocks, % of one core of a 2.1 GHz Xeon VM (single run, not CI): A1 standard 27 %, A2 Full 8 %, A2 Lite 1.5 %,
container (plays Full) 8 %, LSTM example 0.7 %. Task B owns the proper CI Mac/Linux table.

## 4. Local build status (needed for Task B)

- Container: 4 cores, g++ 13.3.0, clang++ present, Ninja, CMake available, network to GitHub works through the proxy.
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_WITH_SEPARATOR=OFF` configures in about 70 s (fetches pinned Eigen, nlohmann/json,
  NAM core, pffft, dr_libs, Catch2). `SAWBLADE_BUILD_PLUGIN` is OFF by default so no JUCE/X11 is needed; the separator option defaults OFF without the
  plugin, so the plain command from CLAUDE.md works as well (the flag above only makes that explicit).
- `cmake --build build`: 210 steps, no warnings (`-Wall -Wextra -Wpedantic -Werror` on Sawblade targets), a few minutes on 4 cores.
- `ctest --test-dir build -j4`: **371 tests, 100 % passed, 0 failed, 1 not run** (#196 "Committed presets ... within 0.5 LU of -18 LUFS", skipped by the
  test itself on this machine). Wall time about 20 s.
- Not built here: the JUCE plugin and its headless plugin tests (`SAWBLADE_BUILD_PLUGIN=ON`; JUCE needs X11/ALSA dev packages and a commercial/AGPL licence
  decision for distribution), the Python bindings (`SAWBLADE_BUILD_PYTHON`, not attempted), pluginval, auval. Task B's core/CLI work and the new tests in
  `tests/` can be developed and run entirely here; plugin-side checks (export panel, 64-sample two-A2-Full rig on macOS) need CI.
- Build dir is `build/` in the repo root (git-ignored).

## 5. Proposals (not done; for the lead)

1. Task B: add `tests/fixtures/a2/` (Full, Lite, packed container, A1) and extend the `test_nam_block.cpp` loops (null vs trainer forward pass, latency 0,
   zero allocations, block-size independence). Add a check that the trainer-exported container/standalone files hit the fast path (`is_a2_shape`), since a
   one-field deviation silently drops Lite to 4.6x the CPU.
2. Decide whether Sawblade should expose the container's Lite submodel (no UI today; always Full). If yes, it needs an off-thread `SetSlimmableSize` +
   SwapSlot hand-over, not a call from `process()`.
3. Replace `ResetAndPrewarm` with `SetPrewarmOnReset(true)` + `Reset` before any future bump (deprecated shim).
4. Ladder size constant (`LadderFetch.h:24`) must follow whatever size name the pool uses for A2 models.
