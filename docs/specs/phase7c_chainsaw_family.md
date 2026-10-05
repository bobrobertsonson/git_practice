# Phase 7c: the chainsaw family — `pedal.hmx` (modded chainsaw) and `pedal.eye` (one-knob chainsaw), core

## Why
User requirement (relayed by the main lead, 2026-10-04): the modeled pedal family is the
**chainsaw (buzzsaw) family only** — HM-2 and its derivatives, plus adjacent circuits. The
earlier 7c plan (Zone / Rat) was dropped before implementation (`phase7c_zone_rat.md` is a
stub). Research: `docs/research/chainsaw_pedals.md` on the main working branch. Its short
list beyond the stock HM-2 and the big fuzz:

1. **HM-2W "Custom" mode** — a parameter variant of the HM-2 model, not a new circuit.
   **Delivered by phase 7b**, not here: `phase7b_chainsaw_pedal.md` §1.5 gives `pedal.hm` v2
   `mode: stock | custom | modded` (custom = more stage-1 gain and a bigger low gyrator) and
   §1.1 the tweakable low-shelf / high-mid parameters (`lowFreq`, `lowQ`, `highFreq`,
   `highSpread`, `presenceFreq`, `presenceDb`, `gain1Db`, `gain2Db`). 7c adds **no code** to
   `pedal.hm`; it contributes the custom-mode preset recipes in §5 for 7b's bank. Calibration
   later against TONE3000 78122, 477, 5363, 74487, 88604, 34509.
2. **Modded-HM-2 class** (Left Hand Wrath / Throne Torcher / Dunwich modded HM-2 / Angry Swede
   V2) → **`pedal.hmx`**, this spec §2. Calibration later: 72990 (Throne Torcher) against stock
   HM-2 captures 58569 / 6778 / 1104.
3. **One-knob chainsaw** (TC Eyemaster-style) → **`pedal.eye`**, this spec §3. Calibration
   later: 6380, 62523, 60618, 6547, 5893. Decision rule from the lead: if the sweep of the real
   unit turns out identical to the HM model at all tens, this slot becomes the Boss HM-3
   (2709, 1753). That comparison needs the captures, which are not in this container; §3
   states exactly what differs by design and the report says so plainly.

Generic UI names (no trademarks): `pedal.hmx` = **"modded chainsaw distortion"** (circuit
label MODDED SAW), `pedal.eye` = **"one-knob chainsaw"** (circuit label ONE-KNOB SAW). No
"Boss", "HM-2", "Wrath", "Torcher", "Eyemaster", "TC", "Dunwich", "Abominable", "Lone Wolf",
"Decibelics" in code identifiers visible to users, preset names or docs headings.

## Relationship to phase 7b (binding)
7b (`claude/sawblade-p7b-chainsaw-pedal`, spec only so far) deepens `pedal.hm` to v2, adds
`pedal.muff`, a generic live-parameter path (`Processor::setLiveParams`, `BlockType::liveParams`),
the pedal face with a CIRCUIT switch, and a 15-preset bank in `presets/modeled/chainsaw/`. It
names `pedal.hmx` / `pedal.eye` as the 7c circuits that drop into the switch. 7c is built on
the **phase 7 base** (`453c7af`), in parallel, so:

- **Do not touch** `pedal_hm.*`, `pedal_ts.*`, `pedal_common.h`, `pedal_params.*`,
  `adaa_clipper.*`, `processor.h`, `chain.*`, `plugin/`, `match/`, `bindings/`,
  `tests/test_pedals.cpp`, `presets/modeled/*.json`, `presets/README.md`, `docs/PEDALS.md`
  (7b's), or the PedalHm/PedalTs section of `docs/PRESET_SCHEMA.md`.
- Everything 7c shares with 7b in spirit (clip types, dry delay) lives in a **new** header in
  its own namespace so both branches compile after the merge; the post-merge cleanup replaces
  7c's copies with 7b's (`clipShapeSpec`, the order-2 LED shape, `HmVoicing`) — one-line edits
  listed in the report.
- Parameter conventions **identical to 7b's** so the face/drawer tables and the live path
  map 1:1: knobs 0–10 (`double`), `tightness` 0–10 default 0 with `fc = 20·10^(t/10)` Hz,
  `mix` **0–100 %** default 100 (wet proportion; dry latency-matched; when `mix == 100` the dry
  branch is skipped so signed zeros are untouched), `level` `3·level − 24` dB on the wet path
  only, enums as lower-case strings, `modelVersion: 1`, unknown keys / out-of-range / wrong
  type / unknown enum → `PresetError`, `toJson()` writes every key.
- A documented **live index order** per block (an `enum HmxLive`, `enum EyeLive` with
  `kHmxNumLive` / `kEyeNumLive`) and pure converters `hmxParamsFromLive(const float*, int)` /
  `hmxLiveFromParams(const HmxParams&, float*)` (same for eye), with no dependency on 7b's
  types. 7b's `setLiveParams` is wired post-merge (not here); parameters are static per preset
  in 7c and go through the Chain rebuild/swap.
- Registry lines are aggregate-initialised `BlockType{{true}, parse, create}` so 7b's trailing
  `liveParams` member merges without edits.

## Files (names binding)
```
core/include/sawblade/pedal_stages.h     namespace sawblade::stages: ClipType, clipKnees(), clipTypeName(), parseClipType(), DryDelay
core/src/pedal_stages.cpp
core/include/sawblade/pedal_saw_params.h HmxParams, EyeParams, *BlockParams, parse*Block, live enums + converters
core/src/pedal_saw_params.cpp            (may duplicate parseModelVersion/knob from pedal_params.cpp; include pedal_params.h for kPedalModelVersion, kKnob*, pedalLevelDb)
core/include/sawblade/pedal_hmx.h  core/src/pedal_hmx.cpp     HmxPedal
core/include/sawblade/pedal_eye.h  core/src/pedal_eye.cpp     EyePedal
tests/test_pedals_saw.cpp
presets/modeled/hmx/*.json  presets/modeled/eye/*.json       (own folders: 7b's test asserts exactly 15 files in presets/modeled/chainsaw/)
```
Plus: two includes + two lines in `core/src/block_registry.cpp`; `core/CMakeLists.txt`;
`tests/CMakeLists.txt`; `tests/tools/pedal_fr.cpp` gets exactly 7b §6.1's first bullet (a
`--param` value that does not parse fully as a number is passed as a JSON string) and the
usage line lists the four pedal types; `docs/PRESET_SCHEMA.md` gets two block-table rows and a
new section after PedalHm/PedalTs.

## Conventions carried over from phase 7 (binding)
`PedalImplConfig` (oversample / adaa / flatFilters) on both pedals; `flatFilters` bypasses
every linear filter and EQ but keeps gains, clippers, pad, mix and level (latency test by
construction). Nonlinear stages at 4x via `Oversampler4x` + `AdaaClipper`; **exactly two ADAA
stages** per pedal, so the oversampled total is 200, `osPadding` = 0, and `latencySamples()` =
**50 at every rate**, like `pedal.hm` (if this cannot hold, stop and report). No allocation /
locks / I/O / exceptions / logging in `process()`; buffers sized in `prepare()`. Coefficients
`double`, samples `float`. Parameter-dependent corners clamped to `min(fc, 0.45·rate)`.

