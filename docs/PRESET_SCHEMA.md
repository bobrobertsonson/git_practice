# Preset schema — `sawblade.preset` v1

A preset is one JSON document. Plugin state **is** the preset; `tonerender` renders exactly
what the plugin will play. All unknown keys are rejected (strict parsing) so typos fail loudly.
Readers must reject `version` greater than they support and migrate lower versions.

## Conventions

- Gains in dB, frequencies in Hz, times in ms, `q` is dimensionless.
- File paths are resolved **relative to the preset file's directory** unless absolute.
- Optional fields show their default; fields without a default are required.
- Sample rate: phase 1 renders at the input WAV's rate. Every NAM model's expected sample
  rate must equal it (error otherwise). IRs at another rate are resampled at load time.

## Top level

```jsonc
{
  "schema": "sawblade.preset",        // required, exact string
  "version": 1,                        // required, integer
  "name": "Gatecreeper-ish v1",        // required
  "notes": "",                         // optional free text
  "input":  { "gainDb": 0.0 },         // optional
  "gate":   { ... },                   // optional; see Gate
  "paths":  { "a": Path, "b": Path },  // required; both keys required
  "align":  { ... },                   // optional; see Align
  "blend":  0.5,                       // optional; 0 = only A, 1 = only B
  "cab":    { ... },                   // required; see Cab
  "postEq": [ EqBand, ... ],           // optional, default []
  "busComp":{ ... },                   // optional; see Bus compressor
  "output": { "gainDb": 0.0 },         // optional
  "playAlong": { ... }                 // optional; plugin UI state, see Play-along (not tone)
}
```

## Gate (keyed on the DI, before the split)

```jsonc
"gate": {
  "enabled": true,
  "thresholdDb": -55.0,   // open threshold on the DI envelope (dBFS)
  "hysteresisDb": 6.0,    // closes at thresholdDb - hysteresisDb
  "attackMs": 0.5,        // gain ramp up time
  "holdMs": 20.0,         // minimum open time after envelope falls below close threshold
  "releaseMs": 60.0,      // gain ramp down time
  "rangeDb": -90.0,       // attenuation when fully closed
  "mode": "gate",         // "gate" | "expander" (phase 3.5)
  "ratio": 4.0,           // expander only: downward ratio below the close threshold, 1.5-10
  "keyHighPassHz": 0,     // 0 = off, else 40-400; 12 dB/oct high-pass on the key signal only
  "releaseCurve": "one-pole"  // "one-pole" | "linear-db" (constant dB/ms: |rangeDb| over releaseMs)
}
```
Defaults are the values shown, with `enabled: false` if the object is omitted.
Expander mode: while closed (envelope below the close threshold, hold elapsed) the gain
follows `max(rangeDb, -(ratio-1) * (closeDb - envDb))` instead of the flat `rangeDb`;
hysteresis and hold are as in gate mode. `keyHighPassHz` filters the key only, never the audio.
`releaseMs` applies when the gain falls; with `releaseCurve: "linear-db"` the fall is a straight
dB ramp (slope `-rangeDb/releaseMs` dB per ms) toward the target. Defaults reproduce v1 behaviour
exactly. Out-of-range values, or `keyHighPassHz` in (0, 40), are preset errors (exit 3).
Envelope detector: peak follower with 0.1 ms attack and 10 ms release (fixed in v1).
Gate is never part of any NAM export.

## Path

```jsonc
{
  "role": "saw",                 // "saw" | "body" (informational; UI + matcher hints)
  "enabled": true,               // disabled path outputs silence (still latency-compensated)
  "preEq": [ EqBand, ... ],      // default []
  "blocks": [ Block, ... ],      // 0..8 blocks, processed in order (the pedal chain)
  "eq": [ EqBand, ... ],         // path EQ after the blocks, default []
  "levelDb": 0.0,                // path output trim before blend
  "invert": false                // manual polarity flip (applied before auto-align)
}
```
Conventional layout: path A = `[pedal, amp]`, path B = `[boost?, amp]`.

