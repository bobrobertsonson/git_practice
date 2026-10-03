# Spec: switch licence policy to personal / non-commercial (queued)

User decision 2026-10-03: Sawblade will not be sold. CLAUDE.md "Capture licensing" updated.
Start only after the in-flight phase 4 (export) and 3.3 (generalize) work is accepted —
both touch match/.

## Changes
1. `match/sawblade_match/t3k/licenses.py`: allow `cc-by-nc`, `cc-by-nc-sa`, `cc-by-nc-nd`
   (keep rejecting unknown/empty licences). Expose `is_noncommercial(lic)`.
2. Pool manifest / resolve: keep `license` on every entry; add `nonCommercial: true` for nc.
3. `match/sawblade_match/export/plan.py`: drop the nc block; instead, if any capture is nc,
   the export metadata licence note says "contains CC BY-NC material — non-commercial use
   only" and lists the creators (attribution block already exists).
4. Matcher / presets: a preset whose captures include nc gets a derived
   `nonCommercial: true` in its report (no schema version bump: derived, not stored).
5. Docs: match/README.md licence section, docs/specs/phase1_5.md business note,
   presets/CAPTURE_SHORTLIST.md (the @peterny HM-2 MiJ v2.0 CC-BY-NC tone is now eligible).
6. `--search` stays opt-in (TONE3000 API terms), but drop "commercial agreement required"
   wording; say "check TONE3000 API terms before using search in anything shared".

## Acceptance
- Unit tests: nc licences accepted, unknown rejected, nc flag propagates to manifest,
  export metadata note and report.
- Full match test suite green; no other behaviour change.
