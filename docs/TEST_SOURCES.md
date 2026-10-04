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

## Death metal and chainsaw-adjacent sessions (researched 2026-10-03)

Nail The Mix sessions (raw multitracks from the band's own session; guitar DIs are
common on NTM but **check each session's track list before relying on it**):

| Band / song | Style | Mixer | Why it matters |
|---|---|---|---|
| Bloodbath "Zombie Inferno" (2022) | Swedish death, HM-2-style buzzsaw | Lawrence Mackrory | best chainsaw-family pair available |
| At The Gates "The Chasm" (2018) | Swedish melodic death | Russ Russell | melodic_death profile |
| Decapitated "One Eyed Nation" / "Just a Cigarette" | tech / modern death | Daniel Bergstrand / David Castillo | us/modern death, tight single path |
| Dyscarnate "Iron Strengthens Iron" | UK death / groove | Jacob Hansen | groove death |
| Amon Amarth "Twilight of the Thunder God" | melodic death | Jens Bogren | melodic_death |
| Converge "I Can Tell You About Pain" | metallic hardcore (Kurt Ballou, GodCity) | Kurt Ballou | blended guitar tones (JMP + Sparrow's Sons + room); same producer lineage as Nails / Black Breath / Harm's Way |
| High On Fire, The Haunted, Septicflesh | sludge / thrash / symphonic death | various | secondary |

Free:
- Hollow Ground "Ill Fate" (Cambridge-MT): death metal, growls + blast beats; forum posts
  confirm guitar DI files. Free start for death metal DI input.
- Cambridge-MT Decypher "Unseen" (melodic death), Cnoc An Tursa (black/folk).
- EOL Studios free death metal / metalcore multitracks ("Anxiety", 2026) — DI status unverified.

Not found: any official multitracks/DIs for Rotten Sound, Harm's Way, Black Breath, Nails,
Entombed, Dismember. For these the plan is **reference mix only** (user supplies mp3s of the
released songs) + the user's own DIs, matched against the reference.


## Community list "Multis & DIs" (user-shared spreadsheet, 2026-10-04)

The list has 521 songs; 83 of them are heavy songs with guitar DIs. Most heavy entries are
Omega Station instrumental covers (same source as the Gatecreeper DIs). Picks downloaded
for validation (local only, under `testdata/multis/`, catalogue in
`testdata/multis/catalogue.json`):

| Style | Songs |
|---|---|
| Chainsaw / powerviolence | Nails "Friend To All", "No Surrender", "I Will Not Follow"; Bloodbath "Ways To The Grave" |
| Death (non-HM-2) | Bolt Thrower "Anti-Tank", "The Shreds Of Sanity", "Those Once Loyal"; Hypocrisy "A Coming Race"; Vader "Sword of the Witcher" |
| Grind | Terrorizer "Fear Of Napalm" |
| Black | Dissection "Retribution"; Darkthrone "Transilvanian Hunger"; Immortal "One By One" |
| Doom / sludge (fuzz) | Electric Wizard "Dunwich"; Conan "Volt Thrower"; Crowbar "Planets Collide" |

Entombed "Left Hand Path" (Death Lab Studio) needs the user to fill in a sign-up form.

## Priority for Sawblade validation (user, 2026-10-03: death metal over deathcore)
1. Death metal DI→mix pairs first: Swedish/HM-2 (have Gatecreeper), US/Florida style,
   cavernous, melodic. Cambridge-MT Decypher "Unseen" (melodic death) is the free start.
2. Nail The Mix: only worth a month if its catalogue has death metal sessions with guitar
   DIs (check before subscribing); the ABR / Suicide Silence / HLB sessions are deathcore /
   metalcore and are deferred.
3. User's own DIs playing death metal riffs + north-star reference songs per substyle.
