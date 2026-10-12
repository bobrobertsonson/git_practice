# v0.7b — NTM sessions as the primary validation and play-along set

Source: user 2026-10-12. Nail The Mix sessions (`/Users/notsch/Desktop/NailTheMix`) replace the Omega Station covers
(`docs/HANDOFF.md` "Results so far", "Open items" 1 and 4) as the primary validation and play-along material: each
session has the raw DI, the amped tracks and the finished mix of the same performance, so no cover alignment and no
demucs separation are needed.

Builds on v0.7 (`docs/specs/v0_7-genre_benchmark.md`); the file map is `docs/benchmark/cases.json` on
`claude/sawblade-v0_7-benchmark` (names only, no audio). **Scheduling:** runs start after v0.4M PHASE ACCEPT and the
v0.8 merge (the numbers depend on both); Bloodbath is already being validated by the v0.4M rerun.

## Where it runs (lead decision)

On the user's Mac, never in the cloud: the matcher needs the TONE3000 cache, OAuth tokens and IR library that live
there, CLAUDE.md and v0.7 keep this audio off the cloud, and Mac compute costs no Claude usage. The lead hands exact
commands; the user pastes text results (JSON summaries, tails), never audio.

## Licence rules (user)

NTM material is personal-use only: never commit audio, never put it in a shared artifact (the published Blend Room
embeds mp3s, so NTM play-alongs are **local-only**), never redistribute. Presets reference TONE3000 ids only.
Exports and presets are for the user's own use; artist names stay out of file names (`NTM_CATALOGUE.md`).

## Song order (style coverage; folder listing 2026-10-12)

1. **Bloodbath "Zombie Inferno"** — HM-2 death metal, two-amp blend (`bloodbath_blend` + path checks).
2. **Immortal Disfigurement** — slam / brutal death (`immortal_disfig`).
3. **Sylosis** — thrash (`sylosis_57`; `sylosis_reamp` as transfer).
4. **Veil of Maya** — djent / metalcore (`veil_of_maya`).
5. Then The Haunted, Jinjer (stem; weaker), Bloodbath MZ.

**Not available:** hardcore, doom/sludge, black metal, crust. Knocked Loose (hardcore) has no DI — loose files and
`Archive.zip` (vocals only) checked 2026-10-12 — so it stays Tier 2 reference-only; its FUZZ HI/LO + RHY + OD layers
are logged as a future fuzz-blend reference. These four styles need new sources (DI + amp tracks of the same take).

## Per song — three deliverables

### 1. Validation match (match-engineer)
- Matcher, `--thorough`: reference = the amped rhythm track(s) of the case (blend cases: the summed amp tracks; mix's
  separated guitar stem only if a session has no amp track); input = the raw rhythm DI; fit/held-out per v0.7.
- **TONE3000-only IRs** for the committed run: a matcher flag that ignores the persistent local IR dirs
  (`~/.config/sawblade/ir_dirs.json`) and `--ir-dir` (add it if absent, with a test). A local-IR run may also be made
  for comparison; its preset is not committed.
- Report: LTAS error vs reference (and the v0.4M feel terms), the chain picked, and a local A/B (reference excerpt →
  DI through the match, same bars, level-matched) in the run folder.
- Commit `presets/matched/<generic_name>.json` (TONE3000 ids only, song in `notes`) and
  `docs/reports/match_<case>.result.json` (NTM session name, DI/reference file names, offsets, scores — no audio).

### 2. Play-along (local-only)
- Backing = the session's own non-guitar tracks summed (drums, bass, vocals, FX), aligned at sample 0; fallback =
  mix minus guitar stem. Guitar = DI through the matched preset; dry DI as a third lane.
- A script writes a **local** Blend-Room-style page + audio into the run folder on the Mac (never published, never
  in git); same player controls and gains logic as `docs/listening/blendroom.html`. Track roles per session are
  listed in the case file (which stems are guitar vs backing), names only.

### 3. NAM export
- Best match exported as a standard-size NAM, nocab (live-compatible blend rule from CLAUDE.md applies), with the
  existing acceptance report (ESR, LTAS, feel terms). Calibration fields per v0.8 I4a. File stays on the Mac.

## Acceptance

Per song: result JSON + preset committed, reviewer ACCEPT on any code (IR flag, play-along script), the user's
listening verdict on the A/B and the play-along. Summary table in `docs/reports/ntm_songs.md` replacing the
HANDOFF "Results so far" role.
