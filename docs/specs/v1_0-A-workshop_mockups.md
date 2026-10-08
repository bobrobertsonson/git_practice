# v1.0 Task A — workshop style sheet and screen mockups (implementation spec)

Parent brief: `docs/specs/v1_0-ui_workshop_skin.md`. User direction (2026-10-08): "it should look like the pedals and amps
in general — worn metal and sawdust is great." These are **mockups for the user to choose from and edit**; nothing here
is built into the plugin. **No change under `plugin/` at all** (v0.8 and v0.3.1 are editing it); `plugin/assets/*` is
read-only input.

**Update 2026-10-08 (user decisions on the first set):** the TS-style pedal is **CHISEL** (replaces TS-STYLE below);
readouts stay amber LCD + orange nixie for BLEND; the kit gains `wear = subtle | strong` and both full sets are rendered
(`png/`, `png_strong/`), with the same flat-well and contrast rules; WOODSHED (07) is built around the cassette-deck
render of `design/render/playalong_deck.py` (committed as the pinned input `design/mockups/workshop/assets/woodshed_deck.png`);
lettering, the A/B mini footswitches and the legacy-levels placement are approved. v0.8: OUT OF TRUE sentence and
INST-only device presets as drawn in 03 / 09.

## Deliverables

| path | what |
|---|---|
| `design/render/workshop_style.py` | the kit (importable module) **and** the style-sheet renderer (`python3 design/render/workshop_style.py --out DIR` writes `00_style_sheet.png`) |
| `design/mockups/workshop/screens_*.py` | the screen scripts (import the kit; one function per screen) |
| `design/mockups/workshop/render_all.py` | driver: renders everything into `design/mockups/workshop/png/`; `--check` re-renders to a temp dir and compares decoded RGBA with the committed PNGs (exit 0 same / 1 differ / 77 deps or fonts missing); `--contrast` prints the measured contrast table (Markdown) |
| `design/mockups/workshop/png/*.png` | the committed renders, 1280 x 800, RGB |
| `design/mockups/workshop/README.md` | screen list, how to run, fonts, measured contrast table, open design choices |

Screens (file names fixed): `00_style_sheet`, `01_main_rig`, `02_top_bar`, `03_settings_calibration`, `04_preset_browser`,
`05_match`, `06_nam_forger`, `07_woodshed`, `08_rig_editor`, `09_notices`.

## Pipeline rules (reproducibility)

- **Pillow + numpy only** (like `design/render/app_icon.py`): no Blender, no system fonts, no network at render time
  except the font fetch below. The existing Blender renders in `plugin/assets/` (amp heads, cab, pedals, knob /
  footswitch / LED / toggle filmstrips) are composited in as images, which is what ties the mockups to the amp and pedal
  look. Read sidecar JSONs for frame sizes; frame for value v = `round(v * (frames - 1))`. Downscale with LANCZOS on
  premultiplied alpha (`convert('RGBa')`), as `app_icon.py` does.
- **Deterministic:** every random texture uses `numpy.random.default_rng(seed)` with a fixed seed per element (derive it
  from a stable string, e.g. `zlib.crc32(name)`, never `hash()`). No time, no dict-order or filesystem-order dependence.
  Same Pillow / FreeType / numpy → bit-identical PNGs. Record the versions used in the README.
