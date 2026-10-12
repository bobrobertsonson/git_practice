# v0.4a A.1 diagnosis: the "~40 dB harmonic error on every fit"

Spec: `docs/specs/v0_4a-pedal_accuracy.md`. Evidence for the metric fix in `match/sawblade_match/calibrate/pedal_fit.py`.
Everything below is reproduced by `python docs/reports/v0_4/a1_diagnosis.py` (needs `tonerender`). No capture is used.

## Method

* Probe: the 7.1 probe, full 45 s at 48 kHz: 10 s sweep (-20 dBFS), 5 s stepped sines (110/220/440/880 Hz x -30/-20/-10/-3
  dBFS, 16 slots of 0.3125 s), 30 s DI. The 7.1 DI (`testdata/gatecreeper_cover/Guitar_L.wav`) is not in the repo, so the
  DI slot holds `tests/fixtures/di_riff.wav` (a 4 s riff, wrapped). The harmonic and level numbers do not depend on it.
* Renders: `tonerender` (the C++ core), single path, gate off, cab off, flat EQ, exactly as the fit does.
* "Legacy" = the 7.1 term (`harmonic_profile`, profiles floored at -70 dB re the fundamental, RMS over 16 x 6 cells).
  "Fixed" = the same profiles clamped at -40 dB. Even = H2 H4 H6, odd = H3 H5 H7.
* Knobs `low 7 high 5 distortion 8 level 8`, `pedal.hm` modelVersion 1 vs 3 (`hm_preset(..., model_version=...)`).
* The container has no scipy/soundfile, so the numbers were produced with numpy stand-ins for `soundfile` and
  `scipy.signal.welch/freqz` (scratch only, not in the repo). The harmonic, lag and dynamics terms do not use them; the
  LTAS figures (3.89 dB below) may differ in the last digit under real scipy.

## 1. Reproducing the 40 dB without captures

| comparison (reference vs model) | legacy | fixed (-40) | even | odd | LTAS | dyn |
|---|---|---|---|---|---|---|
| v3 vs v1 | 22.44 | 4.41 | 4.46 | 4.37 | 3.89 | 1.32 |
| v3 vs v3 (self) | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| 7.1 stock-capture profile vs v1 render | **40.0** | 19.3 | 26.4 | 7.1 | n/a | n/a |
| 7.1 stock-capture profile vs v3 render | 19.3 | 17.2 | 23.4 | 6.7 | n/a | n/a |

The 7.1 stock-capture profile is H2..H7 = -9, -18, -13, -22, -20, -21 dB (the 7.1 report), applied to all 16 slots.
Against the measured `pedal.hm` v1 profile it gives **40.0 dB**, the 7.1 number to the first decimal.

Per-harmonic profile, dB re fundamental, mean over the 16 slots (legacy floor -70), H2 H3 H4 H5 H6 H7:

| | H2 | H3 | H4 | H5 | H6 | H7 |
|---|---|---|---|---|---|---|
| v1 (all four levels identical) | -70.0 | -11.1 | -70.0 | -14.3 | -70.0 | -18.5 |
| v3, mean | -38.3 | -11.1 | -39.3 | -16.5 | -40.2 | -21.2 |
| v3, -30 dBFS | -34.5 | -11.1 | -35.5 | -16.6 | -36.5 | -21.2 |
| v3, -20 dBFS | -38.7 | -11.1 | -39.7 | -16.5 | -40.7 | -21.2 |
| v3, -10 dBFS | -40.0 | -11.1 | -40.9 | -16.5 | -41.9 | -21.1 |
| v3, -3 dBFS | -40.0 | -11.1 | -41.0 | -16.5 | -42.0 | -21.1 |
| v3 - v1, legacy mean | +31.7 | -0.1 | +30.7 | -2.3 | +29.8 | -2.7 |
| v3 - v1, legacy RMS | 32.6 | 2.7 | 31.5 | 4.7 | 30.1 | 5.3 |
| v3 - v1, fixed mean | +4.4 | -0.1 | +3.3 | -2.3 | +1.4 | -2.7 |
| v3 - v1, fixed RMS | 6.0 | 2.7 | 4.2 | 4.7 | 2.5 | 5.3 |

