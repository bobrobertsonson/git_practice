# Phase 7b: the chainsaw pedal — deep `pedal.hm` v2, the `pedal.muff` circuit, pedal face UI, preset bank

## Why
User request (verbatim): *"I want a very tweakable and customizable pedal not a NAM capture. It
should have at least 10 starting presets and follow the full gambit of chainsaw tones."*
Scope change relayed by the main lead while this spec was being written: *"It doesn't have to be
HM-2 exclusive. Other pedals can be used. Swollen pickle etc."*

So the user-facing pedal is **one pedal** (the STOCKHOLM SYNDROME face) with a **CIRCUIT**
switch. Each circuit is its own block type in the core, sharing `Oversampler4x` and
`AdaaClipper`:

| circuit | block type | status |
|---|---|---|
| CHAINSAW | `pedal.hm` (phase 7, deepened to model version 2 here) | this phase |
| PICKLE | `pedal.muff` — Big-Muff-family topology with the Swollen-Pickle extras (scoop, crunch, voice) | this phase |
| ZONE | `pedal.mz` — parametric mids + high gain, scoop | **7c** (design the registry/face mapping so it drops in) |
| RAT | `pedal.rat` — op-amp clipper + filter | **7c** |

Phase 7 gave `pedal.hm` four stock knobs with every voicing constant hard-wired. This phase
opens it up, adds the pickle circuit, gives both live knobs in the plugin on the STOCKHOLM
SYNDROME render, and ships a bank of fifteen starting presets that span the chainsaw gamut:
Swedish death metal, crust, hardcore, grind, modern tight, bass, texture, and three non-HM-2
chainsaw tones built on the pickle circuit. Nothing is hard-wired to one band: the presets are
starting points and the controls cover the space.

Base: branch `claude/sawblade-p7-modeled-pedals` (`453c7af`). Working branch:
`claude/sawblade-p7b-chainsaw-pedal`. Spec of the stock HM model: `docs/specs/phase7_modeled_pedals.md`.

## Roles and task split
- `dsp-engineer`, **task A (core)**: §1–§4 (both circuits, the generic live-parameter path, the
  `pedal_fr` tool, the preset bank, the docs of §6 that concern the core and the presets) with
  the core acceptance tests 1–11. `plugin/` untouched.
- `reviewer` audits A.
- `dsp-engineer`, **task B (plugin)**: §5 (parameters, face, drawer, circuit switch) with tests
  12–15, plus the `docs/PLUGIN.md` paragraph.
- `reviewer` audits B. The lead accepts after both `ACCEPT`s, renders the report plots from
  the CSVs and publishes the Artifact.

## Hard rules (unchanged)
4x oversampled nonlinearities (`Oversampler4x`), ADAA2 clippers, no allocation / locks / I/O /
exceptions in `process()` and in the live-parameter path, deterministic and block-size
independent for static parameters, latency reported exactly. `-Wall -Wextra -Wpedantic -Werror`.
Existing goldens bit-identical. No trademarks in UI names (CHAINSAW, PICKLE, ZONE, RAT are the
generic circuit names; "Swedish chainsaw distortion", "pickle fuzz" the long descriptors).

---

## 1. `pedal.hm` model version 2 (CHAINSAW circuit)

### 1.1 Parameters
`HmParams` grows to the table below. JSON keys are the `key` column. `modelVersion: 2` presets
may set any key; `modelVersion: 1` presets may set only the four v1 keys (anything else is a
`PresetError`, as today). **A v1 preset maps onto the v2 defaults and must render
bit-identically to the phase 7 implementation** (test 1). `toJson()` always writes
`modelVersion: 2` and every key; enums are written as their strings.

| key | type / range | default (= stock) | meaning |
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

### 1.3 Clipper shapes (shared by both circuits)
Generalise `SoftClipShape` to an odd-polynomial order `m ∈ {1, 2}`:

`c(u) = u − u^(2m+1) / ((2m+1)·k^(2m))` for |u| < k, `±2m·k/(2m+1)` beyond; per-sign knees
`k+`, `k−` as today. `m = 1` is the existing cubic. `m = 2` (quintic) has a harder knee: slope
`1 − (u/k)^4`. Provide closed-form `F1`/`F2` for both orders, continuous at 0 and ±k (extend the
existing continuity/derivative test to `m = 2`).

Put the clip-type table in `pedal_common.h` (`enum class ClipType { Silicon, Led, Asymmetric,
Soft }`, `struct ClipShapeSpec { double kPos, kNeg; int order; }`, `clipShapeSpec(ClipType)`,
and the string names) so both circuits and the plugin share it:

