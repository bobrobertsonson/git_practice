# Phase 7: first modeled pedal blocks (`pedal.hm`, `pedal.ts`) — core

## Why
CLAUDE.md roadmap, "Modular pedal chain": modeled recreations of real pedals as new block
types (DSP models, not captures). Modeled pedals give the matcher continuous knobs
(Low/High/Distortion) instead of one fixed capture per knob setting. These work for any heavy
tone: the HM model serves Swedish-style death metal and crust; the TS model serves
thrash, metalcore and djent boosts in front of a high-gain amp. Neither is hard-wired to one
preset.

UI and generic names (no trademarks): `pedal.hm` = **"Swedish chainsaw distortion"**,
`pedal.ts` = **"green overdrive"**.

## Scope (dsp-engineer)
New files in `core/`, plus registry registration, `docs/PRESET_SCHEMA.md`, tests,
`presets/modeled/`. **Do not touch** `plugin/`, `match/`, or `bindings/`, and do not change
behaviour for existing presets. Existing goldens must stay bit-identical.

### 1. Shared building blocks (core, header + source, namespace `sawblade`)
- **`Oversampler4x`**: 2x·2x cascade of **linear-phase** polyphase half-band FIRs. The
  coefficients are designed in `prepare()` (or in a constexpr table) in `double` and stored
  as `float`.
  - Requirements at every supported base rate (44.1–192 kHz):
    - passband 0–0.4167·fs (20 kHz at 48 kHz), ripple ≤ 0.05 dB for the full up+down
      round trip;
    - stopband ≥ 100 dB from 0.5833·fs (28 kHz at 48 kHz) upward, for both imaging (up) and
      decimation (down). Aliases between 20 and 24 kHz are allowed.
  - API: `prepare(maxBlock)`, `reset()`, and RT-safe `upsample(const float* in, int n,
    float* out4n)` / `downsample(const float* in4n, int n, float* out)`. Buffers are
    preallocated.
  - Linear phase is chosen over minimum-phase IIR. The reasons: constant group delay, exact
    integer latency compensation, and no phase distortion of the clipper's input. Document
    this choice in the header.
- **`AdaaClipper`**: a static piecewise-polynomial soft clipper with **2nd-order
  antiderivative anti-aliasing (ADAA2)**, run at the oversampled rate.
  - Shape, per sign with knee `k` (`k+` for u ≥ 0, `k−` for u < 0):
    `c(u) = u − u³/(3k²)` for |u| < k, and `±2k/3` beyond. This is C¹ everywhere and has
    slope 1 at 0, so small signals pass linearly.
  - Symmetric when `k+ = k−`.
  - Provide closed-form antiderivatives F1 and F2. Make each constant of integration
    continuous at ±k and at 0.
  - ADAA2 formula, with the ill-conditioning fallback (|x[n] − x[n−2]| < ε, and per-pair
    |a − b| < ε) evaluating the midpoint as in Parker/Esqueda/Bilbao 2016. Use ε = 1e-5 and
    compute in `double` internally.
  - ADAA2 adds exactly **1 oversampled sample** of delay. Account for it in latency.
- **Latency**: each pedal reports `latencySamples()` = the integer base-rate delay of the
  whole block, i.e. up FIR + ADAA + down FIR (+ any filter group delay you choose to count:
  none; IIR filters are not counted).
  - If the oversampled total is not a multiple of 4, pad with a delay line inside the
    oversampled domain so that it is.
  - The test measures it (§Acceptance 4).

### 2. `pedal.hm` — "Swedish chainsaw distortion" (HM-2 topology, simplified)
Signal flow (base rate fs, OS = 4·fs). Controls are 0–10 knobs; default 5.

| # | Stage | Rate | Design |
|---|---|---|---|
| 1 | Input buffer | fs | unity gain; 1st-order HPF 20 Hz (coupling cap) |
| 2 | Upsample | → OS | `Oversampler4x` |
| 3 | Gain stage 1 pre-filter | OS | 1st-order HPF 60 Hz, then 1st-order LPF 8 kHz (op-amp feedback cap) |
| 4 | Gain 1 | OS | `G1 dB = 6 + 4·distortion` (6–46 dB) |
| 5 | Diode clip 1 | OS | `AdaaClipper`, symmetric, k = 0.5 (silicon pair) |
| 6 | Interstage | OS | 1st-order LPF 5 kHz; gain +20 dB fixed |
| 7 | Diode clip 2 | OS | `AdaaClipper`, symmetric, k = 0.5 |
| 8 | Post-clip LPF | OS | 4th-order Butterworth LPF 6.5 kHz (two biquads) |
| 9 | Downsample | → fs | `Oversampler4x` |
| 10 | "Color mix" EQ: dual gyrator active EQ | fs | RBJ biquads, below |
| 11 | Level | fs | `levelDb = 3·level − 24` (−24 … +6 dB; 0 dB at 8) |

Stage 10, the gyrator EQ as fitted biquads (RBJ cookbook, cascaded in this order):

