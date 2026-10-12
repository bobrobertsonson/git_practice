# Phase 3.4: stop matching cymbals (fizz fix)

## Why
User verdict (2026-10-03): each newer match sounds fizzier and less polished than the
earlier ones; the first hand-built render (v0) sounded best.

Lead measurement (5-8 kHz, 8-12 kHz and 12k+ are levels in dB relative to 1-3 kHz; flatness
is the median spectral flatness from 5 to 10 kHz while the guitars are playing):

| Signal | 5-8 kHz | 8-12 kHz | 12k+ | flatness |
|---|---|---|---|---|
| Original song, full-mix side channel (current matcher target) | -13.4 | -17.0 | -20.9 | 0.189 |
| Original song, demucs guitar stem (`other`), side | -16.8 | -45.9 | -49.4 | 0.010 |
| Original song, demucs guitar stem, mid | -18.9 | -42.6 | -46.4 | 0.022 |
| v0 hand-built render | -17.5 | -44.2 | -74.5 | 0.014 |
| v3 match (current HEAD) | -13.4 | -27.5 | -49.3 | 0.169 |

In the side channel, the demucs drum stem is about level with the guitar at 5-8 kHz and
**28 dB louder at 8-12 kHz**. Above about 5 kHz the matcher has been fitting cymbals, so it
rewards noisy, fizzy chains. The real guitars on the record are smooth up top, about as
smooth as v0. The v2 tone targets were also calibrated on the side channel, so their
`fizz` group (-15.5) is contaminated too.

## Changes (match-engineer)
1. **Reference basis.**
   - Default to the separated guitar stem (htdemucs `other`) whenever separation is available
     or cached. The guitars may be mid or side; use the stem's side channel when the
     guitars are hard-panned (current calibrate logic), otherwise its mid.
   - The full-mix side channel stays as a fallback only. When it is used, the loss ignores
     bands above 4.5 kHz and applies a one-sided ceiling there instead: the render may be
     darker than the reference, never brighter.
   - `result.json` records the basis and any band limit.
2. **High-frequency texture term in the loss.**
   - Add the difference in 5-10 kHz spectral flatness between render and target (playing
     frames only) and the 8-12 kHz level difference.
   - Weight it so that v3's chain scores clearly worse than v0's against the stem target.
     Add a test with those two renders, or synthetic stand-ins.
3. **Search space.** Add a post-EQ high-shelf (3-7 kHz, -8 to 0 dB) and a post-EQ low-pass
   (5-12 kHz, 12 dB/oct) to stage-2 refinement. A real cab-plus-mic roll-off is part of
   these tones.
4. **Calibration.**
   - Rerun `calibrate` on the original song's guitar stem and report the measured group
     values (sub through fizz) next to the v2 values.
   - Do not edit the profiles: the lead approves new values. Add a `fizz_texture` rule to
     the tonecheck rule set (flatness ceiling), with its value marked
     `pending lead approval` until the lead approves it.
5. **Reruns.**
   - Rerun (c), cover DI → original song, and (b), cover DI → cover mix (stem basis for
     both). Write the listening files as `matched_to_ORIGINAL_v4.mp3` and
     `matched_to_COVER_MIX_v3.mp3` in the run dirs.
   - Report the table above for v4 and v3.

## Acceptance
- On the stem basis, v4's 5-8 kHz, 8-12 kHz and flatness figures are within 2 dB (levels)
  and 0.03 (flatness) of the stem target.
- The full pytest suite passes. Tests use no network and no demucs: use cached or synthetic
  stems.
- Another engineer (phase 4 export) has uncommitted work in `match/`. Stage and commit only
  your own files.
