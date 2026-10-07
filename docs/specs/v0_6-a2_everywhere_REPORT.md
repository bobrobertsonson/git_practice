# v0.6 — A2 everywhere: REPORT

Spec: [`v0_6-a2_everywhere.md`](v0_6-a2_everywhere.md) (Tasks A–D, Lead decisions 1–23). Branch
`claude/sawblade-v0_6-a2`, based on `4d580a2` plus v0.4M (`bafcada`, merged in `bc0c4a6`; it was `bbae365`
before, see "Process notes"). Owners: dsp-engineer (core/plugin), match-engineer (trainer/export/device-null),
reviewer on every task.

**Dependency for the integration lead:** v0.6 contains v0.4M up to `bafcada`. Merge v0.4M first (or together).

## 1. Summary

- **Playback:** the pinned NeuralAmpModelerCore (`0b3d3c97`, tag v0.6.0, upstream `main`) already plays A2 Full,
  A2 Lite and the packed container. No dependency bump. Sawblade's own A2 files hit the core's A2 fast path
  (tested). Zero allocations in `process()`, block-size independent, latency 0.
- **CPU:** on the macOS arm64 CI runner, two A2 Full amps + pedals + cab cost **RTF 0.123** at 64-sample blocks
  (real-time limit 1.0; test gate 0.5 → about 4x margin). A2 Full costs half of A1 standard, A2 Lite about 1/12.
- **Export = a standard NAM A2 file** (user decision): `sawblade-export` trains **A2 by default** and the primary
  output is the trainer's own packed container `<stem>.a2.nam` (standalone Full / Lite as extras; A1 kept as `--arch a1`
  with NAM's official presets). A core-only check (NAM code only, no Sawblade code) loads every exported file in CI.
- **Training signal:** by default the **official NAM standard input file**, supplied by the user (`--nam-input`, plugin
  Settings / first-export prompt) and run through the pinned trainer's own data pipeline; Sawblade's synthetic signal is
  an explicit, labelled fallback (`--signal sawblade`). The file is never committed, bundled or downloaded (licence
  finding below). A **reamp pair** export (`--reamp-pair`) lets the user train with any standard NAM trainer.