| band | type | f0 | Q | gain dB |
|---|---|---|---|---|
| Low gyrator | peak | 100 Hz | 0.8 | `−12 + 3·low` (−12 … +18) |
| High gyrator A | peak | 1000 Hz | 1.2 | `−8 + 2.2·high` (−8 … +14) |
| High gyrator B | peak | 1500 Hz | 1.2 | `−8 + 2.2·high` (−8 … +14) |
| HF presence peak (fixed) | peak | 4800 Hz | 2.0 | +6 |
| Output roll-off (fixed) | lowPass | 9000 Hz | 0.707 | — |

Rationale for these targets (used in place of real-unit captures, which are not available
in this session; see §Validation):
- Published HM-2 descriptions put the Color Mix Low near 100 Hz and the High as a broad
  boost spanning about 1–1.5 kHz (two overlapping gyrators).
- Both maxed gives roughly +15–20 dB relative to the 300–500 Hz trough, which produces the
  "chainsaw" scoop-and-bark.
- There is an additional upper-mid/presence peak and a steep top-end roll-off.

### 3. `pedal.ts` — "green overdrive" (Tube-Screamer-style)
Controls: `drive`, `tone` and `level`, 0–10, default 5.

| # | Stage | Rate | Design |
|---|---|---|---|
| 1 | Input buffer | fs | unity; 1st-order HPF 20 Hz |
| 2 | Upsample | → OS | |
| 3 | Pre-emphasis (feedback network) | OS | `Rd = 51k + 500k·drive/10`, `Gd = Rd / 4.7k`. Branch `v = Gd · LPF1(fc_fb)(HPF1(720 Hz)(x))` with `fc_fb = 1/(2π·Rd·51 pF)`. The LPF corner is gain-dependent: about 5.6 kHz at max, about 61 kHz at min (clamp to 0.45·OS). |
| 4 | Soft clip in feedback loop | OS | `AdaaClipper` **asymmetric**, `k+ = 0.45`, `k− = 0.30` |
| 5 | Sum | OS | `y = x + c(v)`. Clean signal plus clipped branch: the feedback-loop topology, so the gain is never below unity. |
| 6 | Downsample | → fs | |
| 7 | Tone | fs | 1st-order LPF, `fc = 723 Hz · 10^(tone/10)` (723 Hz … 7.23 kHz) |
| 8 | Output HPF | fs | 1st-order HPF 10 Hz (removes the DC from asymmetric clipping) |
| 9 | Level | fs | `levelDb = 3·level − 24` |

The mid hump comes from the HPF at 720 Hz feeding the gained branch, combined with the
low-pass tone section.

### 4. Preset schema and registry
- Register `pedal.hm` and `pedal.ts` in `BlockRegistry` (constructor, beside `nam`/`eq`),
  both with `namTrainable = true` (static, nonlinear, time-invariant).
- Block JSON:
  ```jsonc
  { "id": "a1", "type": "pedal.hm", "slot": "pedal", "bypass": false,
    "modelVersion": 1,                       // optional, default 1; any other value = PresetError
    "params": { "level": 5, "low": 10, "high": 10, "distortion": 10 } }   // optional; each key optional (default 5)
  { "id": "b1", "type": "pedal.ts", "slot": "boost",
    "modelVersion": 1, "params": { "drive": 2, "tone": 6, "level": 8 } }
  ```
  - Each param is a number in [0, 10]; out of range → PresetError (exit 3).
  - An unknown key inside `params` → PresetError (use `JsonObject` + `finish()`).
  - `toJson()` always writes `modelVersion` and all params, so round-trip is exact.
- Document both types in `docs/PRESET_SCHEMA.md`: the block-types table, a section per type
  with params/ranges/defaults, the generic UI names, latency, and `namTrainable`. Replace the
  "Future types" sentence with a description of what now exists.
- Params are static per preset; changes go through the existing Chain rebuild/swap path. No
  parameter smoothing in v1.

### 5. Example presets
- Add `presets/modeled/hm_chainsaw.json` (pedal.hm, everything 10, level set sensibly).
- Add `presets/modeled/ts_boost.json` (pedal.ts at drive 2, tone 6, level 8, in front of
  the HM, or alone).
- Both must render with `tonerender` in this container using **only files in the repo**:
  `tests/fixtures/di_riff.wav`, with an IR from `tests/fixtures/ir` if the schema requires a
  cab. They must not use TONE3000 captures.
- A test loads every `presets/modeled/*.json` and renders the fixture DI without error and
  without non-finite output.

### 6. Developer tool for the report
Add `tests/tools/pedal_fr.cpp`, built with the tests and not installed. It writes CSV
columns `freq_hz,mag_db` with the small-signal magnitude response of a pedal block at given
params. Method: impulse at −90 dBFS, 65536 samples, fs = 48 kHz, FFT magnitude, and
normalise by the impulse amplitude.
- Usage:
  `pedal_fr --type pedal.hm --param low=10 --param high=10 --param distortion=10 --out x.csv`.
- The lead renders the plots from these CSVs.

