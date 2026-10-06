# v0.3 — core tool usability: REPORT

Spec: [`v0_3-core_usability.md`](v0_3-core_usability.md). Branch `claude/sawblade-v0_3-core-usability`
(from 865f6c0; base `claude/sawblade-plugin-setup-7k0b8q` merged in at 9c61f4b, incl. v0.2.1).
Lead + dsp-engineer / match-engineer implementers + reviewer, per CLAUDE.md. Every task reached reviewer ACCEPT.

## Verdicts

| Task | What | Commits | Reviewer |
|---|---|---|---|
| A | Rig-editor knobs drag like main-page knobs | 2572a28, 707ba33, 5aaeb42 (+ fe79833 tests) | REVISE ×2 → **ACCEPT 5aaeb42** |
| B | Level-matched auditioning (−18 LUFS trim, capture-swap make-up, A/B) | 4209418 … 195f14c (+ 1703b28) | **ACCEPT 195f14c** (golden independently re-verified) |
| C | BLEND from the knob; body amp always appears or says why | 8f0d1bb, d99b4de | REVISE → **ACCEPT d99b4de** |
| D | Undo/redo for every rig edit | d8ca00c (WIP at a usage cutoff), 21758ae, fc0f53b, ce33c9d (+ 1c6dc04) | REVISE → **ACCEPT ce33c9d** |
| E | Gain-step tags, browser marks, ladder list | 7cae092, a094007, 1df7b7b, aacd891 | **ACCEPT aacd891** |
| merge | base (v0.2.1) into v0.3, match-apply = one step in the new history | 9c61f4b | **ACCEPT 9c61f4b** (+ 60de717 Task E should-fixes) |

Post-merge macOS fix: 3d2949a (test-only, reviewer verdict pending).

CI of record: pending (run on the branch head after 3d2949a). History: green on 195f14c, d99b4de, fc0f53b, aacd891 (all jobs incl. macOS auval +
pluginval AU/VST3 10). Red twice, both macOS-only test timing, both root-caused and fixed: 5aaeb42 (Task A
throttle test waited on a wall-clock timer → driven explicitly in 195f14c) and 9c61f4b (the two-instance
isolation test rendered before the instance's own async level trim landed → `settle()` waits for level work,
plus an assertion that the other instance's edits never touch this instance's trim; 3d2949a).

## Task A — root cause

The "nearly impossible to turn" knob was the chain page's **INPUT** knob: `SlotStrip::refresh`
(`plugin/src/rig/SlotStrip.cpp`, pre-fix ~l.195–198) rebuilt every block card whenever the path's blocks
differed from the shown ones. A live input-gain edit changes exactly that, and the editor refreshes the rig panel
16× per second (`PluginEditor.cpp:647-653`), so the knob under the hand was destroyed within ~60 ms of a drag
starting. Gate and bus-comp knobs turned fine but applied nothing until mouse-up (`RigWidgets.cpp`
`onValueChange` skipped while dragging). Range/skew and parent interception were ruled out.

Fix: refresh updates the INPUT knob in place (ignored while dragged); structural knobs apply on a 150 ms
**throttle** during the drag and once on mouse-up; the knob snaps to the model's value on release (KEY HPF
dead zone), wheel steps move through dead zones in the direction of travel; gestures are paired even if a knob
is destroyed mid-drag. Mouse-driven tests sweep every PresetKnob and the host-parameter knobs in 250 px.

## Task B — loudness

- Reference: a deterministic Karplus-Strong guitar-like DI generated in core (`reference_di.*`, 10 s, sha
  pinned). **Deviation:** the spec named the export's `DI_BUILTIN`, which is numpy-built and not bit-reproducible
  in C++. Documented in `docs/PRESET_SCHEMA.md`.
- `output.autoTrimDb` (schema v3; v1/v2 read) brings a preset to −18 LUFS **with OUTPUT at 0 dB**; OUTPUT is a
  persistent user offset (excluded from the measurement and hash). Rigs with no active non-linear block get no
  trim; trims are clamped to [−48, +12] dB. Ramp 250 ms. LEVEL MATCH setting (default on). NAM export always
  un-trimmed.
- Capture swap: per-slot `makeupDb` measured on the slot's path solo; the browser shows "LEVEL MATCHING…"
  while it computes; the old trim keeps playing (no dip).
- With LEVEL MATCH off every renderable committed preset is bit-identical to cdb4b9a (reviewer rebuilt cdb4b9a
  and compared: 0/32 differ).

Loudness, committed presets (full table: [`docs/reports/v0_3/loudness_table.md`](../reports/v0_3/loudness_table.md)):

| | count | before (no trim) | after |
|---|---:|---|---|
| Modeled presets (no captures needed) | 32 | −7.6 (`hm_v3/grind_buzz`) … −16.9 LUFS (`chainsaw/pickle_doom_saw`): a **9.3 LU spread** | all −18.00 LUFS |
| Presets needing TONE3000 captures | 13 | not measurable here (no captures in the cloud container) | measured in the plugin at first load on the Mac, or rerun `scripts/compute_trims.py` there |

## Task C — root cause of "only the boost"

