# Phase 7c state (lead hold, 2026-10-04 ~18:55 UTC)

Branch `claude/sawblade-p7c-zone-rat` (name historical). **Holding on the main lead's request**
(usage cap) after the first implementer round; waiting for GO.

## Where things are
- Scope: the redirected chainsaw-family scope. Zone/Rat dropped before implementation
  (`phase7c_zone_rat.md` is a stub). Spec: `docs/specs/phase7c_chainsaw_family.md`.
- Pushed to origin: `dd2b891` (old spec), `716c2a7` (new spec), `2d5c736` (core). The push
  happened before the hold request arrived.
- Also pushed (the repo's stop hook requires it): `e4cce00` (tests, `pedal_fr` string params, 7 presets,
  `PRESET_SCHEMA.md`) and the spec amendment + this file.
- Implementer round 1 result: 224/224 ctest Release, 24 new `[saw]` tests clean under
  ASan/UBSan, `-Werror` clean, goldens unchanged. Full report numbers are in the engineer's
  hand-back (to be copied into `phase7c_chainsaw_family_REPORT.md`).
- Lead decisions already taken on the engineer's questions (spec amended accordingly):
  clip-type THD and boost THD asserted at dist 0 / −40 dBFS (the dist-5 condition was
  square-wave saturated); no test seam for the mix-100 skip check.

## Not done yet (in order, after GO)
1. **Reviewer** (`reviewer` subagent) on `git diff 453c7af..HEAD` against the spec; fix loop.
2. Plots from `build/fr_saw/*.csv` (script: scratchpad `plot_zr.py`, needs its figure config)
   into `docs/reports/phase7c/`; Artifact "Sawblade Zone + Rat" renamed to the chainsaw family.
3. `docs/specs/phase7c_chainsaw_family_REPORT.md`.
4. Push the remaining commits, final summary.

## Open points for the main lead
- `pedal.hm` custom mode: not implemented here on purpose. 7b's committed spec
  (`phase7b_chainsaw_pedal.md` §1.1/§1.5) already specifies `mode: stock|custom|modded` with
  the tweakable low/high-mid parameters. 7c lists the four hm custom-mode preset recipes in
  its §5 for 7b's bank. Family preset count after merge: 15 (7b) + 7 (7c) = 22.
- `pedal.eye` is, by construction, the HM EQ at all tens with a 100 Hz pre-clip corner and
  +6 dB more gain range (measured: −2.95 dB at 50 Hz, −1.50 dB at 100 Hz vs `pedal.hm`
  10/10/10, ≤ 0.16 dB above 400 Hz). The HM-3 fallback decision needs the captures.
- My acknowledgement replies to the main session were blocked by the permission classifier
  (`send_message` denied), so nothing was sent back; this file is the status channel.
