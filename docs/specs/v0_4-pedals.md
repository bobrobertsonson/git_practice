# v0.4 — pedals: accuracy, captures on the board, a real pedalboard on the main page

Source: user decision 2026-10-06, after testing v0.2: pedals matter more than the amp knobs; "right now I want
accuracy, captures and models, and moving them around on the main page". Runs **after v0.3 merges** (it builds
on v0.3's undo and per-slot level match). Owners: match-engineer (Task A fitting), dsp-engineer (model fixes,
Tasks B–C), reviewer on every task. Hard rules in CLAUDE.md apply. No new art: generic faces use the existing
skin; propose art in the report. UI names use generic descriptors, not trademarks.

## Task A — accuracy harness, and today's models measured

(Scope note 2026-10-06: the user wants pedals rebuilt as **circuit models** with optional mods; that is v0.5,
`docs/specs/v0_5-circuit_pedals.md`. So v0.4 does not tune the behavioural models further: it fixes and
validates the measurement harness and publishes the baseline numbers the circuit models must beat. Skip A.3's
DSP fixes; keep A.1 and A.2.)

State today (HANDOFF, 7.1 report): `pedal.hm` v1/v2 measured ~4 dB RMS LTAS shape error vs real HM-2 captures,
with the harmonic-profile term at ~40 dB on every model; v3 (7c) applied fixes but was **never re-fit**; `hmx`,
`eye`, `muff`, `ts` have no published accuracy numbers.

1. **Check the metric first.** A ~40 dB harmonic error on every model, free or constrained, suggests a measurement
   problem (level reference, floor, capture input level) as much as a model problem. Verify the harmonic term on a
   known answer (render `pedal.hm` itself, fit a capture *made from* `pedal.hm` → near-zero error) and fix the
   metric if it fails.
2. **Fit every modeled pedal** with `sawblade-calibrate pedal-fit` against the TONE3000 captures of its pedal family
   (labelled knob settings where available, `presets/CAPTURE_SHORTLIST.md` + search; record licence/creator,
   never commit captures). Publish `docs/reports/v0_4/accuracy.md`: per pedal and capture, free and
   knob-constrained LTAS / harmonic / dynamics errors, and a one-line verdict.
3. **Targets** (knob-constrained, i.e. the pedal's knobs set to the capture's labelled settings and only level
   free): LTAS shape ≤ 2.0 dB RMS on ≥ 75 % of labelled captures per pedal, and a harmonic error within 2× the
   capture-to-capture spread of the same pedal. A pedal that misses: root-cause and fix the DSP (knob law, EQ
   curve, clipping stage), re-fit, repeat; if still short, report the remaining gap and what would close it.
   Changing a model's sound changes presets that use it: re-render the committed modeled presets, report the
   level/LTAS change per preset, and bump the model version so old presets can keep the old sound
   (`model.version`; old versions stay loadable and bit-identical).

## Task B — pedal captures on the pedalboard

- Any TONE3000 **pedal** capture (NAM) can be placed on the pedalboard as a pedal, from the board's "+ PEDAL"
  slot: a picker with two tabs, MODELED (the five models) and CAPTURES (TONE3000 pedal captures; search, cached
  first, licence + creator shown, the existing resolve/fetch flow).
- A capture pedal has a generic face (existing skin): name, creator, licence tag, `CAPTURE · FIXED TONE`, a LEVEL
  knob and the bypass footswitch; no knob that does nothing. Where the pack has several knob settings, the
  pedal shows a setting selector (reuse the v0.2 ladder machinery where it fits; a plain list otherwise).
- **Modeled and captured pedals must look different at a glance** (user, 2026-10-06). Rule, using the existing
  skin only: modeled pedals keep their rendered faces; a capture pedal is a plain generic enclosure (flat panel,
  no rendered art), a distinct outline colour and a "CAPTURE" badge on the face, the creator name where a model
  pedal shows its circuit name. The same distinction applies in the + PEDAL picker (tabs + badge), the rig
  editor slot strips and anywhere a pedal is listed. Test: every pedal widget exposes its kind and the capture
  kind renders the badge. Propose dedicated capture-pedal art in the report (the user designs the final look).
- Placement uses v0.3's per-slot level match, so a capture pedal drops in at matched loudness.
- NAM-trainable as today (a capture pedal is a `nam` block); the export rules are unchanged.

## Task C — a real pedalboard on the main page

- **Move:** drag a pedal left/right to reorder within its path, and drag between path A (SAW) and path B (BODY).
  Drop targets highlight; the cables redraw; latency compensation updates (no glitch: swap prepared off the audio
  thread as today).
