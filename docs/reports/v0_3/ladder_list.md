# Gain ladders in committed presets (v0.3 Task E)

Status: **not verified against the live API.** The ladder tool needs a TONE3000 end-user Bearer token (`sawblade-t3k login`); this container has only `TONE3000_CLIENT_ID` (publishable key), no login token, and `www.tone3000.com` returns the website HTML for API paths without a token. No `t3k_cs_` secret was used. Nothing was downloaded. Every "unknown" below needs one run on the Mac.

## Detection rule (today)

`sawblade-t3k ladder <tone_id> [--size standard]` (`match/sawblade_match/t3k/cli.py` `cmd_ladder`, `match/sawblade_match/t3k/ladder.py` `parse_ladder`) lists the tone's models for architecture A2 (then A1, same choice as `resolve`), keeps those of the requested size (plugin uses `standard`; presets do not record size), and returns a ladder only if ALL hold: at least 2 such models; every model name yields exactly one gain number via the patterns `gain|drive|g` followed by a number (`Gain 6`, `G6`, `gain=6`, `g 6.5`), number then keyword (`6 gain`, `6g`) or `@7`, with the keyword and number separated only by whitespace, `=` or `:` (a number > 100 is not a gain); the names are identical once that gain token is removed; the gains are all distinct. Anything else (descriptive names such as `Crunch`/`Lead`, two gain-like tokens, mixed channel names, a single model, duplicate gains) is "no ladder" - it never guesses. The plugin runs this once per tone per session (`SawbladeProcessor::ladderTick`, `docs/PLUGIN.md` "Gain ladders") and stores `model.ladder` in the preset; **no committed preset carries a stored `model.ladder`**.

## Finding: the known gain-range pack would be rejected by the parser

Tone 29230 "6505+ Gain Range Pack (High Gain)" (shortlisted, in the Bolt Thrower match report) has models named `APP-6505Plus-Scooped-Gain-02/-04/-05/-06/-07` (committed `docs/reports/match_bolt_thrower_v1.result.json`, a sample of the pack, not the full list). I ran `_gain_and_rest` on those names offline: it returns `None` for each, because the keyword and number are separated by a hyphen (`Gain-06`), which the pattern does not allow. So even if the pack is a genuine ladder, today's rule would report "no ladder". A parser change to accept `-`/`_` between keyword and number (and check the remainder stays identical, `app 6505plus scooped`) would make it detect; that is outside Task E scope, so it is proposed, not done. Caveat: the full pack may also contain other voicings (non-"Scooped") that would make the remainders differ; only the live list can tell.

## Table: committed presets (amp blocks of type `nam`, slot `amp`)

