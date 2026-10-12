# v0.4 Task A — pedal accuracy harness and baseline

Parent: `docs/specs/v0_4-pedals.md` Task A (scope note: A.1 + A.2 only, no DSP changes). Feeds the targets of
`docs/specs/v0_5-circuit_pedals.md`. Owner: match-engineer (`match/`); dsp-engineer only if a C++ render hook is
needed (none expected: `tonerender` renders any registered block type). Reviewer on every round.

**Do not change** any pedal DSP (`core/src/pedal_*.cpp`, their params/versions), `plugin/`, or committed presets.

## Known facts (lead, from the code and `docs/reports/phase7_1/fits.json`)

* `pedal_fit.hm_preset()` hard-codes `"modelVersion": 1`. Every 7.1 fit was against **`pedal.hm` v1**, which
  clips symmetrically: its even harmonics are numerically absent and land on `HARM_FLOOR_DB = -70`.
* 7.1 measured stock captures at H2 ≈ -9, H4 ≈ -13, H6 ≈ -20 dB re fundamental. Three of six harmonics
  differing by 50–60 dB gives an RMS of ≈ sqrt(0.5 · 55²) ≈ 39 dB, i.e. the "~40 dB on every fit" is what the
  metric must produce for any symmetric model, whatever its knobs. Lead hypothesis: the number is real but the
  metric is badly conditioned. It is dominated by an arbitrary floor (-70 dB is ~50 dB below anything
  audible in a saturated spectrum) and gives no gradient to the knobs. **Prove or refute this with evidence;
  do not assume it.** Other candidates to rule in or out: estimator leakage (±12 Hz bins, Hann window, onset
  skip), level or latency dependence, and slot misalignment when the reference has latency.
* `level` is solved in closed form only because `pedal.hm` ends in a pure `3·level − 24` dB gain. That has not
  been checked for the other four pedals.

## A.1 — measurement verified on known answers, harmonic term fixed

1. **Diagnosis (evidence in the report).** Reproduce a ~40 dB harmonic error without captures: render
   `pedal.hm` v1 and `pedal.hm` v3 (asymmetric clip) at the same knobs and compare them with the 7.1 metric.
   Then show the per-harmonic breakdown (H2..H7, even vs odd) for that comparison and for a v3-vs-v3 self
   comparison. State the root cause in one paragraph and show the numbers.
2. **Metric fix.** Keep the old term, reported as `harm_rms_db_legacy` so 7.1 stays comparable, and add a fixed
   term `harm_rms_db` that is the one used in the cost. Requirements:
   * Profiles are clamped at a floor that reflects audibility and estimator noise, not "absent". Default
     **-40 dB re fundamental**. If the evidence supports another value, use it and justify it. The floor is
     recorded in the output JSON.
   * Also report `harm_even_rms_db` and `harm_odd_rms_db`, so a missing even series is visible as such.
   * The term must be invariant (≤ 0.05 dB change) to output gain (±20 dB) and to a pure delay of up to
     2048 samples on the reference. If latency breaks it, align the slots (e.g. cross-correlate the
     sweep segment) and record the lag.
3. **Generalize to all five pedals.** Add `--pedal {hm,hmx,eye,muff,ts}` (default `hm`) and
   `--model-version N` (default: the current version of that block type; `hm` = 3). Each pedal gets its param
   space from the block's real params (`pedal_params.cpp`, `pedal_saw_params.cpp`). The `level` knob is solved in
   closed form **only** for pedals where a test proves it is a pure output gain. Otherwise it is searched like
   the others. LTAS remains a shape error (offset removed). Existing 7.1 CLI usage and `--merge` keep working.
   Bump the `fits.json` schema version. Old files must still load in `--merge`, or be refused with a clear
   message.
4. **Known-answer tests (pytest, run in CI).** For every pedal, render the probe at a fixed non-default
   parameter set and treat that render as the "capture":
   * the knob-constrained fit at the true params gives LTAS < 0.05 dB, `harm_rms_db` < 0.1 dB and dyn < 0.05 dB;
   * the free fit (seeded, small budget is fine) gives LTAS < 0.2 dB, `harm_rms_db` < 0.5 dB and dyn < 0.3 dB,
     and recovers the params within 0.5 knob units where the param is identifiable (say which ones are not and
     why);
   * gain/delay invariance as in A.1.2;
   * regression: `pedal.hm` v1 vs v3 at the same knobs gives `harm_rms_db_legacy` > 25 dB (the 7.1 symptom
     reproduced) and a fixed `harm_rms_db` that is finite, below the legacy value, and has its even part
     dominating;
   * level-is-pure-gain test per pedal (pass or documented fail, matching the closed-form decision).
   CI time: these must not add more than ~3 min to the python job (use a short probe layout in tests).
5. **Capture-to-capture spread.** Add the statistic that v0.4/v0.5 targets need. For each pedal family and
   each group of captures with the same labelled setting (or the whole family when there are no labels), compute
   the RMS distance of each capture's fixed harmonic profile to the group mean. Report it in the fits JSON and
   the accuracy table. Unit-test it on synthetic profiles.

## A.2 — fit the five pedals against captures; baseline report

1. **Targets manifest** `docs/reports/v0_4/targets.json`: per pedal, candidate TONE3000 tone ids for its family
   with the pedal param to label mapping:
   * `hm`: 58569 (labelled), 74487, 78122, 88604, 6778, and the peterny cc-by-nc HM-2 MiJ v2.0 if found;
   * `hmx`: modded chainsaw, e.g. 72990 Torcher;
   * `eye`: 60618, 62523;
   * `ts`: 30104 TS808 JRI, 92212, and the 70280 boost pack's TS-style models;
   * `muff`: big-fuzz family, resolved by search.

   Ids the docs don't give are filled by `sawblade-t3k search` at run time. Never commit capture files.
   Record licence + creator per capture used (from the API) in the fits JSON and in the report.
2. **Report generator**: `sawblade-calibrate pedal-accuracy` reads the per-pedal fits JSONs and writes
   `docs/reports/v0_4/accuracy.md`. Per pedal and capture it gives free and knob-constrained LTAS,
   `harm_rms_db` (+ even/odd), dyn, licence/creator, and whether the pins are labelled or assumed. Per pedal it
   gives the capture spread, the v0.4 target check (constrained LTAS ≤ 2.0 dB on ≥ 75 % of labelled captures;
   harm within 2× spread), and a one-line verdict. Test it on a synthetic fits JSON.
3. **Run it** if this container can. Today it cannot. TONE3000 needs the user's OAuth token (none in the
   container), pypi/apt are blocked (no scipy/soundfile), and the DI `testdata/gatecreeper_cover/Guitar_L.wav` is
   absent. In that case `accuracy.md` is published with the known-answer results from A.1 and **no
   capture numbers** (never fake them), marked `PENDING USER RUN`. A runbook
   `docs/runbooks/v0_4a_pedal_accuracy.md` gives the exact Mac commands: login, pull per targets manifest,
   fit each pedal, generate the report, commit the JSON/MD/PNG only.

## Acceptance

* A.1 diagnosis with numbers. Known-answer tests pass in CI on all five pedals. The fixed harmonic term is in the cost.
* Full suite green in CI (GitHub Actions is the validation of record: linux gcc, clang -Werror, macOS, python).
* `docs/reports/v0_4/accuracy.md` + targets manifest + runbook; report `docs/specs/v0_4a-pedal_accuracy_REPORT.md`
  (lead) with reviewer verdicts, the metric fix and evidence, the table, and the gaps v0.5 must close.
* No DSP, plugin or preset changes; no capture files or secrets in git; no model identifiers in commits/files.