- **Anagram:** export notes carry an optional generic hint ("any NAM A2 block; on the Anagram a Neural Amp or Neural
  Pedal block, KosmOS 1.16+"); `sawblade-calibrate device-null` measures how closely the Anagram reproduces the export.
- **Task D** (user-run on the Anagram) is pending: steps in section 7.

## 2. Reviewer verdicts

| Task | Commits | Verdict | Rounds |
|---|---|---|---|
| A core/plugin audit | `b6da963`, `8ce7aa1` | ACCEPT | 1 (4 doc should-fixes folded in) |
| A trainer audit, official A1 presets, fixtures | `972118d`, `33c4341`, `4b54500`, fixes `bd0764d`, `979fc0f`, `bb4f789` | ACCEPT | 3 (REVISE: preset test needed `nam` in CI; missing test-results section; fixture check flaky on CI) |
| B A2 playback + CPU | `21e233b`, `0c24470`, `5a7a542`, `ba62827`, `31ef98f`, `45b5fb8` | ACCEPT | 2 (REVISE: bench table invisible in CI logs); generic-path rows ACCEPT |
| C plugin half | `4a3d22e`, `26ba02d`, `45358b6` | ACCEPT | 2 (should-fix promoted: other A2 size's verdict hidden) |
| C Python half + device-null | `d1ef210` … `45cac23`, fixes `54b7207`, `253d1d0` | ACCEPT (delta) once CI is green | 3 (REVISE: device-null band math, wrong reference render, overall-only verdict; drive-only rule; A2 require-accept tests; then the CI fixture check) |
| CI fix: macOS pedal-face test race (pre-existing) | `8205a3f` (committed with the user's approval) | ACCEPT | 1 |
| Anagram wording (device info) | `999e7f0`, `a326d62` | ACCEPT | 2 (unconfirmed TONE3000-block claim removed) |
| Decisions 18–23, plugin + load check | `48476d2`, `ec7a32b`, `9bb6213`, `9870999`, `ea922d2`, `4b5e54e`, `d2c9b62`, CI fix `e7745b1` | ACCEPT | 4 (REVISE: resume signal contract; then parity follow-ups; CI run 263: reamp test missing fake setup, signal prompt invisible over a finished result) |
| Decisions 18–23, Python | `ffd1110`, `39e705a` | ACCEPT | 2 (REVISE: official-path held-out ESR misaligned by the trainer's 1-sample latency; A1 notes signal; A1 wording) |

## 3. Task A — audit

Details: [`docs/reports/v0_6/audit_core.md`](../reports/v0_6/audit_core.md) (core/plugin) and
[`docs/reports/v0_6/audit_match.md`](../reports/v0_6/audit_match.md) (trainer/match).

**What A2 is.** A `.nam` file never says "A2". An A2 file is either a `SlimmableContainer` (packed export, submodels
at `max_value` 0.5 and 1.0) or a standalone 23-layer WaveNet (LeakyReLU 0.01, dilations 1,3,7,17,41,101,239 ×3 +
1,13). The pinned trainer (neural-amp-modeler 0.13.0, `train/_resources/config_model_packed.json`) names the two
sizes `channels_3` and `channels_8`; **A2 Lite = 3 channels, A2 Full = 8 channels** is consistent with the trainer
config, the container order and the core's fast-path shapes. That TONE3000 and the Anagram call these "Full" and
"Lite" is the user's statement (recorded as such). File version is 0.7.0 for A1 and A2 from this trainer; the core
accepts 0.5.0–0.7.x, so a future 0.8.0 file would need a core bump (watch item).

**Official A1 presets.** They are **not in the pinned 0.13.0** (it removed `Architecture` / `get_wavenet_config`;
its `core.py` only builds the packed A2 net). They were last published in **0.12.3 `nam/train/core.py:845-955`**
(byte-identical in 0.11.0, 0.12.0, 0.12.2). Sawblade's `standard` was already the official layout; `lite` and
`feather` had the right channels but the wrong layer split (10+10 instead of 7+13 dilations) and now have 6 553 /
3 025 parameters (were 7 903 / 3 637). Pinned by `match/tests/test_export.py` (layout test without `nam`;
parameter counts 13 801 / 6 553 / 3 025 and receptive field 4093 against the pinned 0.13.0 `WaveNet`, run in the
`python-export` CI job). `nano` not offered.

**A1-assumption inventory.** Full tables with file:line in the two audit files. Summary of what changed in this
phase vs what stays:

| Area | A1 assumption found | Outcome |
|---|---|---|
| Export trainer (`export/train.py`) | A1-only nets, "recalled" sizes, summed `ESR` | A2 packed training, per-submodel `ESR_packed_i`; official A1 presets |
| Export acceptance (`validate.py`) | only `standard` judged | A2 Full and A2 Lite judged per file |
| Resume identity (`resume.py`) | no arch/layout key | `arch` + `layout` added; old lite/feather checkpoints refuse cleanly |
| Plugin export panel / settings / JobRunner | `feather/lite/standard`, no `--arch` | A2 FULL / A2 LITE / A1; settings migrate; `--arch/--size` |
| `NamBlock` metadata | reads top-level `name/gear_type/modeled_by` only | exporter writes them at top level of all three files |
| Container size selection | core picks Full; `SetSlimmableSize` not real-time safe, never called | unchanged; Lite plays from the standalone file (proposal P1) |
| `PRESET_SCHEMA.md` nam latency | said "the model's" | corrected: 0 (receptive field = prewarm, not latency) |
| Ladder fetch (`LadderFetch.h`) | asks TONE3000 for size `standard` (already A2-preferring) | unchanged; A2 size strings unverified offline (user check U1) |
| Capture browser A2 chip | no Full/Lite distinction | unchanged (proposal P3) |
| Pool size ranking (`matcher/pool.py`) | A1 size vocabulary | unchanged; needs U1 |

## 4. Task B — A2 playback and CPU

Tests (`tests/test_a2_playback.cpp`, fixtures `tests/fixtures/a2/` generated by the pinned trainer from synthetic
signals, seeds in its README; untrained weights, no TONE3000 data):

| Check | Result |
|---|---|
| Null vs the trainer's own forward pass (64-sample blocks) | max abs diff: Full 3.7e-8, Lite 2.2e-8, container 3.7e-8, A1 1.5e-8; tolerance 1e-5 (≈60 dB below the quietest reference peak) |
| Container plays | Full (equals standalone Full within 1e-6; differs from Lite by > 1e-3) |
| Latency / prewarm | latency 0 for all; prewarm 6347 (A2) / 4093 (A1) |
| Allocations in `process()` | 0 (all four files, block sizes 1–512) |
| Block-size independence | 1, 37, 64 vs 512: bit-identical for A2, ≤ 7.5e-9 for A1 |
| Fast path | `is_a2_shape` true and dynamic type `A2FastModel` for Full and Lite (and both container submodels); a one-field change falls back to the generic path (negative control) |

CPU (`tests/a2_bench.cpp`, ctest `a2_bench`; 48 kHz, 64-sample blocks, 3 warm-up + 9 timed 2 s passes, median;
RTF = processing time / audio time on one core). CI run 238 (`5a075f8`):

| Runner | A1 standard | A2 Full | A2 Lite | Container (plays Full) | Rig: 2× A2 Full + pedals + cab |
|---|---|---|---|---|---|
| macOS arm64 | 0.096 | 0.048 | 0.0076 | 0.050 | **0.123** (p99 block 458 µs of 1333) |
| Linux gcc | 0.178 | 0.066 | 0.014 | 0.066 | 0.152 |
| Linux clang | 0.232 | 0.091 | 0.015 | 0.089 | 0.207 |

Gate: rig median RTF < 0.5 (Release only) → PASS on all runners; margin on macOS ≈ 4x. The rig is gate, HM pedal
+ A2 Full (path A), TS + A2 Full (path B), path EQs, auto align, blend, shared 4096-tap cab, post EQ, bus comp.

**Why A2 Full is cheaper than A1 standard.** Weight counts (≈ multiply-adds per sample) are close: 12 146 vs
13 802 (`a2_bench` counts the weights array incl. the head scale; the trainer's convention gives 12 145 / 13 801).
Layouts, read from the fixtures' JSON: A1 standard = 2 arrays, 10×16 + 10×8 channel-layers, kernel 3, **Tanh**,
240 nonlinearity evaluations per sample (NAM's 0.12.3 preset); A2 Full = 1 array, 23×8, **LeakyReLU 0.01**,
184 evaluations; A2 Lite = 23×3, 69. To separate the layout from the core's A2 fast path, `a2_bench` also runs the
same A2 weights forced onto the generic WaveNet path (one LeakyReLU slope perturbed; asserted not A2-shaped / not
`A2FastModel`). CI run 246 (`bb4f789`), RTF:

| Runner | A1 standard | A2 Full fast | A2 Full generic | A2 Lite fast | A2 Lite generic |
|---|---|---|---|---|---|
| Linux gcc | 0.215 | 0.080 | 0.093 | 0.015 | 0.071 |
| Linux clang | 0.178 | 0.065 | **0.061** | 0.010 | 0.032 |

CI run 267 (`e7745b1`, the CI of record), all three runners, RTF median:

| Runner | A1 standard | A2 Full fast | A2 Full generic | A2 Lite fast | A2 Lite generic | Container | Rig (2× A2 Full + pedals + cab) |
|---|---|---|---|---|---|---|---|
| macOS arm64 | 0.135 | 0.069 | 0.079 | 0.010 | 0.056 | 0.065 | **0.177** (p99 block 667 µs of 1333) |
| Linux gcc | 0.227 | 0.080 | 0.092 | 0.017 | 0.077 | 0.080 | 0.184 |
| Linux clang | 0.219 | 0.092 | 0.086 | 0.014 | 0.050 | 0.092 | 0.213 |

Conclusion: **A2 Full's advantage over A1 standard comes from its layout** (8 channels throughout, cheap LeakyReLU
instead of Tanh; generic-path A2 Full is still 2.3x / 2.9x cheaper than A1), **not from the fast path**, which buys
A2 Full only ~15 % on gcc and nothing on clang (generic slightly faster). The fast path matters for **A2 Lite**
(3–5x). On macOS (run 267) the fast path buys A2 Full ~15 % and A2 Lite ~5.5x; A1 / A2 Full is 1.9x there and 2.4–2.9x on
Linux, so the ratio is not portable. Runner-to-runner noise is visible (macOS rig 0.123 in run 238, 0.177 in run 267):
the real-time gate (0.5) keeps ≥2.8x margin in every run.

## 5. Task C — A2 export with Anagram notes

**CLI.** `sawblade-export … --arch a2|a1 --size …` (default `a2 full`; invalid pairs exit 1 before any work).
A2 trains the packed net with the trainer's recipe, keeps a best checkpoint per submodel, writes
`<stem>.a2.nam` (container), `<stem>.a2_full.nam`, `<stem>.a2_lite.nam` (extracted submodels, same weights), with
top-level metadata, the `sawblade` block, loudness and the `-nc` marking on all three. Report JSON:
`arch`, `size`, `files{primary,container,full,lite}`, `validation{full,lite}`, `a2FastPath{full,lite}` (both true).

**Acceptance (per file).**

| Size | Held-out ESR | DI LTAS error |
|---|---|---|
| A2 Full | ≤ 0.02 | ≤ 0.5 dB |
| A2 Lite | ≤ 0.05 | ≤ 1.0 dB |
| A1 standard (unchanged) | ≤ 0.02 | ≤ 0.5 dB |

`--require-accept` exits 2 when the **primary** size is NOT MET; the plugin shows the primary verdict as the
headline and the other A2 size's verdict next to its file (red when NOT MET).

**Validation numbers** (CPU-budget pipeline check, not final quality: `tests/fixtures/presets/golden_shared.json`,
in-repo fixture NAMs, no-cab, 5 epochs, 4 threads, x86 cloud box):

| Model | Params | s/epoch | Held-out ESR | DI LTAS | Acceptance | Core RTF (x86) |
|---|---|---|---|---|---|---|
| A1 standard | 13 801 | 96 | 0.0069 | 0.15 dB | MET | 0.279 |
| A2 Full | 12 145 | 141 (both sizes) | 0.0181 | 0.19 dB | MET (limit 0.02) | 0.096 |
| A2 Lite | 1 870 | (same run) | 0.0433 | 0.39 dB | MET (limit 0.05) | 0.022 |

A2 trains both sizes in one run at ≈1.5x the A1 standard time per epoch. A2 Full is close to its limit at 5 epochs;
real heavy-tone numbers need the default budget (22 epochs / 80 min) on the user's machine.

**Anagram profile.** `exportNotes.deviceProfiles.anagram` = `{device, file, message, loaderOrder (string), stages[{stage,
block, position, settings{}, hardware?}]}` and `<stem>.anagram_notes.txt` (format in `docs/PRESET_SCHEMA.md`).
Mapping: gate → Gate block (first); the model → Neural Amp block (Neural Pedal only when every enabled non-EQ block is
explicitly a pedal/boost); no-cab export's IR (cab + post EQ folded) → IR block after the model; bus comp → Compressor
block (last). Only the published block list is used; values are Sawblade's, to be matched by ear/meter. The generic
v0.4M notes are unchanged (`NOTES_VERSION` 1).

**Decisions 18–23 (user: "a standard NAM A2 file, usable on any A2 loader").**
- **Primary file** = `<stem>.a2.nam`, written by the pinned trainer's `PackedWaveNet.export_container` (what its
  standard workflow produces); `files.primary` points to it, standalone `.a2_full.nam` / `.a2_lite.nam` stay as extras
  with their own verdicts. `tests/tools/nam_load_check` (links only NeuralAmpModelerCore) loads the fixtures in every C++
  job and all three freshly exported files in `python-export`. Whether every A2 loader accepts a container is unverified.
- **Licence finding (verbatim):** MIT covers the trainer code (neural-amp-modeler 0.13.0); the trainer ships no input
  file — its GUI links the files on Google Drive (`nam/train/gui/__init__.py:267-269`); no terms are stated for the
  audio; the file is **user-supplied and not redistributed** (recorded in `docs/THIRD_PARTY.md`).
- **Training on the standard input:** `export/standard_input.py` recognises every 48 kHz version the trainer does (its
  MD5 tables, `nam/train/core.py:91-96, 112-158, 204-233`; v1/v2 accepted with the trainer's deprecation log);
  `export/official.py` uses the trainer's latency calibration, data checks, validation split, datasets and −18 dBFS
  normalisation, with Sawblade's own training loop and export (documented deviation: `train()` has no resume, cancel,
  progress, per-submodel checkpoints, `export_container` or metadata). The installed trainer must equal the pin. Held-out
  ESR / LTAS on this path are measured after removing the trainer's latency offset (its safety factor makes models one
  sample late — standard NAM behaviour; recorded as `alignedSamples`). The signal used is recorded per export
  (`trainingSignal`: "nam-standard v3.0.0" / "sawblade-synthetic v1") in metadata, report and notes.
- **Fallback:** `--signal sawblade`, labelled "Trained on Sawblade's test signal, not the standard NAM signal". The
  plugin asks for the file on the first export (CHOOSE FILE / USE SAWBLADE'S TEST SIGNAL INSTEAD / NOT NOW) and never
  falls back silently; the latest explicit choice wins. A resume reuses the recorded signal.
- **Reamp pair:** `--reamp-pair NAM_INPUT.wav [--no-train]` copies the input unchanged and writes the chain's render
  (24-bit mono 48 kHz, same length, latency-compensated) + notes (personal use only, non-commercial with `-nc` captures,
  never upload or share). Plugin: EXPORT REAMP PAIR.

**CI.** New `python-export` job installs `match[dev,export]` (constraints file, CPU torch) and runs the export / A2 /
device-null tests with real tiny trainings and the fixture `generate.py --check`.

## 6. CI of record

**Run 267 on `e7745b1` — all green** (gcc, clang -Werror, macOS arm64 incl. auval + pluginval AU/VST3 level 10,
python, python-export): ctest 1025/1025 on every C++ job (skips: the documented environment skips), python 778 passed /
20 skipped (trainer- or demucs-gated), python-export 160 passed with `SAWBLADE_TEST_TRAIN=1` incl. the core-only load
check of a freshly exported A2, fixtures reproducible. The final REPORT commit on top is documentation only; its own
run is the one cited in the PHASE line. History:

| Run | Head | Result |
|---|---|---|
| 227–237 | various | cancelled by successive pushes (process error, corrected) |
| 238 | `5a075f8` | C++ / plugin / auval / pluginval green; python red (stale v0.4M merge; preset test needing `nam`) |
| 240 | `45cac23` | all green (incl. new `python-export`) |
| 241 | `45358b6` | all green |
| 243 | `253d1d0` | python-export red (strict fixture compare); macOS red: integration 990/991 pedal face |
| 244, 245 | `45b5fb8` | cancelled by the next push |
| 246 | `bb4f789` | all green except macOS: integration 991 pedal drawer (same race) |
| 247, 248 | `c2de2e0`, `f95ad5f` | python-export: fixture check too strict (forward-pass ~1e-8, numpy ~1e-15 across runner CPUs); fixed `1b82e38` |
| 250 | `8205a3f` | all green (pedal-face fix) |
| 251 | `f49777b` | all green |
| 263 | `d2c9b62` | ctest red on all C++ jobs: editor tests 821–823 (reamp test missing fake setup; signal prompt invisible over a finished result); fixed `e7745b1` |
| 267 | `e7745b1` | **all green** |

**macOS integration 990/991 ("pedal face" / "pedal drawer").** Pre-existing test race, not v0.6 code (243/246 touch no
C++ or plugin files; identical plugin code passed 238/240/241): `step10PedalFace` reads the face's bounds right after a
preset load, but the bounds are only set by the editor's 4 Hz refresh (`placeFace`); `waitForLoader` blocks the
message thread and `pump(60)` is shorter than 250 ms, so on an unlucky timer phase the measured rectangle is empty
(fraction 0.0 for all four circuits). Fix (test-only, reviewer ACCEPT): `w.ed->refreshNow()` before reading the
bounds, `REQUIRE(face.getWidth() > 0)`, re-read bounds per circuit. Commit status: see section 11.

**Fixture check.** The strict byte compare of `tests/fixtures/a2` failed on run 243's runner but passed on 240/241 and
locally. Root cause (established on run 247 via the diagnostics added in `bb4f789`): **forward-pass outputs differ by
~1e-8 between runner CPUs** (run 247: `manifest.json/models/a2_lite/packedForwardMaxAbsDiff` off by 2.05e-08), which also
moves the forward-derived loudness / gain metadata and `ref_*.wav`. Seeded weights and configs are identical. The check
(`c2de2e0`, `f95ad5f`) compares structure, configs, versions and sample rates exactly, weights within 1e-6,
forward-derived values within 1e-4 relative (`packedForwardMaxAbsDiff` 1e-6 absolute), and flags stray files.


## 7. Task D — proof on the Anagram (user-run)

Run by the user; the lead relays these steps. Result: PENDING.

Device: the export is a **standard NAM A2 file**; on the Anagram, A2 playback needs **KosmOS 1.16 or later** (Neural Amp /
Neural Pedal / Neural Loader blocks, up to three NAM instances). The file is for your own use — do not upload it to
TONE3000 (models trained from TONE3000 captures need the creators' permission to share).

0. **Confirm the KosmOS version** on the Anagram (1.16 or later is required) and send it back.
1. **Export** one matched preset from the plugin's EXPORT panel (or `sawblade-export`) as **A2 FULL**, **no-cab**,
   with a shared cab (live-compatible blend), trained on your NAM standard input file (set it when the panel asks).
   The export folder then holds the container `<name>-nocab-full.a2.nam`, the extras `.a2_full.nam` / `.a2_lite.nam`,
   `<name>-nocab.ir.wav` (cab + post EQ folded), `export_report.json`, the notes and `listen/`.
2. **Load on the Anagram**: the **container** `<name>-nocab-full.a2.nam` into a **Neural Amp** block (if the device
   rejects it, load `<name>-nocab-full.a2_full.nam` instead and tell me which file it accepted), and
   `<name>-nocab.ir.wav` into the **IR** block right after it. Leave gate / compressor / EQ blocks out for this test
   (bypassed), and set the Neural Amp and IR block levels to unity (0 dB) if the device offers level controls.
3. **Re-amp the same DI**: interface out → Anagram input → Anagram output → interface in. Record at 48 kHz (44.1 or
   96 kHz also work; they are resampled). Keep the interface's input below clipping; level is matched automatically.
   Record the whole DI with ~1 s of silence before it.
4. **Run**
   ```
   sawblade-calibrate device-null --recording anagram_reamp.wav \
       --model EXPORT_DIR/<name>-nocab-full.a2.nam --ir EXPORT_DIR/<name>-nocab.ir.wav \
       --di di.wav --out device_null_out
   ```
   It renders the exported model + IR through Sawblade's core (what the plugin plays for those files), aligns the
   recording (reports the round-trip latency), matches gain and polarity, and writes `device_null_report.json`,
   `render.wav` and a level-matched, aligned A/B pair in `listen/`.
   (If the Anagram only accepted the standalone file, pass that file to `--model`.)
5. **Send back** `device_null_report.json`, the console's band table, which file the Anagram accepted, and your
   listening verdict on `listen/`.

**Tolerance (proposal):** a match = overall residual ≤ −30 dB re the reference **and** every content octave band
(within 40 dB of the loudest) within ±1.5 dB after the global gain. Guide: ≤ −40 dB indistinguishable, ≈ −20 dB
audible. If it fails, the band table says whether it is level, filtering (per-band level differences) or noise /
non-linearity (residual left after per-band correction). Do not compare against the original chain (`--preset`): the
export itself is ~−17 dB (A2 Full) / ~−13 dB (A2 Lite) from it by design.

## 8. Proposals (not done)

- **P1** Container size selection in the plugin (A2 Lite from a packed file): needs an off-thread
  `SetSlimmableSize` + swap hand-over; today Lite plays from the standalone file.
- **P2** Replace the deprecated `ResetAndPrewarm` with `SetPrewarmOnReset(true)` + `Reset` before any core bump.
- **P3** Capture browser: show A2 Full / A2 Lite instead of one "A2" chip.
- **P4** A/B the A1 training recipe (0.13.0 packed recipe vs the 0.12.3 A1 recipe) on a real tone.
- **P5** The CI `python` job runs ~24–34 min against a 45 min timeout (pre-existing); split or shard it.

## 9. User checks

- **U2 Official-signal acceptance (Mac, decision 22):** A2 Full + Lite trained on the standard input vs Sawblade's
  signal, same preset (no TONE3000 captures needed):
  ```
  cd match && source .venv/bin/activate
  sawblade-export ../presets/modeled/hm_chainsaw.json --mode nocab --arch a2 \
    --nam-input ~/NAM/input.wav --di builtin --out ~/sawblade-acceptance/official
  sawblade-export ../presets/modeled/hm_chainsaw.json --mode nocab --arch a2 \
    --signal sawblade --di builtin --out ~/sawblade-acceptance/synthetic
  ```
  Send `validation.full` / `validation.lite` (`heldOut.esr`, `diExcerpt.ltas.aWeightedErrorDb`, `acceptance.status`,
  `alignedSamples`) and `trainingSignal` from both `export_report.json`. The official path has only run on a synthetic
  stand-in here; if the trainer cannot calibrate latency from the rendered blips the run stops with that message.
- **U1** TONE3000 size strings for A2 models (pool ranking, ladder): `sawblade-t3k models <an A2 tone id>` and send
  the `size` values.

## 10. Process notes

- The first v0.4M merge used `bbae365`; v0.4M had moved on (`bafcada`, fixing two matcher tests and adding
  `--notes-preset`). The resulting CI failures were first misread as container-specific; corrected in `45cac23`.
- Commit trailers: `bd0764d` … `a53209a` carry "Claude Sonnet 5.5" and `918ebee`, `392ecd1` none (history not
  rewritten); all later commits carry the required trailers.

## 11. Known gaps and divergences

- The plugin's generic notes text omits the "In the model (nothing to add)" lines that the exporter's
  `.export_notes.txt` prints (pre-existing since the v0.4 port).
- No test asserts that NOT NOW on the training-signal prompt returns the panel to the Result view (the code does).
- The refusal of a resume that names a different training signal is tested in the exporter, not through the plugin
  (the plugin never passes a signal on resume).
- The macOS pedal-face fix (`8205a3f`) was committed by the phase lead after the user approved it; a subagent's commit
  of the same file had been denied by the permission classifier ("Git Destructive").