All 48 even cells of v1 sit exactly at the -70 dB floor (fraction 1.0): v1 clips symmetrically, so its even harmonics
are numerically absent. v3 has a little even content (-34 to -42 dB, more at low drive level), nowhere near the
-9/-13/-20 dB of the stock captures.

Per drive level (v3 vs v1): legacy RMS 24.7 / 22.0 / 21.4 / 21.4 dB at -30 / -20 / -10 / -3 dBFS, fixed 5.1 / 4.2 / 4.1 / 4.1.

**Deviation from the spec's expectation.** The spec expected `harm_rms_db_legacy` > 25 dB for v1 vs v3. Measured 22.4 dB,
because v3 is not at the floor on its even cells (it is 30 dB above it), so the v1-v3 gap is ~31 dB on three of six
cells, not ~60. The ~40 dB symptom is the capture-vs-v1 comparison (row 3), which is reproduced exactly. The regression
test asserts legacy > 20 dB for v1 vs v3 and legacy 38..42 dB / fixed 15..22 dB for the 7.1 capture profile vs v1. The
spec's "fixed term has its even part dominating" is true for the legacy term (even 31.4 vs odd 4.4) and for the
capture profile (26.4 vs 7.1) but not for v1 vs v3 at -40 dB (4.46 vs 4.37), because v3's even cells are within a few dB of
the floor: the tie *is* the result (the audible even content of v1 and v3 differs little).

## 2. Root cause

The number is real arithmetic, not an estimator, level or latency fault, but it is dominated by the arbitrary -70 dB
"absent" floor. `pedal.hm` v1 has no even harmonics, so all of its even cells are exactly -70; a stock HM-2 capture has
H2/H4/H6 at -9/-13/-20 dB, so those three cells differ by 61/57/50 dB while the three odd cells differ by 2-7 dB, and the
RMS over six harmonics is 40.0 dB (even part alone 56.2 dB, 98 % of the squared error). Because v1's evens are absent
whatever the knobs, that even error is a constant: the term was flat in `low/high/distortion` (legacy 22.4 / 23.2 / 22.8 /
22.4 dB over four very different knob sets, against 4.4 / 7.3 / 5.9 / 4.4 dB for the fixed term, so ~3.6x more
knob-gradient) and every 7.1 fit (all v1) reported ~40. The gap itself (a model with no H2/H4/H6 against captures with
them 9-20 dB under the fundamental) is a real structural defect, but at -40 dB it is 19 dB, half of what 7.1 reported, and
-70 dB is ~50 dB below anything audible in a saturated spectrum. v3, which has some even content, would have scored 19.3 dB
legacy vs the same capture profile (17.2 fixed); it was never re-fit.

Ruled out (numbers from `a1_diagnosis.py`):

* **Estimator leakage.** A pure sine through `harmonic_profile` gives every cell at the -70 floor; unfloored, a -30 dBFS
  220 Hz sine has H2..H7 at -104.9, -122.7, -133.7, -141.1, -146.4, -150.9 dB (Hann window, +-12 Hz bins, 35-95 % of the
  slot). Leakage is 65+ dB under the -40 dB floor, so -40 is set by audibility, not by the estimator.