| `clip` | k+ | k− (before bias) | m | character |
|---|---|---|---|---|
| `silicon` | 0.5 | 0.5 | 1 | stock pair |
| `led` | 1.4 | 1.4 | 2 | louder (saturates at 4k/5 = 1.12 vs 0.33), harder knee |
| `asymmetric` | 0.5 | 0.3 | 1 | even harmonics |
| `soft` | 0.3 | 0.3 | 1 | earlier, rounder saturation (germanium-like), quieter |

`bias` multiplies k− of both stages by `1 − 0.5·bias/10` (bias 10 → k− halved).

### 1.4 Clean mix (both circuits)
A base-rate delay line of exactly `latencySamples()` samples (sized in `prepare()`, fixed
afterwards) carries the dry input. Latency is unchanged (50 samples); test 7 proves it for every
mode and clip type. When `m == 1` the dry branch is skipped entirely so a v1 preset renders
bit-identically (adding `0·dry` would flip signed zeros).

### 1.5 Modes
- `stock`: the phase 7 model.
- `custom`: extended range like the modern reissue's second mode: `s_low = 3.6` (low gyrator up
  to +24 dB) and `s1 = 4.6` (stage 1 up to 52 dB). Everything else stock.
- `modded`: brighter, nastier top: interstage LPF 9 kHz and post-clip LPF 9 kHz. Gains stock.

### 1.6 Latency
Unchanged: 50 samples at every rate. The dry delay equals it. IIR group delay still not counted.

---

## 2. `pedal.muff` model version 1 (PICKLE circuit)
Long name "pickle fuzz". Big-Muff-family topology (input stage → two cascaded diode-clipping
gain stages → passive tone stack → recovery stage → volume) with the Swollen-Pickle-style
extras: SCOOP (mid-notch depth), CRUNCH (clipping compression), VOICE (mid shift). Simplified
to the same building blocks as `pedal.hm`. `namTrainable = true`. Latency 50 samples (same
oversampler round trip + two ADAA2 stages + padding), dry delay as in §1.4.

### 2.1 Parameters (`MuffParams`, `modelVersion: 1`)
| key | type / range | default | meaning |
|---|---|---|---|
| `volume` | 0–10 | 5 | output level, `3·volume − 24` dB (wet only) |
| `sustain` | 0–10 | 5 | drive (stage-A gain) |
| `tone` | 0–10 | 5 | tone-stack blend, dark → bright (FILTER on the pedal) |
| `scoop` | 0–10 | 3 | extra mid notch at the stack centre: `−1.6·scoop` dB (0 = stock stack only) |
| `crunch` | 0–10 | 5 | clipping compression: both knees scaled by `1.2 − 0.08·crunch` (1.2 … 0.4) |
| `voice` | 0–10 | 5 | mid shift: stack centre `c = 860 · 2^((voice − 5)/5)` Hz (430 Hz … 1.72 kHz) |
| `tightness` | 0–10 | 0 | input HPF `20 · 10^(tightness/10)` Hz, dry tapped before it |
| `mix` | 0–100 | 100 | wet proportion |
| `clip` | enum as §1.3 | `silicon` | stage A clipper (and B while `clip2 = follow`) |
| `clip2` | `follow` \| enum | `follow` | stage B clipper |
| `stackRatio` | 2.0–8.0 | 4.4 | tone-stack corner ratio `fH / fL` (scoop width): `fL = c/√ratio`, `fH = c·√ratio` (stock 408 Hz / 1.8 kHz) |
| `rolloffHz` | 4000–12000 Hz | 10000 | output low-pass corner |
| `gain2Db` | −12…+12 dB | 0 | trim on the stage-B gain (stock +18 dB) |
| `bias` | 0–10 | 0 | asymmetry as §1.3 |

Same validation rules as the HM block. `toJson()` writes `modelVersion: 1` and every key.