### Block (modular chain element)

Every block shares these fields; the rest depend on `type`:

```jsonc
{
  "id": "a1",            // required; unique within the preset; stable handle for UI/automation
  "type": "nam",         // required; key into the block registry
  "slot": "pedal",       // optional label: "pedal" | "boost" | "amp" | "fx" (UI + matcher hint)
  "bypass": false
}
```

Block types:

| `type` | Purpose | Type-specific fields | NAM-trainable | Latency |
|---|---|---|---|---|
| `nam` | NAM capture (pedal, boost, amp) | see NamBlock below | yes | the model's |
| `eq`  | Extra parametric EQ anywhere in the chain | `"bands": [ EqBand, ... ]` | yes | 0 |
| `pedal.hm` | Modeled "Swedish chainsaw distortion" (HM-2 topology), the CHAINSAW circuit | `modelVersion` (1, 2 or 3), `params`; see PedalHm below | yes | 50 samples (at any rate) |
| `pedal.muff` | Modeled "big fuzz" (Big-Muff-family topology), the BIG FUZZ circuit | `modelVersion` (1), `params`; see PedalMuff below | yes | 50 samples (at any rate) |
| `pedal.ts` | Modeled "green overdrive" (Tube-Screamer topology) | `modelVersion`, `params`; see PedalTs below | yes | 50 samples (at any rate) |
| `pedal.hmx` | Modeled "modded chainsaw distortion" (modded HM-2 class) | `modelVersion`, `params`; see PedalHmx below | yes | 50 samples (at any rate) |
| `pedal.eye` | Modeled "one-knob chainsaw" | `modelVersion`, `params`; see PedalEye below | yes | 50 samples (at any rate) |

Unknown `type` values are a parse error. New block types (further modeled pedal recreations)
are added to the registry without changing this schema's shape: they use `"params": { ... }`
and `"modelVersion": N`. Each registered type declares in code: latency, and whether it is
**NAM-trainable** (time-based effects — delay, reverb, modulation, long-release dynamics — are
not; the export phase refuses or bypasses them). The modeled pedals are DSP models, not
captures: they are static, nonlinear and time-invariant, so they are NAM-trainable, and they
carry no TONE3000 license or creator. UI names are generic descriptors (no trademarks).

### NamBlock (`type: "nam"`)

```jsonc
{
  "id": "a1", "type": "nam", "slot": "pedal",
  "bypass": false,
  "inputGainDb": 0.0,            // into the model
  "outputGainDb": 0.0,           // after the model
  "normalizeLoudness": false,    // if true and the model has metadata.loudness, add
                                 //   (-18 - loudness) dB after the model
  "model": Capture
}
```

### PedalHm (`type: "pedal.hm"`), PedalMuff (`type: "pedal.muff"`) and PedalTs (`type: "pedal.ts"`)

Modeled pedals (DSP, no capture files). Generic UI names: `pedal.hm` = **"Swedish chainsaw
distortion"** (CHAINSAW circuit), `pedal.muff` = **"big fuzz"** (BIG FUZZ circuit), `pedal.ts` =
**"green overdrive"**. Typical `slot`: `"pedal"` for the first two, `"boost"` for the TS model, but
any slot is accepted. The player-facing description of every control is in `docs/PEDALS.md`.

```jsonc
{ "id": "a1", "type": "pedal.hm", "slot": "pedal", "bypass": false,
  "modelVersion": 3,                    // optional, default 1 (the phase 7 four-knob object); 3 = calibrated voicing
  "params": { "level": 2, "low": 10, "high": 10, "distortion": 10, "mode": "stock", "clip": "silicon" } }
{ "id": "a1", "type": "pedal.muff", "slot": "pedal", "modelVersion": 1,
  "params": { "sustain": 10, "tone": 7, "scoop": 8, "volume": 4 } }
{ "id": "b1", "type": "pedal.ts", "slot": "boost",
  "modelVersion": 1, "params": { "drive": 2, "tone": 6, "level": 8 } }
```

