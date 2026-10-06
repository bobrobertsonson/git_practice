# v0.4a pedal accuracy baseline

**Status: PENDING USER RUN** for hm, hmx, eye, muff, ts: no capture fits exist for them yet (TONE3000 needs the user's OAuth login and the DI is not in the repo). No capture numbers appear below for those pedals; follow `docs/runbooks/v0_4a_pedal_accuracy.md`.

Metrics: LTAS = 1/3-octave shape error (60 Hz-12 kHz, offset removed), dB RMS. `harm` = `harm_rms_db` (H2..H7 re fundamental, 16 stepped sines, clamped at the fixed floor), with even/odd parts in brackets. `dyn` = crest + envelope-spread error, dB. Free = knobs searched; constrained = knobs pinned to the capture's label (`labelled` = read from its name, `assumed` = from the targets manifest).

## Known-answer results (A.1)

Probe `1 s sweep + 3.2 s stepped sines + 4 s DI (di_riff.wav)`, harmonic floor -40.0 dB, free-fit budget {'restarts': 3, 'popsize': 8, 'generations': 14, 'refine_generations': 30}. The reference is the pedal itself rendered at the true params, so the error of a perfect fit is 0; metrics are LTAS shape (dB) / `harm_rms_db` (even/odd) / dynamics (dB).

_Produced in the cloud container, where scipy and soundfile cannot be installed: numpy stand-ins replaced scipy.signal.welch/freqz and soundfile (LTAS may differ from a real scipy run in the last digit; the harmonic, lag and dynamics terms do not use them). CI re-runs the same checks at a smaller budget; re-generate with `sawblade-calibrate pedal-fit --known-answers` on a full install._

| pedal | version | true knobs | constrained (true params) LTAS / harm / dyn | free fit LTAS / harm / dyn | max knob error | unrecovered knobs |
|---|---|---|---|---|---|---|
| hm | 3 | low 7, high 3, distortion 8, level 6.5 | 0.000 / 0.000 / 0.000 | 0.003 / 0.009 / 0.002 | 0.02 | none |
| hmx | 1 | low 6, lowMid 3, highMid 7, high 4, distortion 8, presence 6, level 6.5 | 0.000 / 0.000 / 0.000 | 0.043 / 0.068 / 0.007 | 0.20 | none |
| eye | 1 | gain 7, level 6.5 | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 / 0.000 | 0.00 | none |
| muff | 1 | sustain 7, tone 4, scoop 6, voice 6, volume 6.5 | 0.000 / 0.000 / 0.000 | 0.018 / 0.010 / 0.029 | 0.18 | none |
| ts | 1 | drive 7, tone 3, level 6.5 | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 / 0.000 | 0.00 | none |

## Captures

### hm

PENDING USER RUN: no fits file.

### hmx

PENDING USER RUN: no fits file.

### eye

PENDING USER RUN: no fits file.

### muff

PENDING USER RUN: no fits file.

### ts

PENDING USER RUN: no fits file.

## Verdicts

* hm: PENDING USER RUN (no capture fits).
* hmx: PENDING USER RUN (no capture fits).
* eye: PENDING USER RUN (no capture fits).
* muff: PENDING USER RUN (no capture fits).
* ts: PENDING USER RUN (no capture fits).

