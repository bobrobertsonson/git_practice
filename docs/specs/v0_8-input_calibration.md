# v0.8 — input level calibration: every capture sees the level its creator used, automatically

Source: user decisions 2026-10-07 ("4b: yes"; then "Input level calibration is very important. I shouldn't have to
adjust when changing amps and pedals. Should all be calculated as loaded."). Priority raised: this phase runs **before
v0.7** (genre benchmark), because the benchmark's numbers depend on it.

Why: NAM captures are level-sensitive. The same DI 6 dB hotter is a different amount of gain, sag and compression, so a
capture played at the wrong level feels wrong ("doesn't respond naturally"). Captures from different creators were made
at different levels, so today every amp or pedal swap silently changes the feel unless the user re-trims by hand.

Owners: dsp-engineer (core, plugin), match-engineer (matcher, export, t3k metadata), reviewer on every task.
Scheduling: Task A (audit) and the core math of Task B may start while v0.4M and v0.6 are open (new files only); plugin,
preset and matcher integration wait until v0.4M and v0.6 have merged (both touch the same code).

## Principle

One physical reference, set once: the user's interface. Every gain between stages is then **computed from capture
metadata when a rig, amp, pedal or capture is loaded or swapped** — never a knob the user must re-set. The user's INPUT
knob stays as a deliberate offset ("hit it harder"), 0 dB by default, and is preserved across swaps.

## Task A — audit (no behaviour change)

1. From the pinned NeuralAmpModelerCore and trainer: exactly which `.nam` metadata fields carry calibration
   (`input_level_dbu`, `output_level_dbu` or whatever the pinned versions name them), their meaning (dBu at 0 dBFS in /
   out), and how the reference NAM plugin uses them ("calibrate input"). Cite file and line.
2. From the TONE3000 API (match/sawblade_match/t3k) and a sample of the user's cached pool (counts only, no capture files
   in git): what share of A2/A1 captures carry each field, per gear type (amp, pedal, full rig, IR-less amp).
3. Every place in core/plugin/match that applies a level to a NAM block today (input gain, pre-EQ, normalizeLoudness,
   blend trims, level match, the pedal-capture-to-amp hop). Table: where, what, whether it changes drive.

## Task B — the calibrated chain (core)

- **Device calibration (one time, Settings, not preset):** interface max input in dBu (from the manual, e.g. +10 dBu), or
  a guided measurement when unknown; stored with date and method. Until set: a documented default (+9 dBu, common
  interface value — confirm in Task A) and an "interface not calibrated" notice.
- **Per-block input gain, computed on load:** for each NAM block, `gainIn = deviceDbu − captureInputDbu` so 0 dBFS from
  the interface equals the capture's own 0 dBFS reference in physical terms.
- **Stage-to-stage hops:** a pedal capture's output feeding an amp capture uses the pedal's `output_level_dbu` and the
  amp's `input_level_dbu`, so a pedal hits the amp as hard as the real pedal would. Modelled pedal blocks (v0.4/v0.5
  DSP pedals) declare a nominal output level in dBu.
- **Missing metadata:** a documented per-gear-type default, the block marked "uncalibrated" (UI + render report), and a
  fallback estimate if Task A finds a reliable one (e.g. from the capture's own loudness metadata) — never silent.
- **Output side:** each capture's output is brought back to a common internal reference (existing loudness
  normalisation), so swapping amps changes tone and feel, not monitoring volume. Level match keeps working on top.
- **Live gate floor seed (parked from v0.4M Task H, 2026-10-07):** the live gate's floor follower seeds at −70 dBFS
  and needs ~25 s to learn a loud floor (e.g. a −42 dBFS peak floor). Store the learned floor with the device
  calibration and seed from it on prepare. A VST3/AU plugin cannot identify the host's input device, so the key is the
  device calibration record itself, not a device name. Never seed from a preset or a matched reference DI (wrong-high
  seeds cut quiet playing). Write it off the audio thread only.
- All gains are computed off the audio thread at load/swap and handed over with the existing lock-free swap; smoothing on
  change; zero allocations in `process()`.

## Task C — presets, matcher, export

- Presets store **no absolute input gains**; they store the user's INPUT offset and per-block intent (e.g. a boost's
  level knob). Absolute gains are recomputed on load from the capture metadata and the device calibration, so a preset
  plays the same on another interface once that interface is calibrated.
- Matcher and `tonerender` take the same calibration (`--input-dbu`); the reference DI's own level is part of the match
  (a NailTheMix DI is not the user's guitar); match reports state the calibration used.
- NAM export: an exported capture carries the input/output calibration metadata in the standard NAM fields, so any A2
  loader that honours them plays it at the right level; export notes state the interface/loader level to set when a
  loader does not.

## Acceptance

- Unit tests: dBu ↔ dBFS math; per-block gain from metadata; pedal→amp hop; missing-metadata default + flag; swap of an
  amp with a different `input_level_dbu` changes the computed gain by exactly the difference and needs no user action.
- Render test: a capture with known calibration, fed the creator's reference level, reproduces its own reference render.
- Plugin tests: Settings device step; "uncalibrated" flags; INPUT offset preserved across amp/pedal swaps.
- Full CI green; REPORT `docs/specs/v0_8-input_calibration_REPORT.md` with the Task A tables, the user's interface value,
  and the user's before/after feel note on two amp swaps without touching INPUT.
