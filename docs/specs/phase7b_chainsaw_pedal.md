# Phase 7b: deep chainsaw pedal (`pedal.hm` v2), pedal face UI, preset bank

## Why
User request (verbatim): *"I want a very tweakable and customizable pedal not a NAM capture. It
should have at least 10 starting presets and follow the full gambit of chainsaw tones."*

Phase 7 gave `pedal.hm` four stock knobs with every voicing constant hard-wired. This phase
opens the model up (model version 2), gives it live knobs in the plugin on the STOCKHOLM
SYNDROME render, and ships a bank of twelve starting presets that span the chainsaw gamut:
Swedish death metal, crust, hardcore, grind, modern tight, bass, and texture use. Nothing in it
is hard-wired to one band: the presets are starting points and the controls cover the space.

Base: branch `claude/sawblade-p7-modeled-pedals` (`453c7af`). Working branch:
`claude/sawblade-p7b-chainsaw-pedal`. Spec of the stock model: `docs/specs/phase7_modeled_pedals.md`.

## Roles
- `dsp-engineer` implements everything below (core, plugin, CLI tool, presets, docs, tests).
- `reviewer` audits; the lead accepts after `ACCEPT`.
- The lead renders the report plots from the CSVs and publishes the Artifact.

## Hard rules (unchanged)
4x oversampled nonlinearities (Oversampler4x), ADAA2 clippers, no allocation / locks / I/O /
exceptions in `process()` and in the live-parameter path, deterministic and block-size
independent for static parameters, latency reported exactly. `-Wall -Wextra -Wpedantic -Werror`.
Existing goldens bit-identical. No trademarks in UI names.

---

## 1. `pedal.hm` model version 2 (core)

### 1.1 Parameters
`HmParams` grows to the table below. JSON keys are the `key` column. `modelVersion: 2` presets
may set any key; `modelVersion: 1` presets may set only the four v1 keys (anything else is a
`PresetError`, as today). **A v1 preset maps onto the v2 defaults and must render
bit-identically to the phase 7 implementation** (test 1). `toJson()` always writes
`modelVersion: 2` and every key; enums are written as their strings.

| key | type / range | default (= stock HM-2) | meaning |
|---|---|---|---|
| `level` | 0–10 | 5 | output level, `3·level − 24` dB (wet path only) |
| `low` | 0–10 | 5 | low gyrator gain |
| `high` | 0–10 | 5 | high gyrator gain (both gyrators) |
| `distortion` | 0–10 | 5 | drive (stage-1 gain), see modes |
| `tightness` | 0–10 | 0 | input high-pass before stage 1: `fc = 20 · 10^(tightness/10)` Hz (20–200 Hz, log) |
| `mix` | 0–100 (%) | 100 | wet proportion; see §1.4 |
| `mode` | `stock` \| `custom` \| `modded` | `stock` | §1.5 |
| `clip` | `silicon` \| `led` \| `asymmetric` \| `soft` | `silicon` | clipper of stage 1 (and of stage 2 while `clip2 = follow`) |
| `clip2` | `follow` \| `silicon` \| `led` \| `asymmetric` \| `soft` | `follow` | clipper of stage 2 |
| `lowFreq` | 60–160 Hz | 100 | low gyrator centre |
| `lowQ` | 0.5–2.0 | 0.8 | low gyrator Q ("low focus": wide 0.8, narrow 1.6) |
| `highFreq` | 800–2000 Hz | 1000 | high gyrator A centre |
| `highSpread` | 1.0–2.0 | 1.5 | gyrator B centre = `highFreq · highSpread` (1500 Hz stock) |
| `presenceFreq` | 3000–7000 Hz | 4800 | presence peak centre |
| `presenceDb` | 0–16 dB | 8 | presence peak gain |
| `rolloffHz` | 4000–12000 Hz | 9000 | output low-pass corner |
| `gain1Db` | −12…+12 dB | 0 | trim on the stage-1 gain |
| `gain2Db` | −12…+12 dB | 0 | trim on the stage-2 (interstage) gain, stock +20 dB |
| `bias` | 0–10 | 0 | asymmetry: negative knee `k− ·= 1 − 0.5·bias/10` on both stages |

