# v0.9 — VERMIN, a rat-style distortion pedal: REPORT (Tasks A–D; Task E pending)

Spec: [`v0_9-vermin.md`](v0_9-vermin.md). Lead design note (constants, op-amp model, test list, revisions):
[`v0_9-vermin-A_design.md`](v0_9-vermin-A_design.md). Branch `claude/sawblade-v0_9-vermin`, from `8f2c4da`.
Phase lead + dsp-engineer (A–C) + match-engineer (D), reviewer on every task, per CLAUDE.md.

**Task E (plugin face, filmstrip knobs, drawer) has not started.** It waits for v0.8 to merge, because v0.8 edits
the plugin and the preset schema. Its inputs are ready: all seven live parameters are exposed, and the face script
and knob strip render (C).

## Verdicts

| Task | What | Commits | Reviewer |
|---|---|---|---|
| A + B | `pedal.rat` core block + 9 acceptance tests (+ extras) | b0e0722, 61d8ca5, 6ebf1d7, c56256f, e4b11ec | REVISE (header comment, n = 0 read, Stage exposure) → **ACCEPT e4b11ec** |
| C | face script `design/render/pedal_vermin.py`, 6 starter presets, goldens, PEDALS.md | c2ec98a, 8951068, 8cf0145 | **ACCEPT 8951068** (8cf0145 = two notes-text nits from that review) |
| D | `pedal.rat` in the calibration fitter (PEDALS, KNOWN_TRUTH, PEDAL_ORDER) | 27cd711 | **ACCEPT 27cd711** |
| lead docs | design note + revisions, Task D wording fix | d59e96a, 077814c, 1e2f373, 7f23833 | — |

**CI of record: run 317 on 27cd711 (A–D), all green**: linux-gcc (ctest + pluginval VST3), linux-clang `-Werror`,
macos-arm64 (ctest, auval, pluginval AU + VST3), and python (pytest + `compute_trims --check`). Run 310 on e4b11ec
(A + B) was also all green. The only later commit is this report.

## Lead decisions and deviations (all recorded in the design note's revision lines)

1. **Task D scope corrected (main lead).** The spec said "matcher pool, as `pedal.ts` is". On the base, the matcher
   pool holds TONE3000 captures only, and `pedal.ts` lives in the calibration fitter. D therefore registers
   `pedal.rat` in the fitter, with no logic change.
2. **FILTER uses an audio taper, not reverse-log (main lead).** Reverse-log put stock FILTER 5 at 527 Hz, with all
   the bright range in the first tenth of the knob. The audio taper puts FILTER 5 at about 4.2 kHz, with the same
   32 kHz to 475 Hz end points. **This is a voicing choice made by feel, not verified against a schematic.** It is
   one named constant (`RatVoicing::filterReverseLog`) so a capture fit can revisit it.
3. **Op-amp model (lead), after three alias rounds.**
   - The GBW is a single-pole op-amp integrator, `ωt = 2π·1 MHz`, solved implicitly (trapezoidal) with the
     frequency-dependent feedback network. It is not a fixed low-pass.
   - The slew limit is the LM308's differential-pair input stage: integrator input `Vd·tanh(e/Vd)`, with
     `Vd = SR/ωt`, so the maximum rate is exactly 0.3 V/µs.
   - The rail is `Vrail·tanh(v/Vrail)` of the integrator state, with anti-windup clamping at 6·Vrail.
   - Each sample is solved with capped Newton: at most 8 iterations, averaging 1.3 to 2.
   - The op-amp stage, the coupling HPF and the diode clipper run at `stageOversample × 4 fs`, with
     `stageOversample = 2`.
   - The first two attempts did not pass:
     - a hard slew clamp at 4x aliased at −50 dB;
     - the tanh slew at 4x reached −53 dB, still short of the bar.
4. **Output trim +6 dB, stock VOLUME 5.8** (VOLUME uses the shared `pedalLevelDb`). TIGHT uses the shared mapping
   `20·10^(t/10)`, which spans 20 to 200 Hz.
5. **Slew test asserts "the waveform changes", not "THD rises".** The slew-limited trapezoid has *lower* THD than
   the rail-clipped near-square: −13.2 dB against −7.0 dB.
6. **Shared infrastructure.**
   - New `OversamplerNx` (1 to 3 cascaded half-band stages) reuses `Oversampler4x`'s stage design through
     `friend class OversamplerNx`.
   - `Oversampler4x`'s public API is unchanged.
   - Every existing pedal test and golden is bit-identical.

