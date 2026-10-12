# v1.0 — the whole plugin in the workshop look (design brief)

Source: user 2026-10-08: "Would be great for the whole plugin to look as cool as the amps and pedals" and "it
should look like the pedals and amps in general — worn metal and sawdust is great." CLAUDE.md: the user designs the
UI; engineering builds to their design; mockups are reference only.

**Scheduling:** design work (Task A) may start now. The build (Tasks B–C) starts after v0.8 and v0.3.1 have merged,
because both edit the same screens (Settings, preset browser, level match, A/B).

## Direction

The amps, cabs and pedals are already rendered (`design/render/*.py`, `plugin/assets/`) and the rig screen follows
`design/mockups/RigReal.dc.html`. Everything that is still plain widgets — top bar, Settings, preset browser, MATCH,
NAM FORGER, WOODSHED, the rig editor, dialogs, notices — gets the same material language:

- **Materials:** worn painted steel and brushed aluminium with scuffs and edge wear; dark workbench wood; a fine
  sawdust grain as texture, never over text; riveted plates for panel headers; stencil / stamped lettering for section
  names.
- **Controls:** the existing filmstrip knobs and footswitches everywhere a value is set; toggle switches and rotary
  selectors from the pedal set; LCD / nixie-style readouts for numbers (dB, Hz, ms) — this also delivers v0.3.1 Task B's
  readout in the skin.
- **Legibility first:** the texture stays behind; text contrast meets WCAG AA; every state (on/off, uncalibrated,
  OUT OF TRUE, legacy preset) is readable without colour alone.
- **Names:** all display names from `docs/NAMES.md`.

## Task A — design (lead + user) — DONE: branch `claude/sawblade-v1_0-ui-design` 13fb3b3, review page https://claude.ai/artifact/SPkazo41kcKHRVMaJNoVqo


1. A style sheet: palette, materials, type, control kit, panel frames — rendered with the existing deterministic
   render pipeline so it is reproducible.
2. Mockups of each screen in that style (1280 × 800 design px, like the existing mockups): main rig, top bar,
   Settings (incl. INPUT CALIBRATION and the input-channel choice from v0.8), preset browser, MATCH, NAM FORGER,
   WOODSHED, rig editor, notices.
3. The user picks and edits. Only the user-approved set is built.

**User decisions on the mockups (2026-10-08):** TS-style pedal = CHISEL; keep amber LCD readouts plus the orange nixie
for BLEND; try a stronger wear / sawdust variant; stencil caps on aluminium plates + heavy military face for titles;
top-bar A/B as two mini footswitches; WOODSHED uses the existing cassette-deck render; legacy-levels hint above the
preset info panel.

**User feedback on v3 (2026-10-08):** "Looks great so far. Definitely need to simplify and streamline a lot. Vermin
should have a rat stencil. The cab needs a new design also." The v2/v3 choice is superseded by a v4 round.

### v4 round — streamline (design session; lead proposal for the user to react to)

Keep the v3 look (LCDs, LED ladders, rings, lamps, worn metal); cut what is duplicated or rarely used. Each rule
below is a proposal; the user accepts, edits or rejects it on the review page.

1. **One fact, one place.** Remove the caption chips under amps / pedals (`BLADE · THE SAW MILL → SAW HEAD`); path
   membership is shown by one colour edge (BLADE orange, BODY blue) plus a word, for colour-blind readability. The
   inspector does not repeat controls that are already on the selected pedal's face (the CIRCUIT selector lives on
   the face only).
2. **Main rig = rig + one rail.** Drop the separate METERS column: IN / OUT become thin LED ladders in the top bar;
   GR is the LED ring on VISE. The cab gets the space.
3. **Show on demand.** MATCH vs ORIGINAL appears only after a match (dismissable); ALIGN, LAT and CPU move to a
   details pop-over; empty pedal slots collapse to a single `+` at the end of each path's chain.
4. **Top bar:** brand, preset scroller with ◀ ▶, A / B, RIG / WOODSHED, MATCH, NAM FORGER, settings, LIVE lamp. No
   other readouts.
5. **Settings:** one status strip (current state only); the manual dBu field appears only under "Custom"; INPUT
   CHANNEL shows "Auto" with the override behind a disclosure unless a stereo input is detected; GATE FLOOR moves to
   the GATE detail.