#### `pedal.hm` parameters

| key | type / range | default | meaning |
|---|---|---|---|
| `level` | 0-10 | 5 | output level, `3*level - 24` dB (0 dB at 8); wet path only |
| `low` | 0-10 | 5 | low gyrator gain, `-12 + s_low*low` dB |
| `high` | 0-10 | 5 | both high gyrators, `-8 + 2.2*high` dB |
| `distortion` | 0-10 | 5 | stage-1 gain, `6 + s1*distortion + gain1Db` dB |
| `tightness` | 0-10 | 0 | input high-pass `20 * 10^(tightness/10)` Hz (20-200 Hz); the dry signal is tapped before it |
| `mix` | 0-100 (%) | 100 | wet proportion; `out = (1-m)*dry[n-50] + m*level*wet` |
| `mode` | `stock` \| `custom` \| `modded` | `stock` | `custom`: `s_low` 3.6, `s1` 4.6 (more low and gain); `modded`: interstage and post-clip LPFs at 9 kHz |
| `clip` | `silicon` \| `led` \| `asymmetric` \| `soft` | `silicon` | clipper of stage 1 (and stage 2 while `clip2` is `follow`) |
| `clip2` | `follow` \| the four clip names | `follow` | clipper of stage 2 |
| `lowFreq` | 60-160 Hz | 100 | low gyrator centre |
| `lowQ` | 0.5-2.0 | 0.8 | low gyrator Q (wide 0.8, narrow 1.6) |
| `highFreq` | 800-2000 Hz | 1000 | high gyrator A centre |
| `highSpread` | 1.0-2.0 | 1.5 | gyrator B centre = `highFreq * highSpread` |
| `presenceFreq` | 3000-7000 Hz | 4800 | presence peak centre (Q 2) |
| `presenceDb` | 0-16 dB | 8 | presence peak gain |
| `rolloffHz` | v2: 4000-12000 Hz, v3: 4000-16000 Hz | v2: 9000, v3: 16000 | output low-pass corner |
| `gain1Db` | -12..+12 dB | 0 | trim on the stage-1 gain |
| `gain2Db` | -12..+12 dB | 0 | trim on the interstage gain (stock +20 dB) |
| `bias` | 0-10 | 0 | asymmetry: negative knee scaled by `1 - 0.5*bias/10` on both stages |
| `customLowDb` | 0-8 dB, **v3 only** | 3.2 | `custom` mode: low shelf at 100 Hz (Q 0.7); a PresetError on v1 / v2 blocks; preset-static (not a live parameter, not on the face or drawer) |
| `customHighDb` | 0-8 dB, **v3 only** | 3.0 | `custom` mode: high shelf at 6 kHz (Q 0.7); same rules |

All fixed voicing constants (pre-filter corners, mode LPF corners, interstage gain, gyrator Qs, mode
slopes) live in the `HmVoicing` table in `core/include/sawblade/pedal_hm.h`.