### 2.2 Signal flow
| # | Stage | Rate | design |
|---|---|---|---|
| 1 | Input | fs | 1st-order HPF at the `tightness` frequency; dry tap before it |
| 2 | Upsample | → OS | |
| 3 | Stage A pre | OS | 1st-order HPF 80 Hz, 1st-order LPF 4.5 kHz (feedback cap) |
| 4 | Gain A | OS | `GA dB = 6 + 3·sustain` (6–36 dB) |
| 5 | Clip A | OS | `AdaaClipper`, shape from `clip` with knees × crunch factor, `bias` on k− |
| 6 | Stage B pre | OS | 1st-order HPF 30 Hz, 1st-order LPF 4.5 kHz; gain `GB dB = 18 + gain2Db` |
| 7 | Clip B | OS | `AdaaClipper`, shape from `clip2` (`follow` = `clip`) × crunch, `bias` |
| 8 | Post-clip LPF | OS | 2nd-order Butterworth 8 kHz |
| 9 | Downsample | → fs | |
| 10 | Tone stack | fs | `y = (1 − t)·LPF1(fL)(x) + t·HPF1(fH)(x)`, `t = tone/10`; then a peak EQ at `c`, Q 0.8, gain `−1.6·scoop` dB (RBJ) |
| 11 | Recovery / roll-off | fs | 1st-order LPF `rolloffHz`; recovery gain +6 dB fixed |
| 12 | Volume + mix | fs | as §1.4 with `volume` |

Lead's analytic check of the stock stack (t = 0.5, fL 408, fH 1800): −6 dB at the flanks,
about −14.6 dB at 860 Hz, i.e. an 8–9 dB scoop; `scoop` deepens it by up to 16 dB more.

---

## 3. Live parameters (generic, core)
The plugin needs to move every knob without rebuilding the Chain. Add a generic, type-agnostic
live-parameter path:

- `processor.h`: `virtual void setLiveParams(const float* values, int count) noexcept {}` on
  `Processor` (default: ignore). RT-safe contract: no allocation, locks, I/O, exceptions.
- `block_registry.h`: `struct LiveParamDesc { std::string key, name; double min, max, def;
  std::vector<std::string> choices; /* empty = continuous; else value is a choice index */ };`
  and `std::vector<LiveParamDesc> liveParams;` on `BlockType` (index order = the `values` index
  order of `setLiveParams`). `nam`, `eq` and `pedal.ts` leave it empty (`pedal.ts` is **not**
  made live in this phase).
- `pedal_params.h`: `enum HmLive : int { kHmLevel, kHmLow, kHmHigh, kHmDistortion, kHmTightness,
  kHmMix, kHmMode, kHmClip, kHmClip2, kHmLowFreq, kHmLowQ, kHmHighFreq, kHmHighSpread,
  kHmPresenceFreq, kHmPresenceDb, kHmRolloffHz, kHmGain1Db, kHmGain2Db, kHmBias, kHmNumLive };`
  and `enum MuffLive : int { kMuffVolume, kMuffSustain, kMuffTone, kMuffScoop, kMuffCrunch,
  kMuffVoice, kMuffTightness, kMuffMix, kMuffClip, kMuffClip2, kMuffStackRatio, kMuffRolloffHz,
  kMuffGain2Db, kMuffBias, kMuffNumLive };` with `hmLiveParamDescs()` / `muffLiveParamDescs()`
  in the same order and the pure converters `hmParamsFromLive(const float*, int)`,
  `hmLiveFromParams(const HmParams&, float*)` (same for muff), used by core, plugin and tests.
- `chain.h`: `void setBlockLiveParams(int path /*0 = a, 1 = b*/, int blockIndex, const float* v,
  int n) noexcept;` forwards to that block's processor (bounds-checked; bypass is still handled
  in `process()`).
- `HmPedal::setLiveParams` / `MuffPedal::setLiveParams`: store the target params and mark dirty.
  At the start of the next `process()`:
  - gains (stage gains, `levelGain·m`, `1 − m`) ramp linearly over `kRampMs = 20` ms (stage
    gains ramp per oversampled sample);
  - filter coefficients are redesigned once, at the block start, when any of their parameters
    changed (no ramp; document the possible small step);
  - enums (`mode`, `clip`, `clip2`) apply immediately.
  - When no live change has arrived the code path multiplies by the same constants as the static
    build: static renders stay bit-identical to phase 7 for v1 presets (test 1) and across
    block sizes (test 9).

---

## 4. Preset bank: `presets/modeled/chainsaw/*.json` (core task A)
Fifteen full presets. Every one renders in this container from repo files only: path a = the
circuit block(s), path b disabled, `blend: 0`, `align: off`, shared cab = the repo identity IR
(`../../../tests/fixtures/ir/impulse.wav`), no TONE3000 captures. Where the sound needs an amp,
the `notes` field names the recommended TONE3000 amp and cab from `presets/CAPTURE_SHORTLIST.md`
(tone ids) and the user adds the `nam` amp block on the main machine. Band names may appear in
`notes` only, never in `name`. Each `notes` starts with the sound it chases.

Values are the lead's starting hypotheses. The implementer may move `level` / `volume` (only) so
that the fixture render peaks between −6 and −0.5 dBFS, and reports every change. Write every key
explicitly in the files (the schema allows omission; the bank should be self-documenting).