- **Fonts** (all SIL OFL 1.1), fetched at run time into `--font-dir` (default `~/.cache/sawblade_fonts`), each verified
  by SHA-256; a missing or mismatching font is a hard error (exit 77), never a silent fallback. Never commit `.ttf`.
  Record them in the README and in `docs/THIRD_PARTY.md` (design-time only, not shipped).

  | role | font | URL | sha256 |
  |---|---|---|---|
  | brand / big titles | Black Ops One | https://fonts.gstatic.com/s/blackopsone/v21/qWcsB6-ypo7xBdr6Xshe96H3WDw.ttf | bd8a70e63df108745316c6ad277874cbe139bbb90cbcaf705810ecc089fe59f8 |
  | stencil section names | Allerta Stencil | https://fonts.gstatic.com/s/allertastencil/v24/HTx0L209KT-LmIE9N7OR6eiycOeF-w.ttf | 036e8216a18f1b06036ac0081feed72bc63562ee70920a8151ca7c441f91c753 |
  | labels, buttons (caps) | Barlow Condensed 600 | https://fonts.gstatic.com/s/barlowcondensed/v13/HTxwL3I-JCGChYJ8VI-L6OO_au7B4873_3E.ttf | 0d85af813fc3ed87db0c6265515689b2eef5cbaf7aab17922528dfc95a2cd73f |
  | labels, buttons (caps, bold) | Barlow Condensed 700 | https://fonts.gstatic.com/s/barlowcondensed/v13/HTxwL3I-JCGChYJ8VI-L6OO_au7B46r2_3E.ttf | 7dde307fa887fc65ff5830cfada77a7decc5dae8d3c816c9d39ba3f1af1c4ed7 |
  | body text | Barlow 500 | https://fonts.gstatic.com/s/barlow/v13/7cHqv4kjgoGqM7E3_-gc4A.ttf | 91c841fdfa8e7b94ffedbb983a363947ba6ed720f3bbf0c71d48b618053655bc |
  | body text strong | Barlow 600 | https://fonts.gstatic.com/s/barlow/v13/7cHqv4kjgoGqM7E30-8c4A.ttf | c15439e7a03af5714282ec1780ff7b0214ec6a7db96300b54928dbcd2569ca0c |
  | readouts (alphanumeric) | Share Tech Mono | https://fonts.gstatic.com/s/sharetechmono/v16/J7aHnp1uDWRBEqV98dVQztYldFc7pA.ttf | 5f6b57538a1a35469a038dc3073003cebc4c101ad7b3c219e9555a3b3c0a81d2 |

- Render scale: draw at 2x (2560 x 1600) and downsample to 1280 x 800 with LANCZOS, or draw at 1x with antialiased
  primitives; either is fine as long as it is deterministic. Text must stay crisp.
- PNG size: keep each file under 2.5 MB (`optimize=True`); textures are subtle, not photographic noise.

## Legibility (hard requirement, enforced by the kit)

1. **No texture behind text.** Text is drawn only through the kit's text function, which takes the rectangle it sits on
   and **asserts that the area under the text's bounding box (+2 px) is a single flat colour** before drawing (texture,
   sawdust, wood grain, gradients and scratches are all forbidden there). Text sits on *wells*: flat label strips,
   plates, buttons, LCD glass, dialog bodies. Panels may be textured around their wells.
2. **Contrast measured, not assumed.** The text function logs `(style, fg, bg, px size, bold)` for every string drawn on
   every screen. `render_all.py` fails (exit 1) if any logged pair is below WCAG 2.x AA: **4.5:1** for normal text,
   **3:1** only for large text (>= 24 px, or >= 18.67 px bold). `--contrast` prints, per text style: fg, bg, ratio
   (min over all uses), sizes, number of uses. The README carries this table, pasted from the tool's output.
   Disabled controls are not exempt in our mockups: disabled text still meets 4.5:1 and says DISABLED / shows a
   reason, so state is never carried by dimming alone.
3. **No state by colour alone.** Every state has a word, a shape or a position as well as a colour: LEDs come with an
   ON / OFF word; toggles show lever position + the selected label in a well; bypass shows the word BYPASSED and a
   diagonal-hatch overlay on the card (hatch on the card body, never under text); UNCAL is a stamped badge with the
   word and a notched-tag shape; OUT OF TRUE has a "skewed level" glyph + the words; selected rows have a left bar +
   a marker glyph (▶), not only a tint; the A/B active slot has a pressed footswitch frame + the word ACTIVE.
4. Minimum body text 13 px at 1x; minimum label 11 px caps with tracking; readouts >= 14 px.

## Palette (tokens; exact sRGB)

| token | hex | use |
|---|---|---|
| `bench_dark` | #0f0c09 | deepest background, behind the bench |
| `bench` | #22180f | workbench wood base (textured) |
| `bench_hi` | #3a2a1b | wood grain highlights |
| `sawdust` | #c9a36b | sawdust particles (texture only, never under text) |
| `steel` | #1f1d1a | worn painted steel panels (textured) |
| `steel_bare` | #8d8a83 | bare steel showing through chips and edge wear |
| `rust` | #6b3a1c | sparse rust specks at chips |
| `alu` | #b9b5ab | brushed aluminium plates (textured outside wells) |
| `alu_well` | #c4c0b6 | flat label area on aluminium |
| `well` | #1a1714 | flat dark text well (the default text background) |
| `well_raised` | #24201b | buttons, inputs |
| `glass` | #0d0b08 | LCD / nixie glass |
| `bone` | #e8e1d2 | primary text on dark (13.7:1 on `well`) |
| `bone_dim` | #b3a995 | secondary text (7.7:1) |
| `bone_mute` | #9c927f | tertiary / disabled text (5.8:1) |
| `ink` | #14110d | text on aluminium and on bright buttons (9.2:1 on `alu_well`) |
| `blade` | #ff6a1a | path A / BLADE, primary action (6.2:1 as text on `well`) |
| `blade_hi` | #ff8a3d | BLADE text accents |
| `body` | #7fb4ea | path B / BODY (8.2:1) |
| `lcd_amber` | #ffb347 | LCD digits (11:1 on `glass`) |
| `ok` | #7fe08f | OK / ON / calibrated (11:1) |
| `warn` | #ffb347 | warnings, UNCAL (same hue as LCD amber, always with the word) |
| `alert` | #ff6b5a | errors, OUT OF TRUE (6.4:1) |