**Versions.** `modelVersion` 1 (the default when absent) may set only `level`, `low`, `high`,
`distortion`; any other key is a PresetError. A v1 block maps onto the v2 defaults and renders
**bit-identically** to the phase 7 implementation (golden `tests/golden/hm_chainsaw_v1.wav`,
`ts_boost_v1.wav`). `modelVersion` 2 (the phase 7b voicing) may set any key but the two custom
trims. `modelVersion` 3 (phase 7c part 3, the calibrated voicing) accepts every v2 key plus
`customLowDb` / `customHighDb`. `toJson()` writes the **stored** version (v1 blocks are stored and
written as 2; v3 as 3) with every key (enums as strings), so parse -> write -> parse is exact.
A block built from defaults (`HmParams{}`, the plugin's CIRCUIT switch) is **v3**; the 7b bank in
`presets/modeled/chainsaw/` stays at v2 and renders bit-identically. Any other version (0, 4) is a
PresetError.

#### The v3 voicing (`HmVoicing::v3()`; provenance: measured in phase 7.1 against 18 real captures)
Every v3 number lives in the one `HmVoicing::v3()` table in `core/src/pedal_hm.cpp`.

| constant | v2 | v3 | provenance |
|---|---|---|---|
| stage-1 gain | `6 + 4*D` dB (6-46) | `26 + 2*D` dB (26-46) | free fits land at D ~ 10 for every real D label: saturated from D 2 |
| `silicon` knees (stage 1 / stage 2) | 0.5 / 0.5 on both | stage 1 k+ 0.5 / k- 0.5; stage 2 k+ 0.5 / k- 2.10 (spec §3.8 item 1) | asymmetric diode clip: real H2 ~ -9, H3 ~ -18 dBc; k- fitted by the scan in `tests/test_pedals_v3.cpp` (stage 2 only: both stages alias -73 dB) |
| output DC block | none | 10 Hz 1st-order HPF | the asymmetric clip makes DC |
| fit bands (fixed) | none | low shelf 85 Hz +1.7 dB Q 0.707; peak 683 Hz +4.5 dB Q 2.4; peak 5.5 kHz -12.0 dB Q 1.54 | free-cascade residual fit, 8 labelled stock units, 1.36 dB RMS |
| post-clip LPF (4th order) | stock / custom 6.5 kHz, modded 9 kHz | stock / custom 9.5 kHz, modded 11 kHz | model was ~12 dB short at 10 kHz |
| interstage LPF, `modded` | 9 kHz | 6.5 kHz | 11 kHz would alias at -67..-72 dB (budget -80); amended in phase7c spec §3.8 item 2 (post-clip LPF stays 11 kHz) |
| `rolloffHz` default | 9000 | 16000 | the fit ran to its 14 kHz bound |
| `custom` mode | `s1` 4.6, `sLow` 3.6 | stock gain; +2.5 dB output; `customLowDb` shelf 100 Hz; `customHighDb` shelf 6 kHz; k- pulled 25 % toward k+ | four standard / custom pairs agree |

  `pickle_into_saw` (BIG FUZZ). See `presets/README.md`.

### PedalHmx (`type: "pedal.hmx"`) and PedalEye (`type: "pedal.eye"`)

Modeled chainsaw-family pedals (DSP, no capture files, phase 7c). Generic UI names:
`pedal.hmx` = **"modded chainsaw distortion"** (circuit label MODDED SAW), `pedal.eye` =
**"one-knob chainsaw"** (circuit label ONE-KNOB SAW). They are not captures of any real unit; the
voicings are built on the pedal.hm v3 core (calibrated in phase 7.1, see above) with the deltas
below, still hypotheses to be fitted further against captures (`modelVersion`).

```jsonc
{ "id": "a1", "type": "pedal.hmx", "slot": "pedal", "modelVersion": 1,
  "params": { "level": 3, "low": 8, "lowMid": 5, "highMid": 9, "high": 7, "distortion": 9,
              "presence": 8, "tightness": 3, "mix": 80, "clip": "led", "boost": "off",
              "lowMidFreq": 5, "highMidFreq": 6, "midVoice": "stock" } }
{ "id": "a1", "type": "pedal.eye", "modelVersion": 1, "params": { "gain": 10, "level": 2, "tightness": 0 } }
```

| Type | Param | Range | Default | Meaning |
|---|---|---|---|---|
| `pedal.hmx` | `level` | 0-10 | 5 | output level on the wet path, `3*level - 24` dB |
| | `low` | 0-10 | 5 | 100 Hz peak, Q 0.8, `-12 + 3*low` dB |
| | `lowMid` | 0-10 | 5 | low-mid peak gain, Q 1.0, `2*(lowMid - 5)` dB (0 dB at 5) |
| | `highMid` | 0-10 | 5 | high-mid peak gain, Q from `midVoice` (1.2 stock), `-8 + 2.2*highMid` dB |
| | `high` | 0-10 | 5 | 1.5 kHz peak alone (decoupled from highMid), Q 1.2, `-8 + 2.2*high` dB |
| | `distortion` | 0-10 | 5 | first-stage gain `26 + 1.24*distortion` dB (+9 dB with boost; tops out at the stock D 6.2); second stage fixed +20 dB |
| | `presence` | 0-10 | 5 | high shelf at 3.5 kHz, Q 0.7071, `1.2*(presence - 5)` dB (0 dB at 5); a fixed +8 dB peak at 4.8 kHz stays |
| | `tightness` | 0-10 | 0 | input 1st-order high-pass at `20 * 10^(tightness/10)` Hz (20 Hz .. 200 Hz) |
| | `mix` | 0-100 | 100 | wet percent; the dry branch is delayed by the latency; skipped at 100 |
| | `clip` | `"silicon"` \| `"led"` \| `"asymmetric"` \| `"soft"` | `"silicon"` | clip knees, see below |
| | `boost` | `"off"` \| `"on"` (a JSON boolean is also read) | `"off"` | +9 dB ahead of the clippers; always written as the string |
| | `lowMidFreq` | 0-10 | 5 | `200 * 3^(lowMidFreq/10)` Hz (200-600 Hz; 346 Hz at 5) |
| | `highMidFreq` | 0-10 | 5 | `base * 1.6^((highMidFreq-5)/5)` Hz around the `midVoice` base (stock: 625 Hz-1.6 kHz; 1 kHz at 5) |
| | `midVoice` | `"stock"` \| `"low"` \| `"high"` | `"stock"` | HIGH-MID base centre and Q: stock 1000 Hz / 1.2, low 750 Hz / 1.4, high 2000 Hz / 1.2 (`HmxVoicing::kMidVoices`); live index 13 (last) |
| `pedal.eye` | `gain` | 0-10 | 5 | drive `D = 3 + 0.5*gain` (3-8) of the pedal.hm v3 core, i.e. `26 + 2*D` dB = 32-42 dB |
| | `level` | 0-10 | 5 | output level, `3*level - 24` dB |
| | `tightness` | 0-10 | 0 | as above |

| `clip` | knee k+ / k- | character |
|---|---|---|
| `silicon` | 0.5 / 0.5 in the hm v1 / v2 voicings; hmx and the v3 hm use the v3 asymmetric knees (stage 1 0.5 / 0.5, stage 2 0.5 / 2.10) | stock silicon pair |
| `led` | 1.4 / 1.4, order-2 (quintic) shape | later, louder, more open |
| `asymmetric` | 0.5 / 0.3 | silicon + germanium-like pair: even harmonics when not saturated, a little DC (a 10 Hz DC block follows) |
| `soft` | 0.3 / 0.3 | earlier, rounder, quieter |

- `pedal.hmx` = the pedal.hm v3 core plus the decoupled mids, low-mid band, presence shelf, boost,
  clean blend and four fixed deltas measured against a modded unit: a +3.8 dB broad peak at 110 Hz
  (Q 0.7), -2.5 dB at 2.2 kHz (Q 1.0), no extra presence, and a gain range that tops out at the
  stock D 6.2 (`26 + 1.24*distortion` dB). Versus `pedal.hm` v3 at the same knobs it is +3.3 dB at
  110 Hz and -3.0 dB at 2.2 kHz (both re 400 Hz). `pedal.eye` **is** the v3 hm core at fixed
  knobs L 6.2 / H 7.1 (an HM-2-family fixed-knob circuit: the phase 7.1 fits of two real units
  land at D 6.6-7.4); it has no `mix`, no clip choice and silicon clipping.
- `mix`: `out = (1 - m)*dry + m*levelGain*wet`, `m = mix/100`. The dry signal is delayed by
  exactly the reported latency so both branches align; at `mix = 100` the dry branch is not run.
- Value rules as PedalHm/PedalTs: out-of-range, wrong type, unknown key, unknown enum value
  (`clip: "soft"`, `clip: 1`, `boost: "maybe"`) `midVoice: "mid"`, or `modelVersion` other than 1 is a PresetError;
  `toJson()` writes every key. Params are static per preset.
- **Latency: 50 samples at every sample rate** (two ADAA2 stages, as `pedal.hm`), NAM-trainable.
- Preset folders: `presets/modeled/hmx/` (7), `presets/modeled/eye/` (3) and `presets/modeled/hm_v3/` (4, pedal.hm v3).

### Capture (shared by NAM models and IRs)

```jsonc
{
  "file": "captures/hm2_maxed.nam",   // required
  "sha256": "…",                      // optional; if present, verified at load (mismatch = error)
  "source": {                         // optional; if present, provider + id are required
    "provider": "tone3000",
    "id": "12345",                    // TONE3000 tone id
    "modelId": "67890",               // optional: which model variant (size/architecture) of the tone
    "url": "https://www.tone3000.com/tones/…",   // optional (filled by `sawblade-t3k resolve`)
    "title": "HM-2 both knobs max",              // optional (filled by resolver)
    "creator": "someuser",                       // optional (filled by resolver)
    "license": "CC-BY-4.0"                       // optional (filled by resolver), exactly as published
  }
}
```
License + creator travel with every preset so exports can carry attribution.

## EqBand (RBJ biquads, cascaded in array order)

```jsonc
{ "type": "peak",      "freq": 1200.0, "gainDb": 3.0, "q": 1.0, "enabled": true }
{ "type": "lowShelf",  "freq": 120.0,  "gainDb": -2.0, "q": 0.707 }
{ "type": "highShelf", "freq": 6000.0, "gainDb": -4.0, "q": 0.707 }
{ "type": "highPass",  "freq": 80.0,   "q": 0.707 }   // 12 dB/oct
{ "type": "lowPass",   "freq": 9000.0, "q": 0.707 }   // 12 dB/oct
```
`enabled` defaults to `true`; `gainDb` ignored for pass filters. `freq` must be in
(0, 0.49·fs); `q` > 0.

## Align

Each path's latency is first compensated (shorter path delayed to match the longer).
Alignment then fixes the remaining *acoustic* offset between captures (models trained with
different latency calibration, pedal phase shifts, etc.).

```jsonc
"align": {
  "mode": "auto",          // "auto" | "manual" | "off"
  "maxLagMs": 5.0,         // auto search window ±
  "delaySamplesB": 0,      // manual (or result of auto): +n delays B, −n delays A by n
  "invertB": false         // manual (or result of auto)
}
```
- `auto`: before rendering, a fixed deterministic probe (documented in code: 1.0 s, seed 1,
  white noise at −18 dBFS, band-passed 80 Hz–5 kHz) is run through both paths
  *post path-EQ, gate bypassed, at the blend point* — i.e. pre-cab in `shared` mode, but
  including each path's own IR in `perPath` mode (different IRs carry different mic/onset
  delays, which must be aligned too). The lag maximizing |cross-correlation| within
  ±`maxLagMs` sets `delaySamplesB`; a negative peak sets `invertB = true`. The resolved
  values are written to the render report (and in the plugin, back into the preset as
  `manual`, so a preset always replays identically).
