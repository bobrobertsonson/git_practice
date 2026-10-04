# Phase 7c: modeled pedal blocks `pedal.mz` and `pedal.rat` — core DSP only

## Why
Two more circuits for the modular pedal chain (CLAUDE.md roadmap, "Modular pedal chain"), built
on the Phase 7 infrastructure (`Oversampler4x`, `AdaaClipper`, `OnePole`, `ShortDelay`,
`PedalImplConfig`, the `modelVersion`/`params` schema). They widen the matcher's reach to the
tones Phase 7 does not cover:

- `pedal.mz` — **"scooped metal distortion"** (Metal-Zone family): nu-metal, 90s/00s death
  metal, modern "scooped" rhythm tones, and the "Zone into a clean amp" sound.
- `pedal.rat` — **"rodent distortion"** (RAT family): doom/sludge/stoner, grind, crust,
  hardcore, and the "RAT into a cranked amp" grind.

Both are generic recreations by topology; no trademarks in code, UI names or docs. Neither is
hard-wired to any preset; Gatecreeper is still just the first test case.

A parallel session (7b) is adding deep controls to `pedal.hm`, a `pedal.muff`, and the pedal
face UI with a CIRCUIT switch. **This task is core DSP only, no UI.** The two blocks here must
drop into that CIRCUIT switch later, so they follow the Phase 7 block conventions exactly.

## Scope (dsp-engineer)
New files in `core/`, two lines plus two includes in `core/src/block_registry.cpp`, entries in
`core/CMakeLists.txt` and `tests/CMakeLists.txt`, a new test file, two example presets,
`docs/PRESET_SCHEMA.md` sections, and a small extension of `tests/tools/pedal_fr.cpp`.

**Do not touch** `pedal_hm.*`, `pedal_ts.*`, `pedal_common.h`, `pedal_params.*`, `plugin/`,
`match/`, `bindings/`, or any existing test. Session 7b edits those; keep the merge trivial.
Existing goldens stay bit-identical. Existing presets behave the same.

Files (names are binding):

```
core/include/sawblade/pedal_stages.h      shared new stages: ClipType, clipKnees(), SoftSlewLimiter, DryDelay
core/src/pedal_stages.cpp
core/include/sawblade/pedal_zr_params.h   MzParams, RatParams, *BlockParams, parseMzBlock, parseRatBlock
core/src/pedal_zr_params.cpp
core/include/sawblade/pedal_mz.h  core/src/pedal_mz.cpp     MzPedal
core/include/sawblade/pedal_rat.h core/src/pedal_rat.cpp    RatPedal
tests/test_pedals_zr.cpp
presets/modeled/zone_scoop.json  presets/modeled/rat_grind.json
```

`pedal_zr_params.cpp` may duplicate the two small helpers (`parseModelVersion`, `knob`) from
`pedal_params.cpp` rather than exporting them; duplication is preferred over touching 7b's file.
It reuses `kPedalModelVersion`, `kKnob*`, `pedalLevelDb` from `pedal_params.h` (include only).

## Conventions carried over from Phase 7 (binding)
- Knobs are 0..10 `double`s; default 5 unless this spec says otherwise. Non-5 defaults:
  `tight` = 0, `mix` = 10, `clip` = `"silicon"`.
- `modelVersion` = 1 (shared constant `kPedalModelVersion`); any other value is a PresetError.
- Every param is validated in range; unknown keys in `params`, wrong types, `params` that is
  not an object: PresetError. `toJson()` writes `modelVersion` and every param, so round-trip is
  exact.
- Each pedal takes `PedalImplConfig` (oversample / adaa / flatFilters) exactly like `HmPedal`,
  for the test-only aliasing-sensitivity and latency-by-construction tests. `flatFilters`
  bypasses every linear filter **and the slew limiter + its smoothing** (the latency test needs
  a symmetric small-signal impulse response) but keeps the gains, clippers, pad and the dry/mix
  path.
