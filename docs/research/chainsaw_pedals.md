# Chainsaw / buzzsaw pedal research (2026-10-04)

Method notes: WebSearch worked; WebFetch was blocked by the egress proxy for guitarpedalx, boss.info, wikipedia, riffology. All web claims come from search-result snippets, not full-page reads. URLs are the result URLs that surfaced the claim. Anything marked UNVERIFIED could not be confirmed. TONE3000 data is from `sawblade-t3k search --gear pedal --json` (35 queries, 199 unique tones, nothing downloaded).

## PART 1: Pedal family

### a) Official Boss variants
| Pedal | Maker | Notes vs stock HM-2 | Bands / evidence | Source |
|---|---|---|---|---|
| HM-2 Heavy Metal (1983-1991, MIJ/Taiwan) | Boss | Reference. 4 knobs: Dist, Level, Low, High. | Entombed (all 4 knobs at 10, two guitars via HM-2, a third via DS-1 in centre), Dismember, Carnage, Edge of Sanity, Bloodbath, Nails, Trap Them, Disfear | https://articles.boss.info/hm-2-the-sound-of-swedish-death-metal-and-beyond/ ; https://en.wikipedia.org/wiki/Boss_HM-2 ; https://equipboard.com/band/disfear |
| HM-2W Waza Craft | Boss | Standard mode = classic HM-2 voicing with lower noise floor, +3 dB max level, Waza buffer. Custom mode = more gain, "more fundamental", modified low and high-mid for fullness/definition. Exact component/EQ-centre changes UNVERIFIED. | Modern HM-2 users generally | https://www.musicconnection.com/boss-announces-hm-2w-heavy-metal-pedal/ ; https://www.boss.info/sk/products/hm-2w |
| HM-3 Hyper Metal (early 90s, short run) | Boss | Different topology: triple-gyrator EQ with two controls and three clipper stages; more modern/scooped, "does not do the chainsaw thing" per forum posters. Same four-control layout. Few clones. Forum opinion, not Boss. | No chainsaw-band evidence found | https://forum.pedalpcb.com/goto/post?id=245927 ; https://mirosol.kapsi.fi/2014/08/boss-hm-3-hyper-metal/ |

