# match/ — Sawblade matching engine (Python)

Phase 1.5 part A: TONE3000 access (`sawblade_match.t3k`) and the `sawblade-t3k` CLI.
Spec: `docs/specs/phase1_5.md`.

## Setup

```
python3 -m venv match/.venv
match/.venv/bin/pip install -e 'match[dev]'
match/.venv/bin/pytest match          # no network needed
```

### Get the publishable key

1. Sign in at tone3000.com and open **Settings -> API keys**.
2. Copy the **publishable key** (`t3k_pub_...`). It is the OAuth `client_id` and is safe to keep in
   an env var or container secret.
3. Never use the secret key (`t3k_cs_...`); the CLI refuses it.

```
export TONE3000_CLIENT_ID=t3k_pub_xxxxxxxx
```

### Log in (device flow, works headless)

```
sawblade-t3k login
```

It prints a code and `https://www.tone3000.com/activate`. Open that on any device, sign in, enter
the code. Tokens are saved to `~/.config/sawblade/t3k_tokens.json` (mode 0600, override with
`SAWBLADE_T3K_TOKEN_FILE`) and refreshed automatically (rotation is persisted).

Containers are ephemeral: `login` prints the refresh token **once**. Save it as the
`TONE3000_REFRESH_TOKEN` secret and a fresh container logs in without the device step (stored tokens,
when present, win over the env seed). Access tokens and Authorization headers are never printed or logged.

## Usage

```
sawblade-t3k whoami
sawblade-t3k pull [--favorites] [--gear amp pedal ir] [--no-trending] [--no-latest]
                  [--no-download] [--max-models-per-tone 3] [--manifest pool.json] [--cache-dir DIR]
                  [--max-age-months 18] [--min-favorites 100] [--min-downloads 1000]
                  [--popularity-percentile P] [--keep-favorites-below-floor]
                  [--no-a1-fallback] [--favorites-bypass-recency]
sawblade-t3k resolve presets/chainsaw_body.json [--first-model]   # -> presets/chainsaw_body.resolved.json
```

* `pull` builds the candidate pool from **favorited** (always fetched) + **trending** + **latest** and
  applies the quality filter: A2 preferred (A1 only if no A2 models), <= 18 months old (favorites too),
  and absolute popularity floors (>= 100 favorites and >= 1000 downloads). `--popularity-percentile P`
  additionally requires the per-gear Pth percentile (off by default). Favorited tones below the floors are
  excluded with a reason unless `--keep-favorites-below-floor` (then kept and flagged). `amp-cab` rigs are
  references only. The manifest `<cache>/pool_manifest.json` records every decision and reason and lists
  **all** models of each included tone (id, name, architecture_version) as separate candidates: models within
  a tone are usually different settings (gain, channel, boost), so choosing one is a tone decision left to the
  matcher. When downloading, at most `--max-models-per-tone N` (default 3, API order) models per tone are
  fetched, so a 168-IR pack is not bulk-downloaded; `--no-download` fetches none.
* `resolve` finds every capture whose `source.provider == "tone3000"` (needs `source.id`, optional
  `source.modelId`), fetches it (cache hit = no network), and sets `file` (absolute cache path),
  `sha256`, `source.modelId`, `url`, `title`, `creator`, `license`. With no `modelId`: if the tone has exactly one
  model it is used; otherwise `resolve` fails (exit 1) listing the model ids and names, unless `--first-model`
  is given. Output goes to `<name>.resolved.json` next to the input (or `-o`); the input is never modified.
  Resolved presets contain machine-specific absolute paths and are git-ignored (`*.resolved.json`).
* Cache: `~/.cache/sawblade/captures/<tone_id>/<model_id>.<nam|wav>` + `meta.json` (override with
  `SAWBLADE_CACHE_DIR`). Files are sha256-verified on every hit.

### License policy

Sawblade is commercial, so only tones licensed `t3k`, `cc-by`, `cc-by-sa`, `cc-by-nd` or `cco` are used.
`cc-by-nc*` (reason `non_commercial_license:<lic>`) and unknown/empty licenses (`unknown_license:<lic>`) are
excluded from the pool even if favorited, and `resolve`/downloads refuse them (also on cache hits). There is
no override flag.

### `--search` (opt-in, commercial)

