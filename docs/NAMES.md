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
| TS-style boost (`pedal.ts`) | **CHISEL** | TS-style overdrive / boost |
| Rat-style distortion (new, v0.9) | **VERMIN** | op-amp hard-clipping distortion |
| Super-overdrive-style boost (new, later) | **SUPERDRIVER** | asymmetric overdrive / boost |
| Delay (new, later) | **RICOCHET** | post-cab, never trained |
| Reverb (new, later) | **CRYPT CREEPER** | post-cab, never trained |

**TS-style pedal:** the user first proposed a play on the trademarked name, then chose CHISEL (2026-10-08).

Renaming existing UI labels happens in the UI phase (`docs/specs/v1_0-ui_workshop_skin.md`), not before, so it does
not collide with v0.8 / v0.3.1 plugin work.