| preset | path | amp name | tone id | ladder |
|---|---|---|---|---|
| Chainsaw + Body | `presets/chainsaw_body.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Chainsaw + Body | `presets/chainsaw_body.json` (path b, model 584871) | 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac). Pool-manifest samples for this pack are boost-voicing names (`APP-6505+-Boost-S-OD1`), not a gain sweep: likely no |
| Barbaric Pleasures · matched v4 | `presets/matched/barbaric_v4.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| UK Death Fuzz Blend (match v1) | `presets/matched/bolt_thrower_v1.json` (path a, model 785386) | 1986 JCM 800 2204 | 93752 | unknown - needs login (run `sawblade-t3k ladder 93752 --size standard --json` on the Mac) |
| UK Death Fuzz Blend (match v1) | `presets/matched/bolt_thrower_v1.json` (path b, model 785274) | 1986 JCM 800 2204 | 93752 | unknown - needs login (run `sawblade-t3k ladder 93752 --size standard --json` on the Mac) |
| Hardcore Wall (match v1) | `presets/matched/nails_v1.json` (path a, model 584871) | 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac). Pool-manifest samples for this pack are boost-voicing names (`APP-6505+-Boost-S-OD1`), not a gain sweep: likely no |
| Studio Split | `presets/studio_split.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Studio Split | `presets/studio_split.json` (path b, model 584871) | 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac). Pool-manifest samples for this pack are boost-voicing names (`APP-6505+-Boost-S-OD1`), not a gain sweep: likely no |
| Black Metal (Darkthrone / Dissection-style) | `presets/styles/black_metal_raw.json` (path a, model 742871) | Peavey 5150 Blockletter | 87953 | unknown - needs login (run `sawblade-t3k ladder 87953 --size standard --json` on the Mac) |
| Chainsaw Hardcore (Nails-style) | `presets/styles/chainsaw_hardcore_nails.json` (path a, model 785274) | 1986 JCM 800 2204 | 93752 | unknown - needs login (run `sawblade-t3k ladder 93752 --size standard --json` on the Mac) |
| Fuzz Doom (Electric Wizard / Conan-style) | `presets/styles/fuzz_doom_electric_wizard.json` (path a, model 387388) | 1998 Sunn Model T Reissue | 13705 | unknown - needs login (run `sawblade-t3k ladder 13705 --size standard --json` on the Mac) |
| Grind (Terrorizer-style) | `presets/styles/grind_terrorizer.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Sludge (Crowbar-style) | `presets/styles/sludge_crowbar.json` (path a, model 382709) | MESA DUAL RECTIFIER 2025 | 45026 | unknown - needs login (run `sawblade-t3k ladder 45026 --size standard --json` on the Mac) |
| UK Death (Bolt Thrower-style) | `presets/styles/uk_death_bolt_thrower.json` (path a, model 382783) | MARSHALL JCM 900 4100 | 45684 | unknown - needs login (run `sawblade-t3k ladder 45684 --size standard --json` on the Mac) |
| Swedeath Saw | `presets/swedeath_saw.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Swedeath Saw | `presets/swedeath_saw.json` (path b, model 584871) | 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac). Pool-manifest samples for this pack are boost-voicing names (`APP-6505+-Boost-S-OD1`), not a gain sweep: likely no |
| Tight Body | `presets/tight_body.json` (path a, model 731435) | Marshall JCM 800 2203 | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Tight Body | `presets/tight_body.json` (path b, model 584871) | 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac). Pool-manifest samples for this pack are boost-voicing names (`APP-6505+-Boost-S-OD1`), not a gain sweep: likely no |

Unique amp tone ids in committed presets: 86089, 70977, 93752, 87953, 13705, 45026, 45684. Not listed above: `presets/modeled/**` (hm_chainsaw, ts_boost, saw_body_blend_demo, chainsaw/*, hm_v3/*, hmx/*, eye/*) contain no `nam` blocks at all (DSP pedal models only), so they have no TONE3000 amp and no ladder; the chainsaw bank only names suggested amps in `notes` (86089, 76884, ...). `presets/captures/` holds only a `.gitignore`. There are no separate "Classic" files: the four top-level presets (`chainsaw_body`, `swedeath_saw`, `tight_body`, `studio_split`) are the Classic ones.

## Table: capture shortlist amps (`presets/CAPTURE_SHORTLIST.md`)

| amp | tone id | ladder |
|---|---|---|
| Marshall JCM 800 2203 (@sarcobe) | 86089 | unknown - needs login (run `sawblade-t3k ladder 86089 --size standard --json` on the Mac) |
| Marshall 1959BJA (@rjcproductions) | 76884 | unknown - needs login (run `sawblade-t3k ladder 76884 --size standard --json` on the Mac) |
| Marshall Plexi Super Lead 1959 EL34 (@ripper) | 40971 | unknown - needs login (run `sawblade-t3k ladder 40971 --size standard --json` on the Mac) |
| Marshall 1978 JMP 2203 (@origrino) | 1061 | unknown - needs login (run `sawblade-t3k ladder 1061 --size standard --json` on the Mac) |
| EVH 5150iii Ivory FULL Pack | 88689 | unknown - needs login (run `sawblade-t3k ladder 88689 --size standard --json` on the Mac) |
| 6505+ FULL Pack | 70977 | unknown - needs login (run `sawblade-t3k ladder 70977 --size standard --json` on the Mac) |
| 6505+ Gain Range Pack (High Gain) | 29230 | unknown - needs login (run `sawblade-t3k ladder 29230 --size standard --json` on the Mac). Names `APP-6505Plus-Scooped-Gain-02..07` look like a ladder, but the current parser returns none (hyphen), see Finding |
| FRIEDMAN BE-100 (Gain Stages) | 72085 | unknown - needs login (run `sawblade-t3k ladder 72085 --size standard --json` on the Mac). Title says "Gain Stages": a ladder is plausible if model names carry a number |
| Marshall JVM410H FULL Pack | 67714 | unknown - needs login (run `sawblade-t3k ladder 67714 --size standard --json` on the Mac) |
| DIEZEL VH4 | 53749 | unknown - needs login (run `sawblade-t3k ladder 53749 --size standard --json` on the Mac) |
| ENGL SAVAGE 120 MK1 | 53738 | unknown - needs login (run `sawblade-t3k ladder 53738 --size standard --json` on the Mac) |
| Bogner Uberschall MKII (reference only) | 69300 | unknown - needs login (run `sawblade-t3k ladder 69300 --size standard --json` on the Mac) |

Tone ids for 40971, 88689, 72085 in the shortlist URLs are slug-suffixed (`...-40971`); the numeric suffix is the id.

## Proposal: one shortlist amp for seeing gain steps

Tone **29230 "6505+ Gain Range Pack (High Gain)"**. How I know: the committed match report lists its models as `APP-6505Plus-Scooped-Gain-02, -04, -05, -06, -07` (one voicing, one gain number each), the shape of a single-knob sweep. Caveats: (1) today's parser rejects the hyphen form, so it needs the small parser fix above before the plugin would show "STEPS n"; (2) unconfirmed on the live API. If the user prefers no parser change, the only way to know is the Mac run below; "Friedman BE-100 (Gain Stages)" (72085) is the next candidate by title only.

## One command for the Mac (produces the table)

```
for id in 86089 70977 93752 87953 13705 45026 45684 76884 40971 1061 88689 29230 72085 67714 53749 53738 69300; do sawblade-t3k ladder $id --size standard --json | python3 -c 'import sys,json; d=json.load(sys.stdin); r=d["rungs"]; print(d["tone_id"], ("yes %d steps: " % len(r)) + ", ".join(x["name"] for x in r) if r else "no")'; done
```

(Run after `sawblade-t3k login`; `jq` is not needed.)