The kit asserts the table's ratios at import (cheap) and the per-use log does the rest. Add tokens only if a screen
needs them; every new text token goes through the same check.

## Materials (procedural, numpy)

- **Worn painted steel** (panels, overlay frames): flat `steel` paint + low-frequency mottling (a few % luminance),
  fine scratches (thin lines, slightly lighter, random angle clusters), **edge wear**: chips near the panel edges that
  reveal `steel_bare` (distance-to-edge x noise threshold; chips denser at corners), sparse `rust` specks at chips.
- **Brushed aluminium** (header plates, nameplates, sliders): horizontal streaks (1-D noise stretched along x), a few
  scuffs (short bright arcs), darker edge band + a 1 px top highlight bevel, edge nicks.
- **Dark workbench wood** (rig floor and page backgrounds): plank boards with seams, grain from warped sine bands,
  saw-cut marks, oil darkening; very dark overall so renders and panels sit on it.
- **Sawdust**: fine tan speckles (1-3 px, varied alpha), drifting in clumps along plank seams and against the bottom
  edges of objects; masked out of every text well and control (sawdust is texture only).
- **Riveted plate frame**: panel headers are aluminium (or steel) plates with domed rivets (shaded circle + highlight)
  at the corners and every ~120 px along long edges, plus a cast shadow under the plate. The header's text sits in a
  flat `alu_well` strip stamped into the plate.

## Type

- Section names: **Allerta Stencil**, caps, tracking ~0.08 em, `ink` on `alu_well` (stamped look: a 1 px darker inset
  line above the well and a 1 px lighter one below — outside the text box, so the text area stays flat).
- Brand mark SAWBLADE and big screen titles (MATCH, NAM FORGER, WOODSHED, INPUT CALIBRATION...): **Black Ops One**.
- Labels / buttons: Barlow Condensed 600/700 caps, tracking ~0.12 em.
- Body copy: Barlow 500 (600 for emphasis), sentence case, 13-15 px.
- Readouts: (a) **LCD**: procedural 7-segment digits drawn by the kit (0-9, minus, decimal point, blank; segments
  slanted ~6°, unlit segments drawn as a faint ghost which is decoration and not text) in `lcd_amber` on `glass`, with
  the unit (dB, Hz, ms, dBu, %, smp) in Share Tech Mono to the right inside the same glass; (b) **nixie**: Share Tech Mono
  digits in `blade_hi` with a soft glow (blurred copy, same flat `glass` behind — no mesh behind digits), used only for
  the big BLEND readout. Alphanumeric status text in readouts: Share Tech Mono.

## Control kit (all in `workshop_style.py`, reused by every screen)

- `knob(kind, value, size)` — the `knob_amp` / `knob_pedal` filmstrips scaled down, plus a drop shadow, a value arc in
  the path colour (arc is drawn by the UI, per the sidecar note) and a label well under it; optional LCD readout.
- `footswitch(down)` — the filmstrip; used for A/B, bypass, START/TRAIN as "stomp" actions where it fits.
- `led(on)` — `led_orange` sprite (tint variants by hue rotation are fine) + the ON/OFF word.
- `toggle(pos, labels)` — mini toggle (the pedal `toggle` part; if `plugin/assets` lacks a toggle filmstrip, draw it
  procedurally in chrome: bat lever + hex nut) with the labels at the throw positions, selected label in a well with a
  marker.
- `rotary_selector(options, index)` — a pointer knob with stamped option labels around it (2-5 positions), selected
  label boxed; used for topology, input channel (AUTO / L / R / MIX), guitar stem (MUTE / GHOST / FULL), size.
- `button(text, kind)` — `primary` (blade fill, ink text), `secondary` (well_raised, bone text, 1 px steel_bare
  border), `danger`; pressed / disabled variants (disabled says why in a caption next to it).