`pull --search "query"` adds `tones/search` results to the pool. That endpoint is outside TONE3000's
free tier: **a commercial agreement with TONE3000 is required before shipping anything that uses it**
(Sawblade is commercial). It is off by default and uses a separate, tighter client-side rate bucket.

### Rate limits and logging

100 requests/min client-side token bucket; 429 and 502/503/504 are retried with exponential backoff
(honouring `Retry-After`); a 401 triggers one refresh + retry. The `X-Tone3000-Deprecations` response header is
logged at WARNING. Use `-v` for INFO logs.

## Tone check (`sawblade-tonecheck`, phase 1.5 part B)

```
sawblade-tonecheck PRESET.json --di DI.wav [--ref REF.wav] [--ref-channel left|right|mid] [--out DIR]
                   [--tonerender PATH] [--targets docs/tone_targets.json]
sawblade-tonecheck --presets a.json b.json --di DI.wav       # batch: DIR/<preset>/ + DIR/summary.json
sawblade-tonecheck --audio OUT.wav [--di DI.wav] [--ref ...]  # analyse an already-rendered file
```

Implements `docs/tone_targets.json` exactly (LTAS 1/3-oct, activity gate, band groups, rules with tolerances,
metrics). Outputs `report.json` + `report.png` (+ `render.wav`, `tonerender_report.json`). Exit 0 whenever the
analysis ran (rule failures do not change the exit code), 3 on render/analysis error, including a missing or
unreadable `--audio`/`--ref`/`--di` (one-line `error:` message, no traceback).

Method notes: audio is analysed at 48 kHz; Welch segments are used when >= 80 % of their samples are in
active frames; band power integrates the PSD over IEC band edges with fractional edge bins. `lowTightnessMs`:
spectral-flux onsets (STFT 1024/256, 50 Hz-6 kHz, log-magnitude) on the DI, then for each onset the time for the
output's 80-160 Hz energy envelope (Butterworth-4 band-pass, 10 ms mean-square, dB) to fall 20 dB from its peak in
[t-10, t+60] ms; onsets cut off by the next onset (or 2 s) are censored and excluded (counts reported).
`buzz` is flatness of the active-segment mean PSD over 1-3 kHz. `gapNoiseDb` is also reported as rule
`gap_noise` (<= -60, no tolerance). `--ref`: per-band LTAS difference (both normalised to 1 kHz) and
A-weighted error = sqrt(sum(w d^2)/sum(w)), w = 10^(A(fc)/10), bands 80 Hz-8 kHz. Analysis is deterministic
(no random processes).

Addenda: `--ref`/`--ref-channel` repeat (one channel value for all refs, or one per ref); `report.json`
has a `references` list and `report.png` one overlay + difference panel per ref. `gapNoiseDb` is measured on
the DI's gap frames (50 ms frames within 6 dB of the DI's 5th-percentile level), output RMS there relative to
active output RMS; null / rule status `n/a` when fewer than 1 % of frames qualify or no DI is given;
`diNoiseFloorDb` reports that floor (dBFS) for gate calibration. `lowDecayDbPerMs` is the regression slope
(dB/ms) of the 80-160 Hz envelope over [peak+5 ms, min(peak+35 ms, next onset)] (>= 15 ms, else censored); the
causal band-pass transient makes it read ~10-25 % shallower than the true decay, so compare presets, not
absolute values.

Details: the tonerender binary is looked up as `--tonerender`, then `$SAWBLADE_TONERENDER`, then
`build/cli/tonerender` / `build-lead/cli/tonerender` under the current directory (run from the repo root); the
targets file likewise (`--targets`, `$SAWBLADE_TARGETS`, `docs/tone_targets.json` under the current directory).
There is no package-relative fallback. Onset times from the spectral flux are shifted by +5.3 ms (a quarter of the
1024-sample window: the flux peaks about that long before the true onset); accuracy about +-5 ms. `gapNoiseDb`
is also null ("no clear gaps", rule `n/a`) when the DI's noise floor is within 10 dB of its median active frame
level, i.e. a steady DI with no real gaps.

## Core bindings (`sawblade_core`, phase 3.1)

The C++ renderer as a Python extension: the same `renderPreset` path as `tonerender` (bit-identical output),
plus a `CaptureCache` that loads each NAM model / IR once. Spec: `docs/specs/phase3_matcher.md` 3.1.

Build (off by default; needs Python headers, pybind11 is fetched, pinned) from the repo root:

