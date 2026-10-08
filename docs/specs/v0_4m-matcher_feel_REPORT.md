# v0.4M — matcher matches the feel: report

Spec: `docs/specs/v0_4m-matcher_feel.md` (Tasks A–E, B2–B4). Lead-pinned definitions: `docs/specs/v0_4m-tasks.md`.
Branch: `claude/sawblade-v0_4m-matcher-feel` (base `claude/sawblade-plugin-setup-7k0b8q` at 4d580a2 — v0.4 B–E — merged in). CI (GitHub Actions) is the validation of record. Local verification on real scipy 1.17.1 (numpy 2.4.6, `match/.venv`, core built with
that Python); numbers labelled "shim" below were taken earlier against a scipy stand-in (pypi was blocked) and are indicative only.

Status: **Tasks A–C, B2.1–B2.4, B3, B4, D.1 and E accepted by the reviewer and merged. CI of record: run 226 on
0d16fdb — all jobs green (python, linux-gcc + pluginval, linux-clang -Werror, macOS arm64 + auval + pluginval AU/VST3).**
The base (v0.4 B–E, 4d580a2; then v0.4 pedals + TONE3000-in-DAW hotfix, 743bf1d) is merged in. Validation on the real Bloodbath audio (D.2/D.3) is the user's: Mac
commands below.

## What changed, in one paragraph per suspect

1. **The reference basis was wrong for clean amp tracks (likely the main cause of "fizzy").** In the Bloodbath runs,
   `--matched mono` on a clean amp WAV with no stems fell back to the full-mix basis: LTAS above 4.5 kHz was only a
   one-sided ceiling, the STFT term stopped at 4.5 kHz and no texture term ran — the matcher never fitted the top end.
   Now `--matched mono` implies a clean reference (two-sided LTAS to 8 kHz, fizz terms on); `--ref-clean/--ref-mix`
   override it. The run log says which basis it used.
2. **Feel terms (Task A).** Tightness (60-250 Hz per-note t12 + sustain, palm-muted chugs from the DI), fizz (5-12 kHz
   ratio, flatness, HF envelope modulation, compared as distributions), polish (spectral flux, crest, inter-note floor).
   Off, with a recorded reason, wherever the reference can't support them (full mixes; floor needs a clean track).
3. **Search space (Task B, B2, B4).** Tight boost (modeled `pedal.ts`) in front of the amp; post-cab HP/LP with 12/24
   dB/oct; every cab swept, plus an analytic screen of the user's IR library (B3, thousands of IRs in ~1 s) and two-IR
   blends; gate threshold/hold/release/range matched to the reference's inter-note floor; studio-processing detection
   with an optional fast bus comp; pre-EQ before the drive.
4. **Honest A/B (Task C).** `listen/ref.wav` and `listen/render.wav` are BS.1770 loudness-matched, time-aligned, same
   30 s section, float WAV.
5. **Export notes (Task E).** Every NAM export lists what is not in the model (gate, cab IR for no-cab, post EQ, bus
   comp) with hardware settings and where it goes around the loader pedal.

## Feature definitions and weights (as shipped)

See the `feel.py` and `loss.py` docstrings for the exact definitions. Weights: tight 0.25, fizz 0.25, polish 0.125, each
normalised sub-term Huber-softened (delta = one normaliser: 20 ms t12, 3 dB sustain, 1.5 dB hfRatio W1, 0.03 flatness
W1, 0.1 HF-mod W1, 0.5 dB flux, 1.5 dB crest, 6 dB floor). Stage 2's first linear block is LTAS-only; feel enters from
the gain block on. Rationale: the first-pass 0.5/0.5/0.25 without Huber broke the blend known-answer CI test (0.753 vs
0.5 dB) because noise-level texture differences pulled the search off the spectral fit on a wrong combo; with the fix CI
is green. D.1 compared three weight sets (0.125/0.25/0.5 scales) on the synthetic and 6b fixtures: differences were
within search noise, so the weights stay until the user's A/B says otherwise.

## Synthetic results (D.1, shim, seed 1 asserted in CI)

