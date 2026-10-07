# v0.4 — pedals, Tasks B–E: REPORT

Spec: [`v0_4-pedals.md`](v0_4-pedals.md) (Tasks B–E + "Lead decisions"). Branch `claude/sawblade-v0_4-pedals`, from
`bcc875c` (v0.2.1 + v0.3 + v0.4A). Task A was done in v0.4A and is not touched here. Lead + dsp-engineer implementers
+ reviewer on every task, per CLAUDE.md. Order: D → C → B, E in parallel.

**Environment limitation (important for how this phase was validated):** this cloud container could not build the
JUCE plugin. The Ubuntu package mirrors (`archive.ubuntu.com`, `security.ubuntu.com`) are denied by the
environment's network policy, so the X11 / ALSA dev headers JUCE needs are missing, and installing headers into
system paths was refused. So every plugin change was only syntax-checked here (gcc + clang, `-Wall -Wextra
-Wpedantic -Werror`, against the real JUCE 8 / Catch2 / nlohmann headers) and its JUCE-free parts built and run;
**GitHub Actions was the first place the plugin and its editor/integration tests were compiled and run.** Reviewer
verdicts on the JUCE parts are by reading and are conditional on CI. To build locally in a future session, allow those
two hosts in the environment's network settings.

## Verdicts

| Task | What | Commits | Reviewer |
|---|---|---|---|
| E | EXPORT NAM panel shows the export notes (+ COPY) | 31be434, 99984be, b5af362, 75e8667, 074db30 | REVISE ×2 → **ACCEPT 75e8667** (074db30: CI wording fix, reviewed with C) |
| D | Main page: SAW + BODY heads on top, a board under each, cab chip; CAB page | 8ef8baf, ab2decb, 3199715 | **ACCEPT ab2decb** |
| C | Pedalboard: drag reorder / A↔B / remove, menu, + PEDAL picker, scrolling | 78ed28d, 3199715, f1df430 | **ACCEPT 5bdba4d** (f1df430 reviewed with B) |
| B | Capture pedals on the board, CAPTURES tab, insert mode, setting selector | f6d425a, f1df430, 9f1ca2f | REVISE → **ACCEPT 9f1ca2f** |
| CI fixes | test-side fixes for runs 184 / 191, comment fix | 2bdf564, 386fee3, 2916db6 | **ACCEPT 2bdf564, ACCEPT 386fee3** (2916db6 = the comment the 386fee3 review asked for) |
| base merge | `claude/sawblade-plugin-setup-7k0b8q` 716c9b9 (#808 level-match race fix, Apple clang fix, v0.4A.1 report fixes) | 09e85c6 | **ACCEPT 09e85c6** (clean, nothing of v0.4 lost) |

**CI of record: run 197 on 2916db6, all green** — linux-gcc (ctest 972 + pluginval VST3 level 10), linux-clang
`-Werror` (ctest), macos-arm64 (ctest, auval, pluginval AU + VST3 level 10), python (pytest + `compute_trims --check`).
The only later commit edits this report.

CI history (all compile steps green from the first run on; failures were tests):
- run 175 on 257fb3a (D+E): 3 failures — a hidden CAB page label matched an editor-wide search (test scoped to the
  rig editor; the page refreshes on open and per tick while visible, verified by the reviewer); a static export-notes
  line contained "BUS COMP" (reworded, old assertion kept); the pedal drawer's non-background fraction 0.183 < 0.2
  because the drawer is now 696 px wide instead of 574 (threshold 0.16 + a new "fully inside the rig" check).
- run 177 on 5bdba4d (C): 2 failures — a test expectation (the undo helper ends redone; now asserts undo and redo
  explicitly) and **a real bug**: a drop computed from tiles shown at drag time was applied to a preset that had
  changed during the drag, swapping two pedals. Drops now resolve by block id (neighbour ids) against the current
  preset and are cancelled (no edit, no undo step) when neither neighbour exists (f1df430).
- run 184 on 4f9a86a (B): 8 failures, all test-side: a `PopupMenu::MenuItemIterator` built from a temporary menu
  (dangling → SIGSEGV on Linux); fake cache entries with a made-up `sha256` that `verifyCapture` rightly rejected;
  the main-page ink fraction 0.117 < 0.12 because a capture pedal is a flat panel by design (now > 0.10; the per-head
  checks stay). Fixed in 2bdf564.
- run 191 on 09e85c6 (after the base merge): 1 failure, test-side: the make-up test's fixture
  (`linear_identity_loud24.nam`) is a plain identity because `normalizeLoudness` is off by default, so the correct
  make-up is 0 dB; replaced by `linear_05_025.nam` (measured −15.4347 → −18.0514 LUFS, +2.6166 dB, reproduced by the
  reviewer). Fixed in 386fee3.
- The base's #808 race (macOS) never failed here; its fix came in with the base merge.

## Task D — layout

- Rig area 940 × 742, inspector unchanged. SAW column x 16..462, BODY 478..924; heads 330 px, same y (44), same size;
  boards from head bottom + 40 to 672; cab chip 320 × 36 at y 690 (`CAB · <IR> · ● LIVE|STUDIO`, "CAB OFF").
  Cables: board → head per column, both heads → chip. SAW geometry never depends on path B (tested).
- Path B off: BODY head dimmed (0.35), controls disabled, empty board "BODY PATH OFF — turn up BLEND", no drop target.
- CAB page: CAB toggle next to RIG and the chip open it; holds the rig editor's cab controls (one extracted class,
  `CabControls`, used by both), BROWSE IR per card, LIVE/STUDIO notice, MIC POSITIONS → mic page (closing returns to
  the CAB page). In the overlay group and blocked for Cmd+Z. **No cab level** (none exists in `CabPreset`; no new
  DSP/schema this phase — proposal below).