6. **Colour discipline:** amber = values, green = good state only, orange = selection / primary action. No new hues.
7. **VERMIN** on the main rig (BLADE path), with the rat-stencil face (below).
8. **Cab:** 2–3 new cab directions (below) shown on 01 side by side; the user picks one.

Screens 01, 02, 03 and 08 first; the rest follow the accepted rules. Both wear levels; all existing checks
(determinism, `--check`, AA contrast, overpaint) stay green; reviewer ACCEPT before republishing.

### VERMIN rat stencil (v0.9 Task C amendment, owner: the v0.9 session)

`design/render/pedal_vermin.py` gains a rat stencil: a spray-painted stencil silhouette (stencil bridges, overspray,
wear) as the face's main graphic, with the VERMIN name. Our own drawing; no imitation of the original pedal's logo,
lettering or trade dress. Deterministic, same font pinning; goldens / face screenshots re-rendered; reviewer ACCEPT.

### Cab redesign (design session; production render in Task B)

The current 4x12 / 2x12 renders (`design/render/cab_4x12.py`, `cab_2x12.py`, a woven grille with a wolf-and-moon
stencil) are replaced. Mock 2–3 directions in the workshop language for the user to choose from, for example:
(a) worn black tolex, metal corners, salt-and-pepper grille, a riveted steel nameplate `CAB` with the IR title on a
small LCD strip; (b) a stripped open-back / bare plywood cab with stencilled lettering and visible speakers; (c) a
road-case-armoured cab (steel edges, latches, stencilled flight-case lettering). Shared vs per-path IR (LIVE / STUDIO)
must stay readable on the cab. No trademark logos or trade dress copied from real cabinet makers.

### Art direction round (user, 2026-10-10) — concept sketches before any render

User: "fix the cab art as well as the vermin art. The idea of a wolf for the cab is cool but the execution didn't
work. A stencil style wolf would be cool. Show me before a render is done. For vermin it should be a rat like this"
(reference image shown to the lead; not committed — third-party art).

- **VERMIN:** supersedes the spray-stencil rat (8e36ae1). Concept (user: "just a conceptual idea of a rat with nails
  through it"): an original snarling rat with long iron nails driven through its body. Style open; sketches span V1
  spray stencil (matches the cab wolf), V2 woodcut / engraving hatching, V3 bold flat silhouette / screen-print.
- **CAB:** keep the wolf idea, new execution as a **stencil**: bold spray-stencil wolf (head or howling bust),
  stencil bridges, overspray and drips, worn into the grille / tolex; replaces the current cartoon wolf and moon.
- **Process:** flat 2-D concept sketches first (2–3 per item), shown to the user on a small review page. No Blender,
  no mockup re-render, no production asset until the user picks. Then the production render (pedal face, cab) and the
  mockup swap in one round.

## Task B — assets (dsp-engineer)

Render scripts under `design/render/` for every new sprite and panel, deterministic, exported through
`export_ui_assets.py`; asset size budget stated; 1× and 2× scales.

Also in Task B: re-render the pre-existing assets that carry trademark text (found by Task A): the amp OLED panels in
`plugin/assets/amp_saw.png` / `amp_body.png` ("JCM800 2203", "5150III") and the pedal-face labels (STOCKHOLM SYNDROME /
TIGHTEN, replaced by the `docs/NAMES.md` names); export amps and pedals with real alpha instead of a baked backdrop. Port the SHA-pinned font fetcher from the v1.0
design branch into `design/render/common.py`; a failed or mismatched font fetch is a hard error (today's fallback
silently draws wordmarks invisibly). From the v0.9 rat-stencil review (8e36ae1): fix the VERMIN "DISTORTION" corner
label (its yellow shadow overprints the bone text) and hoist the two local PIL imports in `pedal_vermin.py` to module
level. The v4 mockups swap in the rat face (regenerate from `pedal_vermin.py` at 8e36ae1) in the round after the
user's v4 verdicts.

## Task C — build (dsp-engineer)

Reskin each screen to the approved mockups without changing behaviour; renames applied; screenshot tests updated
(`build/screenshots/*`) and the macOS screenshot flake (v0.3.1 D.4) fixed first; pluginval / auval green; no layout
regressions at `uiScale` 1 and 2.

## Acceptance

User approval of the mockups before any build; after the build, the user's check in Logic; REPORT with before/after
screenshots.