```
cmake -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PYTHON=ON \
      -DPython_EXECUTABLE=$PWD/match/.venv/bin/python
cmake --build build-py            # also builds tonerender, used by the bit-identity tests
ctest --test-dir build-py         # C++ suite + the python_bindings pytest run
```

The module lands in `build-py/python/sawblade_core.<abi>.so`. `sawblade_match.core` finds it: already
importable (`PYTHONPATH`/installed), else `$SAWBLADE_CORE_DIR`, else `<repo>/build-py|build|build-lead/python`.
Use the same interpreter for building and running (the ABI tag is part of the file name).

```python
from sawblade_match.core import render, CaptureCache, PresetError, RenderIOError

cache = CaptureCache()                       # share between renders and threads
y, report = render(preset_dict_or_json, di_float32, 48000, base_dir="presets", cache=cache)
# render_rate="auto"|Hz, out_rate="input"|"render", block=256; report = tonerender --report JSON
# (latencySamples, pathLatency, renderRate, timings, warnings, ...). cache.hits / cache.misses / len(cache).
```

The GIL is released while rendering, so renders from several Python threads run in parallel (a thread pool
sharing one `CaptureCache` is the intended use). The cache keeps parsed models and IRs, not DSP state: each
render still builds fresh NAM state, so changing blend, levels, EQ or NAM gains costs no reload. Entries are
keyed by file path + SHA-256 (re-hashed only when size/mtime change, so a same-size edit with preserved mtime is served stale; `cache.clear()` forces a reload); a preset `sha256` is checked on every use.
Errors: `PresetError` (a `ValueError`) and `RenderIOError` (an `OSError`), both with `.json_path`.
`pytest match/tests/test_core_bindings.py` skips itself when the module is not built.

## Calibrate tone targets (`sawblade-calibrate`, phase 2A)

```
sawblade-calibrate [--original ...mp3] [--cover-mix ...mp3] [--di-l ...wav] [--di-r ...wav]   # defaults: testdata/ paths
                   [--sections 0:12,95:110] [--no-separation] [--stems-dir testdata/stems]
                   [--score RENDER.wav ...] [--targets docs/tone_targets.json] [--out DIR] [--policy loosen-only|tighten]
```

Measures guitar-dominant audio of the original and the cover mix with the tonecheck analysis and writes
`tone_targets.proposed.json`, `calibration_report.md`, `calibration.json` and PNGs to `--out`. It never
modifies `docs/tone_targets.json`. Method 1 (stems) needs `pip install -e 'match[separation]'` (demucs
`htdemucs` + torch; weights are downloaded from dl.fbaipublicfiles.com on first use); if that is not
possible the report says `unavailable: <reason>` and the run continues with method 2 (section selection;
heuristics documented in `sawblade_match/calibrate/sections.py`). The original's vocal exclusion is not
reliable for growled vocals: prefer `--sections`. Proposal policy: `sawblade_match/calibrate/propose.py`.

Method 3 (side channel, always runs): `(L - R) / 2` of the stereo reference, analysed with the tonecheck activity
gate. Assumption: both guitars are double-tracked and hard-panned while bass, kick, snare and lead vocal are
centred, so centred content cancels. Failure modes: any guitar that is centred (or a mono guitar layer) is
cancelled too, and wide stereo reverbs, stereo-miked cymbals/toms and stereo synth/ambience stay in the side
signal. Side is the proposal **basis** whenever stems are unavailable (stems > side > sections).
`--policy loosen-only` (default) changes a rule only when the basis original fails/marginally passes it;
`--policy tighten` sets every threshold to the original +- tolerance (spec behaviour). Rules the original fails
by more than the tolerance are "contradicted": listed with evidence, left unchanged in `rules`.

`--out` defaults to `~/.cache/sawblade/calibration` (outside the repo; `calibration_out/` is also git-ignored if you
choose it). Method 1 is installed with `pip install -e 'match[separation]' -c match/constraints-separation.txt`
(the constraints file pins the resolved transitive set; the PyPI linux torch wheel also pulls ~2.5 GB of CUDA
libraries although inference runs on CPU). The real-demucs test runs only with `SAWBLADE_TEST_DEMUCS=1`.

## Matcher (`sawblade-match`, phase 3.2 + 3.3 A/B)