CHAINSAW circuit (`pedal.hm`, `modelVersion: 2`):

| file | name | low / high / dist / level | tight | mix | mode / clip | deep | notes (sound; amp suggestion) |
|---|---|---|---|---|---|---|---|
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

PICKLE circuit (`pedal.muff`) — the non-HM-2 chainsaw tones:

| file | name | sustain / tone / scoop / volume | crunch / voice | tight | mix | clip | deep | notes |
|---|---|---|---|---|---|---|---|---|
| `pickle_chainsaw.json` | Pickle Chainsaw | 10 / 7 / 8 / 4 | 7 / 6 | 3 | 100 | silicon | — | a scooped, saturated fuzz chainsaw with no HM-2 in it (Swedish-style buzz from a muff-family fuzz); into a cranked British-style amp (JCM800 86089) with a V30 cab 45023 |
| `pickle_doom_saw.json` | Pickle Doom Saw | 8 / 3 / 4 / 5 | 4 / 3 | 0 | 100 | soft | stackRatio 6, rolloffHz 7000 | low-voiced doom/sludge saw: mids shifted down, dark top, soft clip (Electric Wizard, Conan); low-gain amp |
| `pickle_into_saw.json` | Pickle Into Saw | 6 / 6 / 2 / 6 then HM | 5 / 5 | 2 | 100 | silicon | second block: `pedal.hm` low 8, high 9, dist 6, level 3, tight 0 | two circuits chained: a mild fuzz pushing the chainsaw for a thicker, more compressed buzz |

7c placeholders (no files): a rat-style grind tone and a zone-style scoop are listed as TODO
rows in `presets/README.md`, to be added when `pedal.rat` / `pedal.mz` land.

Add the bank to `presets/README.md` (table, the amp-suggestion rule, the 7c TODOs).

---

## 5. Plugin (task B)

### 5.1 Parameters (`plugin/src/pedals/CircuitParams.{h,cpp}`, JUCE-free)
Each circuit gets its own host-parameter set (ranges and units differ per circuit; inert when
the preset has no block of that type, exactly like the post-EQ slots), plus one circuit switch:

- `ParamIndex` in `PresetMapping.h`: after the post-EQ slots add `kSawCircuit` (choice:
  `chainsaw`, `pickle` — 7c appends `zone`, `rat`), then `kHmFirst … kHmFirst + kHmNumLive − 1`
  and `kMuffFirst … kMuffFirst + kMuffNumLive − 1`; `kNumParams` follows. `ParamSpec` gains
  `std::vector<std::string> choices` (empty = continuous). Ids: `sawCircuit`; `hmLevel, hmLow,
  hmHigh, hmDistortion, hmTightness, hmMix, hmMode, hmClip, hmClip2, hmLowFreq, hmLowQ,
  hmHighFreq, hmHighSpread, hmPresenceFreq, hmPresenceDb, hmRolloffHz, hmGain1Db, hmGain2Db,
  hmBias`; `muffVolume, muffSustain, muffTone, muffScoop, muffCrunch, muffVoice, muffTightness,
  muffMix, muffClip, muffClip2, muffStackRatio, muffRolloffHz, muffGain2Db, muffBias`. Display
  names "Chainsaw …" / "Pickle …"; units `Hz`, `dB`, `%` where they apply; ranges and defaults
  from the core descriptor lists (single source of truth).
- `createLayout()`: a spec with `choices` becomes an `AudioParameterChoice`, else
  `AudioParameterFloat`. `readParams` / `writeParams` work unchanged (a choice's raw value is its
  index).