Hidden chain = fixture amp + cab + `pedal.ts` boost + post HP/LP 24 dB/oct + gate above the DI floor; DI with synthetic
gaps at a −70 dBFS floor. Tolerances: A-weighted ≤ 0.5 dB, |Δt12| ≤ 10 ms, |Δsustain| ≤ 1.5 dB, hfRatio W1 ≤ 1.0 dB,
hfFlat W1 ≤ 0.02, flux W1 ≤ 0.3 dB, floor ≤ 3 dB.

| seed | A-wt dB | Δt12 ms | Δsus dB | hfRatio | hfFlat | flux | floor dB | pass |
|---|---|---|---|---|---|---|---|---|
| 1 | 0.224 | 0.33 | 0.29 | 0.34 | 0.008 | 0.072 | 0.41 | yes |
| 2 | 0.457 | 11.67 | 0.98 | 0.30 | 0.002 | 0.055 | 0.01 | no (t12) |
| 3 | 0.198 | 0.85 | 0.55 | 0.17 | 0.018 | 0.014 | 0.05 | yes |

Known limitation (accepted): seed 2 picks a slightly over-strong HPF (124 Hz/12 dB vs 86 Hz/24 dB) — the tightness term
is one-sided by design ("floppy" costs double) and A-weighting barely sees < 125 Hz. Real-audio check: the user's A/B.

Suspect contribution on the synthetic case (each suspect switched off with `--ablate`):

| variant | A-wt dB | Δt12 ms | Δsus dB | hfRatio | floor dB |
|---|---|---|---|---|---|
| full | 0.49 | 2.6 | 1.33 | 0.81 | 0.13 |
| no feel | 0.44 | 0.0 | 0.94 | 0.61 | 0.01 |
| no boost | 2.36 | 49.2 | 6.99 | 2.47 | 7.25 |
| no filters | 0.50 | 3.5 | 1.31 | 0.56 | 0.13 |