Numbers out of range, wrong types, unknown enum strings, unknown keys → `PresetError`.
`HmParams` keeps `operator==`.

### 1.2 Signal flow (v2)
Same topology as phase 7 with the constants replaced by the parameters:

| # | Stage | Rate | v2 design |
|---|---|---|---|
| 1 | Input | fs | 1st-order HPF at the `tightness` frequency (20 Hz at default: identical to the v1 coupling cap). The **dry** signal for the mix is tapped *before* this filter. |
| 2 | Upsample | → OS | `Oversampler4x` |
| 3 | Pre-filter | OS | HPF 60 Hz, LPF 8 kHz (unchanged) |
| 4 | Gain 1 | OS | `G1 dB = 6 + s1·distortion + gain1Db`, `s1 = 4` (stock, modded) or `4.6` (custom) |
| 5 | Clip 1 | OS | `AdaaClipper`, shape from `clip` + `bias` (§1.3) |
| 6 | Interstage | OS | LPF `fInter` = 5 kHz (stock, custom) or 9 kHz (modded); gain `G2 dB = 20 + gain2Db` |
| 7 | Clip 2 | OS | `AdaaClipper`, shape from `clip2` (`follow` = `clip`) + `bias` |
| 8 | Post-clip LPF | OS | 4th-order Butterworth, `fPost` = 6.5 kHz (stock, custom) or 9 kHz (modded) |
| 9 | Downsample | → fs | |
| 10 | Colour-mix EQ | fs | low peak `lowFreq`, `lowQ`, gain `−12 + s_low·low` with `s_low = 3` (stock, modded) or `3.6` (custom); high peaks at `highFreq` and `highFreq·highSpread`, Q 1.2, gain `−8 + 2.2·high`; presence peak `presenceFreq`, Q 2.0, `presenceDb`; low-pass `rolloffHz`, Q 0.707 |
| 11 | Level + mix | fs | `out = (1 − m)·dry[n − L] + m·levelGain·wet`, `m = mix/100`, `L = latencySamples()` |

All oversampled-domain corner frequencies stay clamped to `0.45·fsOs` as today.

### 1.3 Clipper shapes
Generalise `SoftClipShape` to an odd-polynomial order `m ∈ {1, 2}`:

`c(u) = u − u^(2m+1) / ((2m+1)·k^(2m))` for |u| < k, `±2m·k/(2m+1)` beyond; per-sign knees
`k+`, `k−` as today. `m = 1` is the existing cubic. `m = 2` (quintic) has a harder knee: slope
`1 − (u/k)^4`. Provide closed-form `F1`/`F2` for both orders, continuous at 0 and ±k (extend the
existing continuity/derivative test to `m = 2`).

| `clip` | k+ | k− (before bias) | m | character |
|---|---|---|---|---|
| `silicon` | 0.5 | 0.5 | 1 | stock pair |
| `led` | 1.4 | 1.4 | 2 | louder (saturates at 4k/5 = 1.12 vs 0.33), harder knee |
| `asymmetric` | 0.5 | 0.3 | 1 | even harmonics |
| `soft` | 0.3 | 0.3 | 1 | earlier, rounder saturation (germanium-like), quieter |

`bias` multiplies k− of both stages by `1 − 0.5·bias/10` (bias 10 → k− halved).

### 1.4 Clean mix
A base-rate delay line of exactly `latencySamples()` samples (sized in `prepare()`, fixed
afterwards) carries the dry input. Latency is unchanged (50 samples); test 7 proves it for every
mode and clip type. When `m == 1` the dry branch is skipped entirely so a v1 preset renders
bit-identically (adding `0·dry` would flip signed zeros).

### 1.5 Modes
- `stock`: the phase 7 model.
- `custom`: extended range like the modern reissue's second mode: `s_low = 3.6` (low gyrator up
  to +24 dB) and `s1 = 4.6` (stage 1 up to 52 dB). Everything else stock.
- `modded`: brighter, nastier top: interstage LPF 9 kHz and post-clip LPF 9 kHz. Gains stock.