- **Slot rule**: the parameters control the **first circuit block** of the preset (path a in
  block order, then path b; a circuit block is any type in the circuit table). `CircuitParams`
  provides `std::optional<CircuitSlot{int path; int block; Circuit circuit;}>
  findCircuitBlock(const Preset&)`, `void circuitParamsFromPreset(const Preset&, ParamValues&)`
  (sets `sawCircuit` from the block type, the block's values into its set, defaults elsewhere)
  and `void applyCircuitParams(Preset&, const ParamValues&)` (writes the active set's values into
  the block; never changes the block type). `paramsFromPreset` / `applyParams` call these two.
  A preset chaining two circuits (e.g. `pickle_into_saw.json`) exposes the first; the second
  stays preset-static in this phase (documented).
- **Circuit switch**: `sawCircuit` is the one parameter whose change rebuilds. The processor
  listens to it (APVTS listener, message/any non-audio thread); when its value names a type
  different from the current circuit block's, it builds a new preset with that block replaced by
  the other circuit's block — carrying over level ↔ volume, mix, tightness and clip, the rest at
  defaults — and `loadPreset()`s it (normal off-thread build, cross-fade). While the preset has
  no circuit block the switch is inert. The host state is the preset, so saved state always
  carries the block type that is playing.
- `Engine`: at `build()` record `findCircuitBlock(preset)`; in `setParams()` extract the active
  set's values and, if any differs from the last applied set, call
  `chain_->setBlockLiveParams(...)` (RT-safe; a cached `std::array<float, kMaxLive>`).

### 5.2 Circuit face table (`plugin/src/pedals/CircuitFaces.{h,cpp}`)
One table row per circuit drives both the face and the drawer, so 7c adds a row:

```cpp
struct FaceKnob { int param; const char* label; };          // -1 = empty position
struct FaceSwitchSpec { int param; const char* label; /* FOCUS: */ double wideValue, narrowValue, threshold; };
struct CircuitFace {
  const char* blockType; const char* oledName;               // "pedal.hm", "CHAINSAW"
  std::array<FaceKnob, 6> knobs;                             // LOW HIGH DIST | IN GAIN OUT MIX positions
  int clipParam; FaceSwitchSpec focus;                       // CLIP switch, FOCUS switch
  std::vector<FaceKnob> drawerKnobs;                         // up to 10, two rows of five
  std::vector<FaceKnob> drawerSwitches;                      // PedalSwitch rows (choice params)
};
```

| circuit | face knobs (positions 1–6) | CLIP | FOCUS | drawer knobs | drawer switches |
|---|---|---|---|---|---|
| CHAINSAW | LOW `hmLow`, HIGH `hmHigh`, DIST `hmDistortion`, TIGHT `hmTightness`, OUT `hmLevel`, MIX `hmMix` | `hmClip` | `hmLowQ`: WIDE 0.8 / NARROW 1.6, threshold 1.2 | LOW HZ `hmLowFreq`, LOW Q `hmLowQ`, HIGH HZ `hmHighFreq`, SPREAD `hmHighSpread`, PRES HZ `hmPresenceFreq`, PRES dB `hmPresenceDb`, ROLL-OFF `hmRolloffHz`, STAGE 1 `hmGain1Db`, STAGE 2 `hmGain2Db`, BIAS `hmBias` | MODE `hmMode`, CLIP 2 `hmClip2` |
| PICKLE | SUSTAIN `muffSustain`, TONE `muffTone`, SCOOP `muffScoop`, TIGHT `muffTightness`, OUT `muffVolume`, MIX `muffMix` | `muffClip` | `muffStackRatio`: WIDE 4.4 / NARROW 2.5, threshold 3.4 | CRUNCH `muffCrunch`, VOICE `muffVoice`, WIDTH `muffStackRatio`, ROLL-OFF `muffRolloffHz`, STAGE 2 `muffGain2Db`, BIAS `muffBias` | CLIP 2 `muffClip2` |

### 5.3 Pedal face (`plugin/src/pedals/PedalFace.{h,cpp}`)
A transparent `juce::Component` laid exactly over the SAW pedal `RigPiece` (render
`plugin/assets/pedal_saw.png`, the STOCKHOLM SYNDROME ortho view). It does **not** intercept
clicks on its background (`setInterceptsMouseClicks(false, true)`), so single clicks still select
the piece and double clicks reach it. Positions are in pedal millimetres, converted with the
same mm → px mapping RigView uses for the footswitches (`kSawPedalMmHeight = 205` over the
image height; +y is up): knob centres (−36, 26), (0, 26), (36, 26), (−36, −15), (0, −15),
(36, −15), 40 mm sprite frame; switches at (−36, −47.5) CIRCUIT, (0, −47.5) CLIP, (36, −47.5)
FOCUS, 11 mm; label chips 5.5 mm tall at y − 18 (knobs) and y = −58.5 (switches).

- The face owns **one set of controls per circuit row** (6 `FilmstripKnob`s, Kind::Pedal, a
  CLIP `PedalSwitch` and a FOCUS `PedalSwitch`), shows the active circuit's set and hides the
  others; plus one CIRCUIT `PedalSwitch` bound to `sawCircuit`. Every parameter therefore has
  exactly one face control (the drawer adds its own; the FOCUS parameter has one switch here and
  one knob in the drawer).
- Label chips: dark rounded chips over the baked captions with the row's labels (the render's
  baked LOW/HIGH/DIST/IN GAIN/OUT/MIX and SLOT/SIZE/NORM cannot be re-rendered in this
  container).