### b) HM-2 clones and derivatives
| Pedal | Maker | Changes vs stock HM-2 | Bands | Source / status |
|---|---|---|---|---|
| Left Hand Wrath (also Gatecreeper Signature "LHP" model) | Lone Wolf Audio | Midrange decoupled from High; added Presence; 10-position mid inductor (6 high-mid, 4 low-mid settings); sub-mid control; three-way clipping; 2nd footswitch "heavier" mode; Vintage/Modern mode; clean blend. Exact feature list partly from retailer text. | Gatecreeper (Eric Wagner: ENGL Powerball II, Orange 4x12, LWA Gatecreeper Signature LHP pedal, ESP EII MII with EMG 81) | https://www.rockboard.de/en/pedalPedia/Lone-Wolf-Audio/Left-hand-wrath-Deluxe/377219404 ; https://killerguitarrigs.com/news-watch-gatecreepers-new-live-performance-video/ |
| Modded HM-2 | Dunwich Amps | 2 clipping switches (hard/soft), 4-band EQ (high/mid/low), much more gain than original. Also Dunwich Tyrant ("HM-2 style with enhanced buzz"). | not found | https://equipboard.com/items/dunwich-modded-hm-2 ; https://equipboard.com/items/dunwich-amps-tyrant |
| "Sonic Reaper" (Dunwich?) | ? | UNVERIFIED: no pedal by this name found under Dunwich. Not on T3K either ("T Lab The Reaper" is unrelated). | | - |
| Throne Torcher | Abominable Electronics | HM-2 based; adds Mids control and Clean Blend; 2nd footswitch for LED/diode clipping (boost + thicker); Russian (Klon-style) diodes; "dials out some harshness". | Retailer text claims Russian Circles, Beartooth (weak) | https://www.effectsdatabase.com/model/abominable/thronetorcher ; https://equipboard.com/items/abominable-electronics-throne-torcher |
| Hail Satan | Abominable Electronics | Referred to as sibling with the same LED clipping footswitch. No spec found. UNVERIFIED. | | same pages |
| Grumbly Wolf | Fredric Effects | NOT an HM-2 clone. MXR Dist+/DOD 250 style asymmetric clipping + Green Ringer octave/ring mod. Drop from list. | | https://www.premierguitar.com/fredric-effects-announces-the-grumbly-wolf |
| Blower Box (+ Deluxe) | Idiotbox | NOT HM-2. Rat-style bass distortion built for Voivod Blacky tone (Nothingface); Deluxe adds mids focus + parallel low-passed clean. Drop. | Voivod bass | https://modulargrid.net/p/idiotbox-effects-blower-box ; https://gearhero.com/products/idiotbox-blower-box-deluxe-bass-distortion |
| Eyemaster Metal Distortion | TC Electronic | 2 knobs (Gain, Volume) only; marketed for Swedish death metal (Entombed, Dismember). Reviewers say closer to original HM-2 than HM-2w/HM300. Internal circuit relation UNVERIFIED. | Marketing: Entombed/Dismember homage | https://www.thomann.de/pt/tc_electronic_eyemaster_metal_distortion.htm ; https://www.sweetwater.com/store/detail/Eyemaster--tc-electronic-eyemaster-metal-distortion-pedal/reviews |
| Angry Swede (V2 adds Blend) | Decibelics | Mini HM-2 clone, through-hole; V2 adds Blend (clean/dry mix) | none found | https://www.guitarpedalx.com/news/gpx-blog/decibelics-delivers-the-new-shape-of-metal-with-its-angry-swede-mini-hm-2-clone |
| HM300 | Behringer | Budget HM-2 clone | none | https://www.guitarpedalx.com/news/gpx-blog/boss-hm-2-heavy-metal-pedal-considerations-and-alternatives |
| Hang Man 2D | Wren and Cuff | HM-2 clone (details UNVERIFIED) | none | https://reverb.com/item/49948207-wren-and-cuff-hang-man-2d |
| The Swede (BYOC), HMD-1 (XIX Tech), Black Arts Toneworks Witch Burner | various | Named as HM-2 style in GPX/effectsdatabase listings; specifics UNVERIFIED | | https://www.effectsdatabase.com/model/xixtech/hmd1 |
| Daredevil "Swedish Chainsaw", Stomp Under Foot (HM-2), Fuzz Imp, Mythos, Nocturne | | UNVERIFIED: no HM-2-type product found. Stomp Under Foot, Mythos, Fuzz Imp make fuzz/other circuits. | | - |

