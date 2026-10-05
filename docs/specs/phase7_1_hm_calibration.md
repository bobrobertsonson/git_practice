# Phase 7.1: calibrate `pedal.hm` against real HM-2 captures

Phase 7 built `pedal.hm` from circuit analysis. This phase measures it against TONE3000
captures of real units and writes down how far off it is, per knob, so the model can be
corrected and so the factory presets land on real settings. No core code changes here:
the output is data (fits, plots, a knob map) and a fix list for the DSP engineer.

Owner: `match-engineer`. Reviewer audits before the lead accepts.

## Inputs (already in the capture cache, never commit them)

`/root/.cache/sawblade/captures/pool_manifest.json`, `tones[]` → `models[]`:

| tone | unit | models (knob labels in the model name) |
|---|---|---|
| 58569 | Boss HM-2 1985 MIJ | `Lv-10 L-10 H-10 D-10`, `Lv-5 L-9 H-9 D-10`, `Lv-7 L-9 H-9 D-2`, `Lv-7 L-7 H-5 D-5`, `Lv-5 L-9 H-9 D-5`, `Lv-7 L-7 H-5 D-0`, `Lv-7 L-5 H-8 D-2`, `Lv-7 L-9 H-9 D-0` (48 in total; pull more with `--max-models-per-tone 48 --force-tone 58569` if useful) |
| 74487 | Boss HM-2W | `maxed out s mode`, `maxed out c`, `2 o'clock distortion s`, `2 o'clock distortion c` |
| 78122 | Boss HM-2W | `CHAINSAW Custom`, `CHAINSAW Standard` |
| 88604 | Boss Waza HM-2 | `Custom`, `Standard` |
| 72990 | Throne Torcher (modded HM-2) | `Maxed V2`, `Maxed 0 Gain` |
| 60618 / 62523 | TC Eyemaster | one model each |

`Lv`=level, `L`=low, `H`=high, `D`=distortion, 0–10. `s`/`Standard` = HM-2W standard mode,
`c`/`Custom` = HM-2W custom mode.

## Method

1. **Probe signal** (`match/sawblade_match/calibrate/probe.py` or reuse what exists): a
   45 s mono 48 kHz WAV: 10 s of −20 dBFS exponential sine sweep 20 Hz–20 kHz, 5 s of
   stepped sines at 110/220/440/880 Hz at −30/−20/−10/−3 dBFS (for harmonic/clipping
   behaviour), then 30 s of real DI from `testdata/gatecreeper_cover/Guitar_L.wav`
   (bar-aligned excerpt). Same probe for every render.
2. **Reference renders:** probe through each capture model with `tonerender`, using a
   single-path preset with only that `nam` block (no cab, gate off, flat EQ). Store under
   the scratchpad, not the repo.
3. **Model renders:** probe through `pedal.hm` with candidate `{level, low, high, distortion}`
   via `tonerender` (or the pybind module if it is built; either is fine, say which).
4. **Fit:** for each reference, minimise over the four params:
   - LTAS error (1/3-octave, 60 Hz–12 kHz, dB RMS) on the DI segment,
   - plus the stepped-sine harmonic profile error (H2..H7 levels at each drive level,
     dB RMS),
   - plus a crest-factor / envelope term on the DI segment so compression depth matches.
   Use CMA-ES or Nelder–Mead from the matcher's existing optimiser code with 3 restarts;
   also run a *constrained* fit where the params are pinned to the capture's labelled knob
   values (only `level` free) so we see the error of the raw knob map.
5. **Report per model:** free-fit params, constrained-fit error, LTAS plot (reference vs
   free fit vs constrained), harmonic plot, and the residual LTAS curve.
6. **Aggregate:**
   - **Knob map:** fit a monotone curve per knob from labelled value → model param
     (e.g. real `D-5` ≈ model `distortion 6.3`). If the map is near identity, say so.
   - **Systematic residuals:** the mean residual LTAS across all stock-HM-2 fits is the
     model's EQ error; report it and propose concrete biquad corrections (frequency / gain
     / Q deltas for the low shelf, 1.0/1.5 kHz peaks, 4.8 kHz presence, 6.5 kHz LPF).
   - **Custom mode:** diff Standard vs Custom for 74487/78122/88604 (LTAS + harmonics).
     Describe the Custom change as parameter deltas on `pedal.hm` (gain, low shelf, high-mid
     peak) for the 7c engineer, with numbers.
   - **Throne Torcher / Eyemaster:** fit with `pedal.hm` and report what the residuals say
     (extra mids? presence? more gain?) — again as proposed parameter/structure deltas.
7. **Presets:** update `presets/modeled/hm_chainsaw.json` only if the fit for
   `Lv-10 L-10 H-10 D-10` says the current values are wrong; otherwise leave it. Propose
   (do not add) preset values for: all-tens, `L-9 H-9 D-10`, `L-7 H-5 D-5`, `L-5 H-8 D-2`,
   Custom all-tens, Custom 2 o'clock.

## Deliverables

- `match/sawblade_match/calibrate/pedal_fit.py` (+ CLI `sawblade-calibrate pedal-fit`), with
  tests on a synthetic reference (render `pedal.hm` at known params, fit must recover them
  within 0.3 on each knob).
- `docs/reports/phase7_1/` : plots (PNG, ≤ 200 kB each) and `fits.json`.
- `docs/specs/phase7_1_hm_calibration_REPORT.md`: tables above, the knob map, the proposed
  EQ corrections, the Custom-mode deltas, and the preset proposals.
- Nothing from the capture cache is committed. Rendered audio stays in the scratchpad.

## Acceptance

- Synthetic self-fit test passes; `pytest match/` green; `ctest` untouched.
- Every listed model has a free fit and a constrained fit with numbers.
- The report states the mean free-fit LTAS error over stock HM-2 models and whether the
  model is within 1.5 dB RMS; if not, the correction list is concrete enough for the
  dsp-engineer to apply without re-measuring.
- Reviewer ACCEPT.
