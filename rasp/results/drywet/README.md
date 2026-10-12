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
