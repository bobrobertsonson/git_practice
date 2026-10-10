# Art-direction concepts (v1.0): VERMIN rat and CAB wolf

Flat 2-D concept sketches for the user to pick from, per `docs/specs/v1_0-ui_workshop_skin.md`, "Art direction round".
Not production assets. All art is original; no third-party reference was used or traced (rat drawn from general anatomy,
wolf authored as Bezier outlines). No trademarks.

    python3 design/concepts/concepts.py            # writes png/ (about 100 s)
    python3 design/concepts/concepts.py --check    # fresh render vs committed PNGs; exit 1 on any difference
    python3 design/concepts/concepts.py --only V2  # one scene, no sheets

Needs Pillow + numpy. Fonts (sheet labels only) come from `design/render/workshop_style.py` (SHA-pinned, never committed;
exit 77 on a font failure). Seeds are `crc32(name)`; output is deterministic (two runs compared with `cmp`).

Output in `png/`: `V1..V3.png` (600x820 art panel for the 110x150 mm face), `V*_300.png` (300x410, small-size check),
`W1..W3.png` (700x700), `sheet_vermin.png`, `sheet_wolf.png`.

## VERMIN: one rearing, snarling rat, three styles (bone on dark; iron nails through the body)

All three share the same pose and rig (rearing, facing the viewer, teeth bared, claws out, long tail looped up behind,
five nails with visible heads and exit points), so only the style differs. V1 renders on dark pebbled tolex rather than
flat enamel because the stencil sprays onto a surface.

| id | style |
|----|-------|
| V1 | spray stencil, same language as the wolves: dashed cut lines (bridges), coarse halftone shade, overspray, drips |
| V2 | woodcut / engraving: flow-field hatching (line width swells with tone), cross-hatch highlights, fur flicks |
| V3 | flat screen-print: solid bone shapes, fine halftone shade, crisp dark keylines, mis-registered offset keyline |

Rig: Bezier head outlines, tapered "sweeps" along hand-authored centre-lines for body and limbs; nails are heads +
shafts + pointed exits with puncture marks.

## CAB wolf (one spray colour per panel)

| id | subject / surface |
|----|-------------------|
| W1 | howling bust in profile, bone on pebbled black tolex |
| W2 | frontal snarl (mirrored half-face), toxic green #8fd14f on perforated steel grille (paint only lands on the metal) |
| W3 | snarling profile, bone on woven speaker cloth |

Technique: hand-authored Bezier silhouette, eye/nose/ear cut-outs split by bridges, tapered fur cuts on a lattice (the
gaps are the bridges), then spray: soft edges, noise edge breakup, overspray haze + speckle, drips, scuffs.

## Honest quality assessment

- **Wolves: reachable and usable as direction.** W2 is the strongest (bold, symmetric, reads at a glance, the grille
  halftone suits it). W3 reads as a snarling wolf. W1 is the weakest: the howl pose is muddled (ear and muzzle compete).
  Muzzle and fangs are crude; a stencil artist would redraw the silhouettes. The spray effects (haze, drips, wear) are convincing.
- **Rats: V3 and V1 are the more successful styles; V2 does not reach real engraving.** V3 is crisp and graphic and
  reads at 300 px (best fit for the pedal face). V1 matches the wolves and reads at 300 px, but the nails get muddy
  (dark halos around them look like screwdrivers) and the halftone is coarse. V2 reads as "rat, woodcut-ish sketch":
  limbs are tubes with visible segment joins, the torso is a blob, shading is tone-driven hatching rather than
  form-following engraving. All three rats look more cartoon mouse than vicious sewer rat (big round head and ears; the
  shared rig and flat sweeps limit anatomy). Production art needs a hand-drawn SVG or a commissioned illustration; pose,
  nails and style choice carry over.
