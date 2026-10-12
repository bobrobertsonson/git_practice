# Phase 10: the full rig editor (plugin)

The plugin runs in Logic. The user's verdict: it "just needs all the blend features". Today the
editor exposes 12 APVTS parameters and the rendered rig; everything else in the engine (two-path
chains, per-path EQ, alignment, cab mode, gate, bus comp) is reachable only by editing preset JSON.
This phase makes every blend feature of the engine usable from the UI, for any heavy tone
(CLAUDE.md scope rule: nothing may be hard-wired to Gatecreeper or the HM-2; single-path tones are as
well supported as blends).

Implementer: `dsp-engineer`. Reviewer: `reviewer`. Branch: `claude/sawblade-p10-rig-editor`
(commit there; do not push, the lead pushes). Other sessions edit `PluginEditor.*`,
`PluginProcessor.*` and `RigView.*` in parallel (mic/presets, capture browser, record+match): keep
the edits to those files minimal and additive, and put all new code in `plugin/src/rig/`.

Read first: CLAUDE.md (signal graph, hard rules, export rules), `docs/PRESET_SCHEMA.md`,
`docs/PLUGIN.md`, `docs/specs/phase2_5_skin_REPORT.md`, `docs/specs/phase5_2_playalong_plugin_REPORT.md`
(the PlayAlongPanel is the pattern for an overlay panel that talks to the processor), and the
mockups `design/mockups/RigReal.dc.html`, `FullRig.dc.html`, `HeadA.dc.html`, `BoardB.dc.html` (reference
only; the user designs the final UI, keep the existing skin's look: palette in
`SawbladeLookAndFeel`, filmstrip knobs, renders).

## 0. Environment notes (this container)

- `build-plugin/` is configured (`-DSAWBLADE_BUILD_PLUGIN=ON`, pluginval registered) and may still be
  building when you start: check `/tmp/claude-0/-home-user-git-practice/d5b631c9-2d53-53fe-8de4-2a13516ce3be/scratchpad/build.log`
  ends with `exit 0` before using it; never delete it (a JUCE rebuild costs a long time on 4 cores).
- pluginval v1.0.4 is at
  `/tmp/claude-0/-home-user-git-practice/d5b631c9-2d53-53fe-8de4-2a13516ce3be/scratchpad/pluginval/build/pluginval_artefacts/Release/pluginval`
  (building in the background; log `pluginval_build.log` next to it). The ctest `plugin: pluginval VST3`
  uses it.
- Editor tests need `xvfb-run -a` (CMake sets it as the emulator). Screenshots land in
  `build-plugin/screenshots/`.
- A clang build is required too (the user is on Macs): `cmake -S . -B build-clang -G Ninja
  -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`
  then build; it must be warning-free (`-Werror` is on for our targets). Run it once at the end, `nice`d.

## 1. Architecture

```
plugin/src/rig/
  RigModel.{h,cpp}        JUCE-free edits on sawblade::Preset: topology, slots, EQ bands, cab,
                          align, gate, comp. Pure functions; every rule below is tested here.
  InputMeter.h            lock-free per-block input peak ring (audio thread writes) for LEARN.
  RigController.{h,cpp}   message-thread facade over SawbladeProcessor: structural edits -> loader
                          (30 ms cross-fade), live edits -> live snapshot, mute/solo, re-measure,
                          LEARN. Owns debouncing. No JUCE GUI, but may use juce::Timer.
  RigEditorPanel.{h,cpp}  the overlay (940 x 742 design px over the rig area): topology selector,
                          tab strip CHAIN / EQ / BLEND / CAB / GATE / COMP, content.
  SlotStrip.{h,cpp}       one path's modular slot strip (block cards).
  EqGraph.{h,cpp}         the interactive graphical EQ.
  (further panel classes as you see fit, all under rig/)
core/                     small, additive changes listed in section 2 only.
```