```
sawblade-match --di Guitar_L.wav [--di-r Guitar_R.wav] --ref REF.mp3 --pool ~/.cache/sawblade/captures/pool_manifest.json
               [--matched left|right|mono] [--offset-ms N] [--ref-channel auto|side|left|right|mid] [--ref-section A:B ...]
               [--stems-dir DIR] [--profile derived|<id>|PATH] [--base-profile swedish_death_hm2] [--prescreen N]
               [--out DIR] [--budget 1.0] [--seed 0] [--excerpt-s 6] [--top-k 3] [--threads 4]
python -m sawblade_match.matcher.known_answer --pool ... --di Guitar_L.wav --out DIR [--seed 1] [--topology blend|single|single2]
python -m sawblade_match.matcher.recall --run RUN_DIR --di ... --ref ... [--matched left] --pool ... --ns 2,3,4,6,9
```

Needs the built `sawblade_core` (see "Core bindings"; `SAWBLADE_CORE_DIR` pins a build) and a pool whose captures are downloaded
(`sawblade-t3k pull`): only downloaded, commercially licensed models are candidates.

* **Slots are gear classes, not titles** (`matcher/classify.py`): pedals are `drive`, `distortion` (HM-2/chainsaw, RAT, DS-1, MT-2...),
  `fuzz`, `preamp` or `pedal_unknown`; amps `amp_low`/`amp_high`; `cab`. Every pedal slot may be "none" and accepts any pedal
  (unknown ones included); every amp slot accepts any amp. Profiles never choose gear: the HM-2 is just a `distortion` capture.
* **Three topologies**, all with one shared cab (live-compatible): `single` [pedal?] -> amp; `single2` pedal1 -> pedal2 -> amp;
  `blend` A [pedal?] -> amp + B [pedal?] -> amp. All are searched; the final choice prefers the simplest topology within 0.1 dB
  of the best loss (single < single2 < blend), then the lighter model set within 0.05 dB, and `result.json` reports the best of
  every topology (`topologies`).
* **Reference**: `--ref-channel auto` uses a cached htdemucs `other` stem (`testdata/stems/`) if present, else the side channel
  `(L-R)/2` (output level +3 dB: two uncorrelated hard-panned guitars). `--matched left` makes the reference a time-aligned pair
  with the DI (STFT term against that mix channel, LTAS target = matching side segment). The DI->reference offset is searched
  within +-3 s (or +-250 ms around `--offset-ms`) and refined (about +-1 ms; distorted renders vs a mix are not sample-exact).
* **Stage 1** (one guitar-dominant 6 s excerpt): every (pedal-or-none, amp) pair is rendered once through the C++ core; because
  the chain after the NAMs is linear, every pair x pair blend is scored from band cross-spectra without another render, singles
  directly; two-pedal chains use the pre-screened top pedals/amps. Top candidates per topology are re-scored with the full loss
  (blend ones after an auto-align probe) and swept over all cab IRs. **Pre-screen** (`prescreen.py`): each capture alone
  (pedals behind two proxy amps) is scored on the excerpt LTAS and the top N per gear class survive. It is applied automatically only
  when the pair product exceeds the pair cap (default 800 pairs; current pool: 703, so the default is the full search);
  `--prescreen N` forces it. Recall vs the full search on the current pool (N per class, kept pedals/amps, blend recall@10):
  see `result.json` of `matcher.recall` runs; N >= 6 keeps the best combo for the original, N = 9 (what the automatic rule picks at
  the cap) recall@10 is 1.0 single / 0.8 blend there and 0.9 / 0.4 for the cover mix. **Stage 2**: seeded CMA-ES (own
  implementation) per topology, blocks linear -> NAM gains -> linear. **Stage 3**: full-length L/R renders with the real chain,
  `sawblade-tonecheck` against the profile on the best and on the starter, clip guard on max(L, R).
* **Loss** weights are in `matcher/loss.py` (A-weighted LTAS error after level-offset removal x1, buzz x0.5/dB, lowDecay x2 per
  dB/ms, STFT x0.25/dB for matched pairs, EQ-gain regulariser x0.02/dB). **Not searched**: gate (DI floor measured on the gate's
  own peak envelope +4 dB, hold 40 ms, release 150 ms, range -50 dB), bus comp (off), alignment (probed once per blend combo,
  written as `manual`), output gain.
