# v0.8 — input level calibration (the DI hits the captures at the level their creators used)

Source: user decision 2026-10-07 ("4b: yes"). NAM amp captures are level-sensitive: the same DI 6 dB hotter is a
different amount of gain. TONE3000 captures carry input-calibration metadata when the creator supplied it (check the
`.nam` metadata fields in the pinned core/trainer: input level dBu, output level dBu). Owners: dsp-engineer (plugin/
core), match-engineer (matcher + export use of the calibration), reviewer on every task.

## Task A — device calibration

- A one-time SETUP step in Settings: the user picks their interface (free text) and enters its max input level in
  dBu from the manual (e.g. +10 dBu), or runs a guided measurement (play a known reference — a phone tone generator
  or the interface's own loopback — at a stated level) when the spec cannot be found. Stored as a plugin setting
  (not preset state), with the date and method.
- Where a capture has input-level metadata, the plugin scales its input so 0 dBFS in Sawblade means the same
  physical level the creator calibrated to; captures without metadata use a documented default and are marked
  "uncalibrated" in the UI. Gain is applied before the drive (it changes tone, so it is visible and undoable, and
  reported in the latency/level chip).

## Task B — matcher, presets, export

- The matcher and `tonerender` take the same calibration (`--input-dbu`), so matches done on the user's DI are
  scored at the level their rig plays them; presets record the calibration they were matched at.
- NAM export: the exported model's metadata carries the input calibration so the Anagram (and other loaders that read
  it) apply the same input level; the export notes state the interface/loader level to set when the loader does not.

## Acceptance

Unit tests for the level math (dBu ↔ dBFS, metadata read, no-metadata default); a render test showing a calibrated
capture at the creator's level matches the capture's own reference render; UI tests for the SETUP step; CI green;
REPORT with the user's interface value and a before/after listening note from the user.