## Measured (Release, 48 kHz unless stated)

| Item | Result | Bar |
|---|---|---|
| Linear FR error vs analytic chain, DIST 0 / 5 / 10 (40 Hz to 0.4 fs, 48 and 44.1 kHz) | 0.000 / 0.071 / 0.072 dB | ±0.5 dB |
| Stage gain at 1 kHz, DIST 0 / 5 / 10 (incl. trim, VOLUME 5.8) | −0.6 / 42.4 / 62.7 dB (ideal network plateau 67 dB) | — |
| GBW −3 dB corner, DIST 5 / 10 | 5503 / 1130 Hz (analytic 5518 / 1131) | ±10 %, moves down |
| Slew, stage alone, DIST 10, 5 kHz, 1 V | max \|dvo\|/T 0.296 V/µs; waveform differs 107 % RMS from slew-free | ±10 % of 0.3 V/µs |
| FILTER corner 0 / 5 / 10 (96 kHz) | 32164 / 4195 / 483 Hz (formula 32152 / 4194 / 475); monotonic, steps 1–9 within 0.7 % | ±10 %, monotonic |
| CLIP peaks (DIST 7, 200 Hz, 0.1 V): none / silicon / led / asym | 4.62 / 0.44 / 1.56 / 0.66 V; asym H2 −54.6 dBc, others exactly 0 | none ≥ +6 dB, led > silicon, asym even harmonics |
| RUETZ at DIST 10: 100 Hz / 200 Hz / 5 kHz | −5.50 / −2.88 / −0.01 dB (analytic −5.52 / −2.88 / −0.01) | ±0.5 dB |
| Latency | 52 samples (44.1–192 kHz, every CLIP mode); 50 / 53 / 54 at stageOversample 1 / 4 / 8 | reported == measured |
| Block-size independence, determinism | bit-identical, blocks 1 / 7 / 64 / 512 / 4096, all stage factors | — |
| Zero allocations in `process()` | 0, including live-parameter changes every block, all stage factors | 0 |
| Stock level (DIST 5, FILTER 5, silicon), −12.00 dBFS-RMS DI fixture | −11.51 dBFS RMS | within ±1 dB of input |

**Alias** (existing recipe: DIST 10, FILTER 0, −6 dBFS). The bar is the worst existing pedal, computed in-test:
`pedal.hm` v3 modded with the asymmetric clip, at **−82.1 dB**.

| stageOversample | none 5k | none 4.7k | silicon 5k | silicon 4.7k | none 2.3k | silicon 2.3k | RTF / instance |
|---|---|---|---|---|---|---|---|
| 1 | −58.0 | −50.4 | −57.4 | −51.0 | −57.5 | −57.6 | 0.027–0.032 |
| **2 (shipped)** | **−98.8** | **−114.9** | **−98.1** | **−108.5** | −69.8 | −69.1 | **0.051–0.067** |
| 4 | −129.7 | −140.1 | −129.4 | −134.2 | −92.6 | −92.6 | 0.102–0.130 |

- The test asserts 1, 2, 4.7 and 5 kHz, for CLIP none and silicon.
- 1 and 2 kHz divide the oversampled rate, so their aliases land on harmonics and those two frequencies add no real
  coverage. 4.7 kHz was added for that reason.
- Sensitivity at 4.7 kHz: without oversampling and ADAA the alias is −22.3 dB, and stageOversample 1 misses the bar.

**Open items:**
- **Off-recipe aliasing.** At the shipped factor, 2.3 kHz aliases at −69 dB and 1.1 kHz at about −82 dB (marginal).
  stageOversample 4 meets the bar everywhere at about 2× the CPU. Proposal: an HQ / offline-render setting.
- **CPU.** RTF is 0.051 to 0.067 per instance at 48 kHz. It varied across runs on the shared container, and is
  about 2× the other modelled pedals (TS ≈ 0.01).
- **Anti-windup.** The clamp at 6·Vrail lengthens recovery after long saturation to about 63 µs. The real device
  takes a few µs. The clamp is one voicing constant.

## Starter presets (`presets/modeled/vermin/`)