Edits to existing files, all small and additive:
- `PluginProcessor.{h,cpp}`: the hooks in section 3 (edit base, live edits, monitor, re-measure,
  input meter, live snapshot in `processBlock`).
- `Engine.{h,cpp}`, `EngineLoader.{h,cpp}`: `setParams` takes the optional live snapshot; the loader
  owns a persistent `CaptureCache` and tags engines with their request id.
- `PluginEditor.{h,cpp}`: one `RIG` top-bar toggle button (like PLAY ALONG), the panel as the last
  child, `setRigEditorOpen/rigEditorOpen`, a refresh tick, and wiring the existing inspector `LEARN GATE`
  button to the same LEARN action (it stops being a placeholder).
- `skin/FilmstripKnob.{h,cpp}`: an unbound constructor (no APVTS) for the panel's preset-only knobs,
  same look and drag behaviour.
- `plugin/CMakeLists.txt`: the new sources and tests.
- `plugin/tests/test_editor.cpp`: the "exactly one knob per parameter" rule becomes "exactly one
  knob per parameter outside the RigEditorPanel" (the panel binds blend, levels and gate threshold a
  second time).
- `docs/PLUGIN.md` (new "Rig editor" section), `docs/PRESET_SCHEMA.md` (one sentence on align
  write-back, section 5.4). No change to the preset schema's shape.

## 2. Core changes (additive, defaults bit-identical)

The graphical EQ, block input gains and mute/solo must be live (ramps through
`Chain::setLiveParams`), not rebuilds. Existing presets and goldens must render bit-identically:
ramps start only when a value differs from the one in effect, and `LiveParams::fromPreset` seeds every
new field from the preset so the first `setLiveParams(fromPreset(p))` is a no-op.

2.1 `ParametricEq::setBand(int bandIndex, double freq, double gainDb, double q) noexcept`: RT-safe
redesign of one configured band (type and enabled unchanged, filter state kept). Ignored for an
out-of-range index, a disabled band, non-finite values, `freq` outside `[10, 0.49 fs)`, `q` outside
`[0.05, 36]`. Gain is ignored for high/low-pass bands (as `setBandGainDb`).

2.2 `LiveParams` grows:
```cpp
struct LiveEqBand { double freq = 0.0, gainDb = 0.0, q = 0.0; };   // freq 0 = no band at that index
using LiveEq = std::array<LiveEqBand, ParametricEq::kMaxBands>;
LiveEq postEq;                               // replaces postEqGainDb (update all users + tests)
std::array<LiveEq, 2> preEq, pathEq;         // [0] = A, [1] = B
struct LiveBlock { double inputGainDb = 0.0, outputGainDb = 0.0; };
std::array<std::array<LiveBlock, kMaxBlocksPerPath>, 2> blocks;   // nam blocks; others ignore
bool muteA = false, muteB = false;           // monitoring; not preset state
```
Ramps, all over `kLiveRampMs`, all on the existing absolute `kEqSubBlock` redesign grid: gain in dB
(linear in dB, as now), `freq` linear in log2(freq), `q` linear in log(q). Each of the five EQs has its
own ramp array. A band whose live `freq` is 0 is ignored. Changing a band's type, enabled flag, or the
number of bands is NOT live (structural; the plugin rebuilds).

2.3 Block gains: `Processor` gets `virtual bool setLiveGainsDb(double inDb, double outDb, int rampSamples) noexcept { return false; }`.
`NamBlock` overrides it: input and output gain become ramped (`Gain`-style linear ramps over
`rampSamples`; the loudness normalisation offset stays folded into the output gain as today). The chain
calls it for every block whose live gains changed. Bit-identical when unchanged.

2.4 Mute: `muteA/muteB` make the path's level ramp to 0 (linear) and back to `(invert ? -1 : 1) * lin(level)`
when unmuted; a level change while muted changes the stored target only.

2.5 `BlockRegistry::typeNames() const` -> `std::vector<std::string>` (sorted). Used by the ADD menu so
new block types (`pedal.*`, from another session) appear without UI changes.

