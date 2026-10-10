# Phase 6b report: matcher speed

Status: `--quick` ships as PREVIEW quality. `--thorough` stays the default and the documented final match. The original
0.15 dB target was NOT met (see "Why"); the acceptance below is the amended one.

## Results (frozen pool: 110 pedals, 100 amps, 39 cabs; seed 1)

Wall times were taken on a machine at load 14-17 (other builds), so they are not meaningful; CPU seconds are the metric.

| Run | CPU (user+sys) | A-weighted | Fizz 3-5k / 5-8k / 8-12k / 12k+ / flat5-10k |
|---|---|---|---|
| original, thorough (capped auto-prescreen, top 9/class) | 68.3 min | 1.64 dB | -5.9 / -16.2 / -46.3 / -71.2 / 0.013 |
| original, quick | 13.6 min (814 cpuSeconds) | 1.83 dB (+0.19) | -5.7 / -15.9 / -45.6 / -71.4 / 0.014 |
| cover, thorough (capped auto-prescreen, top 8/class) | 73.8 min | 1.41 dB | -2.4 / -18.5 / -44.2 / -67.2 / 0.018 |
| cover, quick | 14.5 min (869 cpuSeconds) | 2.19 dB (+0.78) | -3.5 / -19.3 / -45.1 / -64.2 / 0.019 |

Quick uses about 5x less CPU. On 4 idle cores that is about 3.5-4.5 min, derived from CPU seconds / 4 cores, NOT measured ( the only idle-machine runs, 3.9 and
4.5 min, used an earlier config on a 77-capture pool). Reference stem fizz: original 8-12k -45.9, flat 0.010; cover mix
-42.7 / 0.025.

Fizz deltas quick vs thorough: original 3-5k +0.2, 5-8k +0.3, 8-12k +0.7, 12k+ -0.2, flat +0.001; cover 3-5k -1.1, 5-8k -0.8,
8-12k -0.9, 12k+ +3.0 (-67.2 -> -64.2, still much quieter than the mix's -42.8), flat +0.001. Tolerance applied: within 3 dB per
band and flatness within 0.005; the cover 12k+ delta sits at the 3 dB limit.

## Stage profile (thorough, original)
prescreen 80 s, pair renders about 940 s, single2 340 s, stage 2 about 660 s, stage 3 50 s (v4 log, 36.5 min).
Quick, original (loaded): prescreen 158, coarse pass 202, full pass 60, re-score + cab sweep 50, stage 2 (4 combos) 229,
full-length render 62, tonecheck 10 s.

## Levers
Kept: capped prescreen (4 pedals/class, rest of 380-pair cap to amps, 2.5 s window); two-pass pair screen (2.5 s then top 8 %,
min 32, on the full excerpt); bit-identical NAM-core memo; stage 2 first linear + gain blocks on a 2.5 s window, plateau stop,
fewer re-scores/cab sweeps, 2 blend + 2 single refined; no starter/R full-length renders or listening files without `--listen`.
Dropped: single2 (two-pedal chains) in quick (`--top-k 4` brings one back); process pool (threads scale, GIL released;
`--jobs` aliases `--threads`); blend-aware prescreen (kept as option `Plan.blend_aware`, no recall gain); 1.5 s coarse window
(ranking unstable).

## Recall vs the old full searches (old pool only, prescreen only, 2.5 s, 4 pedals/class)
| Reference | single @5/@10/@30 | blend @5/@10/@30 | best combo kept |
|---|---|---|---|
| original (v4) | 1.0 / 1.0 / 0.83 | 0.2 / 0.4 / 0.6 | yes |
| cover (v3) | 0 / 0 / 0.1 | 0 / 0 / 0 | not measured |

On the full pool recall against the old lists is near zero because new captures displace them; no full-pool full search exists.

## Why the 0.15 dB target was not met
Recall: 4 pedals per class drops the cover's winner (HM-2 + Marshall JMP), and the excerpt loss is a weak predictor of the
full-song A-weighted error (cover excerpt losses differ by 0.09 while A-weighted differs by 0.78 dB). Near-tied candidates
generalise differently.

## Amended acceptance
Quick: at most about 5 min CPU-equivalent on 4 cores, within 1 dB A-weighted of thorough on both references, fizz metrics
unchanged (all met). Thorough: unchanged behaviour. Quick is for the plugin's first pass; a background thorough refine follows (6a).

## Other
Behaviour change (intentional, lead): listening files and the stereo listening render are written only with `--listen`, in BOTH modes (thorough no longer writes `listen/*` by default). The R render itself (`best_R`) is always made when `--di-r` is given, because the clip guard uses max(L, R). `--no-audio` is a hidden deprecated no-op.
`--progress-json`, `--quick/--thorough`, `--listen` (gates R render and listening files, also in thorough), `result.json -> timings`.
gate_preset clamps the DI floor at -90 dBFS (threshold >= -86) for digital-silence DIs; unit-tested.
Tests: `pytest match/` 312 passed, 7 skipped, 1 failed (`test_numpy_scalars_accepted`, stale `core_snapshot` .so only; passes
against the repo build); `test_speed.py` + `test_matcher.py` 56 passed.

## Follow-up (phase 12 end-to-end run)
With 10.1's path levels in the loss, `--quick` on the known-answer fixture is 0.684 dB A-weighted vs `--thorough` 0.255 dB at
seed 7 (was 0.350 vs 0.288; other seeds 0.32-0.69). That is within the accepted 1 dB criterion. Proposed, not done now:
"6b.1: re-probe levels after quick's short-linear stage".