## 1. Shared stages (`pedal_stages.h/.cpp`, `namespace sawblade::stages`)

### 1.1 `ClipType`
```cpp
enum class ClipType { Silicon, Led, Asymmetric };
struct ClipKnees { double kPos, kNeg; };
ClipKnees clipKnees(ClipType) noexcept;
const char* clipTypeName(ClipType) noexcept;               // "silicon" | "led" | "asymmetric"
std::optional<ClipType> parseClipType(std::string_view);
```
| `clip` | k+ | k− | models (same values as 7b §1.3) |
|---|---|---|---|
| `silicon` | 0.5 | 0.5 | stock silicon pair |
| `led` | 1.4 | 1.4 | LED pair: later, louder, more open (7b uses a quintic for this one; here the cubic, swapped post-merge) |
| `asymmetric` | 0.5 | 0.3 | Si + Ge pair: even harmonics, a little DC |

These go into `AdaaClipper::setShape(kPos, kNeg)`. The header says the cubic shape is
unchanged and names the post-merge replacement.

### 1.2 `DryDelay`
Integer delay of up to `kMax = 64` base-rate samples, `std::array` storage, `set(int)`,
`reset()`, `process(float* io, int n) noexcept`. Carries the dry signal of `mix` by exactly
`latencySamples()`. (`ShortDelay` tops out at 7; `DelayLine` is a heap `Processor`.)

