# v0.4 Task A report: pedal accuracy harness and baseline

Spec: `docs/specs/v0_4a-pedal_accuracy.md`. Branch `claude/sawblade-v0_4a-pedal-accuracy`, from
`claude/sawblade-plugin-setup-7k0b8q`. No pedal DSP, `plugin/` or preset changes, and no capture files or secrets.

## Status in one paragraph

**A.1 is done and verified.** The ~40 dB harmonic error was a defect in the metric, not a real 40 dB gap. The metric
is fixed. The fit tool now covers all five pedals, and a known-answer check passes on each of them.

**A.2 tooling is done; the A.2 numbers are not.** No real-capture numbers exist, because this cloud container could
not run the fits:

* TONE3000 needs the user's OAuth login, and there is none in the container.
* pypi.org and apt return 403, so scipy and soundfile could not be installed. The user later added pypi to the
  allowlist, but the change did not reach this container.
* The DI file `testdata/gatecreeper_cover/Guitar_L.wav` is not in the repo.

So `docs/reports/v0_4/accuracy.md` is published with the known-answer results only and is marked
**PENDING USER RUN**. The exact Mac commands are in `docs/runbooks/v0_4a_pedal_accuracy.md`. No capture numbers were
invented.

## Reviewer verdicts

| round | commits | verdict | blocking items |
|---|---|---|---|
| 1 | 9bdb98d, 69d96ec, 4bbed7e, 3642dda, 2d7deaa | REVISE | (1) the default `--pedal hm` run ignored `targets.json`, so tones 6778 and peterny would never be fitted; (2) model name in the commit trailers (lead exception, below) |
| 2 | 2252456 | **ACCEPT** | none |

The fixes in round 2:

* The targets manifest is now the default for every pedal. The 7.1 capture list needs `--targets builtin-7.1`.
* `estimate_lag` returns 0 for a silent reference.
* The lag search window is clamped to the sweep's H2 lead (`T·ln2/ln(f1/f0)`), so it cannot lock onto a harmonic.
  A weak peak inside a clamped window is rejected. The reviewer measured the peak on maximum-drive renders of all
  five pedals: 0.45–0.55 against the 0.1 threshold. The full probe is never clamped.
* A manifest entry with no label regex gets no labels, with no HM-2 fallback.
* Captures whose pins were partly filled with defaults are excluded from the target check.
* `--merge` refuses a file made with a different probe layout.
* The CI test budgets were given headroom.

One non-blocking item is left over: `--targets builtin-7.1` is not rejected for pedals other than hm.

**Lead exception (commit trailers).** Every commit ends with the `Co-Authored-By` trailer that the user's task
instructions specify word for word. The "no model identifiers" rule applies to model ID strings in files and
commit bodies. The reviewer confirmed there are none in any file. The commits were not rewritten.

## The metric fix, with evidence (`docs/reports/v0_4/a1_diagnosis.md`, reproducible by its `.py`)

**Root cause.** Every 7.1 fit ran `pedal.hm` **v1**, because `hm_preset` hard-coded `modelVersion: 1`. v1 clips
symmetrically, so all 48 of its even-harmonic cells sat exactly on the -70 dB "absent" floor. The captures' evens
are at -9, -13 and -20 dB, which is a 50–60 dB gap on three of six harmonics: about 40 dB RMS, whatever the knob
settings. Reproduced without captures:

* The 7.1 stock-capture harmonic profile scored against a v1 render gives **40.0 dB**, the 7.1 figure to the first
  decimal.
* The even harmonics account for about 98 % of the squared error.
* The term was flat in the knobs: it moved by 0.8 dB across four knob sets.

**Ruled out, with numbers:**

* Estimator leakage: H2..H7 of a pure sine read -105 to -151 dB.
* Output gain: ±20 dB changed the term by 0.0000 dB.
* Latency: delays up to 2048 samples changed it by 0.0000 dB.
* The level knob: it is a pure 3 dB/step output gain on all five pedals (max relative error 1.3e-7). So `level` is
  solved in closed form for every pedal, and a test per pedal checks this.

**Fix.**

* The harmonic profiles are clamped at **-40 dB** re the fundamental before comparison, and the floor is recorded in
  the JSON.
* The floor sweep for v3 vs v1 gave 3.09 / 4.41 / 9.41 / 22.44 dB at floors of -30 / -40 / -50 / -70 dB. At -30
  the even difference disappears completely; at -50 and below the floor dominates again.
* The new term `harm_rms_db` drives the cost. `harm_rms_db_legacy` (7.1-comparable), `harm_even_rms_db` and
  `harm_odd_rms_db` are reported alongside it.
* Renders are aligned to the probe by cross-correlating the sweep, and the lag is recorded per fit.

**What it changes.** The stock-capture profile against v1 drops from 40.0 to 19.3 dB (even 26.4, odd 7.1). Against
v3 it is 17.2 dB (even 23.4). So the harmonic gap is real, but it is about 17–19 dB, not 40. v3's asymmetric clip
only reaches -34 to -42 dB on the even harmonics; the real HM-2 sits at -9 to -20 dB.

## Known-answer table (each pedal fitted against its own render; error of a perfect fit = 0)

