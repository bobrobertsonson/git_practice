# Workshop mockups (v1.0 Task A)

Procedural mockups of the "worn metal and sawdust" skin. Spec: `docs/specs/v1_0-A-workshop_mockups.md`. These are
mockups for the user to choose from and edit; nothing here is built into the plugin and nothing under `plugin/` changes.

## Status

Step 1 of Task A: the style kit (`design/render/workshop_style.py`), the driver, and screens `00_style_sheet` and
`01_main_rig`. Screens 02-09 follow in step 2 (add a `screens_*.py` exposing `SCREENS = {'NN_name': fn}` and list the module
in `SCREEN_MODULES` in `render_all.py`).

| file | screen |
|---|---|
| `png/00_style_sheet.png` | palette, materials, type, control kit, frames |
| `png/01_main_rig.png` | the rig the user plays |

## Run

    python3 design/mockups/workshop/render_all.py              # render all registered screens into png/
    python3 design/mockups/workshop/render_all.py --check      # fresh render vs committed PNGs (0 same / 1 differ / 77 deps)
    python3 design/mockups/workshop/render_all.py --contrast   # measured contrast table (below); exit 1 if any pair < AA
    python3 design/render/workshop_style.py --out DIR          # only 00_style_sheet.png

Pillow 12.3.0 and numpy 2.5.3 on Python 3.13.16 produced the committed PNGs (FreeType is
Pillow's bundled build). Same versions give bit-identical PNGs; `--check` compares decoded RGBA, so a different zlib still passes.

## Fonts

SIL OFL 1.1, fetched into `~/.cache/sawblade_fonts` (`--font-dir`) and SHA-256 verified; never committed; a missing or
mismatching font is exit 77, never a fallback. Table in `docs/THIRD_PARTY.md`.

## Rules the kit enforces

- Text is drawn only through `workshop_style.text()`, which raises `FlatBackgroundError` unless the pixels under the ink box
  + 2 px are one flat colour, and logs `(style, fg, bg, px, bold)` for the contrast check.
- Glyphs no font carries (play / marker triangles, check, cross, warning triangle, double bar, arrow) are drawn procedurally
  inside `text()`; characters missing from one font fall back to Barlow Condensed (Allerta Stencil has no middle dot, minus or O-slash).
- State is never colour alone: LED words, toggle labels with a marker, bypass hatch + word, badges with shape + glyph + word,
  selected rows / options with a marker, pressed footswitch + ACTIVE.

## Measured contrast (`--contrast`)

| style | fg | bg | ratio | AA needs | px sizes | uses | screens |
|---|---|---|---|---|---|---|---|
| body | bone | well | 13.71:1 ok | 4.5 | 13 | 4 | 00 01 |
| body | bone | well_raised | 12.43:1 ok | 4.5 | 13 | 1 | 00 |
| body | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 1 | 00 |
| body_dim | bone_dim | well | 7.67:1 ok | 4.5 | 13 | 1 | 00 |
| body_strong | bone | well | 13.71:1 ok | 4.5 | 14 | 2 | 00 01 |
| brand | blade | well | 6.23:1 ok | 3.0 | 20,22 | 2 | 00 01 |
| button | bone | well_raised | 12.43:1 ok | 4.5 | 13 | 10 | 00 01 |
| button | bone_dim | well | 7.67:1 ok | 4.5 | 13 | 2 | 01 |
| button | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 1 | 00 |
| button_ink | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 1 | 00 |
| button_ink | ink | alert | 6.72:1 ok | 4.5 | 13 | 2 | 00 |
| button_ink | ink | blade | 6.57:1 ok | 4.5 | 13 | 3 | 00 01 |
| label | alert | well | 6.38:1 ok | 4.5 | 11 | 1 | 00 |
| label | blade | well | 6.23:1 ok | 4.5 | 11 | 1 | 00 |
| label | blade_hi | well | 7.61:1 ok | 4.5 | 11 | 3 | 00 01 |
| label | body | well | 8.18:1 ok | 4.5 | 11 | 3 | 00 01 |
| label | bone | well | 13.71:1 ok | 4.5 | 11 | 24 | 00 |
| label | bone_dim | well | 7.67:1 ok | 4.5 | 11 | 44 | 00 01 |
| label | bone_mute | well | 5.81:1 ok | 4.5 | 11 | 3 | 00 |
| label | lcd_amber | well | 10.02:1 ok | 4.5 | 11 | 1 | 00 |
| label | ok | well | 11.02:1 ok | 4.5 | 11 | 1 | 00 |
| label | warn | well | 10.02:1 ok | 4.5 | 11 | 1 | 00 |
| label_b | blade_hi | well | 7.61:1 ok | 4.5 | 11 | 1 | 01 |
| label_b | body | well | 8.18:1 ok | 4.5 | 11 | 1 | 00 |
| label_b | bone | well | 13.71:1 ok | 4.5 | 11 | 17 | 00 01 |
| label_b | bone | well_raised | 12.43:1 ok | 4.5 | 11 | 1 | 00 |
| label_b | ok | well | 11.02:1 ok | 4.5 | 11 | 5 | 00 01 |
| label_b | warn | well | 10.02:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | alert | 6.72:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | alu_well | 10.36:1 ok | 4.5 | 11 | 5 | 00 |
| label_ink | ink | blade_hi | 8.03:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | body | 8.62:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | bone_dim | 8.09:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | lcd_amber | 10.57:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | ok | 11.62:1 ok | 4.5 | 11 | 1 | 00 |
| lcd | lcd_amber | glass | 11.04:1 ok | 4.5 | 14,20,22,26 | 12 | 00 01 |
| lcd_unit | lcd_amber | glass | 11.04:1 ok | 4.5 | 14 | 14 | 00 01 |
| mono | bone | well | 13.71:1 ok | 4.5 | 14 | 2 | 00 01 |
| mono | bone | well_raised | 12.43:1 ok | 4.5 | 14 | 1 | 00 |
| mono | lcd_amber | glass | 11.04:1 ok | 4.5 | 14 | 2 | 01 |
| mono_dim | bone_dim | well | 7.67:1 ok | 4.5 | 14 | 1 | 01 |
| nixie | blade_hi | #3d2413 | 6.13:1 ok | 3.0 | 34 | 2 | 00 01 |
| section | blade | well | 6.23:1 ok | 4.5 | 18 | 1 | 01 |
| section | ink | alu_well | 10.36:1 ok | 4.5 | 13 | 18 | 00 01 |
| section_mixed | ink | alu_well | 10.36:1 ok | 4.5 | 13 | 1 | 01 |

203 strings measured on 2 screens: all pass WCAG 2.x AA

## Open design choices

- The Blender renders in `plugin/assets/` are opaque and carry a studio backdrop (and the amp OLEDs name real amp models, the
  pedal faces say STOCKHOLM SYNDROME / TIGHTEN). The mockups crop the backdrops and cover titles with riveted nameplates and
  OLEDs with flat wells. Task B re-renders the faces with the new names.
- The cab render is shown at its fixed 4x12 size; the right 170 px of the rig area is left as floor.