- Nonlinear stages run at 4x via `Oversampler4x` + `AdaaClipper`. Each pedal has **exactly two
  ADAA stages**, so the oversampled total is 198 + 2 = 200, `osPadding` = 0, and
  `latencySamples()` = **50 at every rate**, the same as `pedal.hm`. If an implementation choice
  changes this, stop and report; do not hide it.
- Audio-thread rules (CLAUDE.md): no allocation, locks, I/O, exceptions, logging in `process()`.
  All buffers sized in `prepare()` from `maxBlockSize`.
- Coefficients in `double`, samples in `float`. Level law `pedalLevelDb(x) = 3x − 24` dB.
- Filters whose corner is parameter-dependent are clamped to `min(fc, 0.45·rate)` at the rate
  they run at.

## 1. Shared new stages (`pedal_stages.h/.cpp`)

### 1.1 `ClipType` and knees
```cpp
enum class ClipType { Silicon, Led, Asym, Soft };
struct ClipKnees { double kPos, kNeg; };
ClipKnees clipKnees(ClipType) noexcept;
const char* clipTypeName(ClipType) noexcept;        // "silicon" | "led" | "asym" | "soft"
std::optional<ClipType> parseClipType(std::string_view);
```
| type | k+ | k− | models |
|---|---|---|---|
| `silicon` | 0.5 | 0.5 | silicon diode pair (default) |
| `led` | 1.2 | 1.2 | LED pair: later, louder, more open ("turbo" variant of the rodent) |
| `asym` | 0.5 | 0.3 | asymmetric pair (even harmonics, a little DC) |
| `soft` | 0.3 | 0.3 | lower knee of the same cubic shape: compresses earlier, germanium-like |

These go straight into `AdaaClipper::setShape(kPos, kNeg)`; the cubic shape itself is not
changed. Document in the header that `soft` is "earlier knee", not a different curve.

### 1.2 `SoftSlewLimiter`
A gentle slew-rate limit modelling a slow op-amp. Runs at the oversampled rate, one sample at
a time, state = last output `y`:

```
d    = x[n] − y[n−1]
step = S · c(d / S)          c(u) = u − u³/(3k²) for |u| < k, ±2k/3 beyond, with k = 1.5
y[n] = y[n−1] + step
```
With k = 1.5 the saturation of `c` is exactly ±1, so `|step| ≤ S` (S = max units per
oversampled sample), and `c` has slope 1 at 0, so slow signals (|d| ≪ S) track exactly. The
step law is C¹ in `d`, which keeps the output's corners rounded (the hard `clamp(d, −S, S)`
limiter would leave slope discontinuities that alias at 4x; this one does not — the
aliasing test in §Acceptance 5 is asserted with the limiter at its strongest setting).

API: `setRate(double unitsPerSecond, double fsOs)` (S = unitsPerSecond / fsOs), `reset()`,
`process(float* io, int n) noexcept`, state in `double`. No latency.

### 1.3 `DryDelay`
Integer delay of up to `kMax = 64` base-rate samples, `std::array` storage, `set(int d)`,
`reset()`, `process(float* io, int n) noexcept`. Used to align the dry signal of the `mix`
control with the wet path (delay = `latencySamples()`). `ShortDelay` (max 7) is too short, and
`DelayLine` is a `Processor` with heap storage; this is the smallest RT-safe thing.

## 2. `pedal.mz` — "scooped metal distortion"
Topology: input buffer → high-gain op-amp stage with mid pre-emphasis → diode clip → second
stage with diode clip → post low-pass → 3-band EQ with the parametric mid → output roll-off →
mix → level. Controls 0..10.

