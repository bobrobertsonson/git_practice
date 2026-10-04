# The chainsaw pedal

Sawblade's modeled pedal is **one pedal** with a **CIRCUIT** switch. Each circuit is a DSP
model, not a capture, so every knob moves live. The schema is in `docs/PRESET_SCHEMA.md`
(PedalHm / PedalMuff); this page says what the controls do to the sound.

| circuit | long name | block type | status |
|---|---|---|---|
| CHAINSAW | Swedish chainsaw distortion | `pedal.hm` | ready |
| BIG FUZZ | big fuzz | `pedal.muff` | ready |
| (modded chainsaw) | modded-chainsaw class: decoupled mids, presence, 3-way clip, boost, clean blend, 4-band EQ | `pedal.hmx` | coming (phase 7c) |
| (one-knob chainsaw) | one-knob chainsaw | `pedal.eye` | coming (phase 7c) |

Both models are starting points built from published descriptions, not fitted to a real unit.
A capture-based fit comes later (the fixed constants are in one table, `HmVoicing`).

## CHAINSAW circuit
Gain stage, two clippers, a "colour mix" EQ with a low and a high band, a presence peak and a
roll-off. Stock settings are the middle of every range.

- **LOW**: the low gyrator, 100 Hz. Up for thickness and chest, down for tightness. At 0 the low
  band is cut 12 dB; at 10 it is boosted 18 dB.
- **HIGH**: two overlapping mid bands around 1 to 1.5 kHz. This is the "bark". Up for cut and
  buzz, down for a hollow, dark sound.
- **DIST**: gain into the first clipper. Stock range is 6 to 46 dB. Above about 5 the tone is
  fully saturated; the knob then mostly changes how much the notes compress and fizz.
- **TIGHT** (`tightness`): high-pass before the clippers, 20 Hz to 200 Hz. Raise it when the low
  end is flubby or the riff is fast; it also takes the sub out of what the clippers see.
- **OUT** (`level`): output level. The EQ is a big boost, so the stock "unity" is around 2 to 5.
- **MIX**: wet proportion. Below 100 %, the clean DI (delayed to match the pedal) runs underneath.
  Reach for 30 to 50 % for bass, or as a texture layer.
- **LOW HZ / LOW Q**: where and how wide the low band is. Wide (0.8) for fat, narrow (1.6 and up)
  for a focused thump. 60 Hz suits bass; 130 Hz suits grind.
- **HIGH HZ / SPREAD**: where the mid bark sits and how far apart the two bands are. Spread 1.0
  stacks them into one peak; 2.0 widens the bark.
- **PRES HZ / PRES dB**: the top-end peak. More for cut and bite, lower frequency for crust.
- **ROLL-OFF**: output low-pass. Open it (12 kHz) for a fizzy, nasty top; close it (4 to 6 kHz)
  for a dark, safe tone.
- **STAGE 1 / STAGE 2**: trims on the two gain stages, plus or minus 12 dB. Stage 1 changes how
  hard the first clipper is hit (tightness and attack); stage 2 changes the compression.