- Top bar re-laid to fit CAB: preset button 128 px, PLAY ALONG 98 px, latency chip 136 px; layout test guards overlaps.

## Task C — pedalboard

- One `Pedalboard` owns both boards; own mouse handling (no `DragAndDropContainer`). 6 px drag threshold, ghost,
  target outline + insertion bar, drop off-board = REMOVE, right-click / ctrl-click BYPASS / REMOVE, footswitch bypass.
- Every move / add / remove / bypass = one `RigController::edit` = one undo step; cross-path = remove + insert in one
  edit with a fresh id; amp and blocks after the amp never move; a full path (8 blocks incl. amp, so 7 pedals)
  refuses with a status message; no-op / cancelled drops make no edit and no reload.
- Tiles 110–180 px, horizontal scroll beyond; the live pedal face follows its tile and hides when scrolled away; a
  lost mouse-up during a drag is recovered on the next refresh.
- Render-identity test (board-edited rig vs an independently built preset, bit-identical) and latency-report test
  (host latency after a move and after undo).

## Task B — capture pedals

- Capture = a `nam` block in a pedal slot (one shared rule, `isCapturePedal`). Tile: flat panel, 2 px cream outline
  (`L::capture()`, the existing 0xffe8e1d2 — the only new token), filled CAPTURE badge, title, creator, licence tag,
  `CAPTURE · FIXED TONE`, LEVEL (the block's `outputGainDb`, live, ±24 dB, one undo step per drag), footswitch + LED.
  The same badge/outline helper is used on the rig-editor slot cards and picker rows; amp captures get none.
- + PEDAL picker: MODELED (CHAINSAW, MODDED SAW, ONE-KNOB SAW, BIG FUZZ, GREEN OVERDRIVE) | CAPTURES (cached pedal
  tones from the capture cache, then SEARCH TONE3000… → capture browser in insert mode, gear locked to pedal; USE
  inserts before the amp, clamped to the board even if pedals were removed during the fetch).
- Level match on add: the add is one undo step; the make-up (v0.3 `slotMakeupDb`, path solo) lands without a step.
  Two flows, same result: cache-tab add = add then patch; browser USE = measure then add (the existing swap flow).
- Setting selector when a tone has ≥ 2 models: cached models + the tone's online models list (asked once per tone
  per session in the background; cached only when offline / on failure); an uncached choice downloads ("FETCHING…")
  and swaps in one undo step with make-up; ladder order when the names carry numbers.
- Cache reading hardened: one shared rule for ids / file names from `meta.json` (no separators, no `..`), also fixing
  a throw in `cachedToneCapture` on a non-string `file`; hostile-cache test.
- Export unchanged: capture pedals stay `nam` blocks (trainable, credited, `-nc` marking kept).

## Task E — export notes

- JUCE-free C++ port of v0.4M's `notes.py` (`NOTES_VERSION 1`, branch `claude/sawblade-v0_4m-matcher-feel` 9074c0b —
  **not yet merged to the base**): same stages, order, settings and sentences. Parity test against fixtures generated by
  running `notes.py` (5 cases, byte-identical; regeneration command in `plugin/tests/fixtures/export_notes/README.md`).
- Panel: notes before training (current mode / DROP COMP, follow live edits), after training the run report's
  `exportNotes` (version-checked) or the plugin's notes with "(computed by the plugin)"; COPY copies the text.
- Found on the way (cross-phase): with NO CAB + DROP COMP the exporter is handed the comp-dropped preset, so v0.4M's
  notes omit the comp although the model lacks it too. The panel shows its own notes in that case; **v0.4M should build
  its notes from the un-dropped preset** (proposal 1).

## Screenshots