- **Add / remove:** "+ PEDAL" slots open Task B's picker; remove by dragging a pedal off the board or a
  right-click / ctrl-click REMOVE. Bypass with the footswitch (existing).
- **Capacity:** at least 6 pedals per path, laid out so they stay readable at 1280×800 (shrink faces to a minimum
  width, then scroll the board horizontally).
- Every move/add/remove/bypass is one v0.3 undo step and is reflected immediately in the rig editor and vice versa.
- Tests (mouse-driven): reorder within a path; move A→B and B→A; add from both picker tabs; remove; bypass; undo
  of each; the rendered chain matches the board order (render test); latency report updates.

## Task D — main page layout: two heads on top, pedals below; the cab gets its own page

User design direction (2026-10-06): "cab can be its own page — the main page should be the two heads on top and
pedals for 1 or both below." Build to it with the existing skin (no new art); the user refines the look later.

- **Main page (rig area):** the SAW head and the BODY head side by side across the top (SAW left, BODY right,
  same scale as each other). Below each head, that path's pedalboard (Task C), so each column reads
  pedals → head for one path. When path B is off, the BODY column shows the off state ("BODY PATH OFF — turn up
  BLEND") with its head dimmed and an empty board; the SAW column keeps its width (no reflow jump). Single-path
  presets look like one full column plus the dimmed one; blends show both.
- **Cab page:** the cab moves off the main page to its own page, opened from the top bar (a CAB button next to RIG)
  and from a small cab chip on the main page that names the current cab/IR and shows LIVE/STUDIO (shared vs
  per-path). The page holds what the cab offers today: the cab/IR choice per path or shared, the existing mic page
  (grille off, mic positions), and the cab's level. The existing double-click-cab → mic behaviour moves there.
- Cables are redrawn for the new layout (pedals → head per column; the cab chip at the bottom shows where both
  paths meet).
- The inspector (right column) is unchanged.
- Tests: layout test at 1280×800 (both columns, heads aligned, boards below, no overlap) for single-path and
  blend presets; CAB button and cab chip open the cab page; everything the old cab area did is reachable on the
  page (mouse-driven); screenshots of the main page (single, blend) and the cab page in the report.

## Task E — export panel shows what is not in the NAM model

The matcher phase v0.4M (Task E) writes `exportNotes` for every NAM export: each stage left out of the trained model
(bus comp, gate, IR for no-cab, excluded EQ/filters, trim) with hardware-usable settings. The plugin's EXPORT NAM
panel shows that list before and after training, in plain words ("add a compressor after the loader: threshold …,
ratio …, attack …, release …"), with a COPY button. Test: a preset with gate + fast bus comp shows both with the
right numbers.

## Out of scope (propose in the report)

Final visual design of the main and cab pages (the user designs it);
Circuit-level models and mods (v0.5); new pedal types beyond the chainsaw family and adjacent circuits
(the user rejected non-chainsaw pedals once, `docs/specs/phase7c_zone_rat.md`: ask before proposing any);
pedal art, amp-head art.

## Acceptance (phase)

All task tests; accuracy table published with targets met or gaps explained; full suite green (gcc + clang
`-Werror`, ctest, Python, pluginval 10 VST3, macOS auval + pluginval AU/VST3); v0.2/v0.3 tests still pass.
Report `docs/specs/v0_4-pedals_REPORT.md` with reviewer verdicts, the accuracy table, and a hand-test checklist
for the user (Logic, macOS).

## Lead decisions (v0.4 Tasks B–E, 2026-10-06)

Phase lead session for B–E; Task A is done (v0.4A merged) and not touched. Order: **D → C → B**, with **E** in
parallel (independent files). Each task: dsp-engineer implements + full ctest, reviewer ACCEPT/REVISE loop.

### Common

- **D0. What the board shows.** A path's board is its blocks **before its amp** (`ampIndex`; all blocks when the path
  has no amp), in path order. Blocks after the amp (only reachable from the rig editor; no committed preset has one)
  are not tiles; the board shows a small "+n AFTER AMP (rig editor)" note. Drops always insert before the amp. Limit
  is core's `kMaxBlocksPerPath` = 8 incl. the amp, so 7 pedals per path (≥ 6 as asked); a full path greys + PEDAL
  ("path full: 8 blocks") and refuses drops from the other path with a status-line message.
- **D1. Every board edit is exactly one `RigController::edit` call** (= one v0.3 undo step, rebuild prepared off the
  audio thread by the existing load path, latency re-reported by the processor). Cross-path move = remove from the
  source + insert into the target in the same edit; the moved block gets a fresh id (`newBlockId`) and keeps
  everything else (params, bypass, capture, `makeupDb`). Async results (capture make-up) add no step (v0.3 rule).
- **D2. No refresh may destroy a widget under the hand** (v0.3 Task A root cause). The board rebuilds tiles only when
  the shown block list changed, never during a drag; the dragged tile survives until mouse-up.
- **D3. Generic names only**: CHAINSAW (`pedal.hm`), MODDED SAW (`pedal.hmx`), ONE-KNOB SAW (`pedal.eye`), BIG FUZZ
  (`pedal.muff`), GREEN OVERDRIVE (`pedal.ts`) — the existing `circuitInfo` / `docs/PEDALS.md` descriptors.
- **D4. Existing skin only**; one new colour *token* is allowed: `SawbladeLookAndFeel::capture()` = the existing
  cream `0xffe8e1d2` (distinct from SAW orange / BODY blue and from the LIVE/STUDIO semantics).

### Task D — layout (first)

- Rig area stays 940 × 742 (inspector unchanged). Two columns: SAW x 16..462, BODY x 478..924. Heads keep their
  current size (330 px wide, art aspect; the v0.2 `AmpHead` overlays stay valid), centred in their column, both at
  the same y (44). The status line stays at the rig's top. Boards: per column, from head bottom + 40 down to 672;
  a column caption ("SAW · n PEDALS" / "BODY · n PEDALS") between head and board. Cab chip: centred at the
  bottom (y ≈ 690, ~320 × 36): `CAB · <IR title> · ● LIVE|STUDIO` (shared vs per-path, same rule as the mode chip;
  "CAB OFF" when disabled). Cables: board → head per column, then both heads → the cab chip (drawn under the
  pedals). The static mockup pieces `SawPedal` / `BodyPedal` / `Cab` leave the main page.
- Path B off: the BODY head is drawn dimmed (alpha ≈ 0.35) with its controls disabled, the BODY board is empty and
  says "BODY PATH OFF — turn up BLEND"; it is not a drop target. SAW column geometry never depends on B.
- **CAB page** (`CabScreen`, full overlay below the top bar, joins the mutually exclusive overlay group and
  `anyOverlayOpen()`): opened by a new **CAB** toggle next to RIG and by clicking the cab chip. It holds the rig
  editor's cab controls (mode SHARED / PER PATH, CAB ON, the IR cards; extract the rig editor's `CabPage` into a
  reusable component so both places use one class), a **BROWSE IR** button per IR target that opens the capture
  browser for `Slot::Cab` (what selecting the old cab + BROWSE CAPTURES did), the LIVE/STUDIO notice, and a
  **MIC POSITIONS** button that opens the existing mic page (whose close returns to the CAB page). This replaces
  double-click-cab → mic. **The cab has no level control today** (no field in `CabPreset`), so none is added (no
  new DSP / schema in this phase); proposed in the report.
