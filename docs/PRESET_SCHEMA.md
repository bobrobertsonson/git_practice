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
  "category": "Death metal",           // optional UI metadata (see Category); not tone, ignored by the chain
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
| `pedal.hm` | Modeled "Swedish chainsaw distortion" (HM-2 topology), the CHAINSAW circuit | `modelVersion` (1 or 2), `params`; see PedalHm below | yes | 50 samples (at any rate) |
| `pedal.muff` | Modeled "big fuzz" (Big-Muff-family topology), the BIG FUZZ circuit | `modelVersion` (1), `params`; see PedalMuff below | yes | 50 samples (at any rate) |
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

### PedalHm (`type: "pedal.hm"`), PedalMuff (`type: "pedal.muff"`) and PedalTs (`type: "pedal.ts"`)

Modeled pedals (DSP, no capture files). Generic UI names: `pedal.hm` = **"Swedish chainsaw
distortion"** (CHAINSAW circuit), `pedal.muff` = **"big fuzz"** (BIG FUZZ circuit), `pedal.ts` =
**"green overdrive"**. Typical `slot`: `"pedal"` for the first two, `"boost"` for the TS model, but
any slot is accepted. The player-facing description of every control is in `docs/PEDALS.md`.

```jsonc
{ "id": "a1", "type": "pedal.hm", "slot": "pedal", "bypass": false,
  "modelVersion": 2,                    // optional, default 1 (the phase 7 four-knob object)
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
| `rolloffHz` | 4000-12000 Hz | 9000 | output low-pass corner |
| `gain1Db` | -12..+12 dB | 0 | trim on the stage-1 gain |
| `gain2Db` | -12..+12 dB | 0 | trim on the interstage gain (stock +20 dB) |
| `bias` | 0-10 | 0 | asymmetry: negative knee scaled by `1 - 0.5*bias/10` on both stages |

All fixed voicing constants (pre-filter corners, mode LPF corners, interstage gain, gyrator Qs, mode
slopes) live in the `HmVoicing` table in `core/include/sawblade/pedal_hm.h`.

**Versions.** `modelVersion` 1 (the default when absent) may set only `level`, `low`, `high`,
`distortion`; any other key is a PresetError. A v1 block maps onto the v2 defaults and renders
**bit-identically** to the phase 7 implementation (golden `tests/golden/hm_chainsaw_v1.wav`,
`ts_boost_v1.wav`). `modelVersion` 2 may set any key. `toJson()` always writes `modelVersion: 2`
with every key (enums as strings), so parse -> write -> parse is exact. Any other version (0, 3)
is a PresetError.

#### `pedal.muff` parameters (`modelVersion` 1)

| key | type / range | default | meaning |
|---|---|---|---|
| `volume` | 0-10 | 5 | output level, `3*volume - 24` dB (wet only) |
| `sustain` | 0-10 | 5 | stage-A gain, `6 + 3*sustain` dB |
| `tone` | 0-10 | 5 | tone-stack blend, dark -> bright |
| `scoop` | 0-10 | 3 | extra mid notch at the stack centre, `-1.6*scoop` dB |
| `crunch` | 0-10 | 5 | clipping compression: both knees x `1.2 - 0.08*crunch` |
| `voice` | 0-10 | 5 | stack centre `860 * 2^((voice-5)/5)` Hz (430 Hz .. 1.72 kHz) |
| `tightness` | 0-10 | 0 | input high-pass as in `pedal.hm` |
| `mix` | 0-100 (%) | 100 | wet proportion |
| `clip`, `clip2` | as `pedal.hm` | `silicon`, `follow` | stage A / stage B clipper |
| `stackRatio` | 2.0-8.0 | 4.4 | stack corner ratio `fH/fL` (scoop width) |
| `rolloffHz` | 4000-12000 Hz | 10000 | output low-pass corner |
| `gain2Db` | -12..+12 dB | 0 | trim on the stage-B gain (stock +18 dB) |
| `bias` | 0-10 | 0 | asymmetry as `pedal.hm` |

`modelVersion: 2` on `pedal.muff` is a PresetError.

#### The clip table (shared by both circuits)

| `clip` | knees k+ / k- | shape order | character |
|---|---|---|---|
| `silicon` | 0.5 / 0.5 | cubic | stock diode pair |
| `led` | 1.4 / 1.4 | quintic | louder, harder knee |
| `asymmetric` | 0.5 / 0.3 | cubic | even harmonics |
| `soft` | 0.3 / 0.3 | cubic | earlier, rounder, quieter |

Cubic: `c(u) = u - u^3/(3k^2)`, saturating at `2k/3`. Quintic: `c(u) = u - u^5/(5k^4)`, saturating at
`4k/5`, slope `1 - (u/k)^4`.

#### `pedal.ts` parameters

| Param | Range | Default | Meaning |
|---|---|---|---|
| `drive` | 0-10 | 5 | feedback-loop gain, `Rd/4.7k` with `Rd = 51k + 500k*drive/10` |
| `tone` | 0-10 | 5 | 1st-order low-pass, 723 Hz .. 7.23 kHz |
| `level` | 0-10 | 5 | output level, `3*level - 24` dB |

#### Rules common to the modeled pedals

- Out of range, wrong type, an unknown enum string, an unknown key inside `params` (including
  another type's key), or a `params` that is not an object, is a PresetError (exit 3). A preset
  naming a `modelVersion` this build does not know is rejected instead of silently sounding
  different; a later re-fit that changes the sound bumps it.
- **Live parameters.** `pedal.hm` and `pedal.muff` can move every parameter while running
  (`Processor::setLiveParams`, descriptors in `BlockType::liveParams`, `Chain::setBlockLiveParams`;
  the plugin uses it). Gains ramp over 20 ms; filter coefficients are redesigned once at the next
  block start (a small step is possible); enums and the clip knees apply immediately. With no live
  change the render is bit-identical to a static build. `pedal.ts` is not live. Presets themselves
  stay static.
- Internals: the nonlinear stages run at 4x oversampling (linear-phase half-band FIRs) with
  second-order antiderivative anti-aliasing (ADAA2); aliasing is below -80 dB at maximum gain
  (see the phase 7b report for the table).
- **Latency: 50 samples at every sample rate** for every mode and clip (oversampler round trip +
  two ADAA samples, padded to a whole number of base-rate samples; IIR group delay is not counted;
  the clean-mix dry path is delayed by the same 50). It is reported through the block's
  `latencySamples()`, so `pathLatency`, `compensationDelay` and the plugin's reported latency
  include it; a bypassed block adds none. In a preset with a pedal on one path only, the other
  path is delayed by the same amount.
- `namTrainable: true` for all three (static, nonlinear, time-invariant).
- None of the models is a capture of a real unit; a later task fits them to captures and bumps
  `modelVersion` if the sound changes.
- Examples that render with files in this repo only: `presets/modeled/hm_chainsaw.json`,
  `presets/modeled/ts_boost.json` (v1) and the bank `presets/modeled/chainsaw/*.json`:
  `classic_buzzsaw`, `early_raw_demo`, `dbeat_crust`, `powerviolence_hardcore`, `grind`,
  `death_n_roll`, `modern_tight_swedish`, `blend_partner`, `custom_wall`, `modded_nasty`,
  `bass_chainsaw`, `clean_mix_texture` (CHAINSAW) and `pickle_chainsaw`, `pickle_doom_saw`,
  `pickle_into_saw` (BIG FUZZ). See `presets/README.md`.

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

**Capture cache fallback.** When a capture's `file` does not exist and `source.provider` is `"tone3000"` with `id` and
`modelId`, every core loader (tonerender, plugin, bindings) tries `<cacheRoot>/<id>/<modelId>.nam` (`.wav` for IRs).
`cacheRoot` is `$SAWBLADE_CACHE_DIR` if set, else `~/.cache/sawblade/captures` (the same as `sawblade-t3k`). `sha256` is
verified against the cached file; a mismatch is an error. A capture that is not cached either fails with the JSON path
plus "not in the capture cache either; run: sawblade-t3k resolve <preset file>".

## Category

Optional top-level `"category": "<string>"`: a label for the preset browser. It is UI metadata, not tone: the chain and
the render ignore it, and the writer writes it only when it is non-empty. Recommended values: "Death metal",
"Swedish death (HM-2)", "Black metal", "Thrash", "Doom / Sludge / Fuzz", "Hardcore / Crust", "Grind",
"Metalcore / Djent", "Nu-metal", "Prog", "Other". Every factory preset sets one.

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
  values are written to the render report. In the plugin a preset that is `auto` at load time stays `auto`
  (it re-resolves deterministically on every load); only the rig editor's RE-MEASURE writes the result back
  into the preset as `manual`, with the measured `delaySamplesB` / `invertB`.
- `manual`: use the stored values. `off`: no alignment beyond latency compensation.

## Blend

`out = (1 − blend) · A + blend · B` (linear crossfade: aligned paths are highly correlated,
so linear keeps level roughly constant; equal-power would bump the middle by up to 3 dB).

## Cab

```jsonc
"cab": { "mode": "shared",  "ir": Capture, "enabled": true }                 // live-compatible
"cab": { "mode": "perPath", "irA": Capture, "irB": Capture, "enabled": true } // studio blend
"cab": { "mode": "irMix", "irA": Capture, "irB": Capture, "mix": 0.5, "enabled": true } // two mics, one cab
```
- `shared`: the blended signal is convolved with one IR. **Live-compatible**: a no-cab NAM
  export (`blend` of the two paths before the cab) plus that IR (convolved with post EQ) is
  exact.
- `perPath`: each path is convolved with its own IR before the blend. **Studio blend**: only
  the with-cab export is exact. The UI must state this.
- `irMix`: two IRs (typically two mic shots of one cab) combined into **one** IR,
  `h = (1 − mix) · hA + mix · hB`. Each IR is loaded exactly as in `shared` (left channel,
  resampled, truncated to 2.0 s, L2-normalised when `normalize` is true); the shorter one is
  zero-padded; the sum is **not** re-normalised. `mix` is in [0, 1] (default 0.5; out of range is
  a preset error), `irA` and `irB` are both required, and the strict-key rules hold: `ir` is
  rejected in `irMix` mode and `mix` in the other modes. One convolver runs on `h` at the same
  place in the chain and with the same latency as `shared`. It is still one combined IR, so
  the no-cab export is exact (**live-compatible**).
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
  "songFile": "/path/song.mp3",         // optional, only when a song FILE was loaded (separated on this machine; exclusive with folder)
  "separationModel": "htdemucs",        // optional, only when the 4-stem fallback is chosen (default htdemucs_6s)
  "hostSync": false                     // plugin only: follow the host transport
}
```

## Derived properties (not stored; reported by tonerender / plugin)

- `liveCompatible` = `cab.mode` is `"shared"` or `"irMix"`.
- `cabMode` (render report): `"shared"`, `"perPath"` or `"irMix"`; the report's `captures` lists
  `cab.irA` and `cab.irB` for `irMix`.
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
