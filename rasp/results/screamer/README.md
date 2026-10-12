# Screamer factory presets vs RASP (2026-10-12)

`presets.txt`: one unprocessed multi-voice take, then the same take through
Screamer's Default and its scream / clean / shout x dark / natural / bright
presets, measured against the take with `tools/drywet.py`. Black-box
measurement only. The audio stays out of git.

EQ re 1 kHz (dB), the mean of scream / clean / shout for each tone:

| | 100 Hz | 400 Hz | 2.5 kHz | 4 kHz | 6.3 kHz | 10 kHz | 16 kHz |
|---|---|---|---|---|---|---|---|
| Screamer dark | -1.9 | -4.0 | -1.0 | +0.4 | -10.3 | -28.3 | -29 |
| RASP dark (before) | +3.2 | -1.8 | +0.8 | +1.1 | -5.7 | -7.3 | -19.8 |
| RASP dark (after) | +3.2 | -1.7 | +0.7 | +0.6 | -7.9 | -12.2 | -26.1 |
| Screamer natural | -3.1 | -4.9 | +0.9 | +1.1 | -3.3 | -10.5 | -11.6 |
| RASP natural | -0.3 | -3.1 | +2.1 | +3.7 | -1.7 | -6.0 | -14.8 |
| Screamer bright | -3.5 | -4.8 | +1.4 | +1.7 | -0.4 | -2.7 | +0.8 |
| RASP bright (before) | -19.5 | -4.1 | +3.1 | +4.5 | +0.4 | -1.5 | -3.3 |
| RASP bright (after) | -19.3 | -4.0 | +2.3 | +3.6 | +0.1 | -1.4 | -3.0 |

- **Dark:** Screamer's dark is a hard top roll-off. RASP's dark voicing used to
  add air; it now shelves -6 dB from 6 kHz. That's halfway to Screamer's, which
  is very dull on its own.
- **Bright:** Screamer has about 2 dB less 2.5-4 kHz. That matches the
  producer fits (results/presetfit), so bright presence goes from +2 to
  +0.5 dB.
- **Natural:** left alone. Screamer is 2.6 dB below RASP at 4 kHz, but the
  Mantra chains on Andrew's DI are 0.8 dB above it (results/drywet).
- **Character:** scream keeps the lows (+3 dB at 100 Hz). Clean and shout cut
  100-400 Hz by 5-7 dB, and clean is about 2 LU quieter. RASP has no character
  switch.
- **Dynamics:** in/out slope 0.26-0.37 for every preset, the same range as
  Mantra. RASP natural is 0.35-0.41 and Huge 0.23.
- **Gaps:** about -14 dB re singing for every preset, against -18 to -23 for
  RASP. Screamer's presets leave more ambience between phrases.
- **Loudness:** Screamer adds +2.6 to +5.2 LU. RASP adds about +15, because it
  levels to its own target.
