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

## `testdata/reference/barbaric_pleasures_cover_mix.mp3` — the cover's finished mix

MP3 192 kbps, 44.1 kHz stereo, 177.9 s, peak −1.2 dBFS, RMS −18.1 dBFS. Full instrumental
mix (guitars + bass + drums), from the same cover project as the DIs above.

**It is a matched pair with the DIs** (lead analysis, onset-pattern cross-correlation,
300 Hz–4 kHz, 5 ms hop, ±5 s search):

| DI | Best mix channel | Offset of DI within mix | Peak corr vs. median |
|---|---|---|---|
| Guitar_L | left | ≈ +190 ms | 0.168 vs 0.011 |
| Guitar_R | right | ≈ +175 ms | 0.074 vs 0.010 |

So the guitars are double-tracked and panned hard L/R, and the mix's tone is exactly what
these DIs became. This is a known-answer target for the matcher.

Caveats:
- 5 ms resolution; refine to sample accuracy before any per-note/time-aligned comparison
  (the L/R offsets may genuinely differ by ~15 ms or the estimate may be coarse).
- The mix contains bass and drums: compare in guitar-dominant bands / sections, or use
  per-band LTAS differences rather than the absolute rules. MP3 192k: ignore > 16 kHz.
- Mix bus processing (EQ/compression/limiting) is baked in; the matcher must allow a
  post-EQ/level offset and must not try to reproduce limiting in the NAM chain.

The user's YouTube link could not be fetched (blocked; also against YouTube's terms).

## `testdata/reference/barbaric_pleasures_original.mp3` — Gatecreeper original (north star)

MP3 192 kbps, 44.1 kHz stereo, 175.6 s, peak +0.25 dBFS (inter-sample overs from mastering),
RMS −12.5 dBFS (mastered loud). Full mix incl. vocals, bass, drums. Different performance
from the DIs — compare spectra/statistics only, never time-aligned.

First look (lead, 1/3-oct LTAS of the whole mix, normalized to 1 kHz):

| Region | Original | Cover mix |
|---|---|---|
| 125–315 Hz | 0 … +12 dB | +15 … +26 dB |
| 500–1250 Hz | ≈ flat, +0 … +2.6 dB (broad mid plateau) | +0 … +6.5 dB |
| 1.6–2.5 kHz | −5 dB | +1 … +2 dB |
| 3.15–5 kHz | −9 … −13 dB | −2 … −7 dB |

The original is mid-forward (plateau ≈ 500 Hz–1.25 kHz, falling from 1.6 kHz) with far
less low/low-mid than the cover. This challenges the TONE_TARGETS hypotheses "dip at
500–800 Hz" and "peak at 1–2 kHz" — but whole-mix LTAS includes vocals, bass, drums and
mastering, so it is not a guitar-only measurement. Calibrate the targets in the tone-check
phase using guitar-dominant sections (e.g. intro/instrumental passages) before changing the
rules.
