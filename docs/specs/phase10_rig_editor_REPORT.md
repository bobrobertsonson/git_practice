# Phase 10 report: the full rig editor

Spec: `docs/specs/phase10_rig_editor.md`. Status: **accepted by the lead after reviewer ACCEPT (round 2).**
Branch `claude/sawblade-p10-rig-editor`, commits `8a76d65` (spec), `65f1a9a` (core), `a8e42f0` (plugin
plumbing), `dc87599` (rig model + controller), `294fe7f` (UI), `2f7d840` (docs), `9080f4a` (review fixes).
No PR was opened.

Artifact: **Sawblade Rig Editor**, https://claude.ai/artifact/B575dcgrd6nGAg99mzxZfn (1x, 1280 x 800: SINGLE
and BLEND chain views, EQ, gate, plus the blend, cab and comp tabs; private until shared). The screenshots
come from the editor tests on fixture captures; nothing from TONE3000 is committed.

## What the user gets

Every blend feature of the engine is now reachable from the plugin UI, through a `RIG` overlay over the rig
area (the inspector stays visible):

- **Topology** `SINGLE | SINGLE + 2 PEDALS | BLEND`, derived from the preset (Blend iff path B is enabled;
  otherwise by the number of active pedal slots on A). Switching keeps every block: single topologies park
  path B and force blend to 0; surplus pedals are bypassed, not deleted.
- **CHAIN**: per-path slot strips (A · SAW, B · BODY) with one card per block: slot and type, capture title,
  creator and licence line, BYPASS, a live INPUT gain knob (nam blocks), move and remove, and `+ ADD`
  listing every type in the block registry (so `pedal.*` types from the other session appear with no UI
  change). `nam` asks for a file; other types are built through the registry's parser.
- **EQ**: graphical editor for A pre, A post, B pre, B post and the post EQ. Draggable nodes for peak,
  shelves, high and low pass; frequency, gain and Q are live ramps; type, enable and add/remove rebuild.
  The first six post-EQ gains remain the existing host parameters.
- **BLEND**: blend knob, path levels, mute and solo, align `AUTO | MANUAL | OFF` with the measured delay,
  polarity and correlation, nudge in samples, INVERT B, and RE-MEASURE (runs the probe off-thread and
  stores the result as manual).