* **Profiles** (`profiles/`, schema `sawblade.profile`; see `profiles/README.md`): guardrail rules only, the reference LTAS is the
  target. `--profile derived` (default) derives a profile from the reference's isolated guitars with loosen-only tolerances from the
  rule skeleton of `--base-profile`; written to `<out>/profile.derived.json`; result.json lists the changes and the base rules'
  status on the best render. Hand-written profiles are optional; those other than `swedish_death_hm2` are flagged
  `"calibrated": false`.
* **Determinism**: `--seed` seeds subset sampling and CMA-ES; thread-pool results are order-independent.
* **Output** (`--out`, default `~/.cache/sawblade/match_runs/<timestamp>`, never in the repo): `best.preset.resolved.json`
  (absolute capture paths + TONE3000 `source` ids/modelIds), `best.preset.json` (portable names), `alt1..5`, `result.json` (loss
  breakdown, topology, captures with gear class and size category, offsets, before/after, plan, timings, profile),
  `tonecheck/*`, `render_*.wav`, `listen/*.wav|mp3` (L/R DIs panned, peak-normalised to -1 dBFS; gain in result.json).
  Exported/derived models from TONE3000 captures are for the user's own use only.

Cost model: one 4-NAM render runs at ~0.6x real time per core. The default plan (703 pair renders of a 6.5 s excerpt, 3 + 2 + 1
CMA-ES refined combos, 3 full-length renders) measured 20.7 min (original) and 25.9 min (cover mix) on 4 shared cores;
`--budget` scales every count (`--budget 0.05` ~ 4 min).

## NAM export (`sawblade-export`, phase 4)

```
pip install -e 'match[export]' -c match/constraints-export.txt     # neural-amp-modeler 0.13.0; torch stays 2.5.1 (CPU)
sawblade-export PRESET.resolved.json [--mode nocab|withcab] [--size feather|lite|standard] [--epochs N] [--max-minutes M]
                [--seed 0] [--signal-seed 1] [--threads 4] [--allow-inexact] [--target-esr E] [--out DIR] [--name STEM]
                [--di Guitar_L.wav] [--no-validate]
```

Trains one `.nam` (A1 WaveNet) of the preset, e.g. the matcher's `best.preset.resolved.json`. Default output
`~/.cache/sawblade/exports/<name>-<mode>-<size>-<timestamp>/` (never the repo): `<name>-<mode>-<size>.nam`, `<name>-nocab.ir.wav`
(nocab), `export_report.json`, `validation_renders/*.wav`, `listen/ab_original_then_export.{wav,mp3}`. Exit 0 = done, 2 = refused
(message on stderr), 3 = error. Exported models are derived from TONE3000 captures: **personal use only**.

* **Modes.** `nocab` (default; needs `cab.mode == "shared"`): the model is everything from input gain to the blend, plus the
  output gain; the shared cab IR and the post EQ are *folded* into one IR (below). Requires the bus comp **off** (it sits after the cab
  and is nonlinear): otherwise refused, or with `--allow-inexact` dropped and the error it introduces shows up in the validation
  (the reference keeps the comp). `perPath` presets: `nocab` is refused with "only the with-cab export is exact for studio blends"
  (not overridable). `withcab` always works (cab, post EQ and bus comp are in the model); a bus comp with release > 150 ms is
  refused there.
