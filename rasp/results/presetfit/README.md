# presetfit over every NTM session

`tools/presetfit.py` run on every Nail the Mix session that has both vocal stems
and the released mix (2026-10-12, presets as of 658b994, all seven presets
including `air`). One file per fit holds the full per-band table; the stems,
mixes and renders stay on disk and are not in git.

Score = RMS tone difference (dB) between what the producer did to the lead stem
and what the preset does to it, over the bands where the vocals are audible in
the mix. Lower is closer. Level doesn't count, only tone relative to 1 kHz.

| Session | Lead | Closest | 2nd | `huge` | `air` | `dark` |
|---|---|---|---|---|---|---|
| Bloodbath | VOC MAIN 1 | bright 0.7 | huge 0.8 | 0.8 | 2.2 | 1.9 |
| Reflections | Vox Lead 1 | **air 1.0** | huge 2.3 | 2.3 | 1.0 | 9.4 |
| At the Gates | LV 1 | bright 1.0 | raw 1.7 | 2.1 | 3.7 | 6.2 |
| Knocked Loose | MAIN 1 | live 1.2 | natural 1.2 | 2.5 | 3.8 | 1.5 |
| Jinjer (clean) | Vox Cln main | huge 1.2 | bright 2.1 | 1.2 | 3.7 | 7.6 |
| Gojira | VOX 1 | huge 1.6 | bright 1.7 | 1.6 | 4.2 | 6.7 |
| Vesta Collide (sung) | Sing Main1 | **air 1.6** | huge 3.9 | 3.9 | 1.6 | 12.8 |
| Allegaeon | Vocals 1 | huge 1.8 | live 2.1 | 1.8 | 2.8 | 2.6 |
| Sylosis | MAIN VOCAL | bright 1.8 | huge 2.0 | 2.0 | 4.3 | 6.5 |
| Jinjer (extreme) | Vox Extreme main | huge 1.9 | bright 2.0 | 1.9 | 3.6 | 7.6 |
| Vesta Collide (scream) | Scream Main C1 | **air 2.8** | huge 6.2 | 6.2 | 2.8 | 15.0 |
| Immortal Disfigurement | Vox Shane Lead | **air 2.9** | huge 6.0 | 6.0 | 2.9 | 15.1 |
| Veil of Maya | LD VOCAL | bright 3.2 | huge 3.7 | 3.7 | 4.5 | 7.1 |

No fit (too few bands where the vocal is audible in the mix): Cognizance,
Meshuggah, Nothing More, Opeth.

Not run: The Haunted (its lead doesn't line up with any mix on disk), Trees on
Mars (instrumental), Daath, Face Yourself, Slamadeus (zips only, no mix).

## What it says

- `huge` or `bright` is first or second in 12 of the 13 fits (all but Knocked Loose); they're the
  safe default for most heavy leads.
- `air` wins the modern, very bright productions (Immortal Disfigurement,
  Vesta Collide both leads, Reflections) by 1.3–3.4 dB, but ranks fourth to last
  everywhere else. It's a style preset, not a new default.
- `dark` is last or second-last in 12 of 13. The exception is Knocked Loose
  (third, 1.5 dB), where the top three are within 0.3 dB.
- Knocked Loose fits darker than in the earlier four-producer run. This sweep
  feeds all 20 vocal stems (MAIN, DBL, the OD/DIST parallels, VERB, MEGA), so
  the producer's top end is credited to the distortion parallels rather than
  the main stem.
- When the spread between presets is under ~1.5 dB (Bloodbath, Allegaeon,
  Knocked Loose), the session doesn't discriminate between presets much.

## Rerunning one

The header of each file gives the mix, lead and other stems. For example:

```sh
python3 rasp/tools/presetfit.py --presets huge,natural,dark,bright,live,raw,air \
  --mix Sylosis-Poison-For-The-Lost.mp3 --lead "47 MAIN VOCAL.wav" \
  "48 BACKING VOCAL LEFT.wav" "49 BACKING VOCAL RIGHT.wav" ...
```