2.6 Tests (`tests/test_chain_live.cpp`, extend): a live freq/Q change on a post, pre and path EQ band
equals a chain built with those values once the ramp is over (|diff| <= 1e-6 on noise, 1 s); the
ramp is bit-identical for block sizes 1, 7, 64, 1024; live block input gain equals a built one; mute
ramps to exact silence and back; `setLiveParams` and `process` allocate nothing with all new fields
changing (allocation harness). Existing goldens untouched and passing.

## 3. Processor and loader hooks

3.1 Persistent capture cache. `EngineLoader` owns one `CaptureCache` for its lifetime and passes it to
`Engine::build(preset, hostRate, maxBlock, CaptureCache*)`. Structural edits then rebuild without
re-reading model or IR files (the cache re-validates by size+mtime; hash semantics unchanged).

3.2 Generation tag. `EngineLoader` sets `engine->generation()` to the request id before publishing, and
`Request::beforePublish` receives that id: `std::function<void(std::uint64_t id)>`. The processor's
`commit()` records `presetGeneration_ = id`.

3.3 Live snapshot. `struct LiveSnapshot { std::uint64_t generation; LiveParams live; }` travels through a
second `SwapSlot<LiveSnapshot>` (processor member `liveSlot_`). Producer: the processor, always under
`mutex_` (message thread for edits, loader thread in `onOutcome`); consumer: the audio thread. The
mutex serialises the two producer threads, which satisfies the slot's single-producer contract
(document this at the member). `collectGarbage()` runs right after each publish.
- `void publishLive()` (private, mutex held): `live = LiveParams::fromPreset(clampedToParams(preset_))`,
  then `live.muteA/muteB = monitor_`, generation = `presetGeneration_`, publish.
- `onOutcome` (loader thread) calls `publishLive()` after every published engine (so a fresh engine
  always has a snapshot of its own generation, and mutes survive rebuilds).
- `processBlock`: `LiveSnapshot* s = liveSlot_.current(); engine->setParams(pv, (s && s->generation == engine->generation()) ? &s->live : nullptr);`
  and the same for the fading engine (its own generation). `Engine::setParams(const ParamValues&, const LiveParams* extras)`
  starts from `extras` if given, else from its baseline, overlays the APVTS values (input/output gain,
  gate threshold, blend, levels, post-EQ slot gains into `postEq[band].gainDb`), and calls
  `Chain::setLiveParams`. Nothing here allocates or locks (extend the RT tests).
- Identity rule kept: with the restored parameter values and a snapshot of the engine's own
  generation built from the same clamped preset, `liveParams() == baseline()` (existing test must still
  pass; add the snapshot case).

3.4 Processor API (public, message thread):
```cpp
Preset editBasePreset() const;                       // wanted_ (pending user load) if set, else currentPreset()
void applyLiveEdit(const std::function<void(Preset&)>& edit);  // mutex: edit(preset_), publishLive(). Live fields only (EQ freq/gain/q, block gains)
void setMonitor(bool muteA, bool muteB);             // transient; publishLive()
struct Monitor { bool muteA = false, muteB = false; };  Monitor monitor() const;
void remeasureAlignment();                            // 5.4
const InputMeter& inputMeter() const noexcept;        // 5.6
std::uint64_t presetGeneration() const;
```
`applyLiveEdit` must not touch APVTS-mapped fields (the controller writes those through the
parameters) and does not touch `wanted_`; a live edit made while a structural load is in flight can be
overwritten when that load commits (documented limitation; EQ drags are absolute and self-heal on the
next mouse move).
`Status` gains: `AlignResult measuredAlign` and `bool alignMeasuring` (set while a re-measure build is in
flight), `std::uint64_t generation`.

3.5 Structural edits go through the existing `loadPreset(Preset)` (loader thread build, `SwapSlot`
hand-over, 30 ms cross-fade, failed builds keep the previous preset and report `status().error`).
No new code path on the audio thread.