- **CAB**: shared vs per-path IRs with the required notice ("STUDIO BLEND: only the with-cab NAM export is
  exact" / "LIVE-COMPATIBLE: the no-cab NAM export is exact").
- **GATE**: on/off, gate/expander, threshold (host parameter), hysteresis, attack, hold, release, range,
  ratio, key high-pass, release curve, and LEARN (1 s of input peaks, +6 dB, compensated for input gain).
- **COMP**: on/off and all six bus-compressor fields, with the "> 150 ms: not NAM-trainable" flag.
- **State**: everything round-trips through the preset. The parameter list is unchanged (12 APVTS
  parameters). Transient only: mute/solo, panel open/tab, the "+ 2 PEDALS" choice on a preset that reads
  Single, the remembered blend.

## How it is built

- **Core** (additive, goldens bit-identical): `ParametricEq::setBand` (RT-safe redesign of one band);
  `LiveParams` grew per-band `{freq, gainDb, q}` for all five EQs, per-block input/output gains and
  `muteA/muteB`; ramps in dB, log2(freq) and log(q) on the existing 32-sample redesign grid;
  `Processor::setLiveGainsDb` with the `NamBlock` override; `BlockRegistry::typeNames()`.
- **Plugin plumbing**: a `LiveSnapshot` (generation + `LiveParams`) travels through a second `SwapSlot`
  to the audio thread, published only under the processor mutex; `Engine::setParams(values, extras)`
  overlays the host parameters on the snapshot; engines carry the loader request id as their generation,
  so an old engine never receives another preset's live values. The loader owns a persistent
  `CaptureCache`, so structural edits rebuild without re-reading files. New engines are seeded with the
  current mutes before publish and the fading engine takes its mutes from the snapshot, so a soloed path
  stays exactly silent through the 30 ms cross-fade.
- **Rig model** (`plugin/src/rig/RigModel`, JUCE-free): topology, slot, EQ, cab, align, gate and comp edits
  as pure functions on `Preset`, unit-tested on their own.
- **Controller**: structural edits via `loadPreset` (loader + cross-fade; knobs submit on drag end, wheel
  and typed values debounced 150 ms), live edits via `applyLiveEdit`, host parameters with gestures,
  monitor state, re-measure, LEARN through a lock-free `InputMeter` ring.
- New code: 13 files under `plugin/src/rig/` plus two test files (about 3,500 lines). Edits to the
  shared files are small: a `RIG` button and panel hookup in `PluginEditor`, hooks in `PluginProcessor`,
  an unbound `FilmstripKnob` constructor, snapshot/cache/generation in `Engine`/`EngineLoader`. `RigView`
  is untouched. Total diff: 41 files, +4,892 / -113.

## Tests

- `build-plugin` (gcc, Release, `-Werror`): **ctest 276/276 passed**, including `plugin: pluginval VST3`
  at strictness 10 (SUCCESS) and the editor tests under xvfb.
- `build-clang` (clang, Release, plugin ON): built with **0 warnings**; ctest 275/275 (pluginval is
  registered only in the gcc tree).
- Reviewer's independent runs: clean core + CLI build 188/188; Debug ASan/UBSan core + CLI 188/188, no
  sanitizer reports.
- New coverage: live EQ freq/Q/gain equal a chain built with those values after the ramp, block-size
  invariance {1, 7, 64, 1024}, live block gain, mute to exact silence; topology round trips through JSON
  and through the processor; slot add/move/remove/bypass through the loader (one engine build per edit,
  latency follows bypass); live edits without rebuilds; re-measure off-thread (`delaySamplesB == -300`,
  mode written back as manual, no audio-thread allocations or locks); host automation of blend without
  rebuilds; 2000 blocks with concurrent structural edits, live edits at ~200 Hz, mutes and a re-measure at
  zero allocations and locks; LEARN with and without input gain; panel open, titles and tooltips on every
  control; EQ node drag maps to the band within 1e-6 and writes the `postEq1` parameter; topology and cab
  buttons act through the loader; a superseded re-measure leaves no stuck state; mutes survive a rebuild
  with no blip.

## Reviewer rounds

1. **Round 1: REVISE.** Must fix: a re-measure request replaced in the loader queue by a later edit left
   `alignMeasuring` stuck and made the next edit inherit `align: auto`. Should fix: a generation race with
   `restorePreset`, mutes blipping across rebuilds, LEARN not compensating input gain, no message at the
   8-block limit, the inspector blend/level-B knobs staying enabled in single topologies, and the SINGLE
   screenshot showing a stale `SAW 50 / BODY 50` (a test that snapshotted before pumping the message loop,
   not a bug). The lead required all of them; fixed in `9080f4a`.
2. **Round 2: ACCEPT.** Non-blocking notes: the superseded-re-measure test is timing-dependent (it does
   not always hit the exact window); no dedicated test for the generation-serial race; the loader's
   `CaptureCache` is unbounded (grows with distinct files touched in a session); the top-bar widgets were
   narrowed to fit `RIG`, a small merge risk with the parallel sessions.

## Accepted deviations from the spec

- `loadPreset(Preset, bool keepMonitor)`: editor edits keep mute/solo; user loads and restores clear it.
- `userLoadSerial()` resets transient UI state instead of the generation/name heuristic.
- `→ Single` bypasses every pedal slot after the first non-bypassed one; `→ Single + 2 Pedals` un-bypasses
  one whenever fewer than two are active.
- Default slot labels: `eq` → `fx`; the first `nam` on an amp-less path → `amp`; else `pedal`.
- `+ ADD` inserts a pedal in front of the path's amp.
- `nudgeAlign` / `setInvertB` take the measured `AlignResult`; from `Off` a nudge goes to `Manual` from zero.
- Spec test 2 drives the audio on one thread; the concurrent-thread proof is spec test 6.
- A preset in `align: auto` stays auto on load (deterministic re-resolve); only RE-MEASURE writes back as
  manual (`docs/PRESET_SCHEMA.md` amended).

## Process (honest note)

- **Review rounds:** 2 (REVISE, then ACCEPT).
- **Wall-clock:** session start about 13:50 UTC; spec committed 14:03; implementation 14:09 to 14:33 (about
  40 minutes of implementer time, 88 tool calls); fixes 19:10; accepted about 19:20. Elapsed about 5.5
  hours, of which **about 4 hours were lost to the account's 5-hour usage limit**: the first reviewer run
  died with a 429 at about 14:46 and the limit reset at 18:40.
- **Other slowdowns:** the container lacked the X11/ALSA/GL dev packages JUCE needs (first configure
  failed; installed, then the baseline plugin build ran in the background while the spec was written);
  pluginval had to be cloned and built (about 10 minutes, background); a fresh FetchContent clone hung
  behind the proxy for the reviewer's ASan tree, worked around by pointing it at the existing dependency
  sources. The plugin build compiles JUCE three times on 4 cores, so every tree is reused, never wiped.
- **What went well:** the spec's split into a JUCE-free rig model, a controller and the panel let the
  implementer land all of it in one pass with the acceptance tests green; the reviewer found one real
  concurrency bug (the stuck re-measure) and two subtler ones (generation race, mute blip) that the tests
  had not covered, and all were fixed in a single round.

## Open items for the main lead

1. Prune the loader's `CaptureCache` (for example to the current and previous preset's files when idle) if
   browsing many captures turns out to use too much memory.
2. Make the superseded-re-measure test deterministic (hold the loader busy with a slow request first).
3. The inspector still shows the prototype's static selection text ("SELECTED · SAW PEDAL / STOCKHOLM
   SYNDROME"); out of scope here, it belongs to the capture-browser session.
4. Proposed extras (not built): drag-reorder of blocks, typed value boxes next to the EQ readout, a spectrum
   analyser behind the EQ graph, a CPU meter.
5. The PLAY ALONG and RIG overlays can both be open; the most recently opened one is on top.
