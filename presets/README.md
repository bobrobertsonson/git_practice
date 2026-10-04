# Factory presets (starter set)

Tone starting points for the Gatecreeper-style north star (`docs/TONE_TARGETS.md`). EQ,
blend and dynamics values are **hypotheses** to be refined by listening and, later, by the
matcher. Every preset uses `align: auto`.

| Preset | Blend (0 = saw, 1 = body) | Cab | Use |
|---|---|---|---|
| `chainsaw_body.json` | 0.55 | shared V30 (live-compatible) | default north-star blend |
| `swedeath_saw.json` | 0.25 | shared V30 | saw-dominant, classic chainsaw |
| `tight_body.json` | 0.75 | shared V30 | fast palm mutes / D-beat |
| `studio_split.json` | 0.55 | per-path (studio blend) | wider studio tone; with-cab export only |

## Modeled-pedal examples (`modeled/`)

| Preset | Blocks | Use |
|---|---|---|
| `modeled/hm_chainsaw.json` | `pedal.hm` (everything 10) | single-path "Swedish chainsaw distortion" model |
| `modeled/ts_boost.json` | `pedal.ts` (drive 2, tone 6, level 8) into `pedal.hm` | "green overdrive" boost into the chainsaw model |

These use no TONE3000 captures (DSP models only) and a repo fixture identity IR, so they render
anywhere: `tonerender --preset presets/modeled/hm_chainsaw.json --in tests/fixtures/di_riff.wav --out out.wav`.
Starting points, not tuned tones.

## Captures to fetch (not committed)

TONE3000 captures carry their own licenses and are **not** redistributed in this repo. Put
the files in `presets/captures/` with these names (or edit the preset paths), and fill in each
capture's `source` block (id, url, creator, license) in the preset.

| File | What to look for on TONE3000 |
|---|---|
| `saw_pedal_hm2_maxed.nam` | HM-2-style pedal **alone** (pedal capture, no amp), Low/High at or near max, distortion high |
| `saw_amp_lowgain.nam` | Clean-to-low-gain amp (Marshall- or solid-state-style), **no cab** (DI/amp-only capture), edge of breakup at most |
| `body_boost_ts_tight.nam` | TS-style overdrive pedal alone: drive ~0, level high, tone ~noon |
| `body_amp_highgain.nam` | Modern high-gain amp head, **no cab**, gain moderate (~5–6/10) — the boost supplies tightness |
| `cab_4x12_v30.wav` | 4x12 with V30-type speakers, close dynamic mic, minimum-phase or tight onset |
| `cab_4x12_greenback.wav` | 4x12 with Greenback-type speakers (studio_split only) |

Rules for choosing captures: amp captures must be **amp-only** (the cab comes from the IR);
48 kHz NAM models; prefer standard WaveNet; avoid captures that include gates, reverb or
delay (they can't be removed and break NAM export).

## Render

```
./build/cli/tonerender --preset presets/chainsaw_body.json --in my_di.wav --out out.wav --report out.json
```
