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

## Task B — assets (dsp-engineer)

Render scripts under `design/render/` for every new sprite and panel, deterministic, exported through
`export_ui_assets.py`; asset size budget stated; 1× and 2× scales.

Also in Task B: re-render the pre-existing assets that carry trademark text (found by Task A): the amp OLED panels in
`plugin/assets/amp_saw.png` / `amp_body.png` ("JCM800 2203", "5150III") and the pedal-face labels (STOCKHOLM SYNDROME /
TIGHTEN, replaced by the `docs/NAMES.md` names); export amps and pedals with real alpha instead of a baked backdrop.

## Task C — build (dsp-engineer)

Reskin each screen to the approved mockups without changing behaviour; renames applied; screenshot tests updated
(`build/screenshots/*`) and the macOS screenshot flake (v0.3.1 D.4) fixed first; pluginval / auval green; no layout
regressions at `uiScale` 1 and 2.

## Acceptance

User approval of the mockups before any build; after the build, the user's check in Logic; REPORT with before/after
screenshots.
