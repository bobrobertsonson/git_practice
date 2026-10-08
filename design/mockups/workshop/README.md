# Workshop mockups (v1.0 Task A)

Procedural mockups of the "worn metal and sawdust" skin. Spec: `docs/specs/v1_0-A-workshop_mockups.md`. These are
mockups for the user to choose from and edit; nothing here is built into the plugin and nothing under `plugin/` changes.

Review page (all screens, for comments): https://claude.ai/artifact/SPkazo41kcKHRVMaJNoVqo (source: `review.html`).

## Screens

All 1280 x 800 design px, rendered by `render_all.py` in two wear sets: `png/` (subtle, the approved look) and `png_strong/`
(strong: more and larger edge chips with bare steel and rust bloom, deeper scratches, grime, scuffed aluminium, denser sawdust;
texture only, so the flat-well rule and contrast checks are unchanged). Same file names in both. Screens 02-09 come from `screens_panels.py`
(02, 03, 04, 09), `screens_tools.py` (05-08); 01 from `screens_rig.py`; 00 from `design/render/workshop_style.py`.

| file (both sets) | screen | shows |
|---|---|---|
| `png/ · png_strong/00_style_sheet.png` | style sheet | palette with ratios, materials (worn steel, brushed alu, bench wood, sawdust, riveted plate), type, control kit, LCD / nixie, badges, frames |
| `png/ · png_strong/01_main_rig.png` | main rig | BLADE / BODY heads, cab, pedalboard (THE SAW MILL selected, CHISEL), inspector with circuit selector, BLEND nixie, GATE / POST EQ / VISE / OUTPUT with LCDs |
| `png/ · png_strong/02_top_bar.png` | top bar | five states (default, A/B slot names, UNCAL chip, OUT OF TRUE chip, WOODSHED open + MATCH 42 %) and two 2x zooms |
| `png/ · png_strong/03_settings_calibration.png` | Settings: INPUT CALIBRATION | UNCAL / CALIBRATED status, Scarlett 4i4 device presets (3rd Gen INST, INST + PAD, 4th Gen INST, Custom, guided; only INST levels are stored), "Input: L only (auto)" caption, Enter dBu field + LCD, input channel AUTO / L / R / MIX, CALIBRATED LEVELS toggle (BETA), learned gate floor |
| `png/ · png_strong/04_preset_browser.png` | preset browser | banks, categories, list (one PARSE ERROR row), info panel with captures + licences + NON-COMMERCIAL, the LEGACY LEVELS hint with USE CALIBRATED LEVELS / KEEP AS SAVED, resolve status |
| `png/ · png_strong/05_match.png` | MATCH | reference song, your DI (takes), auto-refine, REFINED + PREVIEW results, spectrum (solid vs dashed), 9/10 rules |
| `png/ · png_strong/06_nam_forger.png` | NAM FORGER | mode cards, studio-blend card, size, validation DI, VISE drop/keep, output folder, what goes into the model, credits, TRAIN / RESUME, export notes, training strip |
| `png/ · png_strong/07_woodshed.png` | WOODSHED | the cassette-deck render (`assets/woodshed_deck.png`) docked at the bottom as the panel, with a riveted WOODSHED nameplate over the baked lettering: position LCD `01:23.4 / 04:12.0` on its OLED, BACKING `-3.5 dB`, GUITAR STEM GHOST, count-in + BPM, loop A / B times + LOOP ON, PLAYING; a worn-steel side panel for SONG FILE… / STEMS FOLDER…, song status, KEEP KEYS, OFFSET, SYNC TO HOST, REC + lamp + timer, takes, USE FOR MATCH, MATCH, NAM FORGER |
| `png/ · png_strong/08_rig_editor.png` | rig editor | topology selector, tabs (CHAIN active), lanes A · BLADE / B · BODY, block cards with UNCAL badge and a BYPASSED (hatched) card, cab notice |
| `png/ · png_strong/09_notices.png` | notices | OUT OF TRUE banner + chip, uncalibrated banner + chip, UNCAL badge on a card / amp label / render report, confirm and error dialogs, toast |