### 1.6 Live parameters (generic, core)
The plugin needs to move every knob without rebuilding the Chain. Add a generic, type-agnostic
live-parameter path:

- `processor.h`: `virtual void setLiveParams(const float* values, int count) noexcept {}` on
  `Processor` (default: ignore). RT-safe contract: no allocation, locks, I/O, exceptions.
- `block_registry.h`: `struct LiveParamDesc { std::string key, name; double min, max, def;
  std::vector<std::string> choices; /* empty = continuous; else value is a choice index */ };`
  and `std::vector<LiveParamDesc> liveParams;` on `BlockType` (index order = the `values` index
  order of `setLiveParams`). `nam` and `eq` leave it empty. `pedal.ts` is **not** made live in
  this phase (no scope creep); its list stays empty.
- `pedal_params.h`: `enum HmLive : int { kHmLevel, kHmLow, kHmHigh, kHmDistortion, kHmTightness,
  kHmMix, kHmMode, kHmClip, kHmClip2, kHmLowFreq, kHmLowQ, kHmHighFreq, kHmHighSpread,
  kHmPresenceFreq, kHmPresenceDb, kHmRolloffHz, kHmGain1Db, kHmGain2Db, kHmBias, kHmNumLive };`
  with `std::vector<LiveParamDesc> hmLiveParamDescs()` in the same order, and
  `HmParams hmParamsFromLive(const float* v, int n)` / `void hmLiveFromParams(const HmParams&,
  float* v)` (pure, used by core, plugin and tests).
- `chain.h`: `void setBlockLiveParams(int path /*0 = a, 1 = b*/, int blockIndex, const float* v,
  int n) noexcept;` forwards to that block's processor (bounds-checked; no-op if out of range or
  bypassed-but-present is still forwarded: bypass is handled in `process()`).
- `HmPedal::setLiveParams`: stores the target `HmParams` and marks dirty. At the start of the
  next `process()`:
  - gains (stage 1, stage 2, `levelGain·m`, `1 − m`) ramp linearly over `kRampMs = 20` ms
    (stage gains ramp per oversampled sample);
  - filter coefficients (tightness HPF, interstage/post LPFs, the five EQ bands) are redesigned
    once, at the block start, when any of their parameters changed (no ramp; document the
    possible small step);
  - `mode`, `clip`, `clip2` apply immediately (clipper shape, slopes, filter corners).
  - When no live change has arrived the code path multiplies by the same constants as the static
    build: static renders stay bit-identical to phase 7 for v1 presets (test 1) and across
    block sizes (test 8).

### 1.7 Latency
Unchanged: 50 samples at every rate. The dry delay equals it. IIR group delay still not counted.

---

## 2. Plugin (JUCE)

### 2.1 Parameters (`plugin/src/pedals/HmParamMap.{h,cpp}`, JUCE-free)
Nineteen host parameters, one per `HmLive` index, appended after the post-EQ slots:

- `ParamIndex` in `PresetMapping.h`: add `kHmFirst = kPostEqFirst + kPostEqSlots` and
  `kNumParams = kHmFirst + kHmNumLive`. `ParamSpec` gains `std::vector<std::string> choices`
  (empty = continuous). Ids: `hmLevel, hmLow, hmHigh, hmDistortion, hmTightness, hmMix, hmMode,
  hmClip, hmClip2, hmLowFreq, hmLowQ, hmHighFreq, hmHighSpread, hmPresenceFreq, hmPresenceDb,
  hmRolloffHz, hmGain1Db, hmGain2Db, hmBias`; display names "Saw Pedal …"; units `Hz`, `dB`,
  `%` where they apply; ranges and defaults from `hmLiveParamDescs()` (single source of truth).
