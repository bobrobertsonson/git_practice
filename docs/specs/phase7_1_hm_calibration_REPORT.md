# Phase 7.1 report: `pedal.hm` against real HM-2 captures

Tool: `sawblade-calibrate pedal-fit` (`match/sawblade_match/calibrate/pedal_fit.py`); raw numbers in
`docs/reports/phase7_1/fits.json`, plots `model_<tone>_<model>.png` and `aggregate.png` next to it.
Seeds: base 20261004 + model id (restart r adds 1000 r); all recorded in `fits.json`.
Renderer: `tonerender` through subprocess (the pybind module is not built here); all `pedal.hm` audio is the C++ core.

## Method as run

* Probe: 45 s, 48 kHz mono = 10 s -20 dBFS exp sweep, 5 s stepped sines (110/220/440/880 Hz x -30/-20/-10/-3 dBFS,
  16 x 0.3125 s), 30 s DI = Guitar_L.wav bars 20-37.5 (140 bpm, from Drums.mid), resampled 44.1 -> 48 kHz.
* Reference = probe through the `nam` block only (single path, gate off, cab disabled, flat EQ).
* Cost = LTAS (1/3 oct, 63 Hz-10 kHz bands, 23 bands inside 60 Hz-12 kHz, dB RMS, DI segment) + 0.5 x harmonic
  profile (H2..H7 re fundamental, 16 sines x 6 harmonics, floored -70 dB, dB RMS) + 0.5 x dynamics (RMS of the crest
  factor difference and the p90-p10 50 ms-envelope-spread difference, dB).
* `level` is solved in closed form: `pedal.hm` ends in a pure linear gain `3*level-24` dB (checked by
  `test_level_is_a_pure_output_gain`), so the LTAS-optimal gain is the mean 1/3-oct dB offset; harmonic and dynamics
  terms are level-independent. The search is CMA-ES (matcher.cma, 3 seeded restarts, popsize 8, 14 generations,
  ~340 renders per model) over low/high/distortion in [0,10]. Same optimum as a 4-parameter search.
* LTAS error is a *shape* error (offset removed even when it needs a `level` outside 0..10, marked `*`). 5 of 8 stock
  captures need level < 0, i.e. their absolute output is >24 dB below the model's: the captures' reference level is
  not the model's, so the absolute `level` values below are not meaningful; only differences between models are.
