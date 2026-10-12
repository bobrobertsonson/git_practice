# Nail The Mix sessions on the user's Mac (catalogue)

Source: the user's Nail The Mix subscription, downloaded to `~/Desktop/nailthemix/` (2026-10-05).
Personal mixing-education terms: private evaluation only. **Never commit or redistribute the audio,
and do not share presets/exports tuned on it as the artist's tone.** This file holds names only.

Built from `unzip -l` listings (file names, not audio). "DI?" = the session's track names say
DI. "Amp track" = the same performance through the real rig (reamp / amp / mic), which isolates
the guitar tone with no mix in the way — the strongest possible matcher target.
`(1)` duplicates exist for Cognizance, Gojira, Knocked Loose, Sylosis (zips) and some masters.

## Tier A — DI + the same performance through the real rig (known-answer tone targets)

| Session | Master mp3 | Style | Guitar tracks |
|---|---|---|---|
| Bloodbath "Zombie Inferno" (Mar 2023, 44.1k) | `Bloodbath_Zombie-Inferno.mp3` | Swedish death, HM-2 | per side L/R: **DI + HM2 AMP + UBR AMP**; centre: DI + MZ AMP; leads/solo DI + AMP. A real two-path HM-2 + high-gain blend, each path recorded separately — Sawblade's architecture, measured |
| Immortal Disfigurement "Gospel of Annihilation" (Aug 2025, 44.1k) | `Immortal-Disfigurement-Gospel-Of-Annihilation.mp3` | slam / brutal death | GTR DI L/R + GTR REAMP L/R; tempo/drum MIDI |
| Veil of Maya "Red Fur" (Feb 2025, 44.1k, 125 bpm) | `Veil-Of-Maya-Red-Fur.mp3` | djent / metalcore | GTR L/R DI + GTR L/R (amped), split + FX guitars |
| Sylosis "Poison for the Lost" (Sep 2023, 48k, 202 bpm) | `Sylosis-Poison-For-The-Lost.mp3` | thrash / metalcore | GUITAR DI A–D + "SCOTT Amp DI" A–D (by name, the amped versions; verify) |

## Tier B — guitar DIs + released master (match DI → master stem)

| Session | Master mp3 | Style | Guitar tracks |
|---|---|---|---|
| The Haunted (May 2019, 48k) | none found | Swedish thrash / death | GT 1 DI, GT 2 DI, 2 leads |
| Cognizance "The Succession of Flesh" (44.1k, 225 bpm) | `COGNIZANCE-The-Succession-of-Flesh-1.mp3` | tech death | Gtr Rhy DI ×4, Lead DI ×4, Solo DI ×2; kick MIDI |
| Trees on Mars "In the Wake" (Feb 2016, 44.1k) | `Trees-on-Mars-In-The-Wake.mp3` | prog metalcore | G DI 1–7, 2 solos; tempo MIDI |
| Reflections "Scapegoat" (Aug 2025, 44.1k) | `Reflections-Scapegoat.mp3` | djent / metalcore | GTR DI L/R; FX guitars; tempo map |
| Allegaeon (Jul 2020, 48k/32-bit) | `01-Roundabout-1.mp3` (likely) | tech / melodic death | Rhy Gtr 1–6 (DI or amped: verify), acoustic, Lead DI, Solo DI; drum MIDI |

## Tier C — guitar tracks whose names do not say DI or amp (verify from audio)

| Session | Master mp3 | Style | Guitar tracks |
|---|---|---|---|
| At The Gates "The Chasm" (May 2019, 48k, 150 bpm) | `06-The-Chasm.mp3` | Swedish melodic death | only "Bass DI" matched the name filter: list the full zip |
| Knocked Loose (Dec 2019, 48k) | `01-Mistakes-like-Fractures-1.mp3` (likely) | metallic hardcore | RHY L/R, L2/R2, ALT RHY, LEAD L/R, ROOM LEAD |
| Meshuggah "Future Breed Machine" (Sep 2017, 48k) | `01-Future-Breed-Machine.mp3` | extreme prog / djent | RHYTHMGUITAR L/R, LEAD, CLEAN L/R |
| Opeth "Heir Apparent" (Jun 2018, 48k) | `Opeth-Heir-Apparent.mp3` | prog death | Rhythm 1/2 L/R, 3 leads |
| Jinjer (Mar 2020, 44.1k, 117 bpm) | `01-On-the-Top-1.mp3` (likely) | modern prog metal | Rtm/Crunch/Lead/Clean L/R + "STEM" (processed) |
| Nothing More "We're All Gonna Die" (Sep 2025, 48k/32-bit) | `Nothing-More-Were-All-Gonna-Die-FINAL-48k-24bit.mp3` | alt-metal | RTM Gtr 1–3, solos, rotary |
| Vesta "Collide" (44.1k, 170 bpm) | `Vesta-Collide-VII.mp3` | prog / post-metal | RHY a–d, leads, FX |
| Daath "The Silent Foray" (48k) | none | US groove / melodic death | G RHY 1–8 L/R, leads, cleans (some named DI) |
| Slamadeus "Lethargic Awakening" (44.1k) | none | slam | Guitar L/R, solo; MIDI |
| Face Yourself "Guillotine" (44.1k) | none | deathcore / metalcore | rhy_01 L/R; drum MIDI |

## Not DI material

- Gojira "Toxic Garbage Island" (Dec 2016): guitars are **mic'd amp** tracks only (Gtr 1–3 Mic, doubled) + master — a clean amp-only tone target, but no DI to drive the matcher.

## Masters only (no session) — tone targets, no DI

The Zenith Passage "Algorithmic Salvation" (tech death); `02-Genesis-1.mp3` (Devin Townsend, likely);
`LOST-IN-THE-DARK-MASTER.mp3`, `MASTER_-Secrets-Paralyzed.mp3`, `Slave-Final-Master.mp3`,
`Loud-The-Home-Team.mp3`, `03-Jesse-Zuretti-BORN-FROM-PAIN.wav` (artists not identified).

## Coverage vs the style families (CLAUDE.md scope)

Covered here: Swedish/HM-2 death (Bloodbath, ATG, Haunted), brutal/slam (Immortal Disfigurement,
Slamadeus), tech death (Cognizance, Allegaeon), groove/modern (Gojira, Daath), hardcore (Knocked
Loose), thrash/metalcore (Sylosis), djent/prog (Meshuggah, Veil of Maya, Reflections, Jinjer, Trees
on Mars, Vesta). **Not covered by NTM:** black metal, doom/sludge/fuzz, grind — use the Omega Station
covers in `testdata/multis/` (Darkthrone, Immortal, Dissection, Electric Wizard, Conan, Crowbar,
Terrorizer) for those.

## Proposed first validation runs (after the macOS test fixes)

1. **Bloodbath** — per-path known answer: fit path A to the HM2 AMP track and path B to the UBR AMP
   track from the same DI, then the blend to the master stem. Tests the chainsaw model, the body
   path and the blend separately for the first time.
2. **Immortal Disfigurement** — DI → REAMP (single path, brutal death).
3. **Sylosis** or **Veil of Maya** — DI → amp, a non-death family.