- Inspector: unchanged layout. Selecting a pedal tile shows "SELECTED · SAW PEDAL" + the block title; BROWSE
  CAPTURES on a selected capture tile targets **that** block (path + index), not "the first pedal".
- Screenshots: a test (or test tool) that renders the editor via `createComponentSnapshot` to PNG when
  `SAWBLADE_SCREENSHOT_DIR` is set; it must work with fixture captures from `tests/fixtures` (no TONE3000 needed).
  PNGs go to `docs/reports/v0_4/` (UI only, no capture data).

### Task C — pedalboard

- One `Pedalboard` component owns both boards (so A↔B drags are handled by one component and are testable with
  synthesized mouse events; **no** `juce::DragAndDropContainer`, whose desktop-mouse tracking does not work
  headless). Drag starts after 6 px of movement on a tile's body (footswitch and face knobs never start a drag); a
  translucent ghost follows the mouse; the target board outline highlights and an insertion bar shows the index;
  drop outside both boards = REMOVE (the ghost says REMOVE while outside). Right-click / ctrl-click on a tile:
  menu BYPASS / REMOVE.
- Tiles: width = clamp(board inner width / n, **110**, 180) px at the pedal art's aspect; below 110 the board scrolls
  horizontally (`juce::Viewport`, horizontal bar only). + PEDAL is always the last tile of each board and opens the
  picker (Task B); in C it may offer only the MODELED tab.
- Modeled tiles use the existing renders: circuit pedals (`pedal.hm/hmx/eye/muff`) `pedal_saw.png`, everything else
  modeled (`pedal.ts`, `eq`, …) `pedal_body.png`, each with a name chip over the baked caption (PedalFace's chip
  style). The live `PedalFace` (host parameters of the **first circuit block**, unchanged rule) is laid over that
  block's tile, scaled with the tile; double-click opens the AdvancedDrawer as today. Other modeled tiles have no
  on-board knobs in v0.4 (their params stay in the rig editor); per-pedal knob banks are a report proposal.