- `lcd(value, unit, digits)`, `nixie(text)`, `text_field(text)`, `dropdown(text)`, `checkbox(on, label)`.
- `plate(rect, title)` — riveted header plate; `panel(rect)` — worn steel panel; `card(rect)` — block card (steel with
  a stamped title strip); `badge(kind)` — `UNCAL`, `NON-COMMERCIAL`, `LEGACY LEVELS`, `PREVIEW`, `REFINED`, `BETA`,
  `OUT OF TRUE`: notched-tag shapes with the word, colour + glyph.
- `text(img, xy, string, style, bg_rect)` — the only way to draw text (see Legibility).

## 00 — style sheet (`workshop_style.py --out DIR`)

One 1280 x 800 sheet in the same look: palette swatches with token names, hex and the contrast ratio of each text
token on its background; material swatches (worn steel, brushed alu, workbench wood, sawdust on wood, riveted plate);
type specimens (each role with its font name and size); the control kit (knobs at 0 / 0.5 / 1 with LCD readouts,
footswitch up/down, LED off/on with words, toggles, a rotary selector, buttons in every state, LCD + nixie, badges);
panel frames. Title plate: "SAWBLADE · WORKSHOP STYLE".

## Screens

Common: 1280 x 800. Top bar 58 px (shown on every screen, see 02). Overlays sit over the rig area (940 x 742 at x=0,
y=58) like today, the inspector (340 px, right) stays visible unless the screen says otherwise; behind an overlay the rig
is visible but darkened (multiply ~35 %). Use the display names of `docs/NAMES.md`: **BLADE** (path A), **BODY**
(path B), BLEND, GATE, **VISE** (bus comp), CAB, POST EQ, MATCH, NAM FORGER, WOODSHED, INPUT CALIBRATION, OUT OF TRUE.
The chainsaw pedal is **THE SAW MILL** with circuit names BUZZSAW / TAR PIT / SERRATED / HATCHET. The TS-style pedal is
shown as **TS-STYLE** everywhere (the user is still choosing its name). Never draw a trademark or a real product's
trade dress; capture titles in mockups are generic ("Plexi-style head, bright cap", "Swedish chainsaw pedal", "Boutique
high-gain 5150-style" is NOT allowed — say "high-gain US head"), creators are fictional handles (`@bench_tones`,
`@coldiron_caps`, `@marrow_amps`, `@swamp_rig`). Preset names cover several genres (thrash, doom, black metal,
metalcore, crust, Swedish death) — not only the HM-2 case.

The baked face art of `pedal_saw.png` says STOCKHOLM SYNDROME and its OLED a capture name; `pedal_body.png` says
TIGHTEN. In the mockups, cover those titles with a riveted aluminium nameplate reading THE SAW MILL / TS-STYLE (and the
OLED with a flat OLED well saying `BUZZSAW · A2 · 48k`). Note in the README that Task B re-renders the faces with the
new names.

**01 main rig.** RigReal layout (`design/mockups/RigReal.dc.html`) in the new materials: workbench-wood floor with
sawdust; BLADE amp head (amp_saw) and BODY amp head (amp_body) stacked left, cab (cab_4x12) right of them with label
well `CAB · 4x12 · SHARED · DOUBLE-CLICK FOR MIC`; pedalboard (worn steel with gaffer-tape strips) carrying THE SAW
MILL (selected: a BLADE-colour frame + `▶ SELECTED` label) and TS-STYLE; two `+ PEDAL` empty slots (dashed frame on a
flat well). Path labels in wells: `BLADE · THE SAW MILL → SAW HEAD`, `BODY · TS-STYLE → BODY HEAD`. Inspector (riveted
plate title `INSPECTOR`): `SELECTED · BLADE PEDAL`, `THE SAW MILL`, circuit rotary selector BUZZSAW / TAR PIT /
SERRATED / HATCHET (BUZZSAW selected), capture credit line, BROWSE CAPTURES button, BLEND knob with nixie readout
`79 / 21` and caption `BLADE 79 · BODY 21`, `ALIGN −17 smp · Ø NORMAL` LCD, four master knobs GATE / POST EQ / VISE /
OUTPUT each with an LCD readout (−33.1 dB, 0.0 dB, 2.0 : 1 → show `2.0` + `:1`, −6.0 dB), LEARN GATE button, and a
plate `MATCH vs ORIGINAL` with `6.45 → 1.53 dB · 9/10 rules`.