## 4. Rig model (JUCE-free, `rig/RigModel.{h,cpp}`, namespace `sawblade::plugin::rig`)

4.1 Topology.
```cpp
enum class Topology { Single, SinglePlusTwoPedals, Blend };
Topology topologyOf(const Preset&);
void setTopology(Preset&, Topology to, double blendIfRestored);   // see rules
```
Definitions: a path's **amp** is its last block with `slot == "amp"`, or, if none, its last `nam` block;
every other block is a **pedal slot** (whatever its type: `nam`, `eq`, `pedal.*`).
- `topologyOf`: `Blend` iff `b.enabled`; otherwise `SinglePlusTwoPedals` iff path A has >= 2
  non-bypassed pedal-slot blocks; otherwise `Single`.
- `-> Blend`: `b.enabled = true`; if `blend == 0`, `blend = blendIfRestored` (the controller passes the
  blend value remembered from the last time the preset was in Blend during this editor session, else 0.5).
- `-> Single` / `-> SinglePlusTwoPedals`: `b.enabled = false`, `blend = 0` (the chain's blend is
  `(1-blend)*A + blend*B`, so a disabled B with blend 0.5 would halve A). Path B's blocks are kept.
- `SinglePlusTwoPedals -> Single`: every pedal-slot block of A after the first gets `bypass = true`
  (kept, never deleted).
- `Single -> SinglePlusTwoPedals`: the first bypassed pedal-slot block of A, if any, gets `bypass = false`;
  with none, the preset is unchanged and the panel shows an empty second slot (the panel keeps the user's
  choice as UI state until the next preset load / state restore).
In single topologies the blend knob is disabled in the UI (the host may still automate `blend`; note it
in docs/PLUGIN.md).

4.2 Slots (`PathPreset&`): `bool addBlock(PathPreset&, int index, Block)` (false when 8 blocks),
`removeBlock(PathPreset&, int)`, `moveBlock(PathPreset&, int from, int to)`, `setBypass`,
`bool setBlockInputGainDb(PathPreset&, int, double)` (nam only, clamp -24..+24), `std::string newBlockId(const Preset&, char path)`
(unique within the preset, e.g. `"a3"`), `Block makeBlock(type, id, slot, baseDir, const nlohmann::json& typeFields)`:
builds a block of any registered type by parsing `typeFields` through `BlockRegistry::find(type)->parse`
(so `nam` gets `{"model": {"file": ...}}`, `eq` gets `{"bands": [flat peak @ 1 kHz]}`, a `pedal.*` type
gets `{}` and may throw a `PresetError` naming the missing field; the UI shows that message). Default
slot label for a new block: `"amp"` if the path has no amp yet, else `"pedal"`.
Display helpers: `std::string blockTitle(const Block&)` (capture `source.title`, else the file stem for
`nam`; `"EQ"` + band count for `eq`; the type name otherwise), `blockCredit(const Block&)`
(`"@creator · licence · VIA TONE3000"` when `source` is present, `"LOCAL FILE"` for a nam without
source, `""` otherwise).

4.3 EQ. `enum class EqTarget { PreA, EqA, PreB, EqB, Post }; std::vector<EqBand>& eqBands(Preset&, EqTarget);`
`bool addBand(Preset&, EqTarget, EqBand)` (false at 16), `removeBand`, `setBandLive(Preset&, EqTarget, int, double freq, double gainDb, double q)`
(clamps freq to [20, 20000], gain to [-18, 18] for peak/shelf and 0 for pass filters, q to [0.1, 20]),
`setBandType`, `setBandEnabled`. Which edits are live vs structural is 4.3's table:

| edit | path |
|---|---|
| freq, Q of any band; gain of a pre/path EQ band or a post band without an APVTS slot | live (`applyLiveEdit`) |
| gain of a post EQ band with a slot (`postEqSlotBands`) | APVTS `postEqN` parameter (host sees it) |
| type, enabled, add, remove | structural (`loadPreset`) |

