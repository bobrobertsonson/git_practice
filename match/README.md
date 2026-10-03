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

## Matcher (`sawblade-match`, phase 3.2)

```
sawblade-match --di Guitar_L.wav [--di-r Guitar_R.wav] --ref REF.mp3 --pool ~/.cache/sawblade/captures/pool_manifest.json
               [--matched left|right|mono] [--offset-ms N] [--ref-channel auto|side|left|right|mid] [--ref-section A:B ...]
               [--stems-dir DIR] [--out DIR] [--budget 1.0] [--seed 0] [--excerpt-s 6] [--top-k 3] [--threads 4]
python -m sawblade_match.matcher.known_answer --pool ... --di Guitar_L.wav --out DIR [--seed 1]   # acceptance (a), real captures
```

Needs the built `sawblade_core` (see "Core bindings"; set `SAWBLADE_CORE_DIR` to pin a build) and a pool whose captures are
downloaded (`sawblade-t3k pull`): only downloaded, commercially licensed models are candidates. Slots: pedals titled
"HM-2" -> path A pedal; other pedals (plus "none") -> path B boost; amps -> both amp slots (saw amp sampling prefers
low/medium-gain titles, no hard filter); cabs -> one shared IR (live-compatible).

* **Reference**: `--ref-channel auto` uses a cached htdemucs `other` stem (`testdata/stems/`, from `sawblade-calibrate`) if
  present, else the side channel `(L-R)/2` (output level is then set +3 dB, two uncorrelated hard-panned guitars). With
  `--matched left` the reference is a time-aligned pair with the DI (cover mix: 190 ms L / 175 ms R by default): the LTAS
  target is the matching segment of the side channel and a multi-resolution STFT term is added against that mix channel; the
  DI->mix offset is refined to the sample by cross-correlating the rendered excerpt with the mix channel (envelope, then
  band-limited PHAT waveform correlation) and re-measured on the final full-length renders (reported in `result.json`).
* **Stage 1** (one guitar-dominant excerpt, 6 s, chosen from DI activity): each A pair (HM-2, saw amp) and B pair (boost or
  none, body amp) is rendered once through the C++ core; because the chain after the NAMs is linear, every A x B combination
  is scored from band cross-spectra without another render (blend grid 0.15..0.85), top pairs get an auto-align probe and the
  full loss, and the best are re-scored with every cab IR. If the pair product exceeds the budget a seeded random subset of
  pairs is rendered (default 200 A, 150 B pairs). **Stage 2**: seeded CMA-ES (own implementation, `matcher/cma.py`) on the
  top-K combos, blocks linear -> NAM gains -> linear. **Stage 3**: full-length renders with the real chain (preset, DI L and R),
  `sawblade-tonecheck` on the best and on the starter preset, clip guard (full-length peak <= -1 dBFS).
* **Loss** weights are documented in `matcher/loss.py` (A-weighted LTAS error after level-offset removal x1, buzz x0.5/dB,
  lowDecay x2 per dB/ms, STFT x0.25/dB for matched pairs, EQ-gain regulariser x0.02/dB). Smaller total NAM size wins within
  0.3 dB; combos that clip at the matched level are rejected.
* **Not searched**: gate (fixed from the DI noise floor +4 dB, hold 40 ms, release 150 ms, range -50 dB), A pre-EQ (HP 90 Hz,
  as the starter), bus comp (off), alignment (resolved once per combo then written as `manual`).
* **Determinism**: `--seed` seeds subset sampling and CMA-ES; thread-pool results are order-independent. All seeds are in
  `result.json`.
* **Output** (`--out`, default `~/.cache/sawblade/match_runs/<timestamp>`, never in the repo): `best.preset.resolved.json`
  (absolute capture paths + TONE3000 `source` ids/modelIds), `best.preset.json` (portable file names), `alt1..5`,
  `result.json` (loss breakdown, captures, offsets, before/after, plan, timings), `tonecheck/*` (report.json/png + rule table),
  `render_*.wav`, `listen/*.wav|mp3` (L/R DIs panned, peak-normalised to -1 dBFS; the normalisation gain is in result.json).
  Exported/derived models from TONE3000 captures are for the user's own use only.

Cost model: one 4-NAM render runs at ~0.6x real time per core. The default budget (stage 1: 350 path-pair renders of a 6.5 s
excerpt; stage 2: 3 combos x ~130 NAM-gain evaluations + ~1500 cheap linear evaluations; stage 3: 3 full-length renders)
targets <= 45 min on 4 cores; `--budget` scales every count (e.g. `--budget 0.1` for a quick check).
