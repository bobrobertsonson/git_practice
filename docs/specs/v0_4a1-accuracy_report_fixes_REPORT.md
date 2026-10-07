# v0.4A.1 accuracy report fixes: implementation report

Spec: `docs/specs/v0_4a1-accuracy_report_fixes.md`. Branch `claude/sawblade-v0_4a1-report-fixes` (base 68dc00f).
Reviewer verdict: pending.

## What changed

* **A. Metadata from every manifest.** `pedal_accuracy.load_manifests` merges `sawblade-t3k` manifests by tone id
  (`pool_manifest.json` and `pull --manifest` files have the same shape; the writer is `t3k/pool.py`
  `build_pool`/`decision_to_json`). Per field the first non-empty value wins, model lists are unioned.
  `pedal-fit` and `pedal-accuracy` take `--manifest PATH` (repeatable); with none given they use every one of
  `<cache>/pool_manifest.json` and `~/sawblade-work/pedal_pool.json` that exists (no manifest at all is an error).
  `load_targets(..., manifests=, errors=)`: a tone whose model list resolves to zero models appends an error
  (also stored in the fits file as `errors`). A scored capture missing name / licence / creator is an error in the
  report (`MISSING`, never `None`) and in `pedal-fit` stderr. `pedal-fit` exits 4 (after writing the fits file) when
  there are errors; the run script already continues on a failed fit. `pedal-accuracy --manifest` also back-fills
  records of older fits files. `scripts/run_pedal_accuracy.sh` passes its pull manifest to pull, fit and report.
* **B. TS labels.** `targets.json` `ts.label_regex` = `Lvl(Max|\d+)_OD(Max|\d+)_T(Max|\d+)`, groups
  `level, drive, tone`; digits as given on the 0-10 scale, `Max` = 10 (`parse_labels` handles it; documented in
  `label_note`). Level is a pure gain, so drive and tone are the pinned knobs.
* **C. TS family = TS circuits only.** 70280 restricted to `[577277, 577278]`. Fits files record `target_models`
  (the full resolved target list, before `--tone`/`--model`); the report scores only those and adds
  **"Other circuits (not scored)"** (free-fit numbers) for the rest. Spread and verdicts use target-list captures only.
* **D. Re-fit when the pin changes.** With `--merge`, a stored capture whose pin differs from the current one, or
  that has a pin but no `constrained` result, gets only `fit_constrained_only` (one reference render, no search).
  Its stored name / licence / creator / labels are refreshed from the manifests. A stored free fit is never redone.
* **E. Unlabelled verdict.** Besides "cannot be judged": `k/N free fits <= 2 dB` LTAS and the mean free-fit
  harmonic error vs 2 x family spread, labelled "Lower bound (free fits can only do better than constrained)".

## Tests (`match/tests/test_pedal_fit.py`, new)

TS label parsing on the 12 report names, 70280 restriction, merged manifests (name/licence from the second manifest),
zero-model tone error and no-manifest error, missing-metadata errors and `pedal-accuracy --manifest` fill, other
circuits not scored, unlabelled verdict, merge re-runs only the missing/changed constrained fit (fitter mocked).

Results here: `tests/test_pedal_fit.py` with a locally built `tonerender`: 44 passed. Full `match/` suite without the
pybind module: all pass except `test_fizz`, `test_matcher`, `test_offset_search`, `test_speed`, which cannot be
collected without `sawblade_core` (unrelated to this change; CI builds it).

## Mac re-run

```
scripts/run_pedal_accuracy.sh --di "testdata/ntm/bloodbath/17 GTR RHY L DI.wav" --no-pull
```

Add `--pedals ts` etc. to limit. Only the missing constrained fits and newly found captures (6778) are paid for.
Existing fits files keep their free fits; the old 70280 non-TS fits stay in the file and appear under "Other circuits".

## Decisions

* An explicit `--manifest` replaces the default manifest list (it does not add to it).
* `pedal-fit` returns exit 4 when the target list has errors (fits still written).

## Follow-ups

Reviewer non-blocking findings, applied:

1. `pedal-fit --merge` refresh only overwrites name / unit / group / creator / licence / labels when the new value is
   non-empty (a bare model-id name counts as empty), so a partial `--manifest` keeps good stored metadata.
   Labels follow the name (lead decision): a refreshed name writes its parsed labels (and pin), even None; a bare-id or
   empty name keeps the stored labels and pin and triggers no pin-change re-run. `non_commercial` is derived from the
   stored licence.
2. `pedal-accuracy` tables print `name or model_id` (no "None (id)").
3. `scripts/run_pedal_accuracy.sh`: pedal-fit exit 4 prints "WARN: <pedal> fit finished with ERRORS (see the report's
   Errors section)"; other non-zero codes still print "fit failed". `set -euo pipefail` is kept (`rc` captured via `|| rc=$?`).
4. Direct unit test of `fit_constrained_only` (pins {drive:4,tone:5,level:6} on ts give knob vector [4,5]; result is
   the dict stored as `constrained`).

Tests added (`match/tests/test_pedal_fit.py`): partial-manifest merge, name=None report, `fit_constrained_only`.
`tests/test_pedal_fit.py` with a locally built core-only `tonerender`: 48 passed (test_merge_labels_follow_the_name added).