**02 top bar.** A spec sheet for the top bar: the bar at full width on top, then the bar in its states, each labelled
in a small caption well: (1) default; (2) A/B with slot names (`A ▸ GRAVE DIRT · MATCHED v2` ACTIVE, `B ▸ THRASH TIGHT`)
per v0.3.1 Task C; (3) UNCAL chip (`UNCAL · interface not calibrated`); (4) OUT OF TRUE chip; (5) WOODSHED open
(button pressed) and MATCH running (`MATCH 42 %` LCD). Then 2x zoom crops of the left half and right half of the bar.
Bar contents left → right: SAWBLADE brand mark (Black Ops One, blade orange on a flat well), preset selector `‹ name ›`,
A/B footswitch pair, RIG, WOODSHED, gear (Settings), spacer, status LCD `LAT 92 smp` and `CPU 18 %`, LIVE / STUDIO chip
(with the word), MATCH, NAM FORGER (primary). Bar material: brushed aluminium rail with rivets at both ends; every label
sits in a flat well.

**03 Settings — INPUT CALIBRATION.** Overlay (riveted steel frame, title plate `SETTINGS`, close x, DONE). Left: a
section list (Setup checklist, Tools, TONE3000, **INPUT CALIBRATION** (selected), Captures, Separation, Recording,
Appearance, About Sawblade…). Right: the INPUT CALIBRATION section:
- Status strip: `▲ UNCAL — interface not calibrated: captures play at the +9.0 dBu default` with badge, and the
  calibrated variant shown as a second small state (`✓ CALIBRATED · 4i4 3rd gen · INST · +12.5 dBu · 2026-10-08`).
- `INTERFACE` dropdown of device presets: `4i4 3rd gen — INST input (+12.5 dBu)` (selected), `4i4 3rd gen — LINE input
  (+22.0 dBu)`, `Custom — enter dBu`, `Not sure — measure it (guided)`. A small note: values from the maker's spec
  sheet; check the input is in INST mode.
- `MAX INPUT LEVEL` field: **Enter dBu** — a text field + LCD showing `+12.5 dBu`, with a stepper.
- `INPUT CHANNEL` rotary selector AUTO / L / R / MIX (AUTO selected) + caption `AUTO: uses the channel with signal;
  MIX sums L+R (−6 dB)`.
- `CALIBRATED LEVELS` toggle with a `BETA` badge: lever + `ON` / `OFF` labels, caption `Every capture gets the level its
  creator used, computed on load.`
- `GATE FLOOR` LCD `−42.0 dBFS learned` (read-only, from v0.8 B).
- Buttons: `APPLY`, `MEASURE…` (guided).

**04 preset browser.** Overlay over rig + inspector (full 1280 width below the top bar). Left column: search field,
banks (FACTORY: Classic / Styles / Matched; USER) as stamped tabs, categories with counts. Middle: preset list
(name, category, bank; selected row with left bar + ▶; one unparseable row in muted text with `PARSE ERROR` word).
Right: info panel (name, category, bank, file, notes, captures per path BLADE / BODY / CAB with title, @creator,
licence, TONE3000 link text, a `NON-COMMERCIAL` badge on a `cc-by-nc` capture, one `local file: no attribution
recorded`). **The selected preset is a legacy one**: a hint plate across the top of the info panel —
`LEGACY LEVELS · saved before input calibration. It plays with the input gains it was saved with.` and buttons
`USE CALIBRATED LEVELS` (primary) / `KEEP AS SAVED`. Footer: LOAD FILE…, SAVE, SAVE AS, RENAME, DELETE, resolve status
LCD-style `Resolving 2/5: Swedish chainsaw pedal` with CANCEL.

**05 MATCH.** Full overlay with title plate `MATCH`. Left column: `1 · REFERENCE SONG` (SONG FILE… / STEMS FOLDER…,
loaded song name, separation done), `2 · YOUR DI` (REC / STOP footswitch-style buttons, take picker newest first, the
selected take), TOOLS (auto-refine toggle ON), `START MATCH` primary. Right: results header with `REFINED READY` badge;
REFINED section (3 candidate rows: rank, chain summary e.g. `BUZZSAW → SAW HEAD ║ TS-STYLE → BODY HEAD · 4x12`, score
`1.53 dB`, AUDITION / APPLY), PREVIEW section below (2 rows, PREVIEW badges), `APPLY REFINED BEST`; a spectrum plate
(reference vs match curves, legend with line styles solid / dashed, not colour alone) and LCD `9/10 rules`.