Verified by the reviewer against 195f14c: (1) on a cold capture cache the fill adds only the boost and the amp
arrives later via `BodyFill`; (2) `BodyFill::tick()` ran only while the rig panel was visible, so the amp
never landed with the panel closed; (3) moving BLEND/LEVEL B during the download discarded the arriving amp;
(4) no tool / no network / a failed fetch ended silently. All fixed; the body head reads
"CHOOSING A BODY AMP…", "BODY AMP DOWNLOADING… (name)", or the reason with its one action ("NOT LOGGED IN —
log in via Settings, then touch BLEND", …), and "BODY AMP MISSING — touch BLEND" if the download was
interrupted. Also fixed: closing the plugin window during the fetch hung (BodyFill member order).

## Task D — undo/redo

Processor-owned history (survives closing the window; not saved in the session), 64 steps, whole-preset
snapshots. One step per gesture; wheel bursts coalesce; host automation never recorded and never rewound.
Preset loads are steps (history is never cleared). Async completions (body amp, make-up, trim, ladder) add no
step and are patched into stored snapshots. A/B toggles are not steps; applying a match is one step.
After merging v0.2.1 (9c61f4b) its separate applied-match undo slot is gone: applying a match (audition
APPLY or MATCH-in-DAW) is exactly one `Load` step in the processor history, restoring the pre-audition preset
JSON-exactly. Behaviour changes vs v0.2.1 (accepted by the lead, consistent with the single history): an edit
after an apply no longer erases the apply step (two steps), an apply after a BLEND fill leaves the fill one step
below, and re-loading an identical preset records no step.

## Task E — where gain steps exist

Amp heads show `STEPS n` / `STEPS —` / nothing while unknown. The capture browser looks up the visible amp
rows lazily (≤24 per view, selected first, once per tone per session, stops on the first failure) and marks
`STEPS n`. Parser fix: `Gain-06` / `gain_6` names now parse. Extra beyond spec: a per-tone ladder cache so a
ladder learned in the browser applies when that capture is used later.

**Ladder list** ([`docs/reports/v0_3/ladder_list.md`](../reports/v0_3/ladder_list.md)): **unverified** — the
cloud container has no TONE3000 login. Committed presets use 7 amp tones; the most-used (6505+ FULL Pack, 70977)
likely has **no** ladder (boost-voicing names). Best candidate to see steps: **6505+ Gain Range Pack (High
Gain), tone 29230** (sample names `…-Scooped-Gain-02/04/05/06/07`), not in any committed preset — pick it in an
amp head's capture browser. Caveat: if the pack also holds other voicings it will still read "no ladder".
Run the loop at the end of `ladder_list.md` on the Mac after `sawblade-t3k login` to get the real answers.

## Known limitations / follow-ups (proposals, not done)

1. Body-amp download is owned by the editor: an amp arriving while the plugin window is closed only lands
   after reopening and touching BLEND. Proposal: move `BodyFill` into the processor.
2. Path B is heard when the BLEND knob is released, not during the drag (avoids a mid-drag rebuild).
3. A fresh preset load plays at trim 0 until measured (seconds) when it has no stored trim.
4. Live knobs (chain INPUT) record one undo step per wheel notch.
5. Pre-existing flaky test under parallel load: "ToolRunner: cancel and timeout after the child has finished
   never signal anything" (not touched by this phase).
6. Golden bit-identity is strict on gcc/Linux x86-64 only; other toolchains use an rms/peak tolerance.
7. `PresetAudition` snapshots the pre-audition preset from `currentPreset()`; the history uses
   `editBasePreset()`. Identical in every tested path; could differ only if an apply happens during an in-flight
   load. Proposal: use `editBasePreset()` there.
8. The ladder list could not be checked live (no TONE3000 login in the cloud container).
9. Out of scope as specified: pedals (next priority), amp-head art, separation/play-along, layout.

## Hand-test checklist (macOS, Logic)

Update: `scripts/mac_update.sh`. Log in once: Settings → TONE3000 login.

1. **Rig knobs.** Open the rig editor → Chain. Drag an INPUT knob slowly top to bottom: it moves smoothly the
   whole way and you hear the change while dragging. Same for Gate and Bus comp knobs. Shift-drag = fine;
   double-click resets; mouse wheel moves it. Gate KEY HPF: one wheel notch up from OFF goes to 40 Hz.
2. **Level match.** Settings → LEVEL MATCH on. Step through several Modeled presets: they should sound equally
   loud (±0.5 LU). Load a Classic/matched preset: the LAT chip briefly reads "LEVEL …", then the level settles
   without a jump. Turn OUTPUT down 6 dB, save, reload: still 6 dB down. LEVEL MATCH off = old levels.
3. **Capture swap.** In an amp head, browse captures, USE another amp: "LEVEL MATCHING…" shows, then the swap
   lands at about the same loudness.
4. **A/B.** A/B two presets: both play at matched loudness.
5. **BLEND.** Load a single-path preset; on the main page turn BLEND up from full SAW and release: the body path
   appears (boost + amp). The body head reads "BODY AMP DOWNLOADING… (name)" then the amp's knobs work. Logged
   out: it says NOT LOGGED IN and what to do. Turn BLEND back to 0: path B stays. Close the plugin window
   mid-download, reopen: "BODY AMP MISSING — touch BLEND"; touch BLEND → it downloads.
6. **Undo/redo.** Make several edits (add a pedal, drag a knob, swap a capture, change EQ, load a preset):
   Cmd+Z walks back one edit at a time, Cmd+Shift+Z walks forward. A knob drag = one step. Close and reopen the
   window: history is still there. Applying a MATCH result is one Cmd+Z.
7. **Gain steps.** Run the `ladder_list.md` loop in Terminal (after `sawblade-t3k login`) and send the output.
   In the capture browser on an amp head, rows with steps show "STEPS n"; open the 6505+ Gain Range Pack
   (29230) and check the head shows "STEPS n".
8. **Report** anything that feels off, with the preset name.