- Footswitch + LED per tile (existing `FootswitchButton` / `LedIndicator`) = bypass, one undo step.
- Refresh: the board re-reads `editBasePreset()` on the editor tick and right after its own edits; the rig editor
  shows board edits on its next refresh and vice versa.
- Tests (mouse-driven, `plugin/tests/test_pedalboard.cpp`): reorder within A; A→B; B→A; add (MODELED); remove by
  drag-off and by menu; bypass by footswitch; one undo step each (undo restores the exact preset, redo re-applies);
  refresh during a drag keeps the dragged tile; full path refuses; 6 pedals per path stay ≥ 110 px wide and the
  board scrolls; render test: the processor's rendered output after a drag is identical to rendering a preset built
  with that order directly; latency test: the reported latency after a move equals the core's compensated latency
  of the new preset.

### Task B — capture pedals

- **Picker** (`PedalPicker`, small overlay anchored to the + PEDAL tile): tabs MODELED | CAPTURES. MODELED lists the
  five models (D3) with no badge. CAPTURES lists cached pedal captures first (from the local capture cache's
  `meta.json` entries whose tone gear is pedal; title, creator, licence tag, CAPTURE badge), then a
  **SEARCH TONE3000…** row that opens the existing capture browser in a new **insert** mode
  (`SlotTarget::Kind::InsertNamBlock {path, index}`; gear fixed to pedal; USE inserts a new `nam` block, slot
  "pedal", instead of replacing). Fetch/resolve/licence flow unchanged; `-nc` captures keep their marking.
- **Level match on add:** a new capture pedal gets `makeupDb` from v0.3's `slotMakeupDb` (path solo, before = the
  path without the pedal, after = with it), so it drops in at matched loudness; the add is the undo step, the
  make-up lands without a step. Modeled pedals drop in at their defaults (the global −18 LUFS trim still applies).
- **Capture tile:** flat `L::panel()` enclosure, 2 px `L::capture()` outline, filled "CAPTURE" badge, title, creator
  (where a modeled tile shows its circuit name), licence tag, `CAPTURE · FIXED TONE`, a LEVEL knob (the block's
  output level as it exists today — the nam block's gain field; if a nam block has no output-level field, the
  knob drives `makeupDb` within its ±24 dB clamp and the report says so), footswitch + LED. No other knobs.
- **Setting selector:** when the capture's tone has ≥ 2 models (cached `meta.json`, or the browser's models list
  when online), the tile shows a selector of the model names (v0.2 ladder order when the names parse as a ladder,
  otherwise the plain list); choosing one swaps the capture through the existing swap path (+ make-up), one undo
  step.
- **Kind everywhere:** every pedal widget (board tile, rig-editor slot card, picker row) exposes
  `pedalKind()` ∈ {Modeled, Capture}; one shared paint helper draws the badge/outline. Capture = a `nam` block in a
  pedal slot. Amp captures are not pedals and get no badge.
- Tests: kind + badge on tile, slot card and picker row; add from both picker tabs (CAPTURES with a fake cache
  entry + the fake t3k tool already in `plugin/tests`); insert-mode USE inserts at the index; make-up applied without
  an extra undo step; setting selector swaps; NAM-trainable (export plan still sees a `nam` block).

### Task E — export notes (parallel)

- v0.4M's notes format **has landed on its branch** (`match/sawblade_match/export/notes.py`, `NOTES_VERSION = 1`,
  commit 9074c0b on `claude/sawblade-v0_4m-matcher-feel`), not yet on the base. So: a JUCE-free C++
  `buildExportNotes(preset, mode, dropComp)` in the plugin mirrors `build_export_notes` + `format_notes_txt`
  (same stages, order, settings and `hardware` sentences; `nam_name`/`ir_name` unknown before training).
  **Before** training the panel shows the C++ notes for the current mode/DROP COMP; **after** training it shows the
  run report's `exportNotes` when present (the Python tool is the source of truth once v0.4M merges) and otherwise
  keeps the C++ notes with a dim "(computed by the plugin)" line. COPY puts the `format_notes_txt` text on the
  clipboard (test seam for the clipboard).
- Parity test: a fixture generated by running the v0.4M `notes.py` on two test presets (nocab with gate + fast bus
  comp + post EQ; withcab with DROP COMP) is committed with the command that made it; the C++ output must equal it
  (stages, settings numbers, hardware text, loaderOrder). Panel test: preset with gate + fast bus comp → both
  shown with the right numbers; COPY copies the text.
- Dependency note: if v0.4M changes the format before it merges, the fixture and the C++ port follow (version
  field checked).
