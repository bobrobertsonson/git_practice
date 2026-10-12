# drywet results

## mantra-andrew.txt

Andrew's DI processed through Mantra, one export per setting, measured with
`tools/drywet.py` against the unprocessed DI (2026-10-12). All 30 exports are
mono, 44.1 kHz, 188 s, so the width figures can't show any stereo spread.

Dry reference: `andrew-di-dry.wav`, the clean DI (crest factor 23.4 dB vs
17.5 dB for the most dynamic processed file).

Settings below are only what each file name says; no other notes came with the
exports. Where the name is a preset name, the preset's own settings weren't
recorded.

- `andrew-di-aggressive-charlie.wav`: preset "Aggressive Charlie"
- `andrew-di-all-our-screams-have.wav`: preset "All Our Screams Have…" (name truncated)
- `andrew-di-balanced-sculpt.wav`: preset "Balanced Sculpt"
- `andrew-di-bocanada.wav`: preset "Bocanada"
- `andrew-di-clean-metal.wav`: preset "Clean Metal"
- `andrew-di-distorted-scream.wav`: preset "Distorted Scream"
- `andrew-di-german-metal.wav`: preset "German Metal"
- `andrew-di-growls.wav`: preset "Growls"
- `andrew-di-growlsy.wav`: preset "Growlsy"
- `andrew-di-its-gonna-blow.wav`: preset "It's Gonna Blow"
- `andrew-di-lead-double-fake.wav`: preset "Lead Double Fake" (fake double; output is 18.9 ms early vs the DI)
- `andrew-di-lead-heavy.wav`: preset "Lead Heavy"
- `andrew-di-lead-no-mic-pre.wav`: lead preset with the mic pre stage off
- `andrew-di-lead-vox.wav`: preset "Lead Vox"
- `andrew-di-main-vocal-and-fx.wav`: preset "Main Vocal and FX"
- `andrew-di-main-vocal.wav`: preset "Main Vocal"
- `andrew-di-main-vox-1.wav`: preset "Main Vox 1"
- `andrew-di-metal-all-rounder.wav`: preset "Metal All-Rounder"
- `andrew-di-metal-dry.wav`: preset "Metal Dry"
- `andrew-di-octave-down.wav`: octave-down layer
- `andrew-di-only-human.wav`: preset "Only Human"
- `andrew-di-pushing-the-tape.wav`: preset "Pushing the Tape"
- `andrew-di-scream-vocal.wav`: preset "Scream Vocal"
- `andrew-di-screamy-grit.wav`: preset "Screamy Grit"
- `andrew-di-thick-and-gluey.wav`: preset "Thick and Gluey"
- `andrew-di-tube-crush.wav`: preset "Tube Crush"
- `andrew-di-vintage-drive.wav`: preset "Vintage Drive"
- `andrew-di-vocal-doubler-100width-no-other-processing.wav`: vocal doubler only, width 100, everything else off
- `andrew-di-vocal-doubler.wav`: preset "Vocal Doubler"

## What RASP took from it (2026-10-12)

The 20 chains that stay mostly linear (coherence > 50% at 1 kHz), median EQ
re 1 kHz: 63 Hz -12, 100 Hz -8, 160 Hz -3, 250 Hz -1, 2.5 kHz +2.5,
4 kHz +4.5, 16 kHz -5.6 dB. Across all 29: in/out slope median 0.35
(IQR 0.28-0.43), level left between phrases 4.0 dB, gaps -21.7 dB.

RASP on Andrew's other DI (`Vocal - Main`, 150 s): Huge slope 0.09 / 2.3 dB,
louder than every Mantra chain because it drove the bus limiter. Changes:
Huge/Air output +3 -> -3 dB (slope 0.14, -0.9 LU); Air shelf 7 -> 5 dB
(16 kHz +3.9 -> +1.5). Not changed: Huge keeps ~8 dB more 100 Hz and ~4 dB
more 160-250 Hz than Mantra's median (Body +5, chosen against "thin"), pending
the A/B.