* **Never trained.** The gate is always bypassed in the training chain and reported (with its original settings) in
  `export_report.json -> plan.bypassed`. Non-bypassed blocks that are not NAM-trainable (unknown type, or the core's
  `namTrainable == false` trait, detected from the core's render warnings) are refused. Captures with a `cc-by-nc*` licence are refused.
* **Folding (nocab).** The IR written next to the model is `cab IR (*) post-EQ impulse response`, obtained by rendering a unit impulse
  through the core's own `cab -> post EQ` (empty paths, blend 0): so IR loading, resampling to 48 kHz, the 2 s truncation and the L2
  normalisation are exactly the chain's, and the latency is already trimmed. Trailing samples below -120 dB (re. peak) are cut. The
  output gain is *not* in the IR: it is a scalar before the cab and is part of the model's target level. **Load the IR without
  loudness/peak normalisation** (mono WAV, 48 kHz, 32-bit float; a loader that normalises or resamples changes the level/tone).
  `levels` (RMS/peak of the training input/output) are in the report and the `.nam`; `input_level_dbu`/`output_level_dbu` are left
  empty (digital chain, no analog reference).
* **Training signal** (`export/signal.py`, `SIGNAL_VERSION 1`, seeded by `--signal-seed`, sha256 recorded). 48 kHz mono, 187.7 s train +
  32 s held-out validation, no gates/reverb/delay/compression: 1 s silence; pink-noise level steps (-48..-12 dBFS RMS, 40 Hz-10 kHz);
  white-noise steps (-45..-12); log sweeps up/down 30 Hz-12 kHz at -24/-12/-3 dBFS peak; then 135 s of synthetic chord-like plucks
  (additive 24-partial tones with inharmonic stretch, pick-noise burst, palm-mute / ringing / tremolo phrases, root A1-A3, power
  chords/octaves/tritones, velocities -30..-1 dBFS, overlapping ring-outs). Blocks are separated by 0.4 s of silence. The validation
  segment uses its own random streams (pink steps, a sweep, 4 phrases, a 1.2 s silent gap). The target is the chain rendered by the
  core at 48 kHz (`sawblade_core.render`, latency trimmed) and cached in `~/.cache/sawblade/export_cache/` (key: preset hash, signal
  hash, core build) so a second size reuses it.
* **Trainer API path.** `nam.train.core.train` only accepts NAM's own standard input files (hash-matched, blip latency calibration)
  and imports `tkinter`; Sawblade drives the trainer's lower layers instead (`nam.data.Dataset` from arrays, `NormalizeJointDatasetOutput`
  -18 dBFS with the export hook that restores the level, `LightningModule` + `pytorch_lightning.Trainer` on CPU, `net.export` with
  `other_metadata`), using the trainer's shipped default recipe (ESR validation loss, MR-STFT 5e-4, Adam 4e-3, ExponentialLR 0.994).
  `tkinter` is stubbed when absent. See `export/train.py`. Sizes are the community A1 WaveNets (two layer arrays, 10 dilations
  1..512, kernel 3, Tanh): feather 8/4 channels (3 637 params), lite 12/6 (7 903 params), standard 16/8 (13 801 params);
  receptive field 4093. **A2:** 0.13.0 trains a packed A2 WaveNet by default (`PackedWaveNet`, `export_container`; the core is built
  with `NAM_ENABLE_A2_FAST`); that path is available in the pin but not enabled here, A1 being what loader pedals play.
* **Determinism.** Everything is seeded (`--seed`: model init + batch order; `--signal-seed`). CPU training repeats bit-for-bit for the
  same seed, thread count, machine and library versions (smoke test); it is not guaranteed across thread counts/BLAS builds. The `.nam`
  carries a date stamp, so its bytes differ between runs.
* **Metadata** (`.nam` `metadata`): `name`, `modeled_by: "Sawblade"`, `gear_type` (`pedal_amp` for nocab, `amp_pedal_cab` for withcab),
  `tone_type: hi_gain`, `training.validation_esr`, NAM's own `loudness`/`gain`, and `sawblade`: preset name + sha256 (canonical JSON without
  machine paths), export mode, exactness, bypassed items, seeds, signal hash, levels, IR file name, the full attribution list (title,
  creator, licence, TONE3000 URL, roles) and `licenceNote`: "Derived from TONE3000 captures; personal use only unless permitted by the
  creators and TONE3000."
* **Validation** (skip with `--no-validate`). The reference is the original chain with only the gate bypassed (what can be trained). The
  held-out segment and a 10 s guitar-dominant excerpt of the cover DI (`testdata/gatecreeper_cover/Guitar_L.wav`, excerpt chosen by the
  matcher's `select_excerpt`, 0.5 s pre-roll dropped) go through (a) the original chain and (b) a preset whose single `nam` block is the
  exported model, with the exported IR (not re-normalised) as the cab for `nocab`; both via `sawblade_core` (this proves the `.nam`
  loads in the core's `NamBlock`, the plugin's engine). Metrics: ESR (broadband, level-sensitive) and the A-weighted 1/3-octave LTAS
  error (tonecheck's `--ref` definition: bands 80 Hz-8 kHz, both spectra normalised at 1 kHz). The DI excerpt is also compared with the
  *gated* original. Acceptance for `standard`: ESR <= 0.02 on the held-out segment and LTAS error <= 0.5 dB on the DI excerpt; the
  report states `accepted` honestly (other sizes are reported, not judged).
* **Listening file.** `listen/ab_original_then_export.mp3`: the DI excerpt through the original chain, 0.8 s gap, then the export
  (RMS-matched to the original; the gain is in the report). The gate is bypassed in both.