* Constrained fit: low/high/distortion pinned to the labelled knobs, level free. For captures without a full
  `Lv L H D` label the pinned values are my assumptions (marked "assumed"): maxed = 10/10/10; "2 o'clock" distortion =
  7 (12 o'clock = 5, +1 per hour); Torcher "0 Gain" = D 0; Eyemaster "Gain 12 / Vol 4 o'clock" = 5/5/5 (L/H guessed).
* All 8 listed 58569 models were fitted (the last two, `Lv-7 L-5 H-8 D-2` and `Lv-7 L-9 H-9 D-0`, were pulled into
  the cache afterwards and merged with `--merge`). Every capture here is licence `t3k`; `fits.json` carries a
  per-model `non_commercial` flag (true for `cc-by-nc*`), false for all of them.

## Per-model fits

LTAS / harmonic / dynamics components in dB (the cost is LTAS + 0.5 harm + 0.5 dyn). `*` level beyond its range.
Labels are low/high/distortion.

| tone | model | pinned L/H/D | free L | free H | free D | free Lv | free LTAS / LTAS if level limited to 0..10 / harm / dyn | constrained LTAS / harm / dyn |
|---|---|---|---|---|---|---|---|---|
| 58569 | Boss HM-2 Lv-7 L-9 H-9 D-0 | 9/9/0 | 10.0 | 8.9 | 7.9 | -2.1* | 3.94 / 7.48 / 40.3 / 1.61 | 5.77 / 39.92 / 0.43 |
| 58569 | Boss HM-2 Lv-7 L-5 H-8 D-2 | 5/8/2 | 2.3 | 4.9 | 9.5 | 0.6 | 3.68 / 3.68 / 42.0 / 0.38 | 6.32 / 42.10 / 2.62 |
| 58569 | Boss HM-2 Lv-7 L-7 H-5 D-0 | 7/5/0 | 8.2 | 3.8 | 3.3 | -2.7* | 3.86 / 9.01 / 37.9 / 0.52 | 5.80 / 37.44 / 1.06 |
| 58569 | Boss HM-2 Lv-5 L-9 H-9 D-5 | 9/9/5 | 6.4 | 6.8 | 10.0 | -0.6* | 4.17 / 4.50 / 41.9 / 0.89 | 4.99 / 41.75 / 1.94 |
| 58569 | Boss HM-2 Lv-7 L-7 H-5 D-5 | 7/5/5 | 6.4 | 2.9 | 10.0 | -0.9* | 4.09 / 4.90 / 39.1 / 0.07 | 4.85 / 39.19 / 0.71 |
| 58569 | Boss HM-2 Lv-7 L-9 H-9 D-2 | 9/9/2 | 7.1 | 6.9 | 10.0 | 0.6 | 4.13 / 4.13 / 41.0 / 0.71 | 6.01 / 40.92 / 1.92 |
| 58569 | Boss HM-2 Lv-5 L-9 H-9 D-10 | 9/9/10 | 5.9 | 7.1 | 8.8 | -0.5* | 4.11 / 4.40 / 42.1 / 0.47 | 4.57 / 41.94 / 1.66 |
| 58569 | Boss HM-2 Lv-10 L-10 H-10 D-10 | 10/10/10 | 5.3 | 6.6 | 10.0 | 5.0 | 4.33 / 4.33 / 42.6 / 1.02 | 5.16 / 42.37 / 3.04 |
| 60618 | TC Electronic Eyemaster | 10/10/10 (assumed) | 6.2 | 7.1 | 6.6 | 5.7 | 3.48 / 3.48 / 39.8 / 2.18 | 4.00 / 39.65 / 3.99 |
| 62523 | EyemasterAHe_Gain12oclk_Vol4oclk | 5/5/5 (assumed) | 5.9 | 6.6 | 7.4 | 3.2 | 4.08 / 4.08 / 41.5 / 1.56 | 4.84 / 41.63 / 0.92 |
| 72990 | Throne Torcher Maxed 0 Gain | 10/10/0 (assumed) | 7.4 | 9.5 | 0.0 | 1.7 | 4.70 / 4.70 / 36.7 / 3.44 | 4.90 / 36.45 / 4.18 |
| 72990 | Throne Torcher Maxed V2 | 10/10/10 (assumed) | 6.7 | 7.5 | 6.2 | 3.4 | 3.72 / 3.72 / 41.4 / 1.17 | 3.86 / 41.42 / 2.67 |
| 74487 | boss hm-2w - maxed out s mode | 10/10/10 (assumed) | 6.1 | 5.9 | 10.0 | 3.3 | 4.46 / 4.46 / 42.3 / 0.73 | 5.65 / 42.20 / 3.02 |
| 74487 | boss hm-2w - maxed out c | 10/10/10 (assumed) | 6.7 | 5.0 | 10.0 | 4.2 | 5.05 / 5.05 / 41.2 / 0.52 | 6.49 / 41.27 / 3.43 |
| 74487 | boss hm-2w - 2 o'clock distortion s | 10/10/7 (assumed) | 6.6 | 6.2 | 9.8 | 3.1 | 4.44 / 4.44 / 41.9 / 0.61 | 5.84 / 41.80 / 2.61 |
| 74487 | boss hm-2w - 2 o'clock distortion c | 10/10/7 (assumed) | 6.4 | 4.8 | 9.9 | 4.4 | 5.01 / 5.01 / 41.2 / 0.60 | 6.67 / 41.30 / 3.58 |
| 78122 | Boss HM-2w CHAINSAW Custom | 10/10/10 (assumed) | 5.6 | 4.6 | 8.9 | 4.8 | 4.66 / 4.66 / 40.6 / 0.72 | 6.40 / 40.62 / 4.27 |
| 78122 | Boss HM-2w CHAINSAW Standard | 10/10/10 (assumed) | 4.9 | 5.6 | 9.9 | 3.8 | 4.24 / 4.24 / 41.8 / 0.86 | 5.74 / 41.61 / 3.69 |
| 88604 | Boss_Waza_HM-2_Standard | 10/10/10 (assumed) | 5.2 | 6.0 | 9.8 | 1.4 | 4.14 / 4.14 / 42.5 / 1.19 | 5.24 / 42.17 / 3.69 |
| 88604 | Boss_Waza_HM-2_Custom | 10/10/10 (assumed) | 5.7 | 4.9 | 8.5 | 2.6 | 4.59 / 4.59 / 41.0 / 0.98 | 5.90 / 41.02 / 4.32 |

**Mean free-fit LTAS error over the stock HM-2 models (tone 58569, 8 models): 4.04 dB RMS.
Mean constrained (raw knob map): 5.43 dB RMS. The model is NOT within 1.5 dB.**
(All 20 fits: free 3.5-5.1 dB; the same shape of error on every unit, see below, so it is the model and not one capture.)

## What is wrong with the model (structural, not tunable by the four knobs)

1. **No even harmonics.** `pedal.hm` clips symmetrically, so its H2/H4/H6 are absent (floor). Stock captures (the six first-measured) show
   H2 ~ -9 dB, H4 ~ -13 dB, H6 ~ -20 dB re fundamental (H3 -18, H5 -22, H7 -21) at -20..-3 dBFS and on all four
   fundamentals (strongest at 220/440 Hz); the model has H3 ~ -9 and H5 ~ -12. The harmonic term is ~40 dB RMS for every
   model and barely moves with the knobs. Fix: an asymmetric clipper (bias / different knees for the two polarities)
   in the first and/or second clipper, then re-measure H2 against these numbers.
2. **Drive range.** Free fits peg `distortion` at 10 for every label D-2..D-10 (fitted 8.8-10), only D-0 is lower
   (3.3 and 7.9 for the two D-0 captures; noisy; the knob map is far from identity). The real unit is already saturated at D-2; the model's first-stage range (6..46 dB) is too low at the
   low/mid settings. Fix (proposal, untested): raise the first-stage gain at low settings, e.g. 26..46 dB, and re-measure
   the D-0 points (the only labels with fits below 10, noisy: 3.3 and 7.9).
3. **Dynamics.** Captures: crest 7.6-10.1 dB, envelope spread 2.2-3.0 dB; model at D 10: crest ~9.5, spread 1.6 (0.7 at
   D 5). The real pedal is more squashed and has more envelope movement: dyn error 0.1-3.4 dB (free).

## Systematic residual (mean free-fit residual over stock HM-2, capture minus model, dB)

`63:+0.3 79:+2.6 100:+1.3 126:+0.1 158:-2.2 200:-2.9 251:-3.5 316:-1.3 398:+0.5 501:+2.7 631:+3.6 794:+3.0 1000:+0.9 1259:-0.5 1585:-0.4 1995:-0.1 2512:-0.5 3162:-1.6 3981:-5.0 5012:-8.2 6310:-3.8 7943:+2.8 10000:+12.1`

HM-2W, Torcher and Eyemaster means have the same shape (dip -3..-7 dB at 160-320 Hz, +3..+4 dB at 0.5-0.8 kHz,
-5..-8 dB at 4-5 kHz, +10..+12 dB at 10 kHz), so it is the model's EQ, not the units. Proposed corrections for the
dsp-engineer (fitted to the mean residual, RMS 3.78 dB before):

* **Minimal, gains only on the existing bands** (RMS after 2.50 dB): peak 100 Hz +0.06 dB Q 0.8; peak 1000 Hz +3.32 dB Q 1.2; peak 1500 Hz -1.87 dB Q 1.2; peak 4820.9 Hz -8.31 dB Q 2; lowPass 14000 Hz +0.00 dB Q 6.
  i.e. the 4.8 kHz presence peak +8 -> about 0 dB (delete it), 1 kHz gyrator +3.3 dB, 1.5 kHz gyrator -1.9 dB, 100 Hz
  gain unchanged; the 9 kHz LPF wants to go away (fit ran to its bound: 14 kHz, Q 6).
* **Free cascade, closer fit** (RMS after 1.36 dB): lowShelf 85.2 Hz +1.70 dB Q 0.7; peak 683.2 Hz +4.51 dB Q 2.37; peak 5475.2 Hz -11.96 dB Q 1.57; highShelf 9531.8 Hz +24.00 dB Q 0.7. The +24 dB high shelf is at the
  fit bound: the model has ~12 dB too little output at 10 kHz (the 8 kHz/5 kHz/6.5 kHz-4th-order LPF stack plus the 9 kHz
  LPF is too steep). Raise the post 6.5 kHz Butterworth to ~9-10 kHz and drop the 9 kHz LPF, then re-measure; caveat:
  absolute level at 10 kHz is ~-30 dB re peak, so part of that residual may be capture noise floor / aliasing.
* The 160-320 Hz dip (-3..-4 dB) and the 0.5-0.8 kHz bump (+2..+3 dB) are what the free cascade's 85 Hz low shelf
  (+1.7 dB) and 683 Hz peak (+4.5 dB, Q 2.4) address; the gains-only variant only half covers them.

## Knob map (stock HM-2, 8 captures: labelled -> fitted model param)

Fitted mean per label, then the monotone (isotonic, count-weighted) curve from `fits.json` `aggregate.knob_map[*].points[*].fitted_monotone`:

| knob | labelled -> fitted mean (n) | monotone map (labelled -> model param) | slope of line fit |
|---|---|---|---|
| distortion | 0 -> 5.6 (2); 2 -> 9.8 (2); 5 -> 10.0 (2); 10 -> 9.4 (2) | 0 -> 5.6, 2 -> 9.7, 5 -> 9.7, 10 -> 9.7 | 0.28 |
| low | 5 -> 2.3 (1); 7 -> 7.3 (2); 9 -> 7.4 (4); 10 -> 5.3 (1) | 5 -> 2.3, 7 -> 7.1, 9 -> 7.1, 10 -> 7.1 | 0.63 |
| high | 5 -> 3.3 (2); 8 -> 4.9 (1); 9 -> 7.4 (4); 10 -> 6.6 (1) | 5 -> 3.3, 8 -> 4.9, 9 -> 7.2, 10 -> 7.2 | 0.76 |
| level | 5 -> -0.5 (2); 7 -> -0.9 (5); 10 -> 5.0 (1) | 5 -> -0.8, 7 -> -0.8, 10 -> 5.0 | 1.17 |

**Rule for the 7c engineer:** do not use identity. Apply the monotone map as a piecewise-linear lookup (clamped outside
the sampled labels) from the real label to the `pedal.hm` param, as a provisional table valid for `modelVersion` 1 only:
distortion saturates at about 9.7 from D-2 up (the real pedal is already clipping hard at D-2; the model's low-gain range
is the problem, see structural defect 2), `high` is roughly 0.75 x label, `low` is weakly identified (one capture at L-5
and one at L-10; treat as flat 7 above L-7). `level` offsets are meaningless (reference level unknown); only the slope,
about 1.1 model units per labelled unit (the model's 3 dB/unit taper), is usable. Re-derive after the structural
fixes, since most of this map is the model compensating for its own gaps.

The constrained error (5.43 dB) is 1.4 dB worse than free, mostly because the EQ error is shared.

## Custom mode (HM-2W / Waza; Custom minus Standard, level aligned)

Mean LTAS shift by band (mean removed): `63:+3.1 79:+1.5 100:+1.5 126:+0.8 158:+0.6 200:+0.1 251:-0.6 316:-1.2 398:-1.7 501:-2.1 631:-2.5 794:-2.6 1000:-1.9 1259:-1.4 1585:-0.8 1995:-0.6 2512:-0.4 3162:-0.1 3981:+0.2 5012:+0.9 6310:+2.0 7943:+2.6 10000:+2.4`. Custom has overall +2.5..+2.8 dB more output and, relative to
the mids (flat at 0.6-1.6 kHz), about +3..+5 dB below 150 Hz and +3..+5 dB above 4 kHz: a scooped
low-shelf + high-shelf voicing, with crest 0.6-1.4 dB lower and envelope spread 0.05-0.5 dB lower.

| tone | models | mean dB | free-fit delta low/high/dist | explaining EQ (low shelf + high shelf) | d crest / d spread (dB) |
|---|---|---|---|---|---|
| 74487 | 652449 vs 652448 | +2.5 | +0.5 / -0.8 / +0.0 | lowShelf 104.2 Hz +2.77 dB Q 0.7; highShelf 6078.6 Hz +3.06 dB Q 0.7 | -0.57 / -0.05 |
| 74487 | 652451 vs 652450 | +2.6 | -0.2 / -1.3 / +0.1 | lowShelf 105.1 Hz +2.66 dB Q 0.7; highShelf 5692.4 Hz +2.87 dB Q 0.7 | -1.37 / -0.05 |
| 78122 | 680197 vs 680198 | +2.6 | +0.6 / -1.0 / -1.0 | lowShelf 78.2 Hz +3.84 dB Q 0.7; highShelf 6491.3 Hz +3.34 dB Q 0.7 | -0.78 / -0.41 |
| 88604 | 754123 vs 747049 | +2.8 | +0.5 / -1.1 / -1.3 | lowShelf 49.4 Hz +13.33 dB Q 0.7; highShelf 5453.6 Hz +2.65 dB Q 0.7 | -0.84 / -0.52 |

Proposed `pedal.hm` Custom mode deltas (all four pairs agree): output +2.5 dB; **low shelf +2.7..+3.8 dB at ~100 Hz
(Q 0.7)** (the Waza 13 dB / 49 Hz fit is out of the measurable range); **high shelf +2.7..+3.3 dB at ~6 kHz (Q 0.7)**;
no distortion change needed (free-fit delta 0.0 to -1.3, within fit noise); the free fits also move `high` by about
-1 (~-2 dB on the gyrators) and `low` by +0.5, which is the shelves above seen through the model's EQ. Harmonics (mean H2..H7 delta): about -1 (H2), +2.5 (H3), -2.5 (H4), +1.5..+3 (H5..H7): Custom is a bit odd-heavier.

## Throne Torcher / Eyemaster

* Torcher Maxed V2: free D 6.2, L 6.7, H 7.5 (LTAS 3.72 dB, the best of the metal pedals); constrained 3.86: the label
  is already close. Maxed 0 Gain: D 0.0, L 7.4, H 9.5 (4.70 dB): the Torcher at gain 0 is a low-gain voicing with
  more lows/highs than the HM-2 model. Residual vs the HM-2 mean: -1.5..+3.8 dB at 63-126 Hz (extra low end, +3.8 dB
  at 100-126 Hz), -2.5..-3 dB at 1.6-3 kHz (less upper-mid): no extra presence. Suggest "mod" voicing = HM-2 with
  low shelf +3 dB and a 2 kHz dip -2.5 dB; gain range unchanged.
* Eyemaster (60618, full distortion): LTAS 3.48 dB, D 6.6; (62523, Gain 12 o'clock): D 7.4, 4.08 dB. Residual same as
  stock (mean +4.1 dB at 79 Hz, +4.2 at 0.8 kHz): no structural difference beyond lower fitted drive; the
  Eyemaster is closer to the model than the Boss units (less saturated, so the missing even harmonics matter less).

## Preset proposals (not added)

`presets/modeled/hm_chainsaw.json` is **left unchanged**: for `Lv-10 L-10 H-10 D-10` the free fit wants L 5.3 H 6.6 D 10
but the labelled values (10/10/10) are only 0.8 dB worse in LTAS (4.33 vs 5.16) and the free numbers are mostly
compensating for the EQ/structure errors above; re-fit after the corrections and bump `modelVersion`.

Rule used: take the labelled knob values when the constrained fit is within 1 dB of the free fit (LTAS), otherwise the
rounded free-fit values; all valid for `modelVersion` 1 only (they partly compensate for the model's gaps). Level is a
loudness choice, set per use. Values are low/high/distortion.

| preset | proposed | reason |
|---|---|---|
| all tens | 10/10/10 | labelled is 0.8 dB worse than free (5.16 vs 4.33): within 1 dB |
| `L-9 H-9 D-10` | 9/9/10 | 4.57 vs 4.11 |
| `L-7 H-5 D-5` | 7/5/5 | 4.85 vs 4.09 (0.8 dB) |
| `L-5 H-8 D-2` | 2.5/5/9.5 | labelled 5/8/2 is 2.6 dB worse (6.32 vs 3.68): use the free fit |
| Custom all tens | 6.5/5/10, plus the Custom deltas above | 74487 free fit 6.7/5.0/10 (labelled 10/10/10 is 1.4 dB worse; `pedal.hm` has no Custom mode yet) |
| Custom 2 o'clock | 6.5/5/10 | 652451 free 6.4/4.8/9.9 (labelled 10/10/7 is 1.7 dB worse) |

After the structural fixes re-derive these with `sawblade-calibrate pedal-fit --merge` (about 90 minutes on 4 cores).

## Decisions / assumptions

* Level solved analytically (equivalent to a free level, see Method); shape error reported, range violations flagged.
* The harmonic weight is 0.5 and its ~40 dB constant error dominates the absolute cost but not the knob choice (the
  term is flat in the knobs).
* LTAS 63 Hz-10 kHz: 23 base-10 third-octave centres inside 60 Hz-12 kHz.
* EQ corrections are least-squares fits of RBJ magnitude responses (analysis only) to a 23-point residual; verify by
  re-running the fit after applying them.
