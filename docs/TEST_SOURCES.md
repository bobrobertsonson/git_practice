# Sources of DI tracks / multitracks for matcher validation

Researched 2026-10-03 (web search; most pages not opened directly — verify DI availability and
current terms before relying on an entry). Goal: DI guitars + the finished mix of the same
performance, across heavy styles (see docs/specs/phase3_3_generalize.md part D).

**Licence reality:** almost all of these are licensed for *personal / educational mixing
practice*. Sawblade is a personal, non-commercial project (not for sale), which fits those
terms for private evaluation. Still: never commit or redistribute the audio, and don't share
presets or exports tuned on it as if they were the artist's tone.

## Free

| Artist / song | Style | What's included | Licence / notes | Source |
|---|---|---|---|---|
| Gatecreeper "Barbaric Pleasures" (Omega Station cover) | Swedish death / HM-2 | L/R guitar DIs, bass DI, drum MIDI + cover mix | free download; already in testdata/ | user-supplied |
| Cnoc An Tursa "Bannockburn" | black / folk metal (signed band) | full multitrack; mixers report DI guitar & bass signals | Cambridge-MT: educational only, research → contact contributors | discussion.cambridge-mt.com/showthread.php?tid=50943 |
| Dark Ride — "Burning Bridges", "Deny Control", "Hammer Down", "Dead Enemies", "Piece Of Me" | heavy metal | multitracks (DI status per song to verify) | Cambridge-MT terms | discussion.cambridge-mt.com/showthread.php?tid=32595 |
| Decypher "Unseen" | melodic death metal | multitrack | Cambridge-MT terms | discussion.cambridge-mt.com/showthread.php?tid=18258 |
| Death Of A Romantic "The Well" | metalcore | multitrack | Cambridge-MT terms | cambridge-mt.com/ms/mtk/ |
| The Black Crown "Flames" | progressive metal (baritone 7, Kemper) | multitrack | Cambridge-MT terms | cambridge-mt.com/ms/mtk/ |
| Nine Inch Nails — *The Slip* (all songs), *Ghosts* selections | industrial / heavy rock | official multitracks (guitars are processed, not DI) | Creative Commons BY-NC-SA; non-commercial | nin.wiki/Multitracks (mirrors e.g. nindestruct.com) |
| Alwine Audio "Free Modern Metal Multitracks" | modern metal / djent | DI + Kemper guitars & bass, MIDI drums | producer content (not a known band) | alwineaudio.com/free-modern-metal-multitracks |
| EOL Studios — The Overcoming Project | groove / melodic metal (Mike Heller of Fear Factory on drums) | free multitracks (DI status unverified) | check terms | eolstudios.com/2022/10/free-metal-multitracks-for-mixing/ |

Not recommended: sites rehosting DIs of major-label songs (e.g. Killswitch Engage "The End of
a Heartache", Megadeth "Symphony of Destruction" on ReampZone) — provenance and rights unclear.

## Paid

| Source | Artists (examples) | DI guitars? | Price | Notes |
|---|---|---|---|---|
| Nail The Mix (URM Academy) | August Burns Red "Coordinates" (DI for every guitar), Suicide Silence "Unanswered" (guitar DIs), Humanity's Last Breath "Labyrinthian" (DIs), Slaves (guitar DIs), Meshuggah "Monstrocity", Lamb of God "Redneck", Gojira, Periphery, Architects "Gone With The Wind", Emmure "Flag Of The Beast", Devin Townsend "Genesis" | yes on the songs marked; verify others | $19.99/month (first month $1) | best fit: raw DIs + the commercially released mix of the same performance. Terms are for personal mixing education; private evaluation fits. |
| Periphery official stems (*Periphery IV: Hail Stan*, *Periphery V*) | Periphery | stems, not DIs | paid | usable as guitar-only references, not as DI input |
| Devin Townsend *Contain Us* box set | "Bend It Like Bender", "Juular" stems | stems | box set | reference only |
| Daybreak Studio "Procedurally Generated Metal Multitracks Vol. 1" | not a band | DI for all guitars | from $10 | synthetic/producer material |
| Chernobyl Audio metal multitracks | various styles | raw DI guitars | paid | producer material |

## Priority for Sawblade validation
1. Nail The Mix sessions with confirmed guitar DIs (ABR, Suicide Silence, HLB, Slaves) + buy the
   released track → true DI→mix pairs from signed bands across metalcore/deathcore/djent.
2. Cambridge-MT metal sessions (Cnoc An Tursa, Decypher, Dark Ride) → free DI sessions; no
   official mix, so use for known-answer tests and as DI input against north-star songs.
3. User's own non-HM-2 DIs (they offered) + their mixes.