- `manual`: use the stored values. `off`: no alignment beyond latency compensation.

## Blend

`out = (1 − blend) · A + blend · B` (linear crossfade: aligned paths are highly correlated,
so linear keeps level roughly constant; equal-power would bump the middle by up to 3 dB).

## Cab

```jsonc
"cab": { "mode": "shared",  "ir": Capture, "enabled": true }                 // live-compatible
"cab": { "mode": "perPath", "irA": Capture, "irB": Capture, "enabled": true } // studio blend
```
- `shared`: the blended signal is convolved with one IR. **Live-compatible**: a no-cab NAM
  export (`blend` of the two paths before the cab) plus that IR (convolved with post EQ) is
  exact.
- `perPath`: each path is convolved with its own IR before the blend. **Studio blend**: only
  the with-cab export is exact. The UI must state this.
- IR files: mono WAV (stereo → left channel used, with a warning), any rate (resampled at
  load), truncated to 2.0 s max, normalized so the IR's L2 norm equals 1 unless
  `"normalize": false` is set on the cab object.

## Bus compressor (post EQ, before output)

```jsonc
"busComp": {
  "enabled": false,
  "thresholdDb": -12.0, "ratio": 2.0, "kneeDb": 6.0,
  "attackMs": 10.0, "releaseMs": 100.0, "makeupDb": 0.0
}
```
Feed-forward, peak detector, soft knee. Release > 150 ms is flagged in the report as
"not NAM-trainable" (export phase will refuse or bypass it).