| # | Stage | Rate | Design |
|---|---|---|---|
| 1 | Dry tap | fs | copy of the raw input into the dry buffer (for `mix`) |
| 2 | Input buffer | fs | 1st-order HPF 20 Hz |
| 3 | Tightness | fs | 1st-order HPF `fT = 20 · 20^(tight/10)` Hz (20 → 400 Hz; at `tight` = 0 it doubles the 20 Hz corner — document) |
| 4 | Upsample | → OS | `Oversampler4x` |
| 5 | Stage-1 coupling | OS | 1st-order HPF 80 Hz |
| 6 | Pre-clip emphasis | OS | RBJ peak, `fE = 300 · 10^(emphasis/10)` Hz (300 Hz → 3 kHz; 949 Hz at 5), **+10 dB, Q 1.0** |
| 7 | Feedback cap | OS | 1st-order LPF 10 kHz |
| 8 | Gain 1 | OS | `G1 dB = 5 · dist` (0 → 50 dB) |
| 9 | Clip 1 | OS | `AdaaClipper`, knees from `clip` |
| 10 | Interstage | OS | 1st-order LPF 8 kHz, then fixed **+6 dB** |
| 11 | Clip 2 | OS | `AdaaClipper`, knees from `clip` |
| 12 | Post-clip LPF | OS | 2nd-order Butterworth 10 kHz (one RBJ low-pass, Q 0.7071) |
| 13 | Downsample | → fs | |
| 13b | DC block | fs | 1st-order HPF 10 Hz (the `asym` clip type makes DC; `pedal.ts` has the same stage) |
| 14 | EQ: low | fs | RBJ low shelf, 100 Hz, Q 0.7071, gain `3·(low − 5)` dB (−15 → +15) |
| 15 | EQ: mid (parametric) | fs | RBJ peak, `fM = 200 · 25^(midFreq/10)` Hz (200 Hz → 5 kHz; 1 kHz at 5), gain `3·(mid − 5)` dB (−15 → +15), `Q = 0.5 · 8^(midQ/10)` (0.5 → 4; 1.41 at 5) |
| 16 | EQ: high | fs | RBJ high shelf, 3 kHz, Q 0.7071, gain `3·(high − 5)` dB |
| 17 | Output roll-off | fs | 2nd-order Butterworth low-pass, `fR = 16000 · 0.25^(rolloff/10)` Hz (16 kHz → 4 kHz; 8 kHz at 5), clamped to 0.45·fs |
| 18 | Mix | fs | `w = mix/10`; `y = (1 − w)·dry_delayed + w·wet`, dry delayed by `latencySamples()` |
| 19 | Level | fs | `pedalLevelDb(level)` |

Gain staging rationale: the fixed gain is only +6 dB (HM's +20 dB made it saturate at
distortion 0, see the Phase 7 report §2). At −40 dBFS input and `dist` 0 the clippers stay in
the linear region; at `dist` 10 the total small-signal gain is about 66 dB.

Params (`params` object):

| key | range | default | stage |
|---|---|---|---|
| `dist` | 0..10 | 5 | 8 |
| `level` | 0..10 | 5 | 19 |
| `low` | 0..10 | 5 | 14 |
| `high` | 0..10 | 5 | 16 |
| `mid` | 0..10 | 5 | 15 gain |
| `midFreq` | 0..10 | 5 | 15 frequency |
| `midQ` | 0..10 | 5 | 15 Q (deep) |
| `clip` | `"silicon"`,`"led"`,`"asym"`,`"soft"` | `"silicon"` | 9, 11 (deep) |
| `tight` | 0..10 | **0** | 3 (deep) |
| `emphasis` | 0..10 | 5 | 6 (deep) |
| `rolloff` | 0..10 | 5 | 17 (deep) |
| `mix` | 0..10 | **10** | 18 (deep) |

`MzParams` holds these (`ClipType clip`), with `operator==` defaulted.

## 3. `pedal.rat` — "rodent distortion"
Topology: a non-inverting op-amp stage whose gain-setting network is two series RC legs to
ground, so the gain rises from unity at bass to ~67 dB at treble and the corners move with the
gain pot; a slow op-amp (slew + rails); diode pair to ground; the reversed-sense "filter"
low-pass; volume.

