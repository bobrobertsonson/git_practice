# Names (user decisions, 2026-10-08)

Theme: the workshop / sawmill. These are **display names** for the UI, presets and docs. Code identifiers, block
types (`pedal.hm`, `pedal.ts`, ...) and preset JSON keys do not change. CLAUDE.md applies: generic names, not
trademarks; descriptions may say what a circuit is modelled after ("rat-style distortion"), faces may not carry a
trademark.

## Plugin parts

| Part | Display name |
|---|---|
| Path A (saw path) | **BLADE** |
| Path B (body path) | **BODY** |
| Blend control | **BLEND** |
| Noise gate | **GATE** |
| Bus compressor | **VISE** |
| Cab section | **CAB** |
| Post EQ | **POST EQ** |
| Matcher (reference match) | **MATCH** |
| NAM export | **NAM FORGER** |
| Play-along | **WOODSHED** |
| Input calibration | **INPUT CALIBRATION** |
| Drift notice | **OUT OF TRUE** |
| Genre benchmark | **PROVING GROUND** |

## Pedals

| Pedal / circuit | Display name | What it is |
|---|---|---|
| The chainsaw pedal (CIRCUIT switch family) | **THE SAW MILL** | one pedal, four circuits |
| CHAINSAW circuit (`pedal.hm`) | **BUZZSAW** | Swedish chainsaw distortion |
| BIG FUZZ circuit (`pedal.muff`) | **TAR PIT** (for now) | big fuzz |
| MODDED SAW circuit (`pedal.hmx`) | **SERRATED** | modded chainsaw distortion |
| ONE-KNOB SAW circuit (`pedal.eye`) | **HATCHET** | one-knob chainsaw |
| TS-style boost (`pedal.ts`) | **TUBESQUELER** (pending, see note) | TS-style overdrive / boost |
| Rat-style distortion (new, v0.9) | **VERMIN** | op-amp hard-clipping distortion |
| Super-overdrive-style boost (new, later) | **SUPERDRIVER** | asymmetric overdrive / boost |
| Delay (new, later) | **RICOCHET** | post-cab, never trained |
| Reverb (new, later) | **CRYPT CREEPER** | post-cab, never trained |

**Note (lead):** "Tubesqueler" was proposed by the user. It is a deliberate play on a trademarked pedal name, so it
breaks the CLAUDE.md naming rule. The user decides: keep it (personal, non-commercial, never distributed) and
record that exception here, or pick a generic name (e.g. WHETSTONE). Until then the face keeps its current label.

Renaming existing UI labels happens in the UI phase (`docs/specs/v1_0-ui_workshop_skin.md`), not before, so it does
not collide with v0.8 / v0.3.1 plugin work.