## Acceptance (Catch2, all at fs = 48 kHz unless stated; also run the latency test at 44.1 and 96 kHz)
1. **Frequency response at control extremes.** Measure the small-signal response with the
   impulse at −90 dBFS: linear, because the clipper slope at 0 is 1. Levels are relative
   to |H(400 Hz)|.
   - HM EQ section alone: matches the biquad table within ±0.5 dB at 50, 100, 400, 1000,
     1250, 1500, 4800 and 12000 Hz, for (low, high) ∈ {(0,0), (10,10), (5,5)}.
   - HM whole block, low = high = 10, distortion = 10:
     - local max in 80–130 Hz, ≥ 8 dB above 400 Hz;
     - max in 1.0–1.6 kHz, ≥ 12 dB above 400 Hz;
     - a local max in 3.5–6 kHz;
     - |H(12 kHz)| ≥ 10 dB below the 1–1.6 kHz max.
   - HM whole block, low = high = 0: |H(100)| and |H(1250)| each ≥ 6 dB below |H(400)|.
   - HM knob isolation:
     - low 0→10 changes |H(100)| by ≥ 25 dB and |H(1250)| by ≤ 1 dB;
     - high 0→10 changes |H(1250)| by ≥ 25 dB and |H(100)| by ≤ 1.5 dB.
   - TS, drive = 10, tone = 0:
     - peak in 600–1000 Hz;
     - |H(100)| ≥ 10 dB below the peak;
     - |H(5 kHz)| ≥ 12 dB below the peak.
   - TS tone: |H(4 kHz)| at tone 10 minus at tone 0 ≥ 12 dB, at drive 5.
   - TS drive = 0, tone = 5: |H(100)| ≥ 10 dB below |H(1000)|. The bass cut of the gained
     branch is always present. Lead's analytic estimate is about −13 dB.
2. **THD vs gain is monotonic.**
   - Input: 500 Hz sine at −20 dBFS, other knobs at default. THD = power of harmonics 2–20
     over the fundamental, in dB.
   - Sweep HM `distortion` and TS `drive` over 0, 1, …, 10. Each step ≥ previous − 0.05 dB,
     and THD(10) − THD(0) ≥ 6 dB.
   - TS asymmetry check: H2 at drive 10 is > −60 dBc.
3. **Aliasing.**
   - Input: 5 kHz sine, bin-centred (`f = round(5000·N/fs)·fs/N`, N = 32768), −6 dBFS, max
     gain (HM: distortion 10, low = high = 5; TS: drive 10, tone 10). Discard a 1 s
     warm-up and use a Blackman-Harris window.
   - The largest spectral line in 20 Hz–20 kHz that is not within ±200 Hz of a harmonic
     of 5 kHz must be **< −80 dB relative to the fundamental**.
   - Also assert that the same test **fails** (> −80 dB) when oversampling and ADAA are
     disabled through a test-only hook. This proves the test is sensitive. Give the hook an
     `#ifdef SAWBLADE_TESTING` or an internal-config struct; it must not appear in presets.
4. **Latency.**
   - `latencySamples()` equals the measured delay at 44.1, 48 and 96 kHz. Measure with a
     band-limited click at −90 dBFS: argmax of the cross-correlation with a reference whose
     oversampler and ADAA latency is zero. Alternatively, with the EQ flat, check that the
     peak of the impulse response falls on the reported sample, if that is exact by
     construction; document which.
   - In a Chain with path A = [pedal.hm] and path B = [] (or eq), the paths are compensated:
     an impulse through both paths arrives aligned (existing chain-latency test pattern).
5. **Zero allocation** in `process()` for both pedals, standalone and inside a Chain, using
   the existing alloc-guard harness.
6. **Block-size independence**: render 5 s of the fixture DI with block sizes 1, 7, 64, 512
   and 4096. The output is bit-identical (tolerance 0). If you cannot achieve that, state
   the documented tolerance and why (target ≤ 1e-6 abs).
7. **Determinism**: two renders of the same preset are bit-identical.
8. **Preset round-trip**:
   - parse → `toJson` → parse gives equal presets for both types, with and without
     explicit params;
   - `modelVersion: 2` → PresetError;
   - `params.distortion: 11` → PresetError;
   - `params.foo: 1` → PresetError;
   - `params` that is not an object → PresetError.
9. Example presets render (§5). The full existing suite still passes and the existing
   goldens are unchanged.
10. `-Wall -Wextra -Wpedantic -Werror` clean. Run the tests under ASan/UBSan Debug as well.

## Validation note (must appear in the report)
Real HM-2 NAM captures live only in `~/.cache/sawblade` on the main machine, so this
session cannot compare against them. Validation here rests on two things: the published
frequency-response descriptions encoded in the targets above, and engineering reasoning. A
follow-up task on the main machine should do two things. First, fit the HM EQ table and the
clip knees against captures using small-signal sweeps and THD-vs-level. Second, bump
`modelVersion` if the fitted values change the sound.

## Report back (dsp-engineer)
- Files changed and the design notes (filter lengths, measured round-trip ripple and
  stopband, latency per rate).
- The aliasing level measured for each pedal, with and without OS+ADAA.
- The THD tables.
- The full ctest summary and the commit hashes.