### c) Non-HM-2 circuits with chainsaw-adjacent use (evidence is weak)
- Big Muff family (Elk Big Muff Sustainar, EQD Hizumitas = recreation of Wata's Elk BM Sustainar, Boris collaboration; Way Huge Swollen Pickle = jumbo Big Muff). Evidence is for thick sustaining fuzz (Boris/Wata), not the HM-2 buzzsaw. No source found tying them to Swedish/crust chainsaw use; treat as adjacent. https://www.earthquakerdevices.com/blog-posts/wata-elk-big-muff-sustainar-and-hizumitas ; https://www.effectsdatabase.com/model/earthquaker/hizumitas
- Metal Zone mods / Tube Screamer stacks: no sourced chainsaw evidence found. UNVERIFIED, omit.
- Boss DS-1: used as a third guitar layer on Left Hand Path (Boss article above).

### d) Classic chains
- Entombed, Left Hand Path (Sunlight Studio, Tomas Skogsberg): two guitars through HM-2 with Dist/Level/Low/High all at 10; third through DS-1 centre doubling bass; amp most associated: small Peavey (Studio Pro 40 combo). https://articles.boss.info/hm-2-the-sound-of-swedish-death-metal-and-beyond/ ; https://riffology.co/posts/the-making-of-left-hand-path-by-entombed/
- General: "Peavey amp + Boss Heavy Metal pedal" is the Swedish DM recipe; solid-state Peavey. https://equipboard.com/genres/swedish-death-metal/gear
- Disfear: HM-2 into Peavey Bandit 112 (aggregated site, low confidence). https://equipboard.com/band/disfear
- Modern: Kurt Ballou (Godcity) recorded Nails, Black Breath, Trap Them, Harm's Way with HM-2 and is known for gain staging the amp for it. https://cvltnation.com/buzzsaw-an-oral-history-of-the-hm-2-pedal/ ; https://reverb.com/news/story-hm2-dark-side-boss
- Bloodbath: HM-2 (guide). https://www.musicradar.com/news/guitars/bloodbaths-guide-to-death-metal-guitar-617642
- Gatecreeper: see Left Hand Wrath row (ENGL Powerball II, not Peavey).
- UNVERIFIED (no source found): Marshall vs Yamaha solid-state specifics, Rotten Sound, Carnage and Wolfbrigade rigs, Nails/Ballou amp. Dismember and Wolfbrigade appear only as generic HM-2 / crust associations.

## PART 2: TONE3000 captures (pedal gear)
Floor: favorites >= 100 or downloads >= 1000 (reasons show "below_popularity"), also too_old (before 2025?), and non-commercial (flagged excluded by pool filter; project rules allow nc). Format: fav/dl. "FORCE" = good candidate for `--force-tone` (just under floor or sole example of target pedal).

### HM-2 / HM-2W
| tone_id | title | creator | lic | fav/dl | created | models | pass | reason |
|---|---|---|---|---|---|---|---|---|
| 78122 | Boss HM-2w CHAINSAW | ebheron | t3k | 188/2747 | 2026-07-26 | 2 | PASS | |
| 6778 | BOSS HM-2 1986 | bigmuff | t3k | 232/4942 | 2025-02-23 | 22 | fail | too_old (FORCE: best-liked) |
| 1104 | BOSS HM-2 Heavy Metal (MiJ) v2.0 | peterny | cc-by-nc | 125/2812 | 2023-04-12 | 24 | fail | non_commercial, too_old (FORCE; nc allowed by project) |
| 58569 | Boss HM-2 1985 MIJ TTSV10 | OutmodedElectronics | t3k | 94/1928 | 2026-03-23 | 96 | fail | fav 94<100 (FORCE) |
| 88604 | Boss Waza HM-2 | colossaldave | t3k | 37/489 | 2026-09-03 | 2 | fail | popularity (new) |
| 29604 | Boss HM-2 (Extreme Edition) | v24x | t3k | 40/1500 | 2025-05-27 | 30 | fail | fav<100 |
| 29576 | Boss HM-2 | v24x | t3k | 36/1158 | 2025-05-27 | 30 | fail | fav<100 |
| 477 | Boss HM-2W Waza | servusjon | t3k | 48/904 | 2023-03-30 | 8 | fail | too_old, pop |
| 74487 | Boss HM-2W | gianni | t3k | 30/529 | 2026-07-05 | 4 | fail | pop |
| 5363 | HM-2W Waza Craft | jimbolodisc | t3k | 23/302 | 2024-01-07 | 6 | fail | old, pop |
| 63188 | Boss HM-2 (Japan) | marcgirard | t3k | 17/366 | 2026-04-26 | 2 | fail | pop |
| 42244 | Boss HM2 Waza | staynoisi | t3k | 10/392 | 2025-11-03 | 14 | fail | pop |
| 50489 | Boss HM 2 | amrinbastomi | t3k | 11/438 | 2026-01-13 | 15 | fail | pop |
| 571 / 722 / 558 | Boss HM-2 Mega Pack / Pack / Pack | petern8363 / jz1978 / jz | t3k | 30/462, 34/537, 21/332 | 2023 | 17/6/6 | fail | old, pop |
| 34509 | BOSS LS2 - HM2W Custom, Amazon OD BLEND | draysonc | t3k | 4/257 | 2025-08-04 | 4 | fail | pop (contains HM-2W Custom mode, useful) |
| 90742 | HM2+GE7 ... | sleepdead | t3k | 2/84 | 2026-09-09 | 6 | fail | pop |
| 45217 | Dinosaur DUM-1 Ultra Metal (aka Aria UM-1, HM-2 clone) | morenoteslesstalk | t3k | 7/217 | 2025-11-30 | 2 | fail | pop |

### Clones / derivatives
| tone_id | title | creator | lic | fav/dl | created | models | pass | reason |
|---|---|---|---|---|---|---|---|---|
| 72990 | Abominable Electronics Throne Torcher HM2 Clone | axelghxst | t3k | 13/228 | 2026-06-24 | 2 | fail | pop (only capture of an extended HM-2; FORCE if needed) |
| 1582 | Decibelics Angry Swede v2 | garryy | t3k | 22/274 | 2023-04-16 | 12 | fail | old, pop |
| 6380 | TC Electronic Eyemaster Mini Pack | gianlutallica | t3k | 18/392 | 2024-11-18 | 6 | fail | old, pop |
| 62523 / 60618 / 6547 / 5893 | Eyemaster pedal only / full distortion / Eyemaster Metal Distortion / Chainsaw Boost - TC Eyemaster | various | t3k | 14/265, 8/273, 8/252, 17/224 | 2024-2026 | 2 each | fail | pop |
| 2618 / 35635 | Behringer HM300 / HM300 | markswarbrick / vladimir | t3k | 18/327, 7/253 | 2023/2025 | 6/4 | fail | pop |
| 1673 | Black Arts Toneworks Witch Burner Clone | rickomegastation | t3k | 21/352 | 2023-04-20 | 15 | fail | old, pop |
| 77705 | Black Arts Toneworks Pharaoh Fuzz | baab | t3k | 58/726 | 2026-07-24 | 21 | fail | pop (fuzz, not HM-2) |
| 32952 | Idiotbox Blower Box Bass Distortion | johnbegone | t3k | 19/485 | 2025-07-17 | 8 | fail | pop (Rat-style bass) |
| 67250 | DOD FX56-B American Chainsaw | harmlessmagnitude | t3k | 24/327 | 2026-05-21 | 22 | fail | pop (name only; not an HM-2) |
| 5715/5716/5717 | Bardic Audio Devices HM Demon Mix 0/50/75 | viciousaudio | cc-by | 9/138 .. | 2024-03-04 | 2 | fail | old, pop (name suggests HM-2 style; UNVERIFIED) |
| none | Left Hand Wrath, Sonic Reaper, Hail Satan, Grumbly Wolf, Daredevil, Stomp Under Foot HM-2, Nocturne, Entombed | | | | | | not on TONE3000 (0 relevant hits; "left hand" returned unrelated) | |

### HM-3 / Metal Zone
| 2709 | Boss Hyper Metal HM-3 | bryanholmes | t3k | 27/391 | 2023-07-24 | 14 | fail | old, pop |
| 1753 | Boss HM3 | fatdracula | t3k | 27/457 | 2023-04-24 | 16 | fail | old, pop |
| 45204 | BOSS MT-2 Metal Zone | stomptones | t3k | 96/2883 | 2025-11-30 | 30 | fail | fav 96<100 (FORCE) |
| 64957 | BOSS Metal Zone MT-2 1992 | drprophecystudio | t3k | 76/2254 | 2026-05-02 | 37 | fail | pop |

### Big Muff family (non-HM-2)
| tone_id | title | creator | lic | fav/dl | created | models | pass | reason |
|---|---|---|---|---|---|---|---|---|
| 44970 | Earthquaker Devices Hizumitas Fuzz | stomptones | t3k | 130/2040 | 2025-11-28 | 8 | PASS | |
| 36748 | Earthquaker Hizumitas Complete Pack | hangingpuppetaudio | t3k | 124/2081 | 2025-09-01 | 48 | PASS | |
| 88638 | Earthquaker Devices Hizumitas | colossaldave | t3k | 63/511 | 2026-09-03 | 3 | fail | pop |
| 35659 | EQD Hizumitas Fuzz Pedal | johnbegone | t3k | 31/847 | 2025-08-18 | 18 | fail | pop |
| 50490 | Way Huge Swollen Pickle | amrinbastomi | t3k | 135/2254 | 2026-01-13 | 20 | PASS | |
| 42149 / 96168 | SwollenPickle / Swollen Pickle Muff | ramadhanax / tonsenf | t3k | 20/493, 5/116 | 2025/2026 | 2/1 | fail | pop |
| 37818 | EHX Op-Amp Big Muff | OutmodedElectronics | t3k | 445/9131 | 2025-09-13 | 105 | PASS | |
| 61106 | EHX Big Muff Pi | ethanmh | t3k | 307/6649 | 2026-04-10 | 50 | PASS | |
| 88157 | EHX Big Muff Pi with Tone Wicker | mpusch | t3k | 269/11340 | 2026-09-02 | 2 | PASS | |
| 68162 | Sovtek Big Muff Pi (Tall Font) | dynastic | t3k | 263/4326 | 2026-05-29 | 2 | PASS | |
| 74161 | 76' Big Muff Pi EHX (Rams Head) | mikefromtilt | t3k | 121/1955 | 2026-07-03 | 4 | PASS | |
| 34237 | EHX Bass Big Muff | carlosalvgonz | t3k | 106/2843 | 2025-08-01 | 12 | PASS | |

No Elk Big Muff Sustainar capture found (searches "sustainar", "elk": only Hizumitas, DOD compressor/sustainer). Other Muff/Metal Zone captures (about 40) are fail on popularity or age; full raw JSON per query is in `scratchpad/s/*.json`.

## Recommended short list (beyond stock HM-2 and Swollen Pickle)
1. **HM-2W Custom mode** (more gain, reshaped low/high-mid). Official Boss, concrete documented delta from HM-2, so it is a parameter variant of the HM-2 model rather than a new circuit. Calibrate against: 78122 (passes, "CHAINSAW"), 477 / 5363 / 74487 / 88604 (HM-2W captures; whether any capture Custom mode is UNVERIFIED, check titles/metadata), 34509 (explicitly Custom).
2. **HM-2 + decoupled mids/clipping class (Left Hand Wrath / Throne Torcher / Dunwich Modded HM-2)**. Most-used modern derivative (Gatecreeper itself), and the feature set is consistent across makers: mid control, switchable diode clipping/boost, clean blend. Model as HM-2 + mid band + clipper options. Only capture of this family is 72990 Throne Torcher (use `--force-tone`), plus the stock HM-2 captures as the baseline; Left Hand Wrath has no capture. Treat component values as UNVERIFIED and measure from 72990.
3. **TC Electronic Eyemaster** (2-knob, sold as Swedish DM). Distinct gain/voicing, reviewers say closest to original HM-2. Calibrate against 6380, 62523, 60618, 6547, 5893 (all small, none pass; FORCE). Caveat: circuit relation to HM-2 is UNVERIFIED; check by comparing its sweep to the HM-2 model first, and if it turns out to be near-identical, replace this slot with Boss HM-3 (2709, 1753; different 3-clipper gyrator topology but non-chainsaw per forum reports) or Hizumitas (44970, 36748 pass, but only adjacent evidence).

Not recommended: Grumbly Wolf and Blower Box (not HM-2 family, verified), Metal Zone (no chainsaw evidence found).