All six are path A `pedal.rat` only (Thrash Boost: role `body`), with the identity cab and MIX 100. Each peaks at
−3 dBFS on the fixture (the chainsaw-bank convention). Trims are written by `scripts/compute_trims.py`, so every
preset lands at −18 LUFS. Each preset's notes state its use. Render hashes are in
`tests/golden/preset_render_hashes.json`, with 6 new entries and none changed.

| Preset | DIST | FILTER | VOLUME | TIGHT | CLIP | RUETZ | LUFS before trim | trim dB |
|---|---|---|---|---|---|---|---|---|
| Crust Grinder | 8 | 4 | 7.65 | 1 | silicon | off | −4.10 | −13.90 |
| Doom Filter Down | 6.5 | 8 | 6.03 | 0 | asymmetric | off | −9.91 | −8.09 |
| Thrash Boost | 2.5 | 2.5 | 7.60 | 3 | silicon | off | −5.65 | −12.35 |
| Turbo Crust | 7 | 4 | 0.54 | 1 | none | off | −7.79 | −10.21 |
| Tight Hardcore | 8 | 3 | 7.66 | 4 | silicon | on | −3.76 | −14.24 |
| Grind Wall | 10 | 3 | 3.79 | 2 | led | on | −4.77 | −13.23 |

- Turbo Crust's VOLUME of 0.54 is circuit-true: with no diodes, only the op-amp rails clip, which is about 20 dB
  louder. The preset notes say so.
- The "−18 LUFS within 0.5 LU" ctest skips in the container because captures are uncached. `compute_trims --check`
  covers the vermin trims instead.

## Face (Task C)

`design/render/pedal_vermin.py` follows the `pedal_fuzz.py` pipeline and `common.py`:
- gunmetal powder coat worn to aluminium;
- DIST / FILTER / VOLUME knobs, footswitch and red LED;
- the VERMIN wordmark and an original rodent-skull motif;
- no trademark text and no imitation of the original's logo, lettering or trade dress.

Renders are not committed, like the other faces. Two caveats:
- **The container proxy blocked the Black Ops One font download, so the in-container renders used a stand-in OFL
  font.** A normal run fetches the real font, so the lettering will differ. The script is unchanged.
- Renders are pixel-identical between runs, but the PNG bytes differ (a metadata chunk). This was not checked
  against the other face scripts.

## Calibration fitter (Task D)

`PEDALS["rat"]`:
- block `pedal.rat`;
- searched knobs `distortion` and `filter`;
- level `volume`, a pure output gain, verified in the code and by the parametrized test;
- modelVersion 1.

`KNOWN_TRUTH["rat"] = ((6.5, 3.0), 6.5)`.

Known-answer free fit:
- recovered distortion 6.502, filter 2.992, volume 6.499;
- LTAS / harm / dyn 0.0043 / 0.0025 / 0.0017 dB (limits 0.2 / 0.5 / 0.3).

`pytest match/tests`: 611 passed, 7 skipped (demucs and training opt-ins).

## Follow-ups

- **Merge time (v0.8 calibration):** declare VERMIN's nominal output level in dBu like the other modelled pedals.
  The field does not exist on this base. Use the stock level of −11.51 dBFS RMS on the −12.00 dBFS-RMS fixture.
- **Task E**, after v0.8 merges:
  - plugin face and drawer: DIST / FILTER / VOLUME on the face; TIGHT, CLIP, MIX and RUETZ in the drawer;
  - register the face in `export_ui_assets.py`;
  - display names per `docs/NAMES.md`.
- **Base-branch CI, not v0.9's.** Run 298 (a docs-only commit off 8f2c4da) failed 3 macOS plugin tests:
  - integration:10 and integration:11, the known screenshot flake (v0.3.1 D.4);
  - "swap under load", `test_processor.cpp:355 CHECK(blocks.load() > 100)`, 94 > 100 (now v0.3.1 D.5).

  None repeated in runs 310 or 317.
- **User check after E:** play the six starter presets in Logic and add a feel note here.

## Proposals (not done)

- Modelled pedals as matcher candidates: the matcher pool is captures-only today, so this needs a matcher-logic
  change.
- An HQ setting (stageOversample 4) for offline / NAM-export renders.
- A capture-based fit of VERMIN, as v0.5 does for the saw and TS. It would revisit the FILTER taper, the
  anti-windup constant and Vrail.
- Add VERMIN to `board_shot.py` for the board hero scene.
- Add a rat line to the BUDGET margin comment in `match/tests/test_pedal_fit.py` (reviewer nit).