- `createLayout()` (PluginProcessor.cpp): a spec with `choices` becomes an
  `AudioParameterChoice`, otherwise `AudioParameterFloat` as today. `readParams`/`writeParams`
  work unchanged (a choice's raw value is its index).
- Slot rule (mirrors the post-EQ slots): the parameters control the **first `pedal.hm` block of
  the preset** (path a in block order, then path b). `HmParamMap` provides
  `std::optional<HmSlot{int path; int block;}> findHmBlock(const Preset&)`,
  `void hmParamsFromPreset(const Preset&, ParamValues&)` (defaults when there is no block) and
  `void applyHmParams(Preset&, const ParamValues&)` (no-op without a block).
  `paramsFromPreset` / `applyParams` call these two (the only edits to `PresetMapping.cpp`
  besides the spec table). Values are clamped and snapped (`snapParam`) like every parameter.
- `Engine`: at `build()` record `findHmBlock(preset)`; in `setParams()` extract the 19 values,
  and if any differs from the last applied set, call `chain_->setBlockLiveParams(...)` with them
  (RT-safe; a cached `std::array<float, kHmNumLive>`). Saved state is the preset with the HM
  values written back (so the HM block in the saved JSON is `modelVersion: 2` with the knob
  values).

Parameter changes must still never rebuild (`engineBuilds()` unchanged, test 11).

### 2.2 Pedal face (`plugin/src/pedals/HmPedalFace.{h,cpp}`)
A transparent `juce::Component` laid exactly over the SAW pedal `RigPiece` (render
`plugin/assets/pedal_saw.png`, the STOCKHOLM SYNDROME ortho view). It does **not** intercept
clicks on its background (`setInterceptsMouseClicks(false, true)`), so single clicks still select
the piece and double clicks reach it. Positions are in pedal millimetres, converted with the
same mm → px mapping RigView uses for the footswitches (`kSawPedalMmHeight = 205` over the
image height; +y is up):

| control | pedal mm (x, y) | binding | widget |
|---|---|---|---|
| LOW | (−36, 26) | `hmLow` | `FilmstripKnob`, Kind::Pedal, frame 40 mm |
| HIGH | (0, 26) | `hmHigh` | " |
| DIST | (36, 26) | `hmDistortion` | " |
| IN GAIN | (−36, −15) | `hmTightness` | " (label chip "TIGHT" over the baked caption) |
| OUT | (0, −15) | `hmLevel` | " |
| MIX | (36, −15) | `hmMix` | " |
| MODE | (−36, −47.5) | `hmMode` | `PedalSwitch` (new, below), 3 positions |
| CLIP | (0, −47.5) | `hmClip` | `PedalSwitch`, 4 positions |
| LOW FOCUS | (36, −47.5) | `hmLowQ` | `PedalSwitch`, 2 positions: writes 0.8 (WIDE) / 1.6 (NARROW); shows NARROW when `hmLowQ ≥ 1.2` |

- `PedalSwitch` (`plugin/src/pedals/PedalSwitch.{h,cpp}`): a small code-drawn lever (no new
  sprite: the render's toggles are baked and we cannot re-render here) sized 11 mm, bound through
  `juce::ParameterAttachment`; click cycles to the next position, mouse wheel steps, tooltip and
  title carry the control name and the current value text; a 5.5 mm-tall label chip under it
  (dark, rounded) with the semantic name (MODE / CLIP / FOCUS) drawn over the baked SLOT / SIZE /
  NORM captions, and the current value in small text right under the lever.
- The OLED area (centre (0, 56) mm, 68 × 25 mm) is overlaid with a dark panel showing two mono
  lines: line 1 the preset name (upper case, truncated), line 2 `<mode> · <clip> · <focus>`
  (e.g. `STOCK · SI · WIDE`). Refreshed from the parameters (a `ParameterAttachment` per
  switch, or on the editor's refresh tick).
- Visibility: the face is shown only when `findHmBlock(status preset)` has a value; otherwise
  hidden (the baked render stays). Refreshed on the editor's existing 4 Hz tick.
- Accessibility: every control has a title and a tooltip (existing test).

### 2.3 Advanced drawer (`plugin/src/pedals/HmAdvancedDrawer.{h,cpp}`)
Design note: the pedal sits on the bottom edge of the rig (there are 30 px below it), so a
drawer *below* it cannot fit. It slides out from behind the pedal **to the right**, over the
pedalboard, aligned with the pedal's vertical span: bounds (rig px) `x = pedal.right + 12 … 916`,
`y = pedal.top … pedal.bottom` (about 574 × 270). Animated with `juce::ComponentAnimator` over
180 ms from the pedal's right edge; closing reverses. UI state only (never saved).

Contents: title `ADVANCED · STOCKHOLM SYNDROME` (label font), a close `×` button, two rows of
five `FilmstripKnob`s (Kind::Pedal, 64 px) with labels and value readouts, and one `PedalSwitch`
for stage 2's clipper:

| row 1 | `hmLowFreq` LOW HZ · `hmLowQ` LOW Q · `hmHighFreq` HIGH HZ · `hmHighSpread` SPREAD · `hmPresenceFreq` PRES HZ |
|---|---|
| row 2 | `hmPresenceDb` PRES dB · `hmRolloffHz` ROLL-OFF · `hmGain1Db` STAGE 1 · `hmGain2Db` STAGE 2 · `hmBias` BIAS |
| row 3 | `hmClip2` CLIP 2 (`PedalSwitch`, 5 positions: FOLLOW / SI / LED / ASYM / SOFT) |

Opening: double-click on the SAW pedal (`RigPiece` gains `std::function<void(Piece)>
onDoubleClick` via `mouseDoubleClick`, the one edit to `RigView`), or the face's OLED. Closing:
double-click again, the `×`, or Escape. The drawer is a child of the editor `Content`, above the
rig and below the play-along panel.

### 2.4 Editor integration (minimal shared edits)
`PluginEditor.cpp`: create `HmPedalFace` and `HmAdvancedDrawer`, position them from
`rig_.piece(Piece::SawPedal).getBounds()` translated by the rig's position in `resized()`, wire
`onDoubleClick`, refresh their visibility/OLED in `refresh()`. Register nothing else; keep the
inspector as it is. `CMakeLists.txt`: add the three `pedals/*.cpp` files to
`SAWBLADE_PLUGIN_SOURCES`. `test_editor.cpp`: the "every parameter has exactly one knob" test
counts `FilmstripKnob`s **plus** `PedalSwitch`es (ids unique, one control per parameter; the
LOW FOCUS switch and the LOW Q knob both bind `hmLowQ`, so for that id exactly one knob and one
switch).

### 2.5 Screenshots (editor tests; the lead publishes them)
Load `presets/modeled/chainsaw/classic_buzzsaw.json` in the editor test rig (it renders from
repo files) and save to `${CMAKE_BINARY_DIR}/screenshots/`:
`sawblade_hm_face_2x.png` (editor, drawer closed), `sawblade_hm_drawer_2x.png` (drawer open),
and 3x crops of the saw pedal region `hm_face_crop.png` and of pedal+drawer `hm_drawer_crop.png`.

---

## 3. Preset bank: `presets/modeled/chainsaw/*.json`
Twelve full presets. Every one renders in this container from repo files only: path a =
`[pedal.hm]`, path b disabled, `blend: 0`, `align: off`, shared cab = the repo identity IR
(`../../../tests/fixtures/ir/impulse.wav`), no TONE3000 captures. Where the sound needs an amp,
the `notes` field names the recommended TONE3000 amp and cab from `presets/CAPTURE_SHORTLIST.md`
(tone ids) and the user adds the `nam` amp block on the main machine. Band names may appear in
`notes` only, never in `name`. Each `notes` starts with the sound it chases.

Values are the lead's starting hypotheses. The implementer may move `level` (only) so that the
fixture render peaks between −6 and −0.5 dBFS, and reports every change. Default = stock value
when a key is omitted below; write every key explicitly in the files (the schema allows
omission, the bank should be self-documenting).

| file | name | low / high / dist / level | tight | mix | mode / clip | deep | notes (sound; amp suggestion) |
|---|---|---|---|---|---|---|---|---|
| `classic_buzzsaw.json` | Classic Buzzsaw | 10 / 10 / 10 / 2 | 0 | 100 | stock / silicon | — | all-tens Swedish buzzsaw (Sunlight-studio era: Entombed, Dismember); into a low-gain British-style amp (JCM800 2203 @sarcobe 86089, V30 cab 45023) |
| `early_raw_demo.json` | Early Raw Demo | 7 / 6 / 6 / 5 | 0 | 100 | stock / silicon | lowFreq 110, presenceDb 6 | early demo-tape rawness, lower gain, more mid (Nihilist / Carnage demos) |
| `dbeat_crust.json` | D-Beat Crust | 5 / 10 / 8 / 5 | 4 | 100 | stock / silicon | presenceFreq 4500, presenceDb 10 | d-beat crust: less low, cutting (Anti Cimex, Disfear, Wolfbrigade); plexi-style amp 76884 |
| `powerviolence_hardcore.json` | Powerviolence Hardcore | 7 / 9 / 10 / 6 | 6 | 100 | stock / silicon | lowQ 1.6, gain2Db +3 | metallic hardcore / powerviolence: tight low, max gain (Nails, Trap Them); cranked British amp (JCM800 86089 at high gain) |
| `grind.json` | Grind | 6 / 9 / 10 / 5 | 7 | 100 | stock / silicon | lowFreq 130, presenceFreq 5500, presenceDb 12, rolloffHz 11000 | grind: less sub, more presence (Nasum, Rotten Sound) |
| `death_n_roll.json` | Death 'n' Roll | 8 / 6 / 5 / 6 | 0 | 100 | stock / silicon | lowFreq 90, lowQ 0.6 | death-'n'-roll: looser, lower gain (Wolverine Blues-era Entombed) |
| `modern_tight_swedish.json` | Modern Tight Swedish | 9 / 9 / 9 / 4 | 5 | 100 | stock / silicon | lowQ 1.8, presenceDb 11, gain1Db −2, gain2Db +2 | modern tight Swedish (Bloodbath, Gatecreeper, LIK); high-gain amp 88689 or 70977 |
| `blend_partner.json` | Blend Partner | 4 / 10 / 9 / 8 | 6 | 100 | stock / silicon | — | saw path for Sawblade's two-path blend: level up, lows down, the body path carries the low end |
| `custom_wall.json` | Custom Wall | 10 / 8 / 10 / 1 | 2 | 100 | custom / silicon | — | custom-mode wall: extended low and gain (reissue second mode) |
| `modded_nasty.json` | Modded Nasty | 7 / 10 / 9 / 3 | 3 | 100 | modded / led | presenceFreq 6000, presenceDb 10, rolloffHz 12000 | bright modded nasty: higher interstage filter, LED clip |
| `bass_chainsaw.json` | Bass Chainsaw | 10 / 7 / 8 / 7 | 0 | 40 | stock / silicon | lowFreq 60, lowQ 0.7 | bass chainsaw: low focus at 60 Hz, 40 % mix keeps the clean DI low end |
| `clean_mix_texture.json` | Clean Mix Texture | 6 / 8 / 7 / 8 | 2 | 30 | stock / soft | — | texture layer at 30 % over a clean or other amp tone |

Add the bank to `presets/README.md` (table + the amp-suggestion rule).

---

## 4. Measurement tool (`tests/tools/pedal_fr.cpp`)
- `--param key=value`: a value that does not parse as a number is passed as a string (enums).
- `--preset FILE [--block ID]`: take the block's params from a preset file (first `pedal.hm`
  when `--block` is absent); `--param` overrides on top.
- `--thd`: instead of the FR, write `input_dbfs,thd_db,h2_dbc` for a 500 Hz sine swept from
  −40 to 0 dBFS in 2 dB steps (1 s each, 0.25 s discarded), harmonics 2–20 as in test 2 of
  phase 7.
- The lead runs: FR for the pedal settings of all twelve presets; THD vs input for the four clip
  types (dist 5, others default); FR for the three modes at low = high = dist = 10.

---

## 5. Docs
- `docs/PRESET_SCHEMA.md`: PedalHm section rewritten for v2 (table of §1.1, enums, v1
  compatibility rule, live-parameter note, the latency statement unchanged), bank listed.
- `docs/PEDALS.md` (new, short): every control of the chainsaw pedal in player terms (what it
  does to the sound, where stock is, when to reach for it), the three modes, the four clips, the
  face/drawer mapping, and the twelve presets with one line each.
- `docs/PLUGIN.md`: one paragraph on the live block parameters and the pedal face.

---

## Acceptance (Catch2; fs = 48 kHz unless stated)
1. **v1 → v2 compatibility.** Render `presets/modeled/hm_chainsaw.json` and `ts_boost.json`
   (both `modelVersion: 1`) and compare bit-for-bit against renders made with the phase 7 build
   (`453c7af`): the implementer records those renders once as goldens
   `tests/golden/hm_chainsaw_v1.wav`, `ts_boost_v1.wav` **before touching the model**, with
   the fixture DI at 48 kHz, block 512, and adds the comparison test (tolerance 0). Also: a
   v1 block JSON parses to `HmParams` equal to a v2 block with all defaults; `toJson()` of either
   is the full v2 object; parse → `toJson` → parse is equal for a v2 block with non-default
   values of every key, including every enum value.
2. **Each parameter moves the output the expected way** (small-signal FR at −90 dBFS, relative
   to |H(400)| unless stated, other params default, low = high = 5):
   - `tightness` 0 → 10: |H(50 Hz)| drops ≥ 8 dB; |H(1 kHz)| changes ≤ 0.5 dB.
   - `lowFreq` 60 vs 160 (low = 10): the 40–200 Hz peak moves from within 55–70 Hz to within
     140–180 Hz.
   - `lowQ` 0.5 vs 2.0 (low = 10): the −3 dB bandwidth of the low peak is ≥ 2× narrower at 2.0.
   - `highFreq` 800 vs 2000 (high = 10, spread 1.0): the 500 Hz–4 kHz peak moves from
     700–950 Hz to 1.7–2.3 kHz.
   - `highSpread` 1.0 vs 2.0 (high = 10, highFreq 1000): at 2.0 |H(2 kHz)| − |H(1 kHz)| is
     ≥ 6 dB higher than at 1.0.
   - `presenceFreq` 3000 vs 7000 (presenceDb 16): local max moves from 2.7–3.4 kHz to
     6.2–7.5 kHz. `presenceDb` 0 vs 16: |H(4.8 kHz)| rises ≥ 10 dB.
   - `rolloffHz` 4000 vs 12000: |H(8 kHz)| rises ≥ 12 dB.
   - `mix` 100 vs 0 (level 8 = 0 dB): at 0 the small-signal response is flat within ±0.1 dB
     from 50 Hz to 15 kHz and the output equals the input delayed by 50 samples (tolerance
     1e-6); at 50 the 400 Hz level lies between the two.
   - `gain1Db`, `gain2Db` −12 vs +12 (500 Hz sine at −40 dBFS, dist 5): THD rises by ≥ 6 dB
     each. `distortion` 0 → 10 still monotonic (phase 7 test) in all three modes.
   - `level` 0 vs 10: output RMS rises by 30 ± 0.2 dB (small signal).
   - `bias` 0 vs 10 (clip silicon, −20 dBFS 500 Hz): H2 rises from < −60 dBc to > −40 dBc.
3. **Clip types differ.** 500 Hz sine, dist 5, −20 dBFS: THD(led) < THD(silicon) < THD(soft)
   by ≥ 3 dB each step; output RMS led > silicon > soft; asymmetric has H2 > −40 dBc while
   silicon has H2 < −70 dBc. `clip2 = follow` equals `clip2 = <same as clip>` bit-for-bit;
   `clip2 = led` with `clip = silicon` differs from both. The `m = 2` shape passes the extended
   continuity test (F1' = c, F2' = F1 by central differences within 1e-6 relative; continuity
   at ±k and 0 within 1e-12).
4. **Modes.** low = high = dist = 10: custom vs stock |H(100)| ≥ +4 dB and THD at −40 dBFS
   ≥ +2 dB; modded vs stock |H(7 kHz)| − |H(400)| ≥ +6 dB higher; at default knobs custom
   equals stock bit-for-bit only when the mode-dependent terms coincide — they do not (slopes
   differ at non-zero knobs), so instead assert custom at `low = 0, distortion = 0` equals
   stock at `low = 0, distortion = 0` bit-for-bit (the slopes multiply zero).
5. **Aliasing floor** ≤ −80 dB (phase 7 recipe, 5 kHz −6 dBFS) at maximum gain for each clip
   type (stock mode, dist 10) and for each mode (silicon, dist 10, `gain1Db = gain2Db = +12`).
6. **THD vs input is monotonic** per clip type (the `--thd` sweep, −40…0 dBFS: each step
   ≥ previous − 0.05 dB).
7. **Latency**: `latencySamples() == 50` for every mode and clip at 44.1/48/96 kHz, and
   measured = reported with the flat-filter hook (phase 7 recipe) for `mix` 100 and for
   `mix` 0 (dry path).
8. **Zero allocation**: `process()` standalone and in a Chain, **and** a loop that calls
   `setLiveParams()` with changing values (every index, both enums cycling) between `process()`
   calls, all under the alloc guard.
9. **Block-size independence and determinism** (static params): bit-identical for block sizes
   1, 7, 64, 512, 4096 and across two runs, for one preset per mode and clip type.
10. **Preset bank**: every `presets/modeled/chainsaw/*.json` (exactly 12) parses, renders the
    fixture DI at 48 kHz without error, output finite, peak in [−6, −0.5] dBFS, and the LTAS
    sanity holds: band energy 80 Hz–4 kHz is at least 90 % of the total, and the 100–200 Hz,
    1–2 kHz bands are each within 30 dB of the loudest third-octave band. Every `name` is free of
    band names (the test checks a small list: entombed, dismember, gatecreeper, nails, nasum,
    bloodbath, wolfbrigade, disfear, trap them, rotten sound, carnage, nihilist, lik), every
    `notes` is non-empty. Also run `sawblade-tonecheck --presets presets/modeled/chainsaw/*.json
    --di tests/fixtures/di_riff.wav` (install `match` with `pip install -e match`) and keep
    `summary.json` plus the per-preset LTAS JSONs for the report; rule pass/fail is informational
    (the targets describe a full rig with cab), the run must complete for all twelve.
11. **Plugin (headless `sawblade_plugin_tests`)**: with `classic_buzzsaw.json` loaded,
    (a) `paramsFromPreset` returns its HM values and `applyParams` writes them back; the saved
    state JSON holds `modelVersion: 2` and the knob values; (b) changing `hmDistortion`,
    `hmMode`, `hmClip` and `hmMix` through the parameters changes the processed audio (RMS or
    spectrum differs), `engineBuilds()` does not change, and the processing under moving HM
    parameters passes the existing no-allocation / no-lock RT test; (c) with the Init preset
    (no HM block) the parameters are inert and `findHmBlock` is empty; (d) a `modelVersion: 1`
    preset loads and its parameters read the v2 defaults.
12. **Editor (`sawblade_editor_tests`, xvfb)**: (a) every parameter has exactly one bound
    control (amended test); (b) face knobs and switches round-trip knob → parameter →
    knob at 0.8 / 0.25 as the existing test does; the LOW FOCUS switch writes 0.8 / 1.6 and
    reads NARROW at ≥ 1.2; MODE cycles stock → custom → modded → stock; (c) face hidden on Init,
    shown after loading `classic_buzzsaw.json`; (d) the drawer is closed by default, opens on a
    double-click of the saw pedal piece, closes on a second double-click and on Escape; its
    bounds lie inside the rig and do not intersect the saw pedal piece; (e) OLED line 2 shows
    the mode/clip/focus text after a parameter change; (f) titles and tooltips on every new
    control (existing accessibility test must still pass); (g) screenshots of §2.5.
13. **pluginval**: `--strictness-level 10 --validate-in-process` on the VST3 passes (binary at
    the path the lead gives; registered through `SAWBLADE_PLUGINVAL_EXECUTABLE`).
14. `-Werror` clean; Release ctest all green; Debug ASan/UBSan ctest for the core tests green.

## Report back (dsp-engineer)
Files changed; design notes (the m = 2 antiderivatives, the live-param path, the drawer
placement); every level adjustment made to the presets with the resulting peak; the THD and
alias tables per clip and mode; latency per rate; tonecheck summary; pluginval result; the full
ctest summary; commit hashes; decisions/questions for lead.