* **Output gain.** Scaling the reference by -20, -10, +10, +20 dB changes harm, even, LTAS and dyn by 0.0000 dB.
* **Latency / slot misalignment.** Delaying the reference by 1, 77, 512 and 2048 samples changes the fixed term by 0.0000 dB
  (aligned and also unaligned: the slot window starts at 35 % = 5250 samples, so a delay under ~5000 samples keeps it
  inside its own slot). The sweep cross-correlation still finds the lag (delay + the model's own 5-7 samples) and is kept
  as a safeguard for larger latency; it is recorded per fit as `lag`.
* **Level dependence.** The harmonic term cannot depend on `level` where `level` is a pure gain after the clippers (below).

## 3. The fixed term and the floor

Floor sweep on v3 vs v1 (legacy 22.44 throughout): -30 dB: fixed 3.09 (even 0.01); **-40 dB: 4.41 (even 4.46, odd 4.37)**;
-50 dB: 9.41 (12.58); -60 dB: 15.7 (21.77); -70 dB: 22.44 (31.44, = legacy). Against the capture profile vs v1: floor -30 12.8,
-40 19.3, -50 26.1, -70 40.0.

-40 dB kept: it clamps content that is masked by the fundamental and an H3 at -11 dB (inaudible), the estimator is 65 dB
below it, and it still shows v3's real, audible even content against v1 (-30 dB would hide the whole v1/v3 even difference:
0.01 dB). `--harm-floor -70` reproduces the 7.1 cost exactly; the floor is recorded in every fits JSON (`cost.harm_floor_db`).

## 4. Is `level` a pure output gain? (per pedal)

Rendering at level 8 and at level 5, 2, 10 and comparing with the level-8 render scaled by `10^(3 (L-8)/20)`:
max relative error 5.5e-08 .. 1.3e-07 (float32 round-off) and a slope of exactly 3.0000 dB/level, for **all five pedals**
(`hm`, `hmx`, `eye`, `muff` (its `volume` knob), `ts`). So `level` is solved in closed form for every pedal
(`PedalSpec.level_pure_gain = True`), enforced per pedal by `test_level_is_a_pure_output_gain[<pedal>]`.

## 5. Identifiability on known answers (why a few knob tolerances are loose, and `muff` crunch is not searched)

Cost (LTAS + 0.5 harm + 0.5 dyn) when one knob of the known-answer truth moves by -0.5 / +0.5 (all other knobs true):

| pedal | knob | cost -0.5 | cost +0.5 |
|---|---|---|---|
| hm | low / high / distortion | 0.97 / 1.13 / **0.15** | 0.94 / 1.18 / **0.12** |
| hmx | low / lowMid / highMid / high | 0.84 / 0.55 / 0.66 / 0.59 | 0.83 / 0.56 / 0.66 / 0.60 |
| hmx | distortion / presence | **0.12** / 0.33 | **0.12** / 0.33 |
| eye | gain | 0.11 | 0.10 |
| muff | sustain / tone / scoop / voice | **0.13** / 1.76 / 0.47 / 1.14 | **0.11** / 1.50 / 0.47 / 1.20 |
| ts | drive / tone | 0.34 / 0.64 | 0.34 / 0.63 |

The drive knobs (hm/hmx `distortion`, muff `sustain` at 7) saturate: half a unit changes the cost by 0.1-0.15, so a small
budget cannot pin them within 0.5 (the tests assert 1.0 / 1.0 / 1.5 on those three, and every metric threshold as in the
spec). Everything else is recovered within 0.5 (measured over 3 seeds at the test budgets: all other knobs within 0.4).

**`pedal.muff` v1 `sustain` and `crunch` are exactly redundant.** Stage A gain is `6 + 3 sustain` dB and the clipper knee
scales with `1.2 - 0.08 crunch`; a clipper of gain g and knee k outputs `k f(g/k)`, so only g/k is observable and the level
solve absorbs k. Along the iso-g/k curve from the truth `sustain 7 crunch 3` (solved level 8): `sustain 6 crunch 6.5` gives
LTAS 0.000 / harm 0.000 / dyn 0.001 dB with solved level 9.00 and `sustain 5 crunch 8.99` the same with level 10.00
(`test_muff_sustain_and_crunch_are_redundant`). A 5-knob free fit wandered along that ridge (sustain 1-2 units, crunch 1-5
units off at ~0.03 dB metric error), so `crunch` stays at the block default in the fit and `sustain` carries the drive. A
v0.5 model with this redundancy removed would be cleaner.