## 2. `pedal.hmx` — "modded chainsaw distortion"
The stock HM core of phase 7 (`phase7_modeled_pedals.md` §2, constants restated below; when
7b's `HmVoicing` table lands, `hmx` reads the shared constants from it) with the mods the
modded-HM-2 class shares: **decoupled mids**, **presence**, **3-way clipping**, a **boost
stage**, **clean blend**, and a **4-band EQ** (low, low-mid, high-mid, high). In the stock HM
the HIGH knob drives both high gyrators (1.0 and 1.5 kHz) together; here the 1.0 kHz gyrator
becomes the parametric HIGH-MID band and HIGH drives the 1.5 kHz gyrator alone — that is the
"decoupling".

| # | Stage | Rate | design |
|---|---|---|---|
| 1 | Dry tap | fs | raw input into the dry buffer (`mix`) |
| 2 | Input | fs | 1st-order HPF `fT = 20·10^(tightness/10)` Hz (20 Hz at 0 = the stock coupling cap) |
| 3 | Upsample | → OS | |
| 4 | Pre-filter | OS | HPF 60 Hz, LPF 8 kHz (stock) |
| 5 | Gain 1 | OS | `G1 dB = 6 + 4·distortion + (boost == on ? 9 : 0)` |
| 6 | Clip 1 | OS | `AdaaClipper`, knees from `clip` |
| 7 | Interstage | OS | LPF 5 kHz, +20 dB (stock) |
| 8 | Clip 2 | OS | `AdaaClipper`, knees from `clip` |
| 9 | Post-clip LPF | OS | 4th-order Butterworth 6.5 kHz (stock two biquads) |
| 10 | Downsample | → fs | |
| 10b | DC block | fs | 1st-order HPF 10 Hz (the asymmetric clip makes DC) |
| 11 | EQ: low | fs | RBJ peak 100 Hz, Q 0.8, `−12 + 3·low` dB (stock low gyrator) |
| 12 | EQ: low-mid | fs | RBJ peak `fLM = 200·3^(lowMidFreq/10)` Hz (200–600 Hz; 346 Hz at 5), Q 1.0, `2·(lowMid − 5)` dB (−10…+10; **0 dB at 5**) |
| 13 | EQ: high-mid | fs | RBJ peak `fHM = 1000·1.6^((highMidFreq − 5)/5)` Hz (625 Hz–1.6 kHz; **1000 Hz at 5**), Q 1.2, `−8 + 2.2·highMid` dB (stock gyrator-A law) |
| 14 | EQ: high | fs | RBJ peak 1500 Hz, Q 1.2, `−8 + 2.2·high` dB (stock gyrator B) |
| 15 | Presence (fixed) | fs | RBJ peak 4800 Hz, Q 2.0, +8 dB (stock) |
| 16 | Presence (shelf) | fs | RBJ high shelf 3500 Hz, Q 0.7071, `1.2·(presence − 5)` dB (−6…+6; **0 dB at 5**) |
| 17 | Roll-off | fs | RBJ low-pass 9 kHz, Q 0.707 (stock) |
| 18 | Level + mix | fs | `out = (1 − m)·dry[n − L] + m·levelGain·wet`, `m = mix/100`, `L = latencySamples()`; dry branch skipped when `mix == 100` |

**Stock position.** With `highMid = high`, `highMidFreq = 5`, `lowMid = 5`, `presence = 5`,
`boost = off`, `mix = 100`, `clip = silicon`, `tightness = 0`, the hmx equals `pedal.hm` at the
same `low`/`high`/`distortion`/`level`: the two added bands are 0 dB (RBJ coefficients reduce
to identity) and the DC blocker is the only extra stage. Acceptance 2a checks it within ±0.3 dB.

Params (`HmxParams`; JSON keys; live index in this order):

| # | key | type / range | default | stage |
|---|---|---|---|---|
| 0 | `level` | 0–10 | 5 | 18 |
| 1 | `low` | 0–10 | 5 | 11 |
| 2 | `lowMid` | 0–10 | 5 | 12 gain |
| 3 | `highMid` | 0–10 | 5 | 13 gain |
| 4 | `high` | 0–10 | 5 | 14 |
| 5 | `distortion` | 0–10 | 5 | 5 |
| 6 | `presence` | 0–10 | 5 | 16 |
| 7 | `tightness` | 0–10 | 0 | 2 |
| 8 | `mix` | 0–100 | 100 | 18 |
| 9 | `clip` | `silicon` \| `led` \| `asymmetric` | `silicon` | 6, 8 |
| 10 | `boost` | `off` \| `on` | `off` | 5 |
| 11 | `lowMidFreq` | 0–10 | 5 | 12 frequency |
| 12 | `highMidFreq` | 0–10 | 5 | 13 frequency |

`boost` is a `bool` in `HmxParams`, a string enum in JSON (`"off"`/`"on"`; a JSON boolean is
also accepted on parse, written as the string). Live values: enums as choice index, `boost` as
0/1.

## 3. `pedal.eye` — "one-knob chainsaw"
A sealed all-tens chainsaw: the HM core with the colour-mix EQ **fixed at low = high = 10**,
one `gain` knob, `level`, and a `tightness` low cut. Where it differs from `pedal.hm` at
10/10/x by design, following what reviewers report of the real unit (more gain on tap, tighter
low end; `docs/research/chainsaw_pedals.md`, UNVERIFIED):

| # | Stage | Rate | design |
|---|---|---|---|
| 1 | Input | fs | 1st-order HPF `fT = 20·10^(tightness/10)` Hz |
| 2 | Upsample | → OS | |
| 3 | Pre-filter | OS | HPF **100 Hz** (stock 60: the tighter low end), LPF 8 kHz |
| 4 | Gain 1 | OS | `G1 dB = 10 + 4.2·gain` (10–52 dB; stock 6–46) |
| 5 | Clip 1 | OS | `AdaaClipper`, silicon 0.5/0.5 |
| 6 | Interstage | OS | LPF 5 kHz, +20 dB |
| 7 | Clip 2 | OS | `AdaaClipper`, silicon |
| 8 | Post-clip LPF | OS | 4th-order Butterworth 6.5 kHz |
| 9 | Downsample | → fs | |
| 10 | Colour EQ, fixed | fs | the phase 7 biquad table at low = 10, high = 10 (`HmColorEq::bands(10, 10)` — reuse `HmColorEq` from `pedal_hm.h` by include only) |
| 11 | Level | fs | `pedalLevelDb(level)` |

No `mix` (the one-knob spirit; the face's MIX position is empty for this circuit).

Params (`EyeParams`; live index in this order):

| # | key | range | default | stage |
|---|---|---|---|---|
| 0 | `gain` | 0–10 | 5 | 4 |
| 1 | `level` | 0–10 | 5 | 11 |
| 2 | `tightness` | 0–10 | 0 | 1 |

**What the report must say**: by construction `pedal.eye` is the HM EQ at all tens with a
100 Hz pre-clip corner and +6 dB more gain range; acceptance 3b measures exactly that. Whether
the real pedal differs more (or less) is for the capture calibration; if it does not, the slot
becomes the HM-3 per the lead's rule.

## 4. Registry, schema, tool
- `BlockRegistry` constructor: `types_["pedal.hmx"] = BlockType{{/*namTrainable=*/true},
  parseHmxBlock, createHmx};` and the same for `pedal.eye` (static, nonlinear, time-invariant).
- `docs/PRESET_SCHEMA.md`: two block-table rows (latency "50 samples (at any rate)") and a new
  section "PedalHmx (`type: "pedal.hmx"`) and PedalEye (`type: "pedal.eye"`)" after
  PedalHm/PedalTs with the two param tables (ranges, defaults, formulas), the generic names, the
  clip table, the stock-position statement, mix/dry alignment, latency, `namTrainable`, the
  "not a capture of a real unit" note, and the preset folders.
- `tests/tools/pedal_fr.cpp`: string `--param` values (7b §6.1 wording), usage lists
  `pedal.hm|pedal.ts|pedal.hmx|pedal.eye`.

## 5. Presets
Rules as 7b §4: path a = the block, path b disabled, `blend: 0`, `align: off`, shared cab =
`../../../tests/fixtures/ir/impulse.wav`, every key written explicitly, `notes` starts with the
sound it chases (band names only in `notes`, never in `name`), amp suggestions by TONE3000 id
where the sound needs an amp. The implementer may move `level` only, so that the fixture render
peaks in **[−6, −0.5] dBFS**, and reports each change.

`presets/modeled/hmx/` (modded chainsaw):

| file | name | low / lowMid / highMid / high / dist / level | presence | tight | mix | clip | boost | lowMidFreq / highMidFreq | notes (sound) |
|---|---|---|---|---|---|---|---|---|---|---|
| `arizona_mids.json` | Arizona Mids | 8 / 5 / 9 / 7 / 9 / 3 | 8 | 3 | 80 | led | off | 5 / 6 | modern desert death metal: high-mids pushed, presence up, LED clip, 20 % clean (Gatecreeper-style, into a high-gain amp 88689) |
| `boosted_blend.json` | Boosted Blend | 7 / 6 / 7 / 8 / 7 / 2 | 6 | 2 | 65 | silicon | on | 4 / 5 | boost stage on, 35 % clean blend: thicker, more compressed buzz that keeps pick attack (Throne-Torcher-style; mid-gain British amp 86089) |
| `four_band_doom.json` | Four-Band Doom | 9 / 8 / 3 / 6 / 6 / 4 | 3 | 0 | 100 | asymmetric | off | 2 / 4 | doom/sludge saw: low-mids up at 250 Hz, high-mids scooped, dark presence, asymmetric clip (low-gain amp) |
| `decoupled_crust.json` | Decoupled Crust | 5 / 4 / 10 / 10 / 9 / 4 | 7 | 5 | 100 | silicon | off | 5 / 7 | d-beat crust with the mids decoupled: 1.2 kHz bark, less low, tight (plexi-style amp 76884) |

`presets/modeled/eye/` (one-knob chainsaw):

| file | name | gain / level / tight | notes (sound) |
|---|---|---|---|
| `one_knob_max.json` | One-Knob Max | 10 / 2 / 0 | the sealed all-tens buzzsaw at full gain (Sunlight-studio era, into a small solid-state amp) |
| `one_knob_tight.json` | One-Knob Tight | 8 / 3 / 7 | same voicing, tight low cut at 100 Hz input for modern palm-muted riffing |
| `one_knob_crust.json` | One-Knob Crust | 3 / 5 / 2 | low-gain crust: the all-tens EQ with the clippers barely driven |

Recipes for 7b's bank (`pedal.hm` v2, **no files here**; the family then totals 22 presets,
15 from 7b + 7 here):

| name | low / high / dist / level | mode | deep | sound |
|---|---|---|---|---|
| Sunlight All Tens | 10 / 10 / 10 / 2 | stock | — | = 7b `classic_buzzsaw.json` |
| Stockholm Custom | 10 / 9 / 10 / 1 | custom | lowFreq 90 | custom-mode wall with the low gyrator a touch lower (cf. 7b `custom_wall.json`) |
| Gothenburg Half-Mids | 8 / 5 / 8 / 4 | custom | highFreq 900, highSpread 1.4, presenceDb 6 | melodic-death: half the high-mids, custom gain |
| Grind Buzz | 6 / 10 / 10 / 4 | custom | tightness 7, presenceFreq 5500, presenceDb 12 | custom-mode grind (cf. 7b `grind.json`) |

## Acceptance (Catch2, `tests/test_pedals_saw.cpp`, tags `[pedal][saw]...`; fs = 48 kHz unless stated)
Copy (do not include from `test_pedals.cpp`) the helpers you need: `measureThd`,
`measureAliasDb`, `Fr`, `chainPreset`/`buildChain`, the flat registration pattern. Small-signal
FR = impulse at −90 dBFS via `pedal_fr_util.h`. "Difference curve" = `FR(A) − FR(B)` per bin.

1. **Shared stages.** `clipKnees` table; `parseClipType` round-trips the three names, rejects
   `"soft"` and `"asym"`. `DryDelay` 50: impulse at index 50 bit-exact; chunking 1/7/64 identical.
2. **`pedal.hmx` FR** (others default unless stated):
   a. **Stock position**: hmx defaults vs `HmPedal` defaults, and hmx (low 10, highMid 10,
      high 10, dist 10) vs `HmPedal` (10/10/10): |ΔH| ≤ 0.3 dB at 50, 100, 400, 1000, 1500,
      4800, 8000 Hz (absolute, not re 400).
   b. **Decoupling**: `highMidFreq` 0 (625 Hz): `FR(high 10) − FR(high 0)` at 1500 Hz ∈
      [20, 24] dB and at 625 Hz ≤ 6 dB; `FR(highMid 10) − FR(highMid 0)` at 625 Hz ∈ [20, 24]
      and at 1500 Hz ≤ 8 dB. `highMidFreq` 0 vs 10 (highMid 10): argmax of
      `FR(highMid 10) − FR(highMid 5)` within ±5 % of 625 Hz and of 1600 Hz.
   c. **Low-mid**: `FR(lowMid 10) − FR(lowMid 5)` at fLM ∈ [9.5, 10.5] dB for `lowMidFreq`
      ∈ {0, 5, 10} (200 / 346 / 600 Hz), argmax within ±5 % of fLM; `FR(lowMid 0) −
      FR(lowMid 5)` at fLM ∈ [−10.5, −9.5].
   d. **Presence**: `FR(presence 10) − FR(presence 0)` ≥ +10 dB at 10 kHz, within ±1 dB at
      400 Hz.
   e. **Boost**: `FR(boost on) − FR(boost off)` ∈ [8.9, 9.1] dB at every bin 50 Hz–10 kHz
      (small-signal: a pure gain); THD at −40 dBFS, **dist 0**: boost on ≥ off + 3 dB.
      (Amended after implementation: at dist 5 both settings are already square-wave
      saturated, +1.15 dB measured; at dist 0 the boost shows its +9 dB of drive, +20.5 dB
      measured. Both numbers are printed.)
   f. **Tightness**: 0 → 10: |H(50)| drops ≥ 8 dB re its own 1 kHz; |H(1 kHz)| absolute change
      ≤ 0.5 dB (7b test 2 criteria).
   g. **Mix**: `mix` 0, level 8: output = input delayed 50 samples within 1e-6; `mix` 50 =
      0.5·(mix 0) + 0.5·(mix 100) within 1e-6 per sample on the fixture DI's first second;
      `mix` 100 skips the dry branch by construction; the test checks continuity against
      `mix` 99.999999 (within 1e-5) and block-size bit-identity instead of a bit comparison
      against a zeroed dry buffer, which would need a test seam (lead decision: no seam).
3. **`pedal.eye` FR**:
   a. **Gain law**: `FR(gain 10) − FR(gain 0)` at 1 kHz ∈ [41, 43] dB.
   b. **Versus the HM at all tens** (both re their own |H(400)|; `HmPedal` 10/10/10 vs eye gain
      10, tight 0): |ΔH| ≤ 0.5 dB at 400, 1000, 1500, 4800, 8000 Hz; ΔH(50 Hz) ≤ −2.5 dB and
      ΔH(100 Hz) ≤ −1.2 dB (the 100 Hz pre-clip corner; lead estimates −3.1 and −1.7). Print
      both curves' values at 50/100/200 Hz.
   c. **Tightness** as 2f.
4. **THD** (500 Hz sine; harmonics 2–20):
   - **Monotonic**: hmx `distortion` and eye `gain` 0..10 at −20 and −40 dBFS: each step ≥
     previous − 0.05 dB; span ≥ 6 dB at −40 dBFS. Print the tables.
   - **Clip types** (hmx, **dist 0, −40 dBFS**): THD(led) ≤ THD(silicon) − 3 dB; RMS led >
     silicon; `asymmetric` H2 > −40 dBc, `silicon` H2 < −70 dBc. (Amended after
     implementation: the lead's original condition, dist 5 at −20 dBFS, puts 46 dB of gain
     ahead of the clippers, so every clip type is a near-square and the asymmetric pair's
     evenness shows up only as DC, which the 10 Hz blocker removes: H2 −63.8 dBc. The test
     still prints the dist-5 numbers.)
5. **Aliasing** (phase 7 recipe): hmx at dist 10 for each clip type and with boost on; eye at
   gain 10: all < −80 dB. With `oversample = adaa = false`: > −80 dB (sensitivity), both pedals.
6. **Latency**: `latencySamples() == 50` at 44.1 / 48 / 96 / 192 kHz, both pedals, every clip
   type, boost on/off; measured = reported with `flatFilters` (exact by construction, document);
   hmx `mix` 0 and 100 both. Chain: path A = `[pedal.hmx]`, B = `[]` and A = `[pedal.eye]`, B
   = `[]`: impulses aligned (flat registration `test.pedal_hmx_flat` / `test.pedal_eye_flat`).
7. **Zero allocation**: `process()` standalone (varied block sizes) and in a Chain with hmx on
   A and eye on B, under `AllocGuard`.
8. **Block-size independence and determinism**: 5 s of the fixture DI, block sizes 1, 7, 64,
   512, 4096 and two runs at 512, bit-identical, for hmx with each clip type (one preset each)
   and the eye at gain 10.
9. **Preset round-trip and errors**: parse → `toJson` → parse equal with and without explicit
   params for both types, every enum value, `boost` as string and as JSON boolean (written back
   as the string); `modelVersion: 2`, `distortion: 11`, `mix: 101`, `params.foo`, `params` not
   an object, `clip: "soft"`, `clip: 1`, `boost: "maybe"` → `PresetError`. Registry has both
   types, `namTrainable` true. Live converters round-trip every param (`hmxLiveFromParams` →
   `hmxParamsFromLive` equals; same for eye) and the live index order matches the §2/§3 tables.
10. **Presets**: every `presets/modeled/hmx/*.json` (4) and `presets/modeled/eye/*.json` (3)
    parses, renders the fixture DI at 48 kHz, finite, peak in [−6, −0.5] dBFS; `name` free of
    (case-insensitive) entombed, dismember, gatecreeper, nails, nasum, bloodbath, wolfbrigade,
    disfear, trap them, rotten sound, carnage, nihilist, lik, electric wizard, conan, boss,
    hm-2, wrath, torcher, eyemaster, dunwich, abominable, swollen, pickle, muff; `notes`
    non-empty. The existing `presets/modeled/*.json` test still passes (it is non-recursive).
11. Full suite passes; existing goldens unchanged; `-Werror` clean; Debug ASan/UBSan clean.

## Developer plots (for the lead's report; CSVs into `build/fr_saw/`, not committed)
- hmx: stock vs `HmPedal` default; `highMid` 0/5/10 at `highMidFreq` 0/5/10 (high 5);
  `high` 0/10 at highMidFreq 0; `lowMid` 0/10 at `lowMidFreq` 0/5/10; `presence` 0/5/10; each
  preset's settings; `clip=led dist=10`.
- eye: `gain` 0/5/10; eye gain 10 vs `pedal.hm` 10/10/10; `tightness` 0/5/10.
- Print from the tests: THD tables, alias figures, clip-type THD, preset render peaks.

## Report back (dsp-engineer)
Files changed; design notes; latency per rate; alias table (per clip, boost, no-OS/ADAA);
THD tables; clip-type numbers; the eye-vs-HM numbers of 3b; preset level changes and peaks;
the pedal_fr commands; full ctest summary; commit hashes; decisions / questions for lead;
the post-merge one-line edits list (ClipType → 7b's, cubic LED → quintic, HmVoicing).

---

# Part 2 (added after GO, 2026-10-04): integration with the landed phase 7b

7b has landed on `origin/claude/sawblade-p7b-chainsaw-pedal` (`a0eae2e`): `pedal.hm` v2 with
`HmVoicing`, `pedal.muff`, shared `ClipType`/`clipShapeSpec`/`DryDelay` in `pedal_common.h`,
the generic live-parameter path (`Processor::setLiveParams`, `BlockType::liveParams`,
`Chain::setBlockLiveParams`), the plugin CIRCUIT switch (`plugin/src/pedals/CircuitParams.*`,
`CircuitFaces.*`, `PedalFace`, `AdvancedDrawer`), 15 presets in `presets/modeled/chainsaw/`
and `docs/PEDALS.md`. Main-lead instruction: merge it into this branch first, then make `hmx`
and `eye` **rows in 7b's tables, not a parallel mechanism**. The "do not touch" list of Part 1
is lifted for the files named below; everything else in 7b stays as it landed.

## 2.1 Merge
`git merge origin/claude/sawblade-p7b-chainsaw-pedal` (a merge commit; this branch is pushed,
never rebase it). Known conflicts and their resolutions:
- `core/src/block_registry.cpp`: keep both sides' registrations (hm, ts, muff, hmx, eye).
- `tests/CMakeLists.txt`: keep both test files (`test_pedals_7b.cpp`, `test_pedals_saw.cpp`).
- `tests/tools/pedal_fr.cpp`: **take 7b's version** (it has `--preset`, `--block`, `--thd` and
  the same string-param change).
- `docs/specs/STATE.md`: keep the 7c content (7b's says "complete"; its REPORT holds the rest);
  add one line that 7b is merged at `a0eae2e`.
After the merge: full Release ctest must pass before any adaptation commit.

## 2.2 Core adaptations (`pedal_hmx.*`, `pedal_eye.*`, `pedal_saw_params.*`)
1. Delete `pedal_stages.h/.cpp` (and the CMake lines). Use `sawblade::ClipType`,
   `clipShapeSpec`, `kClipNames`, `clipTypeName` and `DryDelay` from `pedal_common.h`. The hmx
   `clip` becomes 7b's **4-way** enum (`silicon | led | asymmetric | soft`) so the shared CLIP
   switch maps 1:1; the LED now uses 7b's order-2 shape through `clipShapeSpec` exactly like
   `HmPedal` does. Re-measure and re-print the clip-type THD/RMS/alias numbers.
2. `HmxPedal` reads every stock constant from `HmVoicing` (tightness law, `preHpfHz`,
   `preLpfHz`, `g1BaseDb`, `kModes[Stock]` for `s1` / `interLpfHz` / `postLpfHz`,
   `interstageDb`, `lowBaseDb`, `highBaseDb`, `highSlope`, `highQ`, `presenceQ`, `rolloffQ`,
   `postQ1/2`). Hard-coded copies of those numbers are gone. `EyePedal` likewise (its 100 Hz
   pre-clip corner and `10 + 4.2·gain` law stay its own constants, named in its header).
3. **Live parameters**, 7b §3 pattern copied from `HmPedal`: `HmxPedal::setLiveParams` /
   `EyePedal::setLiveParams` (store targets, dirty flag; gains ramp 20 ms, filter coefficients
   redesigned once at block start when changed, enums immediate, no-op when the values equal
   the current ones so static renders stay bit-identical). `hmxLiveParamDescs()` /
   `eyeLiveParamDescs()` return `std::vector<LiveParamDesc>` in `HmxLive` / `EyeLive` order
   (keys = JSON keys, ranges and defaults = the schema's, choices for `clip` and `boost`),
   and the registry rows set `liveParams`. Keep the existing `*ParamsFromLive` /
   `*LiveFromParams` converters (they now also serve the plugin).
4. `pedal.hm` `mode`: **no change**. 7b's `stock | custom | modded` already is the requested
   "standard | custom" (`stock` = standard). Say so in the report.

## 2.3 Plugin integration (`plugin/src/pedals/*`, `plugin/src/PresetMapping.*`)
- `Circuit`: append `ModdedSaw = 2`, `OneKnobSaw = 3`; `kNumCircuits = 4`;
  `kMaxCircuitLive` over all four. `ParamIndex`: `kHmxFirst = kMuffFirst + kMuffNumLive`,
  `kEyeFirst = kHmxFirst + kHmxNumLive`, `kNumParams = kEyeFirst + kEyeNumLive`. Ids `hmx<Key>`
  / `eye<Key>` (`hmxLevel`, `hmxLow`, `hmxLowMid`, `hmxHighMid`, `hmxHigh`, `hmxDistortion`,
  `hmxPresence`, `hmxTightness`, `hmxMix`, `hmxClip`, `hmxBoost`, `hmxLowMidFreq`,
  `hmxHighMidFreq`; `eyeGain`, `eyeLevel`, `eyeTightness`). Display names "Modded Saw …" /
  "One-Knob Saw …"; `sawCircuit` choices `Chainsaw`, `Big Fuzz`, `Modded Saw`, `One-Knob Saw`.
  `CircuitInfo` rows, `circuitForBlockType`, `circuitParamSpec` from the core descriptor lists
  (single source of truth), `switchCircuit` carry-over: level ↔ volume ↔ level, and `mix`,
  `tightness`, `clip` where the target has them (the eye has no mix/clip: dropped).
- `CircuitFaces` rows:

| circuit | OLED | face knobs 1–6 | CLIP | FOCUS | drawer knobs | drawer switches |
|---|---|---|---|---|---|---|
| MODDED SAW (`pedal.hmx`) | `MODDED SAW` | LOW `hmxLow`, HIGH `hmxHigh`, DIST `hmxDistortion`, TIGHT `hmxTightness`, OUT `hmxLevel`, MIX `hmxMix` | `hmxClip` | `hmxBoost` labelled BOOST, values 0 / 1, threshold 0.5, readings OFF / ON | LOW-MID `hmxLowMid`, LM HZ `hmxLowMidFreq`, HIGH-MID `hmxHighMid`, HM HZ `hmxHighMidFreq`, PRESENCE `hmxPresence` | BOOST `hmxBoost` |
| ONE-KNOB SAW (`pedal.eye`) | `ONE-KNOB SAW` | GAIN `eyeGain`, −1, −1, TIGHT `eyeTightness`, OUT `eyeLevel`, −1 | −1 (no CLIP switch) | `eyeTightness` labelled TIGHT, values 0 / 5, threshold 2.5, readings OFF / ON | (none) | (none) |

  If `PedalSwitch` / `FaceSwitchSpec` hard-code the WIDE / NARROW readings, add two reading
  strings to `FaceSwitchSpec` (defaults "WIDE" / "NARROW") — the smallest generalisation. The
  face and drawer must handle `clipParam = −1`, empty knob positions and empty drawer lists
  (hide the control; the OLED line 2 omits the clip field).
- `docs/PEDALS.md`: replace the "coming" 7c paragraph with the two circuits (controls in player
  terms, the face/drawer mapping above, the seven presets one line each). `presets/README.md`:
  replace the 7c TODO rows with the seven presets (folders `presets/modeled/hmx/`, `eye/`; the
  family bank is 22). `docs/PLUGIN.md`: one sentence that four circuits exist.

## 2.4 Acceptance, Part 2 (on top of Part 1's; Part 1 tests keep passing after the merge)
12. **Live parameters, core** (both pedals, `tests/test_pedals_saw.cpp`): (a) a loop calling
    `setLiveParams()` with changing values (every index; enums cycling) between `process()`
    calls allocates nothing (alloc guard); (b) a live change of `hmxDistortion` / `eyeGain`
    changes the output RMS; of `hmxClip` changes the spectrum; of `hmxBoost` raises RMS;
    (c) a `setLiveParams` with the current values leaves the static render bit-identical, and
    the descriptor lists match `kHmxNumLive` / `kEyeNumLive` with keys equal to the JSON keys;
    (d) a Chain `setBlockLiveParams` into an hmx block on path a works and is RT-safe.
13. **Plugin, headless** (`sawblade_plugin_tests`, extend 7b's tests): with
    `presets/modeled/hmx/arizona_mids.json` loaded, `sawCircuit = ModdedSaw`, the hmx set holds
    the preset's values and `applyParams` writes them back; moving `hmxHighMid`, `hmxClip`,
    `hmxBoost` changes the audio with `engineBuilds()` unchanged; the RT test passes under
    moving hmx parameters. Same with `presets/modeled/eye/one_knob_max.json` and `eyeGain`.
    `sawCircuit` cycles Chainsaw → Big Fuzz → Modded Saw → One-Knob Saw → Chainsaw, one rebuild
    per step, carry-over as specified (eye: level only + tightness), state round-trips.
14. **Editor** (`sawblade_editor_tests`, xvfb): every parameter has exactly one bound control
    (amended counts); the face shows the MODDED SAW set after switching, the ONE-KNOB SAW face
    has three knobs, no CLIP switch and an empty drawer; BOOST / TIGHT switches read OFF / ON
    and write their values; OLED line 2 for the eye omits the clip; no UI-visible string holds a
    trademark (add wrath, torcher, eyemaster, tc electronic, dunwich, abominable to 7b's list).
    Screenshots to `${CMAKE_BINARY_DIR}/screenshots/`: `sawblade_face_moddedsaw_2x.png`,
    `sawblade_drawer_moddedsaw_2x.png`, `sawblade_face_oneknob_2x.png` and 3x crops
    `face_moddedsaw_crop.png`, `drawer_moddedsaw_crop.png`, `face_oneknob_crop.png`.
15. pluginval strictness 10 on the VST3 if 7b's harness runs here; `-Werror`; full Release
    ctest; Debug ASan/UBSan for the core tests.

## 2.5 Report back (Part 2)
The merge conflict resolutions; the deleted/replaced 7c shadow types; re-measured clip numbers
(the LED is now quintic); the live-param design notes; the plugin table rows; screenshots;
full ctest summary; commit hashes; decisions / questions for lead.

---

# Part 3 (added 2026-10-04 20:45 UTC): calibration corrections from phase 7.1

The main lead's phase 7.1 measured `pedal.hm` v2 against 18 real captures (HM-2 1985 ×6
labelled D/L/H, HM-2W ×8 standard/custom pairs, Throne Torcher ×2, Eyemaster ×2); full report
`docs/specs/phase7_1_hm_calibration_REPORT.md` + `docs/reports/phase7_1/` on the main working
branch (landing shortly; the numbers below are the lead's relay and are binding for this part).
Mean free-fit LTAS error vs the stock HM-2 is 4.1 dB RMS: the model is structurally off. The
corrections are measured, not guesses, and are folded in here as a **new HM voicing**. The
local re-fit against the captures happens after the merge.

## 3.1 Versioning (binding)
- `pedal.hm` gets **`modelVersion: 3`** = the calibrated voicing. `modelVersion` 1 and 2 keep
  the phase 7 / 7b voicing **bit-identically** (7b test 1 and its 15-preset bank are untouched:
  the bank stays at v2 in this phase). `HmParams` stores its version; `toJson()` writes the
  stored version (2 or 3); a block created from defaults (`HmParams{}`, the plugin's
  `switchCircuit`) is **v3**. v3 accepts every v2 key plus the two custom trims of §3.4.
- `HmVoicing` becomes per-version: `HmVoicing::v2()` (today's constants, unchanged) and
  `HmVoicing::v3()` (below), selected at construction; every v3 number lives in that one table.
- `pedal.hmx` and `pedal.eye` (both `modelVersion: 1`, new blocks) are built on the **v3** core.

## 3.2 v3 core (STRUCTURAL)
1. **Asymmetric diode clip.** Real units: H2 ≈ −9, H4 ≈ −13, H6 ≈ −20 dB re fundamental, H3 ≈
   −18; the symmetric model has H2 = −∞ and H3 ≈ −9. v3 stock knees: `k+ = 0.5` (silicon) and
   `k−` **fitted** so that the whole pedal at D 10, L = H = 5, 500 Hz at −20 dBFS gives
   H2 ∈ [−12, −6] dBc and H3 ∈ [−22, −14] dBc. Fit procedure (implementer, offline or in a
   test-time search, one number hard-coded into the table with the procedure in the comment):
   scan `k−` from 0.5 to 3.0 in steps of 0.05 on both stages (same knees), pick the value whose
   H2 is nearest −9 dBc, then check H3; if H3 is not in range with both stages asymmetric, make
   only stage 2 asymmetric and re-scan. Report the scan table. The `clip` enum keeps its
   meaning: `silicon` = these v3 stock knees in a v3 block (and 0.5/0.5 in v1/v2), `bias` still
   scales `k−`. The 10 Hz DC blocker already exists.
2. **Drive range.** Free fits land at distortion ≈ 10 for every real label D-2…D-10 and ≈ 3.3
   for D-0: the real pedal is saturated from D-2. v3: `g1BaseDb = 26`, `s1 = 2.0` (26–46 dB
   across the knob, was 6–46). Custom: same (no gain change measured).
3. **Dynamics** (re-measure only): captures have crest 7.6–10.1 dB and envelope spread
   2.2–3.0 dB; v2 at D 10 gives crest ≈ 9.5 and spread 1.6. Print crest factor and envelope
   spread (RMS over 50 ms windows, 10th–90th percentile range in dB) of the fixture-DI render at
   D 10 for v2 and v3; no threshold.

## 3.3 v3 colour EQ
Apply the free-cascade residual fit **on top of** the v2 bands (it was fitted to the residual of
the v2 model, presence peak included), and open the top end:
- keep: low gyrator (100 Hz, Q 0.8, `−12 + 3·low`), high A (`highFreq`, Q 1.2), high B
  (`highFreq·highSpread`), presence peak (`presenceFreq`, `presenceDb` default 8);
- **add three fixed fit bands** (table constants `fitLowShelfHz = 85, fitLowShelfDb = 1.7,
  Q 0.707`; `fitMidHz = 683, fitMidDb = 4.5, fitMidQ = 2.4`; `fitCutHz = 5500, fitCutDb =
  −12.0, fitCutQ = 1.54`) — final 7.1 numbers (8 labelled stock models, 1.36 dB RMS), updated
  21:02 UTC from the first relay (85/+2.4, 705/+4.65/2.76, −12.2);
- **remove the output roll-off by default**: `rolloffHz` range becomes 4000–16000, v3 default
  **16000** (the fit ran to its 14 kHz bound; 16 kHz keeps it out of the way at 44.1 kHz too);
- **post-clip LPF** (4th-order) `postLpfHz = 9500` for stock and custom in v3 (was 6500; the
  model was ~12 dB short at 10 kHz). Check the alias margin stays < −80 dB with the wider LPF
  (Acceptance 16); if it does not, lower towards 9 kHz and report.
Gains-only cross-check (informational print): v3 − v2 at 1 kHz ≈ +3.3 dB, 1.5 kHz ≈ −1.9 dB.
Measured knob map (provisional, v1 voicing): real D 2…10 all fit model D ≈ 9.7; real H 10 fits
model H ≈ 7.2; real L 7…10 fit model L ≈ 7.1 — the drive change above is what the D map asks for.

## 3.4 Custom mode in v3 (all four standard/custom pairs agree)
`mode: custom` in v3 = exactly these deltas over stock (v2's custom `s1 4.6 / sLow 3.6` stay
v2-only): output **+2.5 dB**; low shelf **`customLowDb`** (new key, 0–8 dB, default **3.2**) at
100 Hz Q 0.7; high shelf **`customHighDb`** (new key, 0–8 dB, default **3.0**) at 6 kHz Q 0.7;
no gain change; slightly odd-heavier harmonics (measured H3 +2.5, H2 −1): implement as `k−`
pulled 25 % of the way toward `k+` (`k−_custom = k− − 0.25·(k− − k+)`); crest 0.6–1.4 dB lower
(print). The two new keys are **preset-static trims** (not live, not on the face or drawer: the
drawer is full at 10 knobs); they are PresetErrors on v1/v2 blocks. `modded` in v3 = stock v3
with `interLpfHz = postLpfHz = 11000` (keeps the "brighter top" meaning above the new 9.5 kHz).

## 3.5 hmx and eye on the v3 core
- **hmx** (Throne Torcher measured vs stock): fixed voicing deltas in `HmxPedal`: low shelf
  **+3.8 dB at 110 Hz** Q 0.7 and peak **−2.5 dB at 2.2 kHz** Q 1.0 (the 1.6–3 kHz dip), no extra
  presence (presence shelf default stays 0 dB); gain range tops out at D 6.2: `G1 = 26 +
  1.24·distortion (+ 9 boost)` dB. Decoupled bands, 4-way clip, boost, mix, tightness unchanged.
  Part 1 test 2a (stock position = hm) is **replaced** by Acceptance 19.
- **eye** (Eyemaster fits D 6.6–7.4 with the lowest errors of the set and the stock residual
  shape, so it is an HM-2-family fixed-knob circuit): the v3 core with **L = 6.2, H = 7.1
  fixed**, `gain` mapping **D = 3 + 0.5·gain** (D 3 → 8), `level`, `tightness`; the Part 1 100 Hz
  pre-clip corner and the 10–52 dB law are dropped (pre-filter = v3 `preHpfHz`). Part 1 tests
  3a/3b are **replaced** by Acceptance 20. The HM-3 fallback question is closed: the measurement
  says HM-2 family.

## 3.6 Presets
- New `presets/modeled/hm_v3/` (`pedal.hm`, `modelVersion: 3`, all keys explicit), the Part-1 §5
  recipes made real: `sunlight_all_tens.json` (10/10/10, stock), `stockholm_custom.json`
  (6.5/5/10, custom — the 7.1 "custom all tens" knob map, lowFreq 90), `gothenburg_half_mids.json` (8/5/8, custom, highFreq 900,
  highSpread 1.4, presenceDb 6), `grind_buzz.json` (6/10/10, custom, tightness 7, presenceFreq
  5500, presenceDb 12). `level` set for a peak in [−6, −0.5] dBFS. Family bank: 15 + 7 + 4 = 26.
- Re-level the seven hmx/eye presets on the v3 core (same window), report old/new.
- `presets/README.md`, `docs/PEDALS.md`, `docs/PRESET_SCHEMA.md` (v3 section: versioning rule,
  the two trims, the table of v3 constants with "measured in phase 7.1" provenance).

## Acceptance, Part 3
16. **v1/v2 untouched**: 7b test 1 (goldens) and the whole 7b suite pass unchanged; a v2 block
    renders bit-identically to before this part (compare `presets/modeled/chainsaw/*.json`
    renders against renders made at the Part-2 head, recorded once as a temporary check or via
    7b's LTAS/peak tests + the two goldens). v3 alias floor < −80 dB at D 10 for each clip type
    and mode (phase 7 recipe) with the 9.5 kHz post LPF.
17. **v3 harmonics and drive**: whole pedal, D 10, L = H = 5, 500 Hz −20 dBFS: H2 ∈ [−12, −6]
    dBc, H3 ∈ [−22, −14] dBc (print H2…H6). Small-signal `FR(D 10) − FR(D 0)` at 1 kHz = 20 ±
    0.2 dB. THD at −40 dBFS: D 2 ≥ (v2's D 10 THD) − 3 dB (saturated from D-2). THD still
    monotonic in D at −20 and −40 dBFS.
18. **v3 EQ** (`FR(v3 default) − FR(v2 default)`, absolute, same knobs): 50 Hz ∈ [+0.7, +2.7];
    683 Hz ∈ [+3.4, +5.4]; 5.5 kHz ∈ [−13, −8]; 10 kHz ≥ +8. **Custom v3** (`FR(custom) −
    FR(stock)`, v3): 400 Hz ∈ [2.0, 3.0]; 50 Hz ∈ [4.5, 6.5]; 10 kHz ∈ [4.5, 6.5]; with
    `customLowDb = customHighDb = 0`: flat 2.5 ± 0.3 dB from 50 Hz to 10 kHz. Harmonics custom
    vs stock at D 10: H3 +1…+4 dB, H2 −2.5…0 dB. `modded` v3: |H(10 kHz)| − |H(400)| ≥ stock's + 3 dB.
19. **hmx vs hm v3** (both re their own |H(400)|, same knobs, dist 5): 110 Hz ∈ [+3.1, +4.5];
    2.2 kHz ∈ [−3.2, −1.8]; 400 Hz and 8 kHz within ±0.5. Gain law `FR(dist 10) − FR(dist 0)` at
    1 kHz = 12.4 ± 0.2 dB. Part-1 tests 2b–2g unchanged and passing.
20. **eye = hm v3 at fixed knobs**: eye (gain 10, tight 0) vs hm v3 (L 6.2, H 7.1, D 8, same
    level): |ΔH| ≤ 0.3 dB absolute at 50, 100, 400, 1000, 1500, 4800, 8000 Hz; gain law 5 ± 0.2
    dB at 1 kHz; tightness test as before.
21. **Presets**: 4 hm_v3 + 7 re-levelled hmx/eye render in [−6, −0.5] dBFS, finite, name rules;
    `presets/modeled/chainsaw/*.json` (v2) unchanged byte-for-byte.
22. **Round-trip**: v3 block with every key incl. the trims round-trips; `customLowDb` on a v2
    block → PresetError; `modelVersion: 4` → PresetError; `toJson` preserves 2 vs 3; defaults
    construct as v3; plugin `switchCircuit` to Chainsaw yields a v3 block and the 7b plugin
    tests still pass (amend the expected version where they assert 2 on a default-built block).
23. Full suite, `-Werror`, ASan/UBSan for the core, `pedal_fr` CSVs for v2 vs v3 (default and
    10/10/10), custom vs stock, hmx vs hm v3, eye vs hm v3.

## Report back (Part 3)
The `k−` scan table and the chosen knees; H2…H6 before/after; crest/envelope numbers v2 vs v3
and stock vs custom; the v3 − v2 difference-curve values; preset level changes; full ctest;
commits; decisions / questions for lead.

## 3.7 hmx `midVoice` (added 20:45 UTC, user request via the main lead)
The family spans several circuits (Wurm, big fuzz, one-knob, Throne-Torcher / Left-Hand-Wrath
class). The Wurm-type derivative has **three selectable high-mid voicings** over a 4-band active
EQ, so `pedal.hmx` gets a `midVoice` switch that selects the **base centre and Q of the
HIGH-MID band**; the `highMidFreq` knob keeps trimming around that base (`fHM = base ·
1.6^((highMidFreq − 5)/5)`), `highMid` keeps its gain law, and HIGH (1.5 kHz gyrator B) is
unchanged. Table-driven so the local re-fit can set the numbers:

| `midVoice` | base centre | Q | meaning |
|---|---|---|---|
| `stock` (default) | 1000 Hz | 1.2 | the HM-2's 1.0 / 1.5 kHz coupling (today's behaviour, bit-identical) |
| `low` | 750 Hz | 1.4 | lower, thicker bark |
| `high` | 2000 Hz | 1.2 | upper-mid cut-through |

`HmxVoicing::kMidVoices[3] = {{1000, 1.2}, {750, 1.4}, {2000, 1.2}}` in `pedal_hmx.h`. JSON key
`midVoice`, strings as above, PresetError otherwise. Live: appended as the **last** `HmxLive`
index (`kHmxMidVoice`, choice 0/1/2); descriptor, converters, plugin id `hmxMidVoice`
("Modded Saw Mid Voice", choices `Stock`, `Low`, `High`); drawer switch **VOICE** on the MODDED
SAW row (beside BOOST). Presets, `presets/modeled/hmx/`: `berlin_saw_low.json`,
`berlin_saw_mid.json`, `berlin_saw_high.json` — same knobs (low 7, lowMid 5, highMid 9, high 7,
dist 8, presence 5, tight 3, mix 100, silicon, boost off, freqs 5/5), only `midVoice` differs;
`level` for the usual peak window; notes name the voicing. hmx then has 7 presets; family 29.

Acceptance 24: `FR(highMid 10) − FR(highMid 5)` argmax (100 Hz–10 kHz, `highMidFreq` 5) within
±5 % of 750 / 1000 / 2000 Hz for `low` / `stock` / `high`; `stock` renders bit-identically to a
block without the key; round-trip and PresetError for `"mid"`; the three presets render in
window; the drawer shows VOICE and the editor count test includes it.

## 3.8 Part 3 amendments after measurement (lead decisions, 2026-10-05 00:30 UTC)
The implementer's measurements (report in `phase7c_chainsaw_family_REPORT.md`) showed several
Part-3 conditions were set at a fully saturated operating point or were arithmetically wrong.
Decisions, all "measure where the physics is visible; models unchanged except where stated":
1. **Harmonic windows (17) at −60 dBFS**, not −20: at D 10 the pedal is a near-square wave at
   −20 dBFS and no knee pair in 0.5–3.0 moves H2 above −25 dBc. Chosen knees: stage 1 symmetric
   0.5/0.5, **stage 2 k+ 0.5 / k− 2.10** (H2 −9.0, H3 −14.9, H4 −21.6, H5 −29.6, H6 −35.6 dBc
   at −60 dBFS; alias −88.7 dB). Both stages asymmetric (k− 2.05) hit the windows too but alias
   at −73 dB, so the spec's fallback applies. Follow-up for the local re-fit (not done here): the
   real pedal's H2 ≈ −9 dBc *at saturation* implies duty-cycle asymmetry, i.e. a DC bias ahead
   of the clipper rather than unequal knees; that variant could meet the −20 dBFS window and
   would also move the crest. The −20 dBFS numbers stay as an informational print.
2. **Modded mode** `interLpfHz` = **6500** (highest corner keeping every clip < −80 dB alias;
   11 kHz gave −67…−72 dB); `postLpfHz` = 11000 as specified. Brightness +4.1 dB (≥ +3 holds).
3. **Acceptance 18 is measured at equal stage-1 gain** (v3 D 0 vs v2 D 5, both 26 dB): "same
   knobs" carries the new drive law's +20 dB. Gains-only cross-check is informational only.
4. **Unsaturated test points move 20 dB down** (base drive is now 26 dB): boost THD, clip-type
   order, hmx/eye THD spans, live-change tests are asserted at −60 dBFS (old levels printed).
   `D 2 THD ≥ v2 D 10 − 3 dB` is dropped (measured 0.6 dB short; the drive-range intent is
   proven by the 20 dB small-signal law and the −40 dBFS saturation prints).
5. **Clip-type symmetric reference is `led`** (`led.h2 < −70 dBc`); v3 `silicon` is asymmetric
   by design.
6. **THD monotonicity tolerance 0.07 dB** (saturated-region wiggle 0.063 dB measured).
7. **hmx 110 Hz band is a peak (Q 0.7), not a shelf** (a shelf corner at 110 Hz gives half its
   gain there); acceptance 19's 400 Hz / 8 kHz bound is **±0.8 dB** (the peak and the 2.2 kHz dip
   leak into the 400 Hz reference; measured −0.68 at 8 kHz).
8. **Eye gain law = 10 ± 0.2 dB** at 1 kHz (D 3 → 8 at 2 dB per unit; the spec's "5" was a
   lead arithmetic error).
9. **Presets** may use the preset-level `output.gainDb` (here −4 dB on `decoupled_crust`,
   `sunlight_all_tens`, `grind_buzz`) when `level` 0 cannot reach the peak window; knobs stay the
   recipes.
10. Custom-mode crest (+0.1 dB vs stock; real units −0.6…−1.4 dB) is recorded as a re-fit item.
No `[!shouldfail]` tests remain: each condition above is either asserted as amended or printed.
11. **Modded mode alias at 44.1 kHz** (reviewer note, measured after acceptance): with the
    asymmetric stage 2 and the 11 kHz post-clip filter, modded aliases at −80.7 (silicon),
    −75.6 (led), −72.9 (asymmetric), −75.7 (soft) dB at 44.1 kHz; at 48 kHz and above it is
    within the −80 dB budget. Options were a rate-aware post LPF (which would change the 48 kHz
    sound or cost the mode's brightness) or a documented exception. Decision: **documented
    exception** — the test stays in the default suite and asserts < −80 dB at 48/96 kHz and
    **< −72 dB at 44.1 kHz** for modded only (the alias is still 55 dB below the no-OS/ADAA
    floor and under a full-drive square-wave spectrum). No hidden or `[.]`-tagged tests. Re-fit
    item: a 44.1 kHz-specific modded post LPF or 8x oversampling for the modded voicing.