4.4 Cab. `setCabMode(Preset&, CabMode)`: Shared -> PerPath copies `ir` into `irA` and `irB`; PerPath ->
Shared sets `ir = irA`. `setCabEnabled`, `setCabIr(Preset&, which, Capture)` (which: shared / A / B).

4.5 Align. `setAlignMode`, `nudgeAlign(Preset&, int samples)` (clamps `delaySamplesB` to +-2400),
`setInvertB`. Nudging or inverting in `Auto` mode switches the mode to `Manual` first, seeded with the
measured values the controller passes in.

4.6 Gate / comp: setters for every `GateParams` and `BusCompParams` field with the parser's ranges
(`core/src/preset.cpp`: hysteresis 0..60, attack 0.01..1000, hold 0..10000, release 0.01..10000, range
-120..0, ratio 1.5..10, key HPF 0 or 40..400; comp threshold -80..0, ratio 1..100, knee 0..48, attack
0.01..1000, release 1..10000, makeup -24..48). Gate threshold is APVTS, everything else structural.

## 5. Controller (`rig/RigController`)

5.1 Structural edit: `edit(std::function<void(Preset&)>)` applies the function to
`processor.editBasePreset()` and calls `processor.loadPreset(result)`. Edits from knobs are submitted
on drag end (mouse up) and, for wheel / typed values, debounced by 150 ms (latest wins); the display shows
the pending value immediately. The loader's latest-wins coalescing is the second line of defence.

5.2 Live edit: `live(std::function<void(Preset&)>)` -> `processor.applyLiveEdit`; EQ drags call it on
every mouse move. Post EQ slot gains go to the APVTS parameter instead (`setValueNotifyingHost`, with
begin/endChangeGesture around the drag).

5.3 Mute/solo: `setMute(path, bool)`, `setSolo(path, bool)` (solo A == mute B and not A; solo is a
UI state, mapped onto the two mutes) -> `processor.setMonitor`. Cleared on preset load and state restore
(the processor clears `monitor_` in `commit()` for user loads and restores; `publishLive` follows).

5.4 Re-measure (`processor.remeasureAlignment()`): takes `editBasePreset()`, sets `align.mode = Auto`,
submits through the normal loader path and remembers the request id; in `onOutcome`, if that id was
built, writes `preset_.align = {Manual, maxLagMs, info.align.delaySamplesB, info.align.invertB}`,
stores `status_.measuredAlign = info.align`, clears `alignMeasuring`. The engine (built with Auto and
those resolved values) is behaviourally identical to the stored Manual preset, so no rebuild follows.
The button is disabled while loading or when a path is disabled (`Chain` skips alignment then).
A preset that is in `Auto` at load time stays `Auto` (it re-resolves deterministically on every load);
only RE-MEASURE writes back. Amend the one sentence in `docs/PRESET_SCHEMA.md` ("Align") accordingly.

5.5 Topology memory: the controller remembers the last Blend `blend` value for `setTopology`, and the
user's `SinglePlusTwoPedals` choice when the preset reads `Single` (reset on load/restore, detected by
`presetGeneration()` changes with a different preset name or a `wanted_` load).

5.6 LEARN. `InputMeter` (`rig/InputMeter.h`): 512 entries of `std::atomic<float>` (block peak, linear)
plus an `std::atomic<std::uint32_t>` counter; `push()` from `processBlock` on the summed mono input
before the engine (no allocation); readers call `std::vector<float> since(std::uint32_t)`.
`RigController::learnGate()`: notes the counter, waits 1.0 s (timer), reads the blocks since; with
none, shows "no input"; else `threshold = clamp(20*log10(maxPeak) + 6 dB, -80, -20)` written to the
`gateThreshold` parameter (notifying the host). The UI says "LEARN: don't play for 1 s" while measuring
and then "thr set -48.0 dB".

## 6. UI (`rig/RigEditorPanel` and friends)