## Run

    python3 design/mockups/workshop/render_all.py                       # render both wear sets (png/ and png_strong/)
    python3 design/mockups/workshop/render_all.py --wear strong         # one set only (subtle | strong | all, default all)
    python3 design/mockups/workshop/render_all.py --check               # fresh render vs committed PNGs, both sets (0 same / 1 differ / 77 deps)
    python3 design/mockups/workshop/render_all.py --contrast --wear subtle   # measured contrast table (below); exit 1 if any pair < AA
    python3 design/render/workshop_style.py --out DIR                   # only 00_style_sheet.png (subtle)

Pillow 12.3.0 and numpy 2.5.3 on Python 3.13.16 produced the committed PNGs (FreeType is
Pillow's bundled build). Same versions give bit-identical PNGs (both sets use their own fixed seeds: strong derives them from the name + `:strong`); `--check` compares decoded RGBA, so a different zlib still passes.

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

## Measured contrast (`--contrast --wear subtle|strong`)

AA needs 4.5:1 (3:1 for large text). Backdrop strings that stay visible behind an overlay are measured with their colours scaled by the dimming. Lowest ratio of any text in any screen: **5.19:1** (the dimmed BLADE label behind the WOODSHED panel); lowest of the primary UI text is 5.27:1 (`bone_mute` on `well_raised`, disabled / tertiary text); primary body text is 13.71:1.

### subtle

| style | fg | bg | ratio | AA needs | px sizes | uses | screens |
|---|---|---|---|---|---|---|---|
| body | bone | well | 13.71:1 ok | 4.5 | 13 | 55 | 00 01 03 04 05 06 07 08 09 |
| body | bone | well_raised | 12.43:1 ok | 4.5 | 13 | 4 | 00 03 06 |
| body | bone_mute | well | 5.81:1 ok | 4.5 | 13 | 1 | 04 |
| body | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 2 | 00 04 |
| body | ok | well | 11.02:1 ok | 4.5 | 13 | 4 | 05 07 08 |
| body | warn | well | 10.02:1 ok | 4.5 | 13 | 1 | 04 |
| body_dim | bone_dim | well | 7.67:1 ok | 4.5 | 13 | 75 | 00 02 03 04 05 06 08 09 |
| body_dim | bone_dim | well_raised | 6.96:1 ok | 4.5 | 13 | 1 | 04 |
| body_strong | alert | well | 6.38:1 ok | 4.5 | 14 | 1 | 09 |
| body_strong | blade_hi | well | 7.61:1 ok | 4.5 | 14 | 1 | 08 |
| body_strong | bone | well | 13.71:1 ok | 4.5 | 13,14 | 26 | 00 01 02 03 04 05 06 07 08 09 |
| body_strong | bone | well_raised | 12.43:1 ok | 4.5 | 14 | 3 | 03 04 05 |
| body_strong | bone_dim | well | 7.67:1 ok | 4.5 | 14 | 1 | 06 |
| body_strong | ok | well | 11.02:1 ok | 4.5 | 14 | 7 | 03 06 08 |
| body_strong | ok | well_raised | 9.99:1 ok | 4.5 | 14 | 1 | 03 |
| body_strong | warn | well | 10.02:1 ok | 4.5 | 14 | 4 | 06 09 |
| brand | blade | well | 6.23:1 ok | 3.0 | 20,22 | 14 | 00 01 02 03 04 05 06 07 08 09 |
| brand | ink | alu_well | 10.36:1 ok | 3.0 | 22 | 3 | 05 06 08 |
| button | #8f8777 | #151210 | 5.24:1 ok | 4.5 | 13 | 1 | 07 |
| button | blade_hi | well_raised | 6.90:1 ok | 4.5 | 13 | 2 | 03 04 |
| button | bone | well_raised | 12.43:1 ok | 4.5 | 13 | 137 | 00 01 02 03 04 05 06 07 08 09 |
| button | bone_dim | well | 7.67:1 ok | 4.5 | 13 | 12 | 01 03 08 |
| button | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 1 | 00 |
| button_ink | bone_mute | well_raised | 5.27:1 ok | 4.5 | 13 | 1 | 00 |
| button_ink | ink | alert | 6.72:1 ok | 4.5 | 13 | 4 | 00 04 09 |
| button_ink | ink | blade | 6.57:1 ok | 4.5 | 13 | 24 | 00 01 02 03 04 05 06 07 08 09 |
| label | #cc6e31 | #151210 | 5.19:1 ok | 4.5 | 11 | 1 | 07 |
| label | alert | well | 6.38:1 ok | 4.5 | 11 | 1 | 00 |
| label | blade | well | 6.23:1 ok | 4.5 | 11 | 1 | 00 |
| label | blade_hi | well | 7.61:1 ok | 4.5 | 11 | 4 | 00 01 09 |
| label | body | well | 8.18:1 ok | 4.5 | 11 | 3 | 00 01 |
| label | bone | well | 13.71:1 ok | 4.5 | 11 | 31 | 00 04 |
| label | bone_dim | well | 7.67:1 ok | 4.5 | 11 | 119 | 00 01 03 04 05 06 07 08 |
| label | bone_dim | well_raised | 6.96:1 ok | 4.5 | 11 | 1 | 04 |
| label | bone_mute | well | 5.81:1 ok | 4.5 | 11 | 4 | 00 04 |
| label | lcd_amber | well | 10.02:1 ok | 4.5 | 11 | 1 | 00 |
| label | ok | well | 11.02:1 ok | 4.5 | 11 | 1 | 00 |
| label | warn | well | 10.02:1 ok | 4.5 | 11 | 1 | 00 |
| label_b | alert | well | 6.38:1 ok | 4.5 | 11 | 4 | 02 04 09 |
| label_b | blade | well | 6.23:1 ok | 4.5 | 11 | 2 | 04 |
| label_b | blade_hi | well | 7.61:1 ok | 4.5 | 11 | 22 | 01 02 06 09 |
| label_b | body | well | 8.18:1 ok | 4.5 | 11 | 6 | 00 02 04 05 06 |
| label_b | bone | well | 13.71:1 ok | 4.5 | 11 | 114 | 00 01 02 03 04 05 06 07 08 09 |
| label_b | bone | well_raised | 12.43:1 ok | 4.5 | 11 | 6 | 00 04 08 |
| label_b | bone_dim | well | 7.67:1 ok | 4.5 | 11 | 1 | 04 |
| label_b | ok | well | 11.02:1 ok | 4.5 | 11 | 21 | 00 01 02 03 04 05 06 07 08 09 |
| label_b | ok | well_raised | 9.99:1 ok | 4.5 | 11 | 2 | 03 05 |
| label_b | warn | well | 10.02:1 ok | 4.5 | 11 | 7 | 00 02 03 05 09 |
| label_b | warn | well_raised | 9.09:1 ok | 4.5 | 11 | 1 | 03 |
| label_ink | ink | alert | 6.72:1 ok | 4.5 | 11 | 1 | 00 |
| label_ink | ink | alu_well | 10.36:1 ok | 4.5 | 11 | 19 | 00 04 05 06 08 09 |
| label_ink | ink | blade_hi | 8.03:1 ok | 4.5 | 11 | 2 | 00 03 |
| label_ink | ink | body | 8.62:1 ok | 4.5 | 11 | 3 | 00 05 |
| label_ink | ink | bone_dim | 8.09:1 ok | 4.5 | 11 | 3 | 00 04 06 |
| label_ink | ink | lcd_amber | 10.57:1 ok | 4.5 | 11 | 7 | 00 03 08 09 |
| label_ink | ink | ok | 11.62:1 ok | 4.5 | 11 | 2 | 00 05 |
| lcd | lcd_amber | glass | 11.04:1 ok | 4.5 | 14,16,18,20,22,26 | 48 | 00 01 02 03 05 06 07 08 |
| lcd_unit | lcd_amber | glass | 11.04:1 ok | 4.5 | 14 | 72 | 00 01 02 03 04 05 06 07 08 09 |
| mono | blade_hi | well_raised | 6.90:1 ok | 4.5 | 13,14 | 3 | 05 07 |
| mono | bone | well | 13.71:1 ok | 4.5 | 13,14 | 31 | 00 01 03 04 05 06 07 08 09 |
| mono | bone | well_raised | 12.43:1 ok | 4.5 | 14 | 4 | 00 03 04 06 |
| mono | lcd_amber | glass | 11.04:1 ok | 4.5 | 14 | 2 | 01 |
| mono_dim | bone_dim | well | 7.67:1 ok | 4.5 | 13,14 | 26 | 01 03 04 05 06 07 08 09 |
| mono_dim | bone_dim | well_raised | 6.96:1 ok | 4.5 | 13,14 | 2 | 05 07 |
| nixie | blade_hi | #3d2413 | 6.13:1 ok | 3.0 | 34 | 5 | 00 01 03 07 08 |
| section | blade | well | 6.23:1 ok | 4.5 | 18 | 4 | 01 03 07 08 |
| section | ink | alu_well | 10.36:1 ok | 4.5 | 13 | 50 | 00 01 03 04 05 06 07 08 09 |
| section_mixed | ink | alu_well | 10.36:1 ok | 4.5 | 13 | 4 | 01 03 07 08 |
| title | bone | well | 13.71:1 ok | 4.5 | 18,22 | 2 | 03 04 |

1030 strings measured on 10 screens: all pass WCAG 2.x AA

### strong

strong set: identical ratios (same layouts), verified by `--contrast --wear strong`, lowest 5.19:1.

## Notes and open items

- Task B re-renders the Blender faces with the new names: the amp OLEDs name a real amp model, the pedal faces say
  STOCKHOLM SYNDROME / TIGHTEN, and the cassette deck's baked "PLAY-ALONG DECK" lettering is covered by a WOODSHED
  nameplate. The deck render also has baked micro-lettering (REW, FF, STOP, PLAY / PAUSE, LOOP A / B, COUNT-IN, 4 STEM, about
  5 px) that cannot be restyled; Task B re-renders the deck without lettering so the UI draws those labels. In the mockup the
  GHOST / MUTE legends are covered by kit wells showing MUTE / GHOST / FULL with GHOST marked. The mockups crop the backdrops and cover titles with riveted nameplates and OLEDs with flat wells.
- The cab render is shown at its fixed 4x12 size; the right 170 px of the rig area is left as floor.
- Backdrop text: every string the rig drew that stays visible behind an overlay or panel (the inspector, the BLADE label above
  the WOODSHED deck) is measured in the contrast log, dimmed colours included; only strings fully covered are left out.
- Wording invented for the mockups (notes, error reasons, toast text, file names, creator handles, capture titles) is
  placeholder and fictional. The Scarlett 4i4 levels (3rd Gen +12.5 dBu INST, +14 dBu INST + PAD; 4th Gen +12 dBu INST) are the instrument-input spec-sheet figures, to confirm in v0.8; device names are a nominative compatibility reference, no logos.
- OUT OF TRUE is drawn as "Your playing level is running ~4.5 dB hotter than when this interface was set up — did the interface gain change?" (the real UI says hotter or quieter); the drift definition belongs to v0.8.

## WOODSHED deck asset (pinned input)

`assets/woodshed_deck.png` (1210 x 878, 1.1 MB) is the cassette deck from `design/render/playalong_deck.py`, rendered with
Blender (bpy 4.2.0, Cycles) on 2026-10-08, cropped to the object from the full 1260 x 980 frame (crop box 26,54 - 1236,932):

    cd design/render && nice python3 playalong_deck.py --mode ortho --out <scratch dir> --scale 70 --samples 64

Cycles output is not bit-reproducible across machines, so the committed PNG is the pinned input and the mockup render stays
deterministic from it. The mockup's overlays (nameplate, LCDs, wells) are positioned in that image's pixel coordinates
(`DECK_NATIVE` and `D()` in `screens_tools.py`), so re-rendering the deck needs the overlay positions re-checked.