| pedal | version | constrained at the true params: LTAS / harm / dyn | free fit: LTAS / harm / dyn | max knob error |
|---|---|---|---|---|
| hm | 3 | 0.000 / 0.000 / 0.000 | 0.003 / 0.009 / 0.002 | 0.02 |
| hmx | 1 | 0.000 / 0.000 / 0.000 | 0.043 / 0.068 / 0.007 | 0.20 |
| eye | 1 | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 / 0.000 | 0.00 |
| muff | 1 | 0.000 / 0.000 / 0.000 | 0.018 / 0.010 / 0.029 | 0.18 |
| ts | 1 | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 / 0.000 | 0.00 |

The spec limits are LTAS < 0.2 dB, harm < 0.5 dB and dyn < 0.3 dB. At the smaller CI budgets, the worst result over
three seeds is: hm 0.024 / 0.098 / 0.029, hmx 0.088 / 0.221 / 0.026, muff 0.051 / 0.046 / 0.093, and ts and eye at or
below 0.011. The table above was produced with numpy stand-ins for scipy (this is disclosed in the file); runbook step 1
regenerates it on a full install.

## Real-capture accuracy table

**PENDING USER RUN** for all five pedals (see `docs/reports/v0_4/accuracy.md`). For the Mac run:

* The targets manifest `docs/reports/v0_4/targets.json` lists hm 58569 (labelled), 74487, 78122, 88604 and 6778; hmx
  72990; eye 60618 and 62523; ts 30104, 92212 and 70280.
* muff and the peterny HM-2 capture are resolved with `sawblade-t3k search` during the run.
* Licence and creator are recorded per capture in the fits output.
* Then `sawblade-calibrate pedal-accuracy` writes the table: free and knob-constrained LTAS / harm (even and odd) /
  dyn per capture, the capture-to-capture spread per pedal, the v0.4 target check and a verdict per pedal.

## Spec deviations, with lead sign-off

* **(a)** The test that v1 vs v3 shows legacy > 25 dB was lowered to > 20 dB. The measured value is 22.4, because
  v3's evens sit 30 dB above the floor. The real 40 dB symptom is asserted separately: legacy 38–42 dB and fixed
  15–22 dB for the capture profile against v1. **Accepted.** The spec text was my estimate, and the measurement
  replaces it.
* **(b)** "Even part dominates the fixed term" does not hold for v1 vs v3 at -40 dB (4.46 even vs 4.37 odd). It does
  hold against the capture profile (26.4 vs 7.1). **Accepted.** At an audible floor, v1 and v3 really do differ little.
* **(c)** muff `crunch` is not searched, because it is exactly redundant with `sustain` in v1 (there is a test).
  **Accepted.** The v0.5 muff circuit must remove the redundancy.
* **(d)** The knob-recovery tolerance is loosened on three knobs that saturate: hm and hmx distortion to 1.0, muff
  sustain to 1.5. The metric thresholds are unchanged. **Accepted.** Half a knob unit on these moves the cost by only
  0.12–0.15.
* **(e)** The default model version is now v3 for hm and schema-2 fits files, and a schema-1 `--merge` is refused.
  7.1 can be replayed with `--model-version 1 --harm-floor -70 --refine-generations 0 --targets builtin-7.1`.
  **Accepted.**
* **(f)** hmx searches its six stock knobs. Voicing, boost, clip, tightness and mix stay at their defaults.
  **Accepted** for a stock-capture baseline.
* **(h)** The target check uses only captures whose name carries a full label. The harmonic target is the mean
  constrained harm compared with 2× the same-setting spread, falling back to the family spread. **Accepted.**
* **(i)** The default `--refine-generations` is 16, about 464 renders per capture. **Accepted.**

## Gaps v0.5 must close (from today's evidence; capture numbers will sharpen them)

1. **Even harmonics (the chainsaw circuit).** `pedal.hm` v3's H2/H4/H6 are 25–30 dB below the real HM-2's. That is a
   fixed-metric error of about 17 dB against the stock profile. The circuit model's asymmetric clipping (diode
   pair, op-amp bias) has to produce H2 near -9 dB at 220–440 Hz.
2. **Drive range and EQ (7.1, v1 numbers).** These numbers were measured on v1:
   * The real unit is already saturated at D-2.
   * The EQ residual dips at 160–320 Hz, bumps at 0.5–0.8 kHz, cuts at 4–5 kHz and is short +10 dB at 10 kHz.

   v3 has not yet been measured against captures. The user's run gives the v3 baseline these targets compare against.
3. **muff:** remove the sustain/crunch redundancy.
4. **Targets to beat:** v0.4 asks for knob-constrained LTAS ≤ 2.0 dB on ≥ 75 % of labelled captures, with harm within
   2× the capture spread. v0.5 tightens LTAS to ≤ 1.5 dB at ≥ 3 settings. The metric to use is `harm_rms_db` at the
   -40 dB floor; the legacy term is not usable as a target.

## For the user (Mac)

Follow `docs/runbooks/v0_4a_pedal_accuracy.md`: install, `sawblade-t3k login`, pull per manifest, fit each pedal,
`sawblade-calibrate pedal-accuracy`, then commit only the JSON, MD and PNG files.

## Validation

CI (GitHub Actions) is the record. Results on the head commit are in the section below.