| # | Stage | Rate | Design |
|---|---|---|---|
| 1 | Dry tap | fs | raw input into the dry buffer |
| 2 | Input buffer | fs | 1st-order HPF 20 Hz |
| 3 | Tightness | fs | 1st-order HPF `fT = 20 · 20^(tight/10)` Hz (as mz) |
| 4 | Upsample | → OS | |
| 5 | Gain network | OS | `Rf = 300 · (100000/300)^(distortion/10)` Ω (300 Ω → 100 kΩ; 5.48 kΩ at 5). Two legs: `R1 = 47 Ω, C1 = 4.7 µF` (corner **720 Hz**), `R2 = 560 Ω, C2 = 2.2 µF` (corner **129 Hz**). `g = (Rf/R1)·HPF1(720 Hz)(x) + (Rf/R2)·HPF1(129 Hz)(x)` |
| 6 | Op-amp bandwidth | OS | on `g` only, two 1st-order LPFs: feedback cap `fCf = 1/(2π·Rf·100 pF)` (15.9 kHz at max) and gain-bandwidth `fGbw = 3.5e6 / (1 + Rf/R1 + Rf/R2)` Hz (**≈1.5 kHz at max**, 27 kHz at 5, >400 kHz at 0); each clamped to 0.45·fsOs |
| 7 | Sum | OS | `v = x + g'` (the unity path of the non-inverting stage: gain never below 1) |
| 8 | Rail clip | OS | `AdaaClipper`, symmetric **k = 4.5** (saturates at ±3.0: the supply rails; the op-amp output can never exceed them) |
| 9 | Slew | OS | `SoftSlewLimiter`, rate `R = 0.25 · 10^((5 − slew)/5)` units/µs (2.5 at 0, 0.25 at 5 ≈ a 0.3 V/µs op-amp when 1 unit ≈ 1.2 V, 0.025 at 10) |
| 9b | Slew smoothing | OS | 2nd-order Butterworth low-pass **24 kHz** (one RBJ low-pass, Q 0.7071), clamped to 0.45·fsOs. Anti-aliasing for the limiter's corners, which have no ADAA (see below); above the audio band and the op-amp's own bandwidth |
| 10 | Diode clip | OS | `AdaaClipper`, knees from `clip` |
| 11 | Downsample | → fs | |
| 11b | DC block | fs | 1st-order HPF 10 Hz (for the `asym` clip type) |
| 12 | Filter | fs | 1st-order LPF `fF = 1 / (2π · (1500 + 10000·filter) · 3.3 nF)` Hz (32 kHz at 0 → clamped 0.45·fs; 936 Hz at 5; 475 Hz at 10). **Reversed sense: up = darker.** |
| 13 | Mix | fs | as mz |
| 14 | Volume | fs | `pedalLevelDb(volume)` |

Why rails → slew → smoothing → diodes, in that order: a real op-amp output is bounded by its
rails *and* slews between them, so the limiter must see the rail-bounded signal (slewing towards
a 300-unit target would hold the output saturated far too long). The limiter's output is a
trapezoid/triangle whose corners are slope discontinuities (the soft step law only rounds them
over about one oversampled sample), i.e. a 1/f² spectrum with no ADAA; at 4x this would alias
at roughly −60 dB. The 24 kHz 2nd-order smoothing attenuates the folding region (≈150–200 kHz)
by more than 30 dB, which puts the alias below −90 dB (lead estimate), while changing the audio
band by under 1.3 dB at 18 kHz. The aliasing test is asserted with the limiter at its strongest
setting (§Acceptance 5).

`flatFilters` for the rat: stages 2–3, 5–6 (so `v = x`), 9, 9b, 11b and 12 are bypassed; the
rail clip, diode clip, pad, mix and volume stay.

Analytic small-signal expectations (fs = 48 kHz, filter 0, used for the targets below):

| distortion | |H(40 Hz)| | |H(500)| | |H(5 kHz)| |
|---|---|---|---|
| 0 | ≈ +1 dB | ≈ +14 dB | ≈ +18 dB |
| 5 | | ≈ +38 dB | ≈ +42 dB |
| 10 | ≈ +45 dB | ≈ +62 dB | ≈ +56 dB (GBW corner has closed the top) |

Params:

| key | range | default | stage |
|---|---|---|---|
| `distortion` | 0..10 | 5 | 5, 6 |
| `filter` | 0..10 | 5 | 12 |
| `volume` | 0..10 | 5 | 14 |
| `clip` | `"silicon"`,`"led"`,`"asym"`,`"soft"` | `"silicon"` | 10 (deep; `led` is the "turbo" variant) |
| `slew` | 0..10 | 5 | 9 (deep) |
| `tight` | 0..10 | **0** | 3 (deep) |
| `mix` | 0..10 | **10** | 13 (deep) |

## 4. Registry, schema, tool
- `BlockRegistry` constructor: `types_["pedal.mz"]` and `types_["pedal.rat"]`, both
  `namTrainable = true` (static, nonlinear, time-invariant; the slew limiter is time-invariant
  and memoryless-rate, still trainable), beside the Phase 7 lines.
- `docs/PRESET_SCHEMA.md`: two rows in the block-types table (latency "50 samples (at any
  rate)"), and a **new section** "PedalMz (`type: "pedal.mz"`) and PedalRat (`type:
  "pedal.rat"`)" after the PedalHm/PedalTs section, with the two param tables above (ranges,
  defaults, meaning, formulas), the generic UI names, the clip-type table, the mix/dry
  alignment, latency, `namTrainable`, and a "not a capture of a real unit" note like Phase 7's.
  Do **not** edit the PedalHm/PedalTs section (7b owns it).
- `tests/tools/pedal_fr.cpp`: a `--param key=value` whose value does not parse fully as a
  number (`strtod` end pointer not at the string end) is passed as a JSON **string**, so
  `--param clip=led` works. Update the usage line to list all four types.

## 5. Example presets (render from repo files only)
- `presets/modeled/zone_scoop.json`: single path, `pedal.mz` with `dist` 7, `low` 7, `high` 7,
  `mid` 0, `midFreq` 5 (1 kHz scoop), `clip` `"silicon"`, `level` chosen so the fixture render
  (`tests/fixtures/di_riff.wav`) peaks between −12 and −1 dBFS (state the value in the report).
  Cab: the repo identity IR `../../tests/fixtures/ir/impulse.wav`, `align.mode = "off"`, like
  `hm_chainsaw.json`. Notes field: say it is a starting hypothesis, not a fitted tone.
- `presets/modeled/rat_grind.json`: single path, `pedal.rat` with `distortion` 8, `filter` 4,
  `clip` `"silicon"`, `volume` chosen the same way. Same cab/align rules.
- The existing test "every presets/modeled/*.json renders ..." covers them automatically; keep
  it passing.

## Acceptance (Catch2, `tests/test_pedals_zr.cpp`, tags `[pedal][zr]...`; fs = 48 kHz unless stated)
Reuse `tests/pedal_fr_util.h`, `fft_util.h`, `alloc_guard.h`, and copy (do not include from
`test_pedals.cpp`) the helpers you need: `measureThd`, `measureAliasDb`, the `Fr` struct,
`chainPreset`/`buildChain`. Small-signal FR = impulse at −90 dBFS (linear: clipper and slew
limiter have slope 1 at 0). "Difference curves" below mean `FR(settings A) − FR(settings B)` in
dB per bin, which isolates one stage exactly because every other stage is identical.

1. **Shared stages.**
   - `clipKnees` table equals §1.1; `parseClipType` round-trips all four names and rejects
     `"germanium"`.
   - `SoftSlewLimiter`, S = 0.1 per sample: a step from 0 to 10 → every consecutive output
     difference in the first 60 samples is `0.1 ± 1e-9`; a ramp of slope 0.005/sample tracks
     within `1e-4` after 200 samples; input identically 0 → output 0.
   - `DryDelay` set to 50 → impulse comes out at index 50, bit-exact; chunking 1/7/64 gives
     identical output.