The panel is an overlay exactly over the rig area (`0, kTopBar, 940, 742` in design px), opened by a
`RIG` toggle button in the top bar (place it next to PLAY ALONG; narrow A/B if needed), closed by default;
open/closed and the active tab are UI state, never saved. The inspector stays visible on the right.
Look: `SawbladeLookAndFeel` palette and fonts, `FilmstripKnob` (pedal size) for continuous controls, path
A in `saw()`/orange, path B in `body()`/blue, panels `panelDeep()`, dividers `rule()`. Every control has
`setTitle` and a tooltip (the existing accessibility test is extended to the panel).

Top strip (always visible in the panel): topology segmented buttons `SINGLE | SINGLE + 2 PEDALS | BLEND`,
then the tab strip `CHAIN | EQ | BLEND | CAB | GATE | COMP`, then a one-line status (loading / error /
first warning, as the main message label).

6.1 CHAIN: two lanes, `A · SAW` and `B · BODY`. Each lane is a `SlotStrip`: left-to-right cards for the
blocks in preset order (max 8; cards shrink to fit), each card showing the slot label and type (`PEDAL ·
NAM`, `AMP · NAM`, `FX · EQ`, `PEDAL · pedal.hm`...), `blockTitle`, `blockCredit` (small, dim), a BYPASS
toggle (LED-style on/off text), an INPUT knob (nam only; unbound FilmstripKnob, -24..+24 dB, live on
every move), `‹ ›` move buttons and `✕` remove; at the end a `+ ADD` button whose popup lists
`BlockRegistry::typeNames()`; `nam` opens a `.nam` file chooser (async), `eq` adds a flat band, other types
go through `makeBlock` (error shown in the status line). In single topologies lane B shows only
"BLEND OFF - choose BLEND above to use path B" (its blocks are kept). Cards for the bypassed pedal slot
hidden by `SinglePlusTwoPedals -> Single` are shown dimmed in `Single` too (so nothing is invisible).