- **BIAS**: asymmetry. Adds even harmonics and a rougher, less hi-fi tone.
- **MODE**: `stock`; `custom` (more low and more gain, like a modern reissue's second mode);
  `modded` (brighter top: interstage and post-clip filters opened to 9 kHz).
- **CLIP / CLIP 2**: the clipper of each stage (below). CLIP 2 `follow` uses the first one.

## BIG FUZZ circuit
Two cascaded clipping stages into a passive tone stack, with extras for a scooped buzz.

- **SUSTAIN** (`sustain`): gain, 6 to 36 dB into stage A. Fuzz sounds from 3 up; for a chainsaw go to 8 to 10.
- **TONE**: the stack blends a dark low-pass with a bright high-pass; the middle is a deep
  scoop around 860 Hz. Turn to the left for doom darkness, to the right for bite.
- **SCOOP**: an extra mid notch (up to 16 dB more). Raise it for the hollow, scooped buzz.
- **CRUNCH**: clipping compression. Higher means the clippers engage earlier and harder: tighter, more compressed.
- **VOICE**: shifts the stack's centre from 430 Hz (low, doom) to 1.72 kHz (high, nasal).
- **WIDTH** (`stackRatio`): how wide the mid notch is. 2 is narrow, 8 is a wide valley.
- **TIGHT, MIX, OUT** (`volume`): as on CHAINSAW.
- **ROLL-OFF**: the final low-pass. **STAGE 2**: trim on stage B. **BIAS**: asymmetry. **CLIP / CLIP 2**: as below.

## The four clips
| clip | what you hear |
|---|---|
| `silicon` | the stock diode pair: balanced, medium level |
| `led` | louder, firmer knee: more open and aggressive, keeps dynamics at moderate gain |
| `asymmetric` | even harmonics, rougher and "tube-like" or fuzzy; use with BIAS for more |
| `soft` | earlier, rounder, quieter: germanium-like, for doom and texture layers |

At high gain every clip is close to a square wave and the differences shrink; they show most
when you play softly or turn the gain down.

## Face and drawer (plugin)
The pedal face shows six knobs per circuit plus CIRCUIT, CLIP and FOCUS switches; the advanced
drawer (double-click the pedal) shows the rest. The mapping is a table in
`plugin/src/pedals/CircuitFaces` (phase 7b task B).

| circuit | face knobs | CLIP | FOCUS | drawer |
|---|---|---|---|---|
| CHAINSAW | LOW, HIGH, DIST, TIGHT, OUT, MIX | `clip` | `lowQ` wide 0.8 / narrow 1.6 | LOW HZ, LOW Q, HIGH HZ, SPREAD, PRES HZ, PRES dB, ROLL-OFF, STAGE 1, STAGE 2, BIAS; MODE, CLIP 2 |
| BIG FUZZ | SUSTAIN, TONE, SCOOP, TIGHT, OUT, MIX | `clip` | `stackRatio` wide 4.4 / narrow 2.5 | CRUNCH, VOICE, WIDTH, ROLL-OFF, STAGE 2, BIAS; CLIP 2 |

## The fifteen starting presets (`presets/modeled/chainsaw/`)
All render from repo files only (identity cab); the notes name the suggested amp and cab from
`presets/CAPTURE_SHORTLIST.md`.

| preset | circuit | the sound |
|---|---|---|
| Classic Buzzsaw | CHAINSAW | all-tens Swedish buzzsaw, low output |
| Early Raw Demo | CHAINSAW | lower gain, more mid, demo-tape rawness |
| D-Beat Crust | CHAINSAW | less low, cutting presence, tight |
| Powerviolence Hardcore | CHAINSAW | tight low, maximum gain, narrow low focus |
| Grind | CHAINSAW | less sub, more presence and top |
| Death 'n' Roll | CHAINSAW | looser, lower gain, fat low mids |
| Modern Tight Swedish | CHAINSAW | controlled low end, strong presence |
| Blend Partner | CHAINSAW | saw path for the two-path blend: lows down, level up |
| Custom Wall | CHAINSAW | custom mode: extended low and gain |
| Modded Nasty | CHAINSAW | modded mode and LED clip: bright and nasty |
| Bass Chainsaw | CHAINSAW | 60 Hz low focus, 40 % mix keeps the clean low end |
| Clean Mix Texture | CHAINSAW | soft clip at 30 % mix over another tone |
| Big Fuzz Chainsaw | BIG FUZZ | scooped, saturated fuzz buzz with no chainsaw circuit in it |
| Big Fuzz Doom Saw | BIG FUZZ | low-voiced doom and sludge saw, dark and soft-clipped |
| Fuzz Into Saw | BIG FUZZ then CHAINSAW | a mild fuzz pushing the chainsaw: thicker, more compressed |

Two or more circuits can sit in a chain; the plugin's face controls the first circuit block.