- `PedalSwitch` (`plugin/src/pedals/PedalSwitch.{h,cpp}`): a small code-drawn lever bound through
  `juce::ParameterAttachment`; click cycles to the next position, mouse wheel steps, tooltip and
  title carry the control name and current value text; value text under the lever. A FOCUS-style
  switch binds a continuous parameter with two values and a threshold.
- The OLED area (centre (0, 56) mm, 68 × 25 mm) is overlaid with a dark panel: line 1 the preset
  name (upper case, truncated), line 2 `<circuit> · <clip> · <focus>` (e.g. `CHAINSAW · SI ·
  WIDE`). Refreshed on the editor's existing tick.
- Visibility: shown only when `findCircuitBlock(preset)` has a value; otherwise hidden (the
  baked render stays).
- Accessibility: every control has a title and a tooltip.

### 5.4 Advanced drawer (`plugin/src/pedals/AdvancedDrawer.{h,cpp}`)
Design note: the pedal sits on the bottom edge of the rig (30 px below it), so a drawer *below*
it cannot fit. It slides out from behind the pedal **to the right**, over the pedalboard, aligned
with the pedal's vertical span: bounds (rig px) `x = pedal.right + 12 … 916`, `y = pedal.top …
pedal.bottom` (about 574 × 270). `juce::ComponentAnimator`, 180 ms, from the pedal's right edge;
closing reverses. UI state only (never saved).

Contents, from the active circuit's row: title `ADVANCED · <circuit name>`, a close `×`, up to
two rows of five `FilmstripKnob`s (Kind::Pedal, 64 px) with labels and value readouts, then one
row of `PedalSwitch`es (MODE, CLIP 2 …). The drawer, like the face, owns a set per circuit and
shows the active one.

Opening: double-click on the SAW pedal (`RigPiece` gains `std::function<void(Piece)>
onDoubleClick` via `mouseDoubleClick`, the one edit to `RigView`). Closing: double-click again,
the `×`, or Escape. Child of the editor `Content`, above the rig, below the play-along panel.

### 5.5 Editor integration (minimal shared edits)
`PluginEditor.cpp`: create `PedalFace` and `AdvancedDrawer`, position them from
`rig_.piece(Piece::SawPedal).getBounds()` translated by the rig's position in `resized()`, wire
`onDoubleClick`, refresh visibility / active circuit / OLED in `refresh()`. Keep the inspector as
it is. `plugin/CMakeLists.txt`: add the `pedals/*.cpp` files to `SAWBLADE_PLUGIN_SOURCES`.
`test_editor.cpp`: the "every parameter has exactly one knob" test counts `FilmstripKnob`s
**plus** `PedalSwitch`es per parameter id (FOCUS parameters: one knob and one switch).

### 5.6 Screenshots (editor tests; the lead publishes them)
Load `presets/modeled/chainsaw/classic_buzzsaw.json` and then `pickle_chainsaw.json` in the
editor test rig (both render from repo files) and save to `${CMAKE_BINARY_DIR}/screenshots/`:
`sawblade_face_chainsaw_2x.png`, `sawblade_drawer_chainsaw_2x.png`,
`sawblade_face_pickle_2x.png`, `sawblade_drawer_pickle_2x.png` (full editor), and 3x crops
`face_chainsaw_crop.png`, `drawer_chainsaw_crop.png`, `face_pickle_crop.png`,
`drawer_pickle_crop.png` (saw pedal, and pedal + drawer).

---

## 6. Measurement tool and docs

### 6.1 `tests/tools/pedal_fr.cpp` (task A)
- `--param key=value`: a value that does not parse as a number is passed as a string (enums).
- `--preset FILE [--block ID]`: take the block's params from a preset file (first circuit block
  when `--block` is absent); `--param` overrides on top. `--type` then comes from the block.
- `--thd`: instead of the FR, write `input_dbfs,thd_db,h2_dbc` for a 500 Hz sine swept from
  −40 to 0 dBFS in 2 dB steps (1 s each, 0.25 s discarded), harmonics 2–20 as in test 2 of
  phase 7.
- The lead runs: FR of every preset's first block; THD vs input for the four clip types on each
  circuit (dist / sustain 5, others default); FR of the three HM modes at low = high = dist = 10;
  FR of the pickle at tone 0 / 5 / 10 and scoop 0 / 10.

### 6.2 Docs
- `docs/PRESET_SCHEMA.md` (task A): PedalHm section rewritten for v2 (table of §1.1, enums, v1
  compatibility rule, live-parameter note); new PedalMuff section (§2.1); block-types table rows
  for both; the bank listed.
- `docs/PEDALS.md` (task A, new, short): the one pedal and its circuits; every control of each
  circuit in player terms (what it does to the sound, where stock is, when to reach for it); the
  HM modes; the four clips; the face / drawer mapping (§5.2); the fifteen presets with one line
  each; the 7c circuits as "coming".
- `docs/PLUGIN.md` (task B): one paragraph on live block parameters, the circuit switch
  (rebuild via the loader) and the pedal face.

---

## Acceptance (Catch2; fs = 48 kHz unless stated)

### Core (task A)
1. **v1 → v2 compatibility.** Render `presets/modeled/hm_chainsaw.json` and `ts_boost.json`
   (both `modelVersion: 1`) and compare bit-for-bit against renders made with the phase 7 build:
   the implementer records those renders once as goldens `tests/golden/hm_chainsaw_v1.wav`,
   `ts_boost_v1.wav` **before touching the model** (the baseline `build/` tree at `c4b3905`
   already holds that `tonerender`), fixture DI, 48 kHz, block 512, and adds the comparison test
   (tolerance 0). Also: a v1 block JSON parses to `HmParams` equal to a v2 block with all
   defaults; `toJson()` of either is the full v2 object; parse → `toJson` → parse is equal for a
   v2 HM block and a muff block with non-default values of every key, including every enum
   value; `modelVersion: 2` on `pedal.muff` and `3` on `pedal.hm` are `PresetError`s.
2. **Each HM parameter moves the output the expected way** (small-signal FR at −90 dBFS,
   relative to |H(400)| unless stated, other params default, low = high = 5):
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
3. **Each muff parameter moves the output the expected way** (same method, relative to |H(5 kHz)|
   where a flank is needed, others default):
   - `tone` 0 vs 10: (|H(5 kHz)| − |H(100 Hz)|) rises by ≥ 20 dB.
   - stock stack (tone 5, scoop 0): a minimum in 600–1200 Hz at least 6 dB below both |H(100)|
     and |H(5 kHz)|.
   - `scoop` 0 vs 10 (tone 5): |H(860 Hz)| drops by ≥ 8 dB more; `voice` 0 vs 10 (tone 5,
     scoop 10): the notch minimum lies within ±30 % of 430 Hz and of 1720 Hz respectively.
   - `stackRatio` 2 vs 8 (tone 5, scoop 0): the −3 dB width of the notch (relative to its
     flanks) is ≥ 2× wider at 8.
   - `crunch` 0 vs 10 (500 Hz, −20 dBFS, sustain 5): THD rises ≥ 3 dB; `sustain` 0 → 10 THD
     monotonic with span ≥ 6 dB at −40 dBFS; `gain2Db` −12 vs +12: THD ≥ +6 dB.
   - `tightness`, `mix`, `volume`, `rolloffHz`, `bias`: the same criteria as the HM ones
     (`rolloffHz` 4000 vs 12000: |H(8 kHz)| ≥ +12 dB; `volume` 0 vs 10: +30 ± 0.2 dB).
4. **Clip types differ** (each circuit; 500 Hz sine, drive knob 5, −20 dBFS): THD(led) <
   THD(silicon) < THD(soft) by ≥ 3 dB each step; output RMS led > silicon > soft; asymmetric has
   H2 > −40 dBc while silicon has H2 < −70 dBc. `clip2 = follow` equals `clip2 = <same as
   clip>` bit-for-bit; `clip2 = led` with `clip = silicon` differs from both. The `m = 2` shape
   passes the extended continuity test (F1' = c, F2' = F1 by central differences within 1e-6
   relative; continuity at ±k and 0 within 1e-12).
5. **HM modes.** low = high = dist = 10: custom vs stock |H(100)| ≥ +4 dB and THD at −40 dBFS
   ≥ +2 dB; modded vs stock |H(7 kHz)| − |H(400)| ≥ +6 dB higher; custom at `low = 0,
   distortion = 0` equals stock at the same knobs bit-for-bit (the slopes multiply zero).
6. **Aliasing floor** ≤ −80 dB (phase 7 recipe, 5 kHz −6 dBFS) at maximum gain for each clip
   type on each circuit (HM stock dist 10; muff sustain 10, crunch 10) and for each HM mode
   (silicon, dist 10, `gain1Db = gain2Db = +12`).
7. **THD vs input is monotonic** per clip type and circuit (the `--thd` sweep, −40…0 dBFS: each
   step ≥ previous − 0.05 dB).
8. **Latency**: `latencySamples() == 50` for every mode / clip / circuit at 44.1/48/96 kHz, and
   measured = reported with the flat-filter hook (phase 7 recipe) for `mix` 100 and for `mix` 0
   (dry path), both circuits.
9. **Zero allocation**: `process()` standalone and in a Chain for both circuits, **and** a loop
   that calls `setLiveParams()` with changing values (every index, enums cycling) between
   `process()` calls, all under the alloc guard.
10. **Block-size independence and determinism** (static params): bit-identical for block sizes
    1, 7, 64, 512, 4096 and across two runs, for one preset per HM mode and clip type and for two
    muff settings.
11. **Preset bank**: every `presets/modeled/chainsaw/*.json` (exactly 15) parses, renders the
    fixture DI at 48 kHz without error, output finite, peak in [−6, −0.5] dBFS, and the LTAS
    sanity holds: band energy 80 Hz–4 kHz is at least 90 % of the total, and the 100–200 Hz and
    1–2 kHz bands are each within 30 dB of the loudest third-octave band. Every `name` is free of
    band names (the test checks: entombed, dismember, gatecreeper, nails, nasum, bloodbath,
    wolfbrigade, disfear, trap them, rotten sound, carnage, nihilist, lik, electric wizard,
    conan), every `notes` is non-empty, at least three presets use `pedal.muff`, and
    `pickle_into_saw.json` has two circuit blocks and renders. Also run `sawblade-tonecheck
    --presets presets/modeled/chainsaw/*.json --di tests/fixtures/di_riff.wav` (install `match`
    with `pip install -e match`) and keep `summary.json` plus the per-preset LTAS JSONs for the
    report; rule pass/fail is informational (the targets describe a full rig with cab), the run
    must complete for all fifteen.

### Plugin (task B)
12. **Headless `sawblade_plugin_tests`**: with `classic_buzzsaw.json` loaded, (a)
    `paramsFromPreset` returns its HM values and `sawCircuit = chainsaw`, `applyParams` writes
    them back; the saved state JSON holds `modelVersion: 2` and the knob values; (b) changing
    `hmDistortion`, `hmMode`, `hmClip` and `hmMix` through the parameters changes the processed
    audio (RMS or spectrum differs), `engineBuilds()` does not change, and processing under moving
    HM parameters passes the existing no-allocation / no-lock RT test; (c) with the Init preset
    (no circuit block) the parameters and the circuit switch are inert and `findCircuitBlock` is
    empty; (d) a `modelVersion: 1` preset loads and its parameters read the v2 defaults; (e) with
    `pickle_chainsaw.json` loaded `sawCircuit = pickle`, the muff parameters hold its values and
    moving `muffScoop` changes the audio without a rebuild; (f) setting `sawCircuit` to `pickle`
    on the chainsaw preset rebuilds exactly once (`engineBuilds()` + 1), the current preset's
    first block is `pedal.muff` with `volume = hmLevel`, `mix`, `tightness`, `clip` carried over,
    the rest default, and the state round-trips; setting it back restores a `pedal.hm` block
    (values default except the carried ones). The "parameter changes never rebuild" test excludes
    `sawCircuit`.
13. **Editor (`sawblade_editor_tests`, xvfb)**: (a) every parameter has exactly one bound
    control (amended test); (b) face knobs and switches round-trip knob → parameter → knob at
    0.8 / 0.25 as the existing test does; the FOCUS switch writes its two values and reads NARROW
    past the threshold; CIRCUIT cycles chainsaw → pickle → chainsaw and the face shows the pickle
    set after the loader settles; (c) face hidden on Init, shown after loading
    `classic_buzzsaw.json`; (d) the drawer is closed by default, opens on a double-click of the
    saw pedal piece, closes on a second double-click and on Escape; its bounds lie inside the rig
    and do not intersect the saw pedal piece; its visible knobs are the active circuit's; (e) OLED
    line 2 shows the circuit / clip / focus text after a parameter change; (f) titles and tooltips
    on every new control; (g) screenshots of §5.6.
14. **pluginval**: `--strictness-level 10 --validate-in-process` on the VST3 passes (binary at
    the path the lead gives; registered through `SAWBLADE_PLUGINVAL_EXECUTABLE`).
15. `-Werror` clean; Release ctest all green; Debug ASan/UBSan ctest for the core tests green.

## Report back (dsp-engineer, per task)
Files changed; design notes (the m = 2 antiderivatives, the live-param path, the muff stack,
the circuit switch, the drawer placement); every level adjustment made to the presets with the
resulting peak; THD and alias tables per clip, mode and circuit; latency per rate; tonecheck
summary; pluginval result; the full ctest summary; commit hashes; decisions/questions for lead.