## Play-along (`playAlong`, plugin UI state, not tone)

Written only by the plugin's state (`getStateInformation`), after the user has touched the play-along panel. It is
**not part of the tone**: the core parser accepts the object (it must be an object) and ignores its contents, never
writes it back, and so the matcher, `tonerender` and the NAM export, which all read presets through that parser, are
unaffected by it. Unknown keys inside it are ignored, wrong-typed fields fall back to their defaults and numbers are
clamped (the plugin reader never throws). Songs and their stems are never stored, only the folder path.

```jsonc
"playAlong": {
  "folder": "/home/me/stems/song",     // folder of stems (.wav / .flac); "" or missing = none
  "offsetMs": 0.0,                      // where the DI starts inside the song, ms (-10000..10000), matcher sign:
                                        // positive = the stems lead; same meaning as tonerender --backing-offset-ms
  "loop": { "on": false, "aMs": 21000.0, "bMs": 33500.0 },  // playhead time; aMs/bMs omitted when unset
  "countIn": { "on": false, "bpm": 120.0 },                 // one bar, 30..300 bpm
  "guitarMode": "mute",                 // "mute" | "ghost" | "full": what the song's own guitar stem does
  "backingLevelDb": 0.0,                // -40..+6; a state restore keeps it (the loudness suggestion is for user loads)
  "otherRole": "guitar",                // "guitar": a 4-stem `other` is the guitar | "other": keep it (KEEP KEYS)
  "hostSync": false                     // plugin only: follow the host transport
}
```