(Pre-fix table; the boost is the only suspect that matters on a chain built from the same captures. The feel term can't
show its value when the reference is a render of the same kind of chain — that's what the Bloodbath runs test.)

## Reviewer verdicts

| Task | Verdict | CI |
|---|---|---|
| A feel terms | ACCEPT after 3 REVISE rounds (full-mix gating, STFT cache, Huber + staging after the CI failure) | green (run 166) |
| C listening | ACCEPT (+2 should-fixes applied) | green |
| E export notes | ACCEPT after 1 REVISE (gate default, no non-schema keys) | green |
| B search space | ACCEPT after adaptations (export accepts modeled pedals; gate sweep honours clean-reference rule); combined-code fix (post.hp as post-CMA grid) | green (run 170) |
| B2.1 core hook | ACCEPT (gcc + clang 356/356) | green (run 170) |
| B3 IR library | ACCEPT after 1 REVISE (concurrent-write race on duplicate IRs; relative paths) | green (run 172) |
| D.1 + search fixes | ACCEPT (joint HP/LP × slope with re-polish, boost level fixed, gate tolerance, pedal Occam) | green (run 185) |
| B2.1 matcher, B2.3, B4 | ACCEPT after 1 REVISE (pre-EQ moved after stage 2 on the refined winner with a re-fit keep rule ≥ 0.05; it had picked a spurious HPF on a plain chain); merged with B3 + D.1; canonical IR-pair orientation | green (run 221) |
| CI fixes after the v0.4M merge (run 195/203) | ACCEPT: studio eqd judged only on residual the post EQ can't absorb; widened post EQ must beat an equal-budget ±6 dB re-fit; pre-EQ confirmation re-fit keeps feel in every block (a real bug the spy test caught) | green (run 221) |
| Task E follow-up: dropped bus comp in export notes (found by the v0.4 lead) | ACCEPT after 1 REVISE: `sawblade-export --notes-preset` (original rig; nocab only; repeated on resume; refuses a different rig) + plugin `ExportGlue.cpp` writes `<jobs>/inputs/<hash16>.notes_preset.json` and passes it for no-cab DROP COMP exports and resumes | green (run 221) |

## Open

- Studio detector: CI under real scipy caught a false `eqd` on the plain chain (an unconverged fit's smooth residual);
  fixed by judging only the residual the post EQ can't absorb within ±6 dB, and the ±9 dB stage must beat an
  equal-budget ±6 dB re-fit (be378c0). Remaining (harmless) risk: a smooth mismatch < 100 Hz or > 8 kHz can still set
  `studio.eqd`; the stage then runs but can't make the preset worse. Follow-up: restrict the judged bands to the post
  EQ's reach.
- Export resume edge case (deferred, fails safe): if the user toggles the bus comp between cancelling and resuming a
  drop-comp export, the panel offers RESUME and Python refuses it (notes-preset mismatch). Fix: record "used a notes
  preset" on the job snapshot and decide the resume flag from the run, not the rig.
- Bright-DI widening threshold (`diTilt > −1.5 dB/oct`) fires on the fixture DI; rebase on the user's DIs (printed by
  every run).
- TONE3000 `gears` value for IR tones ("cab" vs "ir") unverified against the live API (one constant, `IR_GEAR`).
- Follow-ups noted, not done: `plan.py` gate-default mismatch (`gate: {}` is enabled in the core); plugin IR preview
  does not model irMix offset/invert; `_refine_native` uses the left channel for a stereo `--matched mono` reference.

## Mac validation commands

Run in Terminal on the Mac (your shell is zsh, so the script is started with `bash`). Two lines:

```
cd ~/sawblade && git fetch origin claude/sawblade-v0_4m-matcher-feel && git checkout claude/sawblade-v0_4m-matcher-feel && scripts/mac_update.sh --no-models
bash scripts/run_v04m_validation.sh            # add --quick-only first if you want a few-minute smoke test
```

The script rebuilds the Python core, installs the matcher, indexes your IR folder, runs the left-side quick timing run and the thorough
HM2 / UBR runs (with `--listen`), the seven `--ablate` quick runs, and the right-side held-out runs when the R files are found (in the
Bloodbath folder, else searched under `~/Desktop/NailTheMix/...`; otherwise it says so and skips them). It then prints the step-7
summary and the `pre-EQ: DI tilt` lines, and the `open ".../listen"` commands for the A/B folders. Everything is logged to
`~/sawblade-work/v04m_validation.log`; an interrupted run resumes (finished runs are skipped, `--force` redoes them); `--help` lists the
flags for other file names or folders. Paste the printed summary and tilt lines back to the lead and listen to the `listen/` folders.

### Manual equivalent

The same steps by hand (the script is exactly this, with the paths as defaults). Paste the printed summaries (step 7) back to the lead;
listen to step 4's `listen/` files.

### 1. Update (once)

```
cd ~/sawblade
git fetch origin claude/sawblade-v0_4m-matcher-feel
git checkout claude/sawblade-v0_4m-matcher-feel
scripts/mac_update.sh --no-models
match/.venv/bin/pip install -e match
# the Python core module must be rebuilt (the core gained irMix offset/invert keys)
cmake -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PYTHON=ON -DSAWBLADE_BUILD_TESTS=OFF \
      -DPython_EXECUTABLE=$PWD/match/.venv/bin/python
cmake --build build-py
export SAWBLADE_CORE_DIR=$PWD/build-py/python
```

### 2. Set the paths once per Terminal window (edit the Bloodbath file names to match your NTM folder)

```
BB="$HOME/path/to/Bloodbath Zombie Inferno"            # NTM session folder
L_DI="$BB/<left rhythm DI>.wav";  L_HM2="$BB/<left HM2 AMP>.wav";  L_UBR="$BB/<left UBR AMP>.wav"
R_DI="$BB/<right rhythm DI>.wav"; R_HM2="$BB/<right HM2 AMP>.wav"; R_UBR="$BB/<right UBR AMP>.wav"
IRS="/Users/notsch/Music/Studio_Notsch/_IRs/Guitar_Cabs"
POOL="$HOME/.cache/sawblade/captures/pool_manifest.json"
OUT="$HOME/.cache/sawblade/match_runs/v04m"; mkdir -p "$OUT"
M="match/.venv/bin/sawblade-match"
COMMON=(--pool "$POOL" --matched mono --offset-ms 0 --ir-dir "$IRS" --listen --trace-tones 57492,79751)
```

### 3. Index your IR library (one time; first run may take a minute, later runs are seconds)

```
match/.venv/bin/python -m sawblade_match.matcher.irlib --scan "$IRS" --json "$OUT/ir_scan.json"
```
Check the printout: accepted / unique counts, rejected files by reason, **near-duplicate pairs** (are any of them
genuinely different mics? say so), truncated count, tag coverage.

Optional, wider TONE3000 IR set (all models of each IR tone): `match/.venv/bin/sawblade-t3k pull --gear ir`
(add `--ir-search "V30"` etc. for more cab families). If it finds no IR tones, tell the lead (one API constant to flip).

### 4. Main runs: left side, quick timing + thorough HM2 and UBR (the A/B files)

```
time $M --di "$L_DI" --ref "$L_HM2" "${COMMON[@]}" --quick    --out "$OUT/L_hm2_quick"
     $M --di "$L_DI" --ref "$L_HM2" "${COMMON[@]}" --thorough --out "$OUT/L_hm2"
     $M --di "$L_DI" --ref "$L_UBR" "${COMMON[@]}" --thorough --out "$OUT/L_ubr"
```
A/B (both level-matched, same 30 s, time-aligned): `open "$OUT/L_hm2/listen"` and `open "$OUT/L_ubr/listen"` —
compare `ref.wav` vs `render.wav` (and `before.wav`). Does it still sound floppy / fizzy / less pro? What is still off?

### 5. What each suspect buys (quick runs, HM2, one suspect switched off each)

```
for a in feel boost filters irsweep irblend studio preeq; do
  $M --di "$L_DI" --ref "$L_HM2" "${COMMON[@]}" --quick --ablate $a --out "$OUT/L_hm2_quick_no_$a"
done
```

### 6. Held-out check: right side

```
$M --di "$R_DI" --ref "$R_HM2" "${COMMON[@]}" --thorough --out "$OUT/R_hm2"
$M --di "$R_DI" --ref "$R_UBR" "${COMMON[@]}" --thorough --out "$OUT/R_ubr"
```

### 7. Print the numbers to send back

```
match/.venv/bin/python - "$OUT" <<'PY'
import json, sys, pathlib
for d in sorted(pathlib.Path(sys.argv[1]).iterdir()):
    f = d / "result.json"
    if not f.exists(): continue
    r = json.loads(f.read_text())
    def g(*ks):
        v = r
        for k in ks:
            v = v.get(k) if isinstance(v, dict) else None
        return v
    b = r.get("best", {}); bd = b.get("breakdown", {}) or {}; ft = bd.get("feelTerms") or {}
    print(f"== {d.name}  wall {r.get('wallSeconds', 0)/60:.1f} min")
    print("  A-weighted dB:", (r.get("after") or [{}])[0].get("aWeightedErrorDb"), "| loss", bd.get("total"),
          "ltas", bd.get("ltas"), "feel", bd.get("feel"))
    print("  feel: tight", ft.get("tight"), "fizz", ft.get("fizz"), "polish", ft.get("polish"), "dropped", ft.get("dropped"))
    print("  chain:", b.get("topology"), {k: (v or {}).get("title") for k, v in (b.get("captures") or {}).items()})
    print("  boost won:", g("tightBoost", "won"), "| IR pair won:", g("irBlend", "won"), "| IR winner:", g("irPool", "winner"))
    print("  pre-EQ:", g("preEq", "chosen"), "| studio:", {k: g("studio", k) for k in ("compressed", "eqd", "busCompUsed")})
    print("  gate:", r.get("gateFinal"), "| post filters:", r.get("postFilters"))
    print("  listening gain dB:", g("listening", "gainDb"), "| IR pool:", {k: g("irPool", k) for k in ("total", "screened", "prefiltered")})
    tc = r.get("tonecheck", {}).get("best_L", {})
    print("  guardrails:", [(x.get("id"), x.get("status")) for x in tc.get("rules", []) if x.get("status") != "pass"])
    if r.get("trace"): print("  trace:", {k: (v or {}).get("why") for k, v in r["trace"].items()})
PY
```
Also paste the run-log lines that start with `pre-EQ: DI tilt` (one per run): they calibrate the guitar-difference
thresholds.

Before (first known-answer run, `--quick`): HM2 A-weighted 2.72 dB, UBR 3.45 dB; `gap_noise` −10.8 / −11.0 (fail),
`fizz_texture` fail on both; chosen IR "V30 3 SM58 6" both times; neither Überschall capture won UBR.

## Task H (matcher side)

- **Gate floor (H.1):** the gate is set from the core's `peak_floor_db` (92.5th percentile of the gate's own peak envelope) over the
  DI's gap regions, reduced to their stationary-noise frames (10 ms RMS within 6 dB of the gaps' 10th percentile: `gap_regions`
  only needs 10 ms RMS under -50 dBFS, so decaying ring-out tails land in the mask and inflated the floor: on the synthetic gap DI
  with true noise at -70 dBFS RMS the floor was -51.7 dBFS, now -62.72 against -62.8 for the noise alone). Without real gaps (the
  user's -49.5 dBFS RMS bed never stays under -50 dBFS for 120 ms) it falls back to the quietest 20 % of 20 ms frames when
  something plays over the bed, and to the whole DI for a steady signal. result.json `gateFloor: {rmsDb, peakDb, source}`.
  Default cell: open = peakFloor + 10 dB, hysteresis 6 (close = + 4); sweep grid {6, 8, 10, 12, 16, 20, 24, 28} dB re peakFloor.
  For a -49.5 dBFS RMS floor: old open -45.50 / close -51.50 dBFS, new open -32.27 / close -38.27 (peak floor -42.27); noise alone
  (white, pink) stays closed > 95 % after 0.5 s; a decaying note is not attenuated while it is > 12 dB above the floor (the
  attenuation starts about 3.1 dB above the floor on the record gate, 2.8 dB on the live gate).
  Live set (core, ratio 4 / range -40, open = floor + 10): noise alone is attenuated by a median of about -10.8 to -11.1 dB at
  -70, -65 and -49.5 dBFS floors (an expander, not a closed gate). Sustain loss of a note whose envelope sits 4 / 2 / 0 dB above the
  floor: new ratio 4 / -40 gives 0.0 / 5.1 / 11.1 dB, the old ratio 2 / -24 gave 0.0 / 1.7 / 3.7 dB.
  Core, follower: it qualifies only frames below the estimate + 20 dB and starts at -70 dBFS, so it learns a -49.5 dBFS RMS floor
  (peak statistic about -42 dBFS) only after about 25 s (the 10 s leak, +1 dB/s). A per-device seed is parked for v0.8: a VST3/AU
  host gives no device identity.
  The D.1 feel known-answer case keeps its hidden gate at peak floor + 16 dB (the default is + 10).
- **Full-DI gate sweep (H.2):** with < 100 ms of gaps in the excerpt the sweep takes up to 6 gap windows (longest, each <= 1 s,
  0.5 s pre-roll) from the full DI and the reference floor at the same windows; `gateSweep.gapSource: "excerpt" | "fullDi"`.
- **Topology margin (H.3):** `BLEND_OCCAM_DB = 0.25` (a single beats the best blend when within 0.25 dB; single2 and the boost keep
  0.10). result.json `topology: {bestSingle, bestBlend, deltaPct, determined}` (determined = |delta| >= 10 % of the smaller
  loss; `BLEND_OCCAM_DB = 0.25` is the named constant); `--topology single|blend|auto` (single = single and single2). The script adds `L_blend_quick_forced` / `L_blend_forced`.
- **Pedal-Occam noise (CI run 280):** the D.1 feel known-answer test (`test_known_answer_feel.py:78`, capture identity) failed on CI only:
  the matcher kept a loud-linear pedal (3/6) in front of the right amp (2/4). That pedal is almost a pure gain stage: it ties its
  pedal-less partner exactly at stage 1 (screen loss 2.041 for both), so stage 2 compares two fits of near-equivalent chains, and
  one stage-2 fit varies by ~0.2 dB of loss with the seed, far more than `PEDAL_OCCAM_DB` (0.05). Measured on real scipy 1.17.1, same
  case, 8 seeds each (stage-2 loss): bare partner 0.319 / 0.566 / 0.516 / 0.556 / 0.532 / 0.437 / 0.299 / 0.401; pedal variant
  0.565 / 0.623 / 0.635 / 0.515 / 0.817 / 0.519 / 0.519 / 0.430. Before, the refine seed was the position in the refine list
  (`seed * 1000 + 10 * position`), so when platform float noise swapped the two tied candidates their seeds swapped. Reproduced on
  21a9d03 by swapping the two (`work[0], work[1]`): pedal 0.487 vs bare 0.554 -> the pedal passes the margin, the test fails at
  line 78 with the amp right and `pedal == 3/6`. With the original order the same code passes (4/4 locally), which is why it was
  invisible off CI. Fix, two parts:
  (a) `refine_seed(seed, combo)` = `seed * 1000 + 10 * (crc32(pair_key) % REFINE_SEED_SLOTS)`, `REFINE_SEED_SLOTS = 50`, so a
  candidate's fit no longer depends on the order of the refine list. Seed layout per `seed * 1000`: refine 0..490 (+4 inside
  `refine_combo`), trace 500, confirmation `CONFIRM_SEED_BASE = 600` + 10 j, cab sweeps 900 + n, final 950, studio 970, pre-EQ
  980 + n; a unit test pins that the ranges do not overlap.
  (b) a pedal variant that would pass the rule is accepted only after its partner (same amp, same boost flag; `choose` now compares
  against that partner first) was refitted `OCCAM_CONFIRM_STARTS = 2` more times (`confirm_seed(seed, j)`) and still loses by the
  margin; the best partner fit is kept (`confirm_partner`). Why (b) on top of (a): (a) removes the order dependence but not the
  noise, and a fit of the same seed can differ across CPUs / libm. From the 8-seed tables, a single bare fit vs a single pedal fit
  lets the pedal win by more than 0.05 in 7 of 72 pairs (~10 %); against the best of three bare fits in 4 of 504 (~1 %). A pedal the
  chain needs (5.1 -> 0.5) passes easily. Cost: nothing when the pedal does not pass the rule (the case on the stable seeds: pedal
  0.6395 vs bare 0.6426); when it does, 2 extra stage-2 fits of the partner, measured with the margin forced open: known-answer run
  69 s -> 103 s (+34 s, ~17 s per fit). `pedalOccam.partners` in result.json records pedal / partner losses, the extra fits and
  whether the pedal is still justified. Local verification on real scipy 1.17.1: the test fails on 21a9d03 with the swapped order
  and passes with the fix; unit tests with fixed losses (`test_confirm_partner_with_fixed_losses`,
  `test_pedal_occam_uses_the_same_amp_same_boost_bare_partner`, `test_refine_seed_is_stable_per_candidate_not_per_position`) do not
  involve scipy. The CI divergence itself (different CPU, Python 3.11) was not reproduced without forcing the order.