2. **`pedal.mz` frequency response** (other knobs at default unless stated).
   - **Mid sweep moves the peak.** For `midFreq` ∈ {0, 5, 10}, `D = FR(mid 10) − FR(mid 5)`:
     the argmax of `D` over 100 Hz–10 kHz lies within ±5 % of 200 / 1000 / 5000 Hz, and
     `D(fM) ∈ [14, 16]` dB. Also `FR(mid 0) − FR(mid 5)` at fM ∈ [−16, −14] dB.
   - **Mid Q.** `D_q = FR(mid 10, midQ 10) − FR(mid 5, midQ 10)` at 1 kHz vs the same with
     `midQ` 0: the half-gain bandwidth (where D crosses 7.5 dB) at `midQ` 10 is at least 4×
     narrower than at `midQ` 0.
   - **Low shelf.** `FR(low 10) − FR(low 5)`: ≥ +11 dB at 50 Hz, ≤ +1 dB at 1 kHz.
     `FR(low 0) − FR(low 5)`: ≤ −11 dB at 50 Hz.
   - **High shelf.** `FR(high 10) − FR(high 5)`: ≥ +11 dB at 10 kHz, ≤ +1 dB at 400 Hz.
   - **Roll-off.** `FR(rolloff 10) − FR(rolloff 0)` at 8 kHz ≤ −9 dB (design ≈ −12).
   - **Tightness.** `FR(tight 10) − FR(tight 0)` at 80 Hz ≤ −10 dB (design ≈ −14), and
     within ±1 dB at 2 kHz.
   - **Emphasis.** `FR(emphasis 10) − FR(emphasis 0)` ≥ +6 dB at 3 kHz and ≤ −6 dB at 300 Hz.
   - **Scoop preset character.** With `low` 7, `high` 7, `mid` 0, `midFreq` 5, `dist` 7:
     |H(1 kHz)| is ≥ 4 dB below |H(150 Hz)| and ≥ 6 dB below |H(4 kHz)| (lead estimates ≈ 6
     and ≈ 8 dB: the pre-clip emphasis peak, +10 dB near 1 kHz, offsets part of the −15 dB
     scoop in the small-signal response; at playing level the clippers flatten the emphasis
     and the full scoop shows). With `emphasis` 0 as well, ≥ 10 dB below both.
   - **Gain law.** `FR(dist 10) − FR(dist 0)` at 1 kHz ∈ [49, 51] dB.

3. **`pedal.rat` frequency response** (`filter` 0 unless stated, other knobs default).
   - **Filter darkens, reversed sense.** At 4 kHz, `distortion` 5:
     `FR(filter 10) − FR(filter 0) ≤ −15 dB` (design ≈ −18), and
     `FR(filter 10) < FR(filter 5) < FR(filter 0)`.
   - **Gain-dependent pre-emphasis.** `T(d) = |H(5 kHz)| − |H(500 Hz)|` at distortion `d`:
     `T(10) − T(5) ≤ −8 dB` (design ≈ −10: the GBW corner closes the top as gain rises), and
     `T(5) ≥ +2 dB`.
   - **Bass cut.** `|H(40)| − |H(500)|` ≤ −14 dB at distortion 10 (design ≈ −17) and ≤ −10 dB
     at distortion 0 (design ≈ −13).
   - **Gain range.** `|H(5 kHz)|` at distortion 10 minus at distortion 0 ≥ +30 dB (design ≈ +38).
   - **Tightness.** As mz: `FR(tight 10) − FR(tight 0)` at 80 Hz ≤ −10 dB, ±1 dB at 2 kHz.

4. **THD** (500 Hz sine; harmonics 2–20 over the fundamental; copy `measureThd`).
   - **Monotonic with gain.** Sweep `dist` (mz) / `distortion` (rat) over 0..10 at −20 and
     −40 dBFS, other knobs default (rat `filter` 0): each step ≥ previous − 0.05 dB.
     `THD(10) − THD(0) ≥ 6 dB` at −40 dBFS for both (lead estimate: mz ≈ 35 dB, rat ≈ 45 dB).
     Print the tables.
   - **Clip types order.** At −40 dBFS, mz `dist` 5 and rat `distortion` 4:
     `THD(led) + 1 dB ≤ THD(silicon) ≤ THD(soft) − 1 dB`.
   - **Asymmetry.** At max gain, −20 dBFS: H2 with `clip` `"asym"` > −60 dBc; with `"silicon"`
     < −80 dBc. Both pedals.
   - **Slew (rat).** 2 kHz sine at −20 dBFS, `distortion` 10, `filter` 0: the power of the
     harmonics above 10 kHz (7th and 9th) relative to the fundamental is ≥ 4 dB lower at
     `slew` 10 than at `slew` 0 (lead estimate ≈ 6 dB: the limiter slows the clipper's
     transitions). Also the fundamental's level changes by less than 3 dB between the two.