Exports were mono, so doubler/width settings couldn't be measured; stereo
exports of the doubler and harmony settings would fix that.

### Same DI, like for like (2026-10-12)

With `andrew-di-dry.wav` itself, and RASP's reverb, delay and doubles off to
match the Mantra exports (after the output change above):

| | in/out slope | phrase spread | gaps | 100 Hz | 250 Hz | 4 kHz | 16 kHz |
|---|---|---|---|---|---|---|---|
| Mantra median (IQR) | 0.35 (0.28-0.43) | 4.0 dB | -21.7 | -8 | -1 | +4.5 | -5.6 |
| RASP huge | 0.17 | 2.0 dB | -18.1 | -1 | +2 | +3.8 | -1.3 |
| RASP natural | 0.25 | 2.8 dB | -20.6 | -2 | -0.5 | +3.6 | -14.7 |
| RASP bright | 0.27 | 2.7 dB | -21.2 | -20 | -2 | +4.9 | -3.3 |

RASP is still denser than any Mantra chain but 3 (slope < 0.25), and Huge
lifts the gaps ~4 dB more. Left for now: compression was asked for in Logic,
and the stereo re-exports and A/B round 2 come first.

## mantra-andrew-stereo.txt

Stereo re-exports of the same 29 settings (`_Stereo/`, 44.1 kHz, 2 ch), with
reverb, delay and modulation off, measured against the same mono
`andrew-di-dry.wav`. File names are identical to the mono set; the only extra
file is a stereo `andrew-di-dry.wav`, which measures as the DI itself (flat EQ,
100% coherence), a check that the alignment is right.

Loudness reads about +3 LU higher than in the mono table for nearly every
file, including the stereo dry. That's the second channel adding to the
loudness sum, not a level change; subtract 3 LU to compare with
`mantra-andrew.txt`. German Metal (-4.3 vs mono) and Growls (+5.8) differ
beyond that offset.

Width (side/mid, L/R correlation): only the doubler and pitch/fake-double
chains are really stereo: Lead Double Fake -0.0 dB / 0.00 (fully
decorrelated), Vocal Doubler 100 width -3.3 dB / 0.36, Vocal Doubler
-9.4 dB / 0.79, Growls and Growlsy about -15 dB / 0.95, Lead Vox and Scream
Vocal about -21 dB / 0.99. Metal All-Rounder (-28.5) and German Metal (-33.8)
are slightly wide; everything else is below -38 dB, effectively mono.

### Stereo: RASP's doubles vs Mantra's doubler (2026-10-12)

Same DI, reverb and delay off. Side/mid by band (dB):

| | overall | 100 Hz | 1 kHz | 4 kHz | 10 kHz | 16 kHz |
|---|---|---|---|---|---|---|
| Mantra Vocal Doubler (width 100, nothing else) | -3.3 | -1.7 | -3.4 | -3.4 | -3.4 | -3.3 |
| Mantra Vocal Doubler (preset) | -9.4 | -8.6 | -9.4 | -9.7 | -9.6 | -9.8 |
| RASP huge, darken 0.3 (old default) | -2.6 | -7.6 | -2.5 | -3.0 | -7.5 | -15.0 |
| RASP huge, darken 0.1 (new default) | | -7.6 | -2.5 | -2.7 | -4.2 | -7.0 |

RASP's doubles were already as wide as Mantra's at its widest; what differed
is that Mantra keeps the width flat to 16 kHz, where double-tracking is heard
(consonants, fizz), and ours closed above ~6 kHz. Darken default 0.3 -> 0.1.
Lows stay narrower on purpose (mono-compatible low end).

Lead Double Fake is fully decorrelated (L/R correlation 0.00, no center): two
fake takes hard-panned with nothing in the middle. RASP has no such mode yet.