- **Shared Occam confirmation (pedal, single2, blend):** the same seed noise makes every simpler-vs-complex choice partly a coin flip.
  `test_single_path_known_answer_is_found_as_single` failed on real scipy at 21a9d03 and dc0fa07 for that reason without any pedal
  involved: single2 0.2035 vs the single 0.3243 (gap 0.121 against `OCCAM_DB` 0.10). One helper now serves all three decisions
  (`confirm_partner`, `occam_contested`, `simpler_partner`, `occam_margin` in `run.py`). A complex candidate that beats its simpler
  partner by its margin but by less than margin + `OCCAM_NOISE_DB` (0.20) is "contested": the partner is refitted
  `OCCAM_CONFIRM_STARTS` (2; old name `PEDAL_CONFIRM_STARTS` kept) more times with other seeds and its best fit is kept before `choose`
  decides. A win of margin + noise or more is robust and costs nothing; a smaller win already loses (a refit can only help the
  partner). Margins: pedal `PEDAL_OCCAM_DB` 0.05, single2 `OCCAM_DB` 0.10, blend vs a single `BLEND_OCCAM_DB` 0.25 (vs a single2: 0.10).
  Partners: pedal -> the pedal-less single of the same amp, preferring the same boost flag and falling back to the same amp with the
  other flag exactly like `choose` (a boosted pedal variant against a plain bare candidate is confirmed); single2 -> the best single
  with the same amp, preferring one that keeps one of its two pedals (the chain with one block removed, the actual Occam comparison)
  and the same boost flag; blend -> the best single-path candidate (single or single2). Decisions are judged on a snapshot of the
  stage-2 fits and the refits are cached per partner, so two variants sharing a partner do not repeat the fits and the
  `pedalOccam.partners` records (decision, complex, partner, margin, losses, extraFits, partnerLossAfter, stillJustified) do not
  depend on order. Confirmation fits count in the progress bar and in `timings.stage2PerCombo` (`"confirm": true`).
  `OCCAM_NOISE_DB = 0.20` is about 2 sd of one stage-2 fit (sd 0.06 to 0.11 dB across seeds measured below) and covers the mean 0.11 /
  maximum 0.27 dB by which the best of three fits is below one fit. Seeds: the stage seeds are named constants (`refine.SEED_TRACE`
  500, `SEED_SWEEP` 900 + n, `SEED_FINAL` 950, `SEED_STUDIO` 970, `SEED_PREEQ` 980 + n; refine 0..490, confirmation 600 + 10 j),
  a test checks that the ranges are disjoint under the stated bounds on n.
  Measured spurious-win rate (real scipy 1.17.1, fixture seeds shifted, 9 fits of each chain; the rate is the share of
  (complex fit, simpler fit) pairs in which the complex chain wins by more than its margin although it is not truly better, vs the
  same with the simpler fit replaced by the best of three of its fits):

  | decision | fixture (truth) | one fit vs one fit | vs best of 3 |
  |---|---|---|---|
  | pedal vs none | feel known answer (no pedal) | 7 of 72 = 10 % | 4 of 504 = 0.8 % |
  | single2 vs single | single-path known answer (single) | 12 of 81 = 15 % | 10 of 756 = 1.3 % |
  | blend vs single | single-path known answer (single) | 0 of 9 runs: the blend is 0.54 to 0.87 dB worse than the best single | n/a (never contested) |
  | blend vs single | blend known answer (blend) | 0 of 4 runs: the blend is 3.0 dB or more better than the single | n/a (never contested) |

  single fit across 9 seeds: 0.146 to 0.324 (sd 0.061); single2: 0.156 to 0.364 (sd 0.065). The blend decision is covered by the same
  code but the fixtures never put it inside the contested band, so it adds no refits there. Cost: nothing when no decision is
  contested; otherwise `OCCAM_CONFIRM_STARTS` fits per distinct partner (about 17 s each on the known-answer fixtures). The feel known-answer run needs 0 extra fits (73 s, as before); the full match suite on real scipy 1.17.1: 798 passed, 7 skipped (demucs not installed, SAWBLADE_TEST_TRAIN / SAWBLADE_TEST_DEMUCS unset), 0 failed.
  Local verification on real scipy 1.17.1: `test_single_path_known_answer_is_found_as_single` passes (it failed at 21a9d03 and dc0fa07).
- **Other result.json additions:** every candidate JSON (`best`, `alternatives`, `candidatesStage2`) carries `pairKey` (the candidate's
  identity without its cab); the cab-sweep test matches a candidate to its own sweep with it. Known limitation: the full-DI gate
  sweep indexes the reference with `ref.offset_samples` as refined at the start of the run, not the final per-render offset.

## User results

(pending)
