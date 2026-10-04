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
| `pedal.hm` | Modeled "Swedish chainsaw distortion" (HM-2 topology) | `modelVersion`, `params`; see PedalHm below | yes | 50 samples (at any rate) |
| `pedal.ts` | Modeled "green overdrive" (Tube-Screamer topology) | `modelVersion`, `params`; see PedalTs below | yes | 50 samples (at any rate) |

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

### PedalHm (`type: "pedal.hm"`) and PedalTs (`type: "pedal.ts"`)

Modeled pedals (DSP, no capture files). Generic UI names: `pedal.hm` = **"Swedish chainsaw
distortion"**, `pedal.ts` = **"green overdrive"**. Typical `slot`: `"pedal"` for the HM
model, `"boost"` for the TS model, but any slot is accepted.

```jsonc
{ "id": "a1", "type": "pedal.hm", "slot": "pedal", "bypass": false,
  "modelVersion": 1,                    // optional, default 1; any other value = PresetError
  "params": { "level": 5, "low": 10, "high": 10, "distortion": 10 } }   // optional; every key optional
{ "id": "b1", "type": "pedal.ts", "slot": "boost",
  "modelVersion": 1, "params": { "drive": 2, "tone": 6, "level": 8 } }
```

| Type | Param | Range | Default | Meaning |
|---|---|---|---|---|
| `pedal.hm` | `level` | 0-10 | 5 | output level, `3*level - 24` dB (0 dB at 8) |
| | `low` | 0-10 | 5 | low gyrator of the colour-mix EQ, 100 Hz peak, -12..+18 dB |
| | `high` | 0-10 | 5 | high gyrators, 1 kHz + 1.5 kHz peaks, -8..+14 dB each |
| | `distortion` | 0-10 | 5 | first-stage gain, 6..46 dB (second stage fixed +20 dB) |
| `pedal.ts` | `drive` | 0-10 | 5 | feedback-loop gain, `Rd/4.7k` with `Rd = 51k + 500k*drive/10` |
| | `tone` | 0-10 | 5 | 1st-order low-pass, 723 Hz .. 7.23 kHz |
| | `level` | 0-10 | 5 | output level, `3*level - 24` dB |

- Every param is a JSON number in [0, 10]; out of range, wrong type, or an unknown key inside
  `params` (including another type's key), or a `params` that is not an object, is a
  PresetError (exit 3). `modelVersion` is the model revision: it is `1`; a later re-fit of the
  EQ table or clip knees that changes the sound bumps it, and a preset naming a version this
  build does not know is rejected instead of silently sounding different.
- `toJson()` always writes `modelVersion` and all params (explicit defaults), so
  parse -> write -> parse is exact.
- Params are static per preset: changing one goes through the Chain rebuild/swap path, there is
  no parameter smoothing in v1.
- Internals: both run their nonlinear stages at 4x oversampling (linear-phase half-band FIRs)
  with second-order antiderivative anti-aliasing (ADAA2) on a soft clipper; aliasing is below
  -80 dB (measured around -92 .. -94 dB) at maximum gain.
- **Latency: 50 samples at every sample rate** (oversampler round trip + ADAA, padded to a whole
  number of base-rate samples; IIR group delay is not counted). It is reported through the block's
  `latencySamples()`, so `pathLatency`, `compensationDelay` and the plugin's reported latency
  include it; a bypassed block adds none. In a preset with a pedal on one path only, the other
  path is delayed by the same amount.
- `namTrainable: true` for both (static, nonlinear, time-invariant).
- Neither model is a capture of a real unit: the targets are published frequency-response
  descriptions plus engineering reasoning (see `docs/specs/phase7_modeled_pedals.md`); a later
  task fits them to captures and bumps `modelVersion` if the sound changes.
- Examples that render with files in this repo only: `presets/modeled/hm_chainsaw.json`,
  `presets/modeled/ts_boost.json`.

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