6.2 EQ: selector `A PRE | A POST | B PRE | B POST | POST` (B's disabled in single topologies). `EqGraph`
(700 x 380): x = log frequency 20 Hz .. 20 kHz, y = gain -18 .. +18 dB; grid lines at decades/octaves
and 6 dB; the combined response of the enabled bands drawn (analytic, `designBiquad` +
`biquadMagnitudeDb` at `status().modelRate`, 48 kHz if 0); a node per band (filled: enabled; hollow:
disabled; colour by type), the selected node highlighted. Mapping (document in `EqGraph.h`, tested):
`freq = 20 * 1000^(x / width)`, `gainDb = 18 - 36 * y / height` for peak/shelf; for high/low-pass y maps
Q: `q = 0.1 * 200^((height - y) / height)` (so the node sits at the band's Q). Interaction: drag = freq +
gain (or Q for pass filters), shift-drag or mouse wheel = Q, double-click a node = enable/disable
(structural), right-click = menu (type x5, remove; structural), double-click empty space = add a peak
band at that position (structural), `+ BAND` button. Below the graph a readout of the selected band
(`PEAK · 1.20 kHz · +3.0 dB · Q 1.00`). Post EQ slot gains go through APVTS (section 4.3); the knobs in the
inspector and the graph agree (both bound to the parameter; the graph listens to the APVTS).

6.3 BLEND: the blend knob (bound to `blend`; disabled in single topologies), readout `SAW 79 / BODY 21`;
path level knobs (bound to `levelA`, `levelB`) with `M` and `S` toggles per path; ALIGN: segmented
`AUTO | MANUAL | OFF`, readout `Δ -17 smp · Ø NORMAL · corr 0.93` (Auto: `status().info.align`; Manual: the
preset's values; Off: `—`), nudge buttons `-10 -1 +1 +10` and an `INVERT B` toggle (both enabled in
Manual only; in Auto they switch to Manual seeded with the measured values, 4.5), and `RE-MEASURE`
(shows `measuring…` while `alignMeasuring`).

6.4 CAB: segmented `SHARED | PER PATH`, `CAB ON` toggle, IR cards (shared: one; per path: `A` and `B`) with
title/credit and a `CHOOSE…` file button (`.wav`), and the required notice: per path =>
`STUDIO BLEND: only the with-cab NAM export is exact` in `studio()`; shared => `LIVE-COMPATIBLE: the
no-cab NAM export is exact` in `live()`.

6.5 GATE: `GATE ON`, mode `GATE | EXPANDER`, knobs THRESHOLD (bound to `gateThreshold`), HYSTERESIS, ATTACK,
HOLD, RELEASE, RANGE, RATIO (expander only, dimmed otherwise), KEY HPF (0 = OFF, else 40..400 Hz; the
knob's bottom position means off), release curve toggle `ONE-POLE | LINEAR dB`, and `LEARN` with its
status text. Unbound knobs follow 5.1 (submit on drag end).

6.6 COMP: `COMP ON`, knobs THRESHOLD, RATIO, KNEE, ATTACK, RELEASE, MAKEUP; when release > 150 ms show
`release > 150 ms: not NAM-trainable` in `warning()`.

6.7 Refresh: the editor's existing timer calls `panel->refresh()` when visible; the panel rebuilds its
cards only when `presetGeneration()` or the preset changed (compare `Preset ==`), so refresh is cheap.

## 7. State and automation

Plugin state is the preset (unchanged). Everything the panel edits lives in the preset and round-trips
through `getStateInformation` / `setStateInformation`. Transient (never saved): mute/solo, panel
open/tab, the `SinglePlusTwoPedals` choice when the preset reads `Single`, the remembered blend.
APVTS parameters stay exactly the existing 12 (ids never change): blend, path levels, gate threshold,
input/output gain and the six post-EQ slot gains are host-automatable; structural edits are
preset-only. Document in `docs/PLUGIN.md`.

## 8. Acceptance tests

Core (`tests/`): section 2.6.

Headless plugin tests (`plugin/tests/`, in `sawblade_plugin_tests`; new files `test_rig_model.cpp`,
`test_rig_controller.cpp`):
1. **Topology round trip.** Build presets through `RigModel`, serialise with `toJson`, parse back with
   `parsePreset`: `Blend -> Single -> SinglePlusTwoPedals -> Blend` keeps every block of both paths (ids,
   order, params), sets `b.enabled`/`blend` as 4.1, bypasses/unbypasses as 4.1, and `topologyOf` reads
   back each step. Also through the processor: the identity two-path preset switched to `Single` and
   back via `RigController`, state saved and restored at each step, `topologyOf(currentPreset())` as
   expected, exactly one engine build per structural edit.
2. **Slot add/remove/reorder through the loader.** From the identity preset: add an `eq` block with a
   +6 dB peak at 1 kHz to path A -> after `waitForLoader` the processor's output on a 1 kHz tone is +6 dB
   (A only: topology Single); move it before/after the nam block (output unchanged: linear); remove it ->
   identity again. Add a `test.latency` block (the test stub; see processor_harness.h) -> reported latency
   grows by its latency; bypass it -> latency back. `engineBuilds()` grows by exactly one per edit; the
   audio thread (Host harness, processing throughout) allocates nothing and takes no lock.
3. **Live EQ, live block gain, mute.** `applyLiveEdit` moving a post band's freq changes
   `engineParamState().live` without a rebuild (`engineBuilds()` unchanged) and the processed output matches
   a processor loaded with that preset after 25 ms (|diff| <= 1e-5); a path EQ gain edit likewise; a nam block
   input gain edit changes the output level by that gain (identity model); `setMonitor(true,false)` ramps A to
   silence; state saved after live edits contains them; after a state restore the generation matches and
   `liveParams() == baseline()`.
4. **Align re-measure off-thread.** Preset: A = identity, B = `linear_delay_300.nam`, `align.mode = off`,
   `maxLagMs = 10`. Call `remeasureAlignment()` while the Host keeps processing blocks on another thread
   with the allocation and lock counters on; after `waitForLoader`: `currentPreset().align.mode == Manual`,
   `delaySamplesB == -300`, `invertB == false`, `status().measuredAlign.peakCorrelation > 0.99`, no
   allocations or locks on the audio thread, and a state round trip keeps the manual values.
5. **Automation of blend.** Through the host-facing parameter (`getParameter("blend")->setValueNotifyingHost`),
   sweep 0 -> 1 across blocks while processing A = identity, B = `linear_05_025.nam`: the output moves
   from x to the B response, every block's change is bounded (no jumps > the 20 ms ramp allows),
   `engineBuilds()` unchanged, `getStateInformation` has the final blend (1e-4 grid).
6. **Zero allocation in `processBlock`.** Extend the existing RT test: during 2000 blocks at 48 kHz,
   another thread does structural edits (add/remove a band), live edits at ~200 Hz, mute toggles and a
   re-measure; `allocs == 0 && locks == 0` for the audio thread.
7. **Gate LEARN.** Feed -60 dBFS noise for 1 s with the controller's learn running (drive the timer or
   expose a `finishLearn()` test hook); `gateThreshold` ends at -54 +- 1 dB; with no blocks processed the
   status reads "no input" and the parameter is unchanged.

Editor tests (`plugin/tests/test_editor.cpp`, xvfb):
8. The panel exists, is closed by default, opens from `RIG`, shows the six tabs; every control in the
   panel has a title and a tooltip; the knob rule amended as in section 1.
9. **EQ node drag maps to the band.** Open EQ, target POST, on a preset with `[peak 1 kHz 0 dB q1, highPass 80]`:
   synthesise a drag of node 0 by (+70, -40) px -> the band's `freq` and `gainDb` equal the inverse of the
   documented mapping at the new pixel (relative 1e-6), the `postEq1` parameter equals the new gain, and the
   readout text matches; drag node 1 vertically -> its `q` follows the Q mapping and `freq` is unchanged.
10. Topology buttons and the CAB mode buttons change the preset through the loader (wait, then check
    `currentPreset()`), and the studio notice appears in PER PATH.
11. **Screenshots** (1x, 1280 x 800, under `build-plugin/screenshots/`): `rig_single.png` (CHAIN tab,
    Single), `rig_blend.png` (CHAIN tab, Blend with both lanes populated from the identity preset plus an
    eq block), `rig_eq.png` (EQ tab with three bands of different types), `rig_gate.png` (GATE tab,
    expander mode). Use fixture captures only (nothing from TONE3000 is committed).

Whole suite: full `ctest` in `build-plugin` green (including `plugin: pluginval VST3` at strictness 10)
and the clang `-Werror` build (section 0) warning-free. Paste the counts in the report.

## 9. Rules recap

- No allocation, locks, I/O, exceptions or logging on the audio thread; the alloc/lock harness covers
  every new audio-thread path (snapshot read, `setParams` with extras, meter push).
- Structural changes only through `EngineLoader` + the existing 30 ms cross-fade; continuous changes
  only through `Chain::setLiveParams` ramps.
- Keep the skin: renders, filmstrip knobs, palette. No new assets needed.
- Determinism: goldens bit-identical; ramps block-size independent.
- Nothing hard-wired to a style: labels say SAW / BODY (path roles from the schema), never a band or a
  pedal name; `nam` block titles come from the capture metadata.
- No scope creep: propose extras (drag-reorder, typed value boxes, spectrum behind the EQ, CPU meter) in
  the report instead. Do not touch `match/`, `cli/`, `bindings/`.
- Commit on the current branch, small commits (core, processor/loader, rig model + controller, UI,
  tests + docs), each ending with the trailer the lead gives you. Do not push.

## 10. Report

Write your implementer report back to the lead (files changed, design notes, decisions, the pasted test
summary with counts, pluginval result, clang result, screenshot paths). The lead writes
`docs/specs/phase10_rig_editor_REPORT.md` after the reviewer's ACCEPT.
