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
  "output": { "gainDb": 0.0 }          // optional
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
  "rangeDb": -90.0        // attenuation when fully closed
}
```
Defaults are the values shown, with `enabled: false` if the object is omitted.
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

Block types in v1:

| `type` | Purpose | Type-specific fields |
|---|---|---|
| `nam` | NAM capture (pedal, boost, amp) | see NamBlock below |
| `eq`  | Extra parametric EQ anywhere in the chain | `"bands": [ EqBand, ... ]` |

Unknown `type` values are a parse error in v1. Future types (modeled pedal recreations, e.g.
`"type": "pedal.hm"` with `"params": { ... }` and `"modelVersion": 1`) are added to the
registry without changing this schema's shape. Each registered type declares in code:
latency, and whether it is **NAM-trainable** (time-based effects — delay, reverb,
modulation, long-release dynamics — are not; the export phase refuses or bypasses them).

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

### Capture (shared by NAM models and IRs)

```jsonc
{
  "file": "captures/hm2_maxed.nam",   // required
  "sha256": "…",                      // optional; if present, verified at load (mismatch = error)
  "source": {                         // optional for local files, required for TONE3000 assets
    "provider": "tone3000",
    "id": "12345",
    "url": "https://www.tone3000.com/tones/…",
    "title": "HM-2 both knobs max",
    "creator": "someuser",
    "license": "CC-BY-4.0"            // exactly as published on TONE3000
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
  *pre-cab, post path-EQ, gate bypassed*. The lag maximizing |cross-correlation| within
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

## Derived properties (not stored; reported by tonerender / plugin)

- `liveCompatible` = `cab.mode == "shared"`.
- `exportExactness`: `{ "withCab": true, "noCab": liveCompatible }`.
- `latencySamples` per path and total.
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
