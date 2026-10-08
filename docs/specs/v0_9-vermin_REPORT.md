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
| A (alias rule) | default stageOversample 4: the alias bar holds at 1.1 / 2.3 / 4.7 / 5 kHz; latency 53 | dbed442 | **ACCEPT dbed442** |
| D | `pedal.rat` in the calibration fitter (PEDALS, KNOWN_TRUTH, PEDAL_ORDER) | 27cd711 | **ACCEPT 27cd711** |
| lead docs | design note + revisions, Task D wording fix | d59e96a, 077814c, 1e2f373, 7f23833 | — |

**v0.9 Tasks A–D ACCEPT (lead), Task E pending v0.8 merge.**

**CI of record: run 323 on 76a4e5a (A–D with stageOversample 4), all green**: linux-gcc (ctest + pluginval VST3),
linux-clang `-Werror`, macos-arm64 (ctest, auval, pluginval AU + VST3), and python (pytest + `compute_trims --check`).
Earlier runs 310 (e4b11ec) and 317 (27cd711) were also all green. The only later commit is this report line.

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
     `stageOversample = 4` (main lead's rule; see the alias section).
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
| Latency | 53 samples (44.1–192 kHz, every CLIP mode); 50 / 52 / 54 at stageOversample 1 / 2 / 8 | reported == measured |
| Block-size independence, determinism | bit-identical, blocks 1 / 7 / 64 / 512 / 4096, all stage factors | — |
| Zero allocations in `process()` | 0, including live-parameter changes every block, all stage factors | 0 |
| Stock level (DIST 5, FILTER 5, silicon), −12.00 dBFS-RMS DI fixture | −11.51 dBFS RMS | within ±1 dB of input |

**Alias** (existing recipe: DIST / drive at maximum, FILTER 0, −6 dBFS, 48 kHz, same in-test detector). The main
lead set the bar as "≤ the worst existing pedal at each frequency", not only at the recipe frequencies. The rule: if
factor 2 is worse than every existing pedal at 2.3 kHz or 1.1 kHz, the default becomes 4, with no HQ toggle. It was,
so **the shipped default is stageOversample 4**.

| pedal | 2.3 kHz | 1.1 kHz | 4.7 kHz | 5 kHz |
|---|---|---|---|---|
| hm v2 | −97.7 | −103.8 | −95.0 | −93.8 |
| hm v3 stock (worst clip) | −96.0 | −109.4 | −81.4 | −85.5 |
| hm v3 custom (worst clip) | −95.5 | −108.7 | −82.6 | −86.4 |
| hm v3 modded, asymmetric | −88.4 | −100.5 | −77.1 | −82.1 |
| ts (drive 10, tone 10) | −99.1 | −106.5 | −92.4 | −91.6 |
| muff (sustain 10) | −100.5 | −103.8 | −95.4 | −90.6 |
| **worst existing** | **−88.4** | **−100.5** | **−77.1** | **−82.1** |
| VERMIN factor 2, none / silicon | −69.8 / −69.1 | −82.5 / −81.6 | −114.9 / −108.5 | −98.8 / −98.1 |
| **VERMIN factor 4 (shipped), none / silicon** | **−92.6 / −92.6** | **−105.3 / −105.3** | **−140.1 / −134.2** | **−129.7 / −129.4** |

- The test asserts, per frequency (1.1, 2.3, 4.7 and 5 kHz), that VERMIN with CLIP none and silicon is ≤ the worst
  existing pedal, computed in the test.
- 1 and 2 kHz divide the oversampled rate, so their aliases land on harmonics; they keep the 5 kHz bar.
- The test also pins that factor 2 stays worse than the worst existing pedal at 2.3 kHz (the reason for the default),
  and that factor 1 misses the bar.
- Sensitivity at 4.7 kHz: without oversampling and ADAA the alias is −22.3 dB.

**CPU, per instance at 48 kHz, Release, DIST 10:**

| stageOversample | latency | RTF (earlier runs) | RTF (last runs, machine ≈ 1.75× slower) |
|---|---|---|---|
| 1 | 50 | 0.027–0.032 | 0.047–0.048 |
| 2 | 52 | 0.051–0.067 | 0.092–0.094 |
| **4 (shipped)** | **53** | **≈ 0.10–0.13** (estimated from the stable 3.6× ratio to factor 1) | 0.173–0.176 |
| 8 | 54 | 0.217–0.255 | 0.346–0.358 |

**Open items:**
- **CPU.** At about 10–13 % of a core per instance, VERMIN is the most expensive modelled pedal (TS ≈ 1 %). The main
  lead accepted about 0.13 on desktop. Cheaper options not yet taken: a looser Newton tolerance, or batching the tanh
  divisions; either must keep every alias row above.
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

Renders are not committed, like the other faces. Notes:
- **Font.** The base's `common.py` fetches Black Ops One from raw.githubusercontent.com, unpinned, and the proxy
  blocks that host; its fallback then draws the wordmark as nothing visible. The final renders used the v1.0 design
  branch's pin: fonts.gstatic.com, sha256 `bd8a70e63df108745316c6ad277874cbe139bbb90cbcaf705810ecc089fe59f8`. The
  file was fetched with TLS verification on, matched exactly, and was passed with `--font-dir`. `common.py` was not
  changed on v0.9, because v1.0 rewrites it.
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

  None repeated in runs 310, 317 or 323.
- **User check after E:** play the six starter presets in Logic and add a feel note here.

## Proposals (not done)

- Modelled pedals as matcher candidates: the matcher pool is captures-only today, so this needs a matcher-logic
  change.
- At the v1.0 merge, port the SHA-pinned font fetcher into `common.py`, and make a failed fetch a hard error instead
  of a silent invisible wordmark.
- A capture-based fit of VERMIN, as v0.5 does for the saw and TS. It would revisit the FILTER taper, the
  anti-windup constant and Vrail.
- Add VERMIN to `board_shot.py` for the board hero scene.
- Add a rat line to the BUDGET margin comment in `match/tests/test_pedal_fit.py` (reviewer nit).
