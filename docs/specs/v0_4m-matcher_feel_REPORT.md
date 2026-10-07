# v0.4M — matcher matches the feel: report

Spec: `docs/specs/v0_4m-matcher_feel.md` (Tasks A–E, B2–B4). Lead-pinned definitions: `docs/specs/v0_4m-tasks.md`.
Branch: `claude/sawblade-v0_4m-matcher-feel` (base `claude/sawblade-plugin-setup-7k0b8q` at 4d580a2 — v0.4 B–E — merged in). CI (GitHub Actions) is the validation of record; the local container could
not install scipy/pytest (pypi blocked), so engineers also ran the render tests against a local core build with a scipy
shim — those numbers are labelled "shim" below and are indicative only.

Status: **Tasks A–C, B2.1–B2.4, B3, B4, D.1 and E accepted by the reviewer and merged; CI of record on the merge head
below.** Validation on the real Bloodbath audio (D.2/D.3) is the user's: Mac commands below.

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
| B2.1 matcher, B2.3, B4 | ACCEPT after 1 REVISE (pre-EQ moved after stage 2 on the refined winner with a re-fit keep rule ≥ 0.05; it had picked a spurious HPF on a plain chain); merged with B3 + D.1; canonical IR-pair orientation | see CI of record |
| CI fixes after the v0.4M merge (run 195/203) | ACCEPT: studio eqd judged only on residual the post EQ can't absorb; widened post EQ must beat an equal-budget ±6 dB re-fit; pre-EQ confirmation re-fit keeps feel in every block (a real bug the spy test caught) | see CI of record |
| Task E follow-up: dropped bus comp in export notes (found by the v0.4 lead) | ACCEPT after 1 REVISE: `sawblade-export --notes-preset` (original rig; nocab only; repeated on resume; refuses a different rig) + plugin `ExportGlue.cpp` writes `<jobs>/inputs/<hash16>.notes_preset.json` and passes it for no-cab DROP COMP exports and resumes | see CI of record |

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

Run in Terminal on the Mac. Paste the printed summaries (step 7) back to the lead; listen to step 4's `listen/` files.

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

## User results

(pending)
