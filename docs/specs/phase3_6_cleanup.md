# Phase 3.6: matcher cleanups (licence policy, real gap noise, offset hint, review nits)

## 1. Non-commercial licences (docs/specs/licence_noncommercial.md, items 1-6)
- `t3k/licenses.py` allows `cc-by-nc*` and exposes `is_noncommercial(lic)`. Unknown or
  empty licences are still rejected.
- The pool manifest, `resolve` and `result.json` carry `nonCommercial: true` per nc
  capture. A preset report gets a derived `nonCommercial` (the export already does this).
- Docs: the match/README licence section, the phase1_5 business note, and
  presets/CAPTURE_SHORTLIST.md (the @peterny HM-2 MiJ v2.0 tone is now eligible).
- `--search` stays opt-in. Replace the "commercial agreement required" wording with
  "check the TONE3000 API terms before using search in anything shared".

## 2. gap_noise measures real silence
The 3.5 gate report showed that the current "gap" frames (the quietest 5% + 6 dB of DI
frames) are mostly quiet playing: palm mutes and ring tails. The gate never closes there,
and gap_noise is +2.8 dB whatever the gate does.
- Redefine the gaps as DI regions whose short-term RMS (10 ms) stays below
  `floor + 6 dB` for at least 120 ms. Skip the first 50 ms after each region starts, so a
  ringing tail isn't counted as noise.
- Report `gapNoiseDb` as the output RMS in those regions minus the output RMS over the
  playing frames. Also report the number and total length of the gaps.
- If there are fewer than 1 s of real gaps in total, the rule is n/a.
- The rule's threshold stays as it is. Report the new values for v4 and cover v3 with the
  current gate, and with the 3.5 expander settings (mode expander, ratio 4, key HPF 120 Hz,
  linear-dB release 150 ms), so the lead can pick the gate default.

## 3. Offset hint is respected
When `--offset-ms` is given, the first-pass excerpt refinement searches only ±20 ms
around it, and the final refinement does the same. The cover run picked 176.9 ms over a
192.2 ms hint. Add a test.

## 4. Review nits from 3.4
- Add a note in `ref.notes` and the log when an explicit full-mix channel has no HF limit,
  suggesting `--ref-hf-limit 4500`.
- Fix the indentation in measure.py.
- Tidy the README sentence on the old side-channel basis.

## Acceptance
- The full pytest suite passes (report the counts with -rs). Tests use no network.
- The gap_noise table for v4 and cover v3 (current gate vs expander) is in the report.
- Do not rerun full matches. Render the existing best presets via tonerender only.