## Derived properties (not stored; reported by tonerender / plugin)

- `liveCompatible` = `cab.mode == "shared"`.
- `exportExactness`: `{ "withCab": true, "noCab": liveCompatible }`.
- `latencySamples` per path and total: processing latency only (alignment delay is part of
  the tone and reported separately as `alignDelay`).
- `align.resolved`: `{ delaySamplesB, invertB, peakCorrelation }`.

## Example

```json
{
  "schema": "sawblade.preset",
  "version": 1,
  "name": "Chainsaw + Body",
  "gate": { "enabled": true, "thresholdDb": -55 },
  "paths": {
    "a": { "role": "saw",
           "preEq": [ { "type": "highPass", "freq": 60, "q": 0.707 } ],
           "blocks": [ { "id": "a1", "type": "nam", "slot": "pedal", "model": { "file": "hm2.nam" } },
                       { "id": "a2", "type": "nam", "slot": "amp",   "model": { "file": "jcm_lowgain.nam" } } ],
           "eq": [ { "type": "lowPass", "freq": 8000, "q": 0.707 } ] },
    "b": { "role": "body",
           "blocks": [ { "id": "b1", "type": "nam", "slot": "boost", "model": { "file": "ts808.nam" } },
                       { "id": "b2", "type": "nam", "slot": "amp",   "model": { "file": "5150.nam" } } ] }
  },
  "align": { "mode": "auto" },
  "blend": 0.55,
  "cab": { "mode": "shared", "ir": { "file": "v30_4x12.wav" } },
  "postEq": [ { "type": "peak", "freq": 1500, "gainDb": 2, "q": 1.2 } ],
  "output": { "gainDb": -3 }
}
```