Rendered by the editor tests on CI (fixture captures, no TONE3000 data) and uploaded as artifacts of **run 197**
(`v0_4-screenshots-macos-arm64`, `v0_4-screenshots-linux-gcc`; kept until 2027-01-05):
https://github.com/bobrobertsonson/git_practice/actions/runs/37558258067 → Artifacts. The files for this report are
`main_single.png`, `main_blend.png` and `cab_page.png` (plus the per-panel screenshots of the other editor tests).
**They are not committed here**: the artifact storage host (`*.blob.core.windows.net`) is also denied by this
environment's network policy, so this session could not download them. The tests that write them also assert they
are not blank and check the layout numerically (both columns, aligned heads, boards below, no overlap at 1280×800).

## Known limitations

1. Only the first circuit block has live knobs on the board (unchanged host-parameter rule); other modeled pedals
   are edited in the rig editor.
2. A capture pedal's make-up is lost if the pedal is removed, undone away, or moved to the other path before the
   measurement lands (seconds). Loudness only.
3. A setting chosen on a capture tile is dropped silently if the pedal is moved to the other path while it
   downloads (the file stays cached; pick it again).
4. Every capture tile causes one background `models` lookup per tone per session when the TONE3000 tool exists (fails
   silently when not logged in); honours `SAWBLADE_NO_NETWORK`. The selector uses the tool path from Settings, not a
   path set only inside the capture browser.
5. BROWSE IR in PER PATH mode opens the cab browser with both targets (no per-card preselection); closing it returns to
   the main page, not the CAB page.
6. Result-view export notes judge "the run dropped the comp" from the current rig (the plugin has no other record).
7. All JUCE behaviour was verified on CI (Linux, macOS), not by hand: the checklist below is the first hands-on test.
8. The integration test's main-page ink threshold (0.10) has ~15 % margin over today's value; a per-tile check would
   guard an empty board area better (next time the file is touched).

## Proposals (not done)

1. v0.4M: build `exportNotes` from the preset before DROP COMP (or list the dropped comp from the plan).
2. Cab level: `CabPreset.levelDb` + a knob on the CAB page.
3. Per-pedal knob banks so every modeled pedal has live knobs on the board.
4. Art (the user designs it): a dedicated capture-pedal enclosure, a generic face per modeled circuit (today non-circuit
   pedals share the "BODY" render), board/cable art for the two-column layout, a cab-chip icon.
5. The capture browser returns to the CAB page when opened from it; per-card preselection of the IR target.
6. Environment: allow `archive.ubuntu.com` / `security.ubuntu.com` (local plugin builds) and
   `*.blob.core.windows.net` (CI artifacts such as screenshots) in the cloud environment's network settings.
7. Wire the capture-tile setting selector to the tool path set in the capture browser as well as Settings.

## Hand-test checklist (macOS, Logic)

Update: `scripts/mac_update.sh`. Log in once: Settings → TONE3000 login.

1. **Layout.** Load a single-path preset: SAW head top-left with its pedals below; BODY column dimmed, "BODY PATH OFF —
   turn up BLEND". Load a blend preset: both heads level, pedals under each, the cab chip at the bottom.
2. **CAB page.** Click CAB (top bar) and the cab chip: the cab page opens. Switch SHARED / PER PATH (chip says LIVE /
   STUDIO), CAB ON off/on, BROWSE IR, MIC POSITIONS → the mic page; closing it returns to the CAB page.
3. **Move pedals.** Drag a pedal left/right within SAW, then onto the BODY board, then back. Drag one off the board:
   it is removed. Right-click a pedal: BYPASS / REMOVE. Each step: Cmd+Z undoes exactly that, Cmd+Shift+Z redoes it.
   Listen: no click or dropout while moving (the chain swaps off the audio thread).
4. **Add pedals.** + PEDAL → MODELED: add each of the five. Add 6 to one path: the board scrolls sideways; the 8th
   block is refused ("path full").
5. **Capture pedals.** + PEDAL → CAPTURES: cached pedal captures listed with creator + licence; add one: it looks
   plainly different (flat panel, cream outline, CAPTURE badge) and arrives at matched loudness. SEARCH TONE3000…:
   USE adds it as a pedal. On a pack with several settings, the selector lists them (FETCHING… for one not yet
   downloaded). LEVEL knob changes its level; one drag = one undo step.
6. **Export notes.** EXPORT NAM on a preset with gate + bus comp: the notes list both with numbers; switch NO CAB / WITH
   CAB; COPY and paste into Notes. After a training run, the notes are still there (with the comp if it was dropped).
7. **Logic specifics.** Two instances of Sawblade: board edits in one never appear in the other. Close/reopen the
   plugin window mid-drag: nothing stuck; undo still works.

PHASE v0.4: ACCEPT 2916db6