5. **Aliasing** (copy `measureAliasDb`: 5 kHz bin-centred, −6 dBFS, N = 32768, 1 s warm-up,
   Blackman-Harris, worst non-harmonic line in 20 Hz–20 kHz).
   - mz at `dist` 10 (rest default) and rat at `distortion` 10, `filter` 0, `slew` 5: < −80 dB.
   - rat at `slew` 10 (strongest limiter): < −80 dB.
   - With `oversample = false` and `adaa = false` through `PedalImplConfig`, the same
     measurement is > −80 dB for both (sensitivity).

6. **Latency.**
   - `latencySamples()` == 50 for both at 44.1, 48, 96 and 192 kHz, and equals the measured
     impulse peak with `flatFilters = true` (exact by construction, as Phase 7; document it in
     the test).
   - **Mix 0 is a pure delay.** With `mix` 0, `level`/`volume` 8 (0 dB), default everything
     else, a 2000-sample noise burst comes out equal to the input delayed by `latencySamples()`
     within 1e-6, for both pedals. (This also proves the dry path alignment.)
   - **Mix is linear.** For the fixture DI's first second, `render(mix 5) == 0.5·render(mix 0)
     + 0.5·render(mix 10)` within 1e-6 per sample, both pedals (the wet path is identical in
     the three renders; the level gain is applied after the mix).
   - Chain: path A = `[pedal.mz]`, B = `[]`; and A = `[pedal.rat]`, B = `[]`: impulses arrive
     aligned (reuse the Phase 7 chain-latency pattern with a flat-filter registration under
     `test.pedal_mz_flat` / `test.pedal_rat_flat`).

7. **Zero allocation** in `process()` for both pedals standalone (varied block sizes, the
   Phase 7 pattern) and in a Chain holding `pedal.mz` on A and `pedal.rat` on B, with
   `AllocGuard`.

8. **Block-size independence and determinism**: 5 s of the fixture DI through each pedal (knobs
   at the preset values) with block sizes 1, 7, 64, 512, 4096, and twice at 512: all
   bit-identical (tolerance 0).

9. **Preset round-trip and errors** (both types): parse → `toJson` → parse equal, with and
   without explicit params, including `clip` as every one of the four strings; `modelVersion:
   2`, `dist: 11`, `params.foo`, `params` not an object, `clip: "germanium"`, `clip: 1` → all
   PresetError. `toJson` writes `clip` as its string. Registry has both types, `namTrainable`.

10. Example presets render (existing test), finite output, peak within the band stated in §5.
    Full suite passes; existing goldens unchanged; `-Werror` clean; Debug ASan/UBSan clean.

## Developer plots (for the lead's report)
After the tests pass, write these CSVs with `pedal_fr` into `build/fr_zr/` (not committed) and
list the exact commands in the report:
- mz: `mid=10 midFreq=0|5|10`, `mid=0 midFreq=5`, `low=0|10`, `high=0|10`, the scoop preset
  settings, `dist=0|5|10`, `clip=led dist=10`.
- rat: `distortion=0|5|10 filter=0`, `filter=0|5|10 distortion=5`, `slew=10 distortion=10 filter=0`.
Also print, from the tests, the THD tables and the alias figures (they are the report's
numbers).

## Report back (dsp-engineer)
Files changed; design notes; latency per rate (confirm 50); the alias table (with and without
OS+ADAA, rat at slew 5 and 10); THD tables; the clip-type THD numbers; the measured slew
effect; preset level values and render peaks; the full ctest summary and commit hashes;
"Decisions / questions for lead".
