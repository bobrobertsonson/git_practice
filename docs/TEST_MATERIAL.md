# Test material (not committed)

Audio lives in `testdata/` (git-ignored). Never commit it: it is third-party/band material.

## `testdata/gatecreeper_cover/` — Gatecreeper cover (user-supplied, 2026-10-03)

| File | Format | Notes |
|---|---|---|
| `Guitar_L.wav` | 44.1 kHz, 24-bit, mono, 174.9 s | DI, left double; peak ≈ −4.9 dBFS (99.99th pct), noise floor ≈ −40 dB |
| `Guitar_R.wav` | 44.1 kHz, 24-bit, mono, 174.9 s | DI, right double (L/R correlation 0.01 — true double-track); one clipped spot (19-sample full-scale run) |
| `Bass.wav` | 44.1 kHz, 24-bit, mono, 174.9 s | bass DI |
| `Drums.mid` | SMF format 0, 960 PPQ, 140 BPM | drum MIDI only (no guitar MIDI) → beat grid, not note onsets |

Implications:
- The DIs are 44.1 kHz; NAM captures are 48 kHz → rendering needs sample-rate conversion
  (T5 below). Hosts at 44.1 kHz need the same in the plugin.
- Guitar note onsets for the palm-mute metrics come from onset detection on the DI; the drum
  MIDI provides the 140 BPM grid to sanity-check them.
- The −40 dB floor (hum/hiss between notes) is a realistic gate test.
- Render L and R separately through the same preset and pan hard L/R for listening.

Reference: the user's YouTube link (the song) cannot be fetched from the build environment
(blocked; downloading YouTube audio also conflicts with its terms). Supply reference audio as
a file (your cover's mix, or a purchased/owned copy of the original) in `testdata/reference/`.