**06 NAM FORGER.** Full overlay, title plate `NAM FORGER` (Black Ops One). Configure view: two mode cards `NO-CAB + IR`
(selected, `LIVE-COMPATIBLE: exact`) / `WITH CAB`; `STUDIO BLEND` info card; SIZE rotary FEATHER / LITE / STANDARD with
`last run: 14 min`; VALIDATION DI (`LAST TAKE · take_2026-10-08_2114`); VISE (DROP COMP / KEEP COMP toggle); OUTPUT
FOLDER + CHOOSE…; rig summary; "what goes into the model" list (✓ / ✗ glyphs + words: `✗ GATE — left out`,
`✓ VISE — release 80 ms, trainable`); credits with a NON-COMMERCIAL badge; the personal-use notice; `TRAIN EXPORT`
(primary, stomp-style) and `RESUME · epoch 41 / 100`. Right column: EXPORT NOTES box (flat well, mono text) + COPY. A
small inset strip at the bottom shows the training state (progress bar on a flat well, `epoch 41 / 100`, `best ESR
0.0123` LCD, ETA, CANCEL).

**07 WOODSHED.** Rig visible (darkened slightly), the WOODSHED panel docked along the bottom (~330 px tall, worn steel,
title plate `WOODSHED` with a saw-tooth edge motif): SONG FILE… / STEMS FOLDER…, song name; transport (play / pause as
footswitches with words), position LCD `01:23.4 / 04:12.0`, seek bar (aluminium slider rail) with loop A / B markers and
`LOOP ON`; COUNT-IN toggle + BPM LCD `142`; guitar stem rotary MUTE / GHOST / FULL (GHOST); KEEP KEYS toggle; BACKING
level knob + LCD `−3.5 dB`; OFFSET LCD `+120 ms`; SYNC TO HOST toggle; record band: REC footswitch + LED + `REC` word,
take list, USE FOR MATCH, MATCH, NAM FORGER.

**08 rig editor.** Overlay 940 x 742 over the rig area, inspector visible. Top strip: topology rotary SINGLE / SINGLE +
2 PEDALS / BLEND (BLEND), tabs `CHAIN | EQ | BLEND | CAB | GATE | VISE` as stamped tabs (CHAIN active: raised + marker),
status line well. CHAIN: lane `A · BLADE` and lane `B · BODY` (lane plates in path colour + word), block cards (worn
steel cards with a stamped title strip): BLADE: THE SAW MILL (BUZZSAW, modelled pedal), `SAW HEAD` capture card with
credit `@marrow_amps · cc-by · VIA TONE3000` and an **UNCAL** badge (`no level metadata — default +9 dBu`), EQ card;
BODY: TS-STYLE card **BYPASSED** (hatch + word), BODY HEAD capture card. Each card: BYPASS mini toggle, INPUT knob +
LCD, ◀ ▶ move, ✕ remove; `+ ADD` slot. Bottom strip: CAB notice `LIVE-COMPATIBLE: the no-cab NAM export is exact`.

**09 notices.** A sheet of the notice designs, each labelled with a caption well:
- **OUT OF TRUE** banner (top-of-rig plate, alert colour + skewed-level glyph): `OUT OF TRUE · your input is running
  4.5 dB hotter than when you calibrated (interface gain moved?)` with `RE-CALIBRATE` / `IGNORE THIS SESSION`, and the
  compact chip form for the top bar.
- **Uncalibrated** banner: `UNCAL · interface not calibrated — captures play at the +9.0 dBu default. CALIBRATE IN
  SETTINGS`.
- **UNCAL badge** in its three placements: on a block card, on an amp head label well, in a render-report line.
- A confirm dialog (riveted steel, `DELETE TAKE?`, body text, DELETE (danger) / CANCEL) and an error dialog
  (`MATCH FAILED`, reason, LOCATE… / CLOSE).
- A toast (`Preset saved: GRAVE DIRT · MATCHED v3`).

## Acceptance

- `python3 design/mockups/workshop/render_all.py` renders all ten PNGs; running it twice gives byte-identical files;
  `--check` passes against the committed PNGs; `--contrast` passes (no pair below AA) and its table is in the README.
- The kit's flat-background assertion never fires on the committed screens (it is on, not disabled).
- Every screen listed above exists, uses the NAMES.md display names, shows TS-STYLE for the TS pedal, and contains no
  trademark text or trade dress.
- No file under `plugin/` changes. No `.ttf` or third-party audio committed.
- Reviewer ACCEPT on determinism, reproducibility and the contrast numbers.
