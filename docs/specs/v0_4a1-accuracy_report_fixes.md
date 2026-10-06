# v0.4A.1 — pedal accuracy: fix the report so the targets can be judged

Source: the user's first full run (2026-10-06, Mac, 52 min, `scripts/run_pedal_accuracy.sh`, Bloodbath L DI).
Owner: match-engineer; reviewer audits. Python only (`match/sawblade_match/calibrate/`, `scripts/`,
`docs/reports/v0_4/targets.json`); no C++.

## What went wrong (lead diagnosis)

1. **Capture names, licence and creator missing for most captures** (report shows `496937 (496937)`,
   `None / None`, `pins none`). `load_targets` reads metadata only from `<cache>/pool_manifest.json` (the
   matcher's pool). The run script pulls pedal captures into its own manifest
   (`~/sawblade-work/pedal_pool.json`) so the matcher pool stays untouched; tones not in the matcher pool
   (58569, 74487, 88604, 72990, 60618, 62523, 6778) therefore have no names → no labels → no pins → **no
   knob-constrained fits**, which are what the v0.4 target is judged on. Also breaks the CLAUDE.md rule
   "every capture keeps its licence + creator".
2. **Tone 6778 (BOSS HM-2 1986, 11 models) was never fitted**: its model list is empty ("all cached in the
   manifest") and it is not in the matcher pool, so the list resolved to nothing, silently.
3. **TS family polluted**: tone 70280 is a mixed boost pack (DS-1, Klon, Grind, Hex, SansAmp, Badass, ED
   Plumes, SD-1, TS 808 ×2). All were scored as TS.
4. **TS labels unread**: v24x names carry settings (`TS808_Hot_LvlMax_OD5_T4`, `TS808_Hot_Lvl6_OD0_T5`)
   but `ts` has no `label_regex`.

## Tasks

A. **Metadata from every manifest.** `pedal-fit` and `pedal-accuracy` take `--manifest PATH` (repeatable).
   Default: every one of `<cache>/pool_manifest.json` and `~/sawblade-work/pedal_pool.json` that exists,
   merged by tone id. A target capture with no name/licence/creator in any manifest is an error listed in
   the report (not `None`), and a tone whose model list resolves to zero models is an error, not silence.
   `scripts/run_pedal_accuracy.sh` passes its pull manifest explicitly.
B. **TS labels.** `targets.json` `ts.label_regex` parses the v24x names into `level`, `drive`, `tone`
   (`Max` = 10; digits as given, 0–10 scale; document the mapping). Unit tests on the 12 names in the
   report.
C. **TS family = TS circuits only.** `targets.json` restricts 70280 to the TS models (577277 TS 808 BOOST,
   577278 TS 808 MOD). The report adds a section **"Other circuits (not scored)"** for fits that exist in a
   fits file but are not in the pedal's target list (keeps the DS-1/Klon/etc. numbers visible as
   information for future pedal models). Verdicts use only target-list captures.
D. **Re-fit when the pin changes.** With `--merge`, a capture whose stored fit has no constrained result (or a
   different pin) gets its constrained fit run; a capture whose free fit is done keeps it. So the user's
   re-run only pays for the missing constrained fits and the newly found captures (6778).
E. **Verdict for unlabelled captures.** Besides "cannot be judged", print the free-fit share under the 2.0 dB
   LTAS target (`k/N free fits ≤ 2.0 dB`) and the harmonic error vs 2× the family spread, labelled as a
   lower bound (free fits can only do better than constrained).

## Acceptance

Unit tests: merged manifests (name/licence from the second manifest); empty-model-list tone → error;
v24x label parsing; 70280 restriction + "other circuits" section; merge re-runs only the missing
constrained fit (mock the fitter). Existing calibrate tests pass; CI python job green. Report
`docs/specs/v0_4a1-accuracy_report_fixes_REPORT.md` with the reviewer verdict and the exact Mac re-run
command (`scripts/run_pedal_accuracy.sh --di "testdata/ntm/bloodbath/17 GTR RHY L DI.wav" --no-pull`).
