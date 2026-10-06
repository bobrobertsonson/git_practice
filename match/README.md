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

`login --json` (alias of `login --json-events`) and `whoami --json` are the machine-readable forms used by the
plugin. `login --json` prints one JSON object per line on stdout, flushed immediately: a `device_code` event
(`user_code`, `verification_uri`, `verification_uri_complete` or null, `expires_in`), then `logged_in`
(`username`, `display_name`, `id`, `token_file`; the user fields are omitted if the profile fetch fails after a
successful login). `whoami --json` prints one line `{"id", "username", "display_name", "token_file"}`. Failures
in `--json` mode print one `{"error", "code"}` line and exit 1 (4 if not logged in). Nothing but JSON goes to
stdout and the refresh token is never printed (it is only in the token file); plain-text `login` still prints it
for the container workflow.

## Usage

```
sawblade-t3k whoami
sawblade-t3k pull [--favorites] [--gear amp pedal ir] [--no-trending] [--no-latest]
                  [--no-download] [--max-models-per-tone 3] [--manifest pool.json] [--cache-dir DIR]
                  [--max-age-months 18] [--min-favorites 100] [--min-downloads 1000]
                  [--popularity-percentile P] [--keep-favorites-below-floor]
                  [--no-a1-fallback] [--favorites-bypass-recency]
                  [--search QUERY]... [--add-tone ID]... [--force-tone ID]...
sawblade-t3k search QUERY [--gear amp pedal ir] [--limit 25] [--json]
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

### Capture browser commands (spec 8a, used by the plugin)

```
sawblade-t3k models TONE_ID --json
sawblade-t3k fetch TONE_ID [--model MODEL_ID] [--cache-dir DIR] --json
sawblade-t3k list --source favorites|pool [--query Q] [--gear amp pedal ir] [--limit N] [--cache-dir DIR] --json
sawblade-t3k whoami --json
sawblade-t3k login --json-events      # alias: --json
```

With `--json`, stdout is exactly one JSON document (notes/logs go to stderr). Failure: exit 1 and
`{"error": "<message>", "code": "license|auth|not_found|network|error"}` (also for `search --json`).
Without `--json` behaviour is unchanged. Licence policy: `cc-by-nc*` captures are allowed, unknown licences are refused (code `license`); see "License policy" below.

* `models` -> `{"tone_id": int, "architecture": str, "models": [{"model_id": int, "name": str, "size": str|null}]}`
  (A2 then A1 candidates as `pull` picks; `""` and `[]` if none; empty `size` -> `null`).
* `fetch` -> `{"tone_id", "model_id", "path" (absolute), "sha256", "kind": "nam"|"ir", "gear",
  "source": {"provider": "tone3000", "id", "modelId", "url", "title", "creator", "license"}}`
  (`source` = preset `CaptureSource`). The licence is checked before any download; `--model` must be one
  of `models`' ids, else `not_found`; a cache hit downloads nothing.
* `list` -> array of the same records as `search --json` (`tone_id, title, creator, gear, format, license,
  favorites_count, downloads_count, created_at, models_count, a2_models_count, a1_models_count, irs_count,
  sizes, url, passes, status, reasons, flags`). `favorites` uses the API; `pool` reads
  `<cache>/pool_manifest.json` (its included tones, no network; missing/corrupt -> `[]`; fields the
  manifest lacks are `null`).
* `whoami --json` -> `{"id", "username", "display_name"}`.
* `login --json-events` prints JSON lines `{"event":"device_code","verification_uri",
  "verification_uri_complete","user_code","expires_in"}` then `{"event":"logged_in"}`. The refresh token is
  saved by the token store and never printed.

### License policy

Sawblade is a personal, non-commercial project (CLAUDE.md "Capture licensing"), so tones licensed `t3k`, `cc-by`,
`cc-by-sa`, `cc-by-nd`, `cco` and the non-commercial `cc-by-nc`, `cc-by-nc-sa`, `cc-by-nc-nd` are used. A passing `-nc`
tone carries the flag `non_commercial` (anything derived from it is marked non-commercial; exports are for the user's own
use). Unknown/empty licenses (reason `unknown_license:<lic>`) are excluded from the pool even if favorited, and
`resolve`/downloads refuse them (also on cache hits). There is no override flag.

### Search and extra pool sources (opt-in, personal use)

`tones/search` is outside TONE3000's free tier. This is a personal, non-commercial project, so check the
API terms before sharing anything that uses search.

* `sawblade-t3k search QUERY [--gear ...] [--limit N] [--json]` is **read-only** (no manifest, no download).
  It lists tone id, title, creator, licence, gear, favorites/downloads, created date, model count
  (A2/A1 counts, or IR count) and sizes, plus a `PASS`/`FAIL`/`REF` verdict from the same quality filter as
  `pull` (<= 18 months, >= 100 favorites, >= 1000 downloads, A2 preferred, licence allow-list) with the failing
  reasons. The filter flags (`--max-age-months`, `--min-favorites`, ...) apply to the verdict.
* `pull --search QUERY` is repeatable; results are merged with the other sources and de-duplicated by tone id
  (a tone found by several sources lists them all in the manifest).
* `pull --add-tone ID` (repeatable) adds specific tones regardless of source; the manifest source is
  `lead-pick`. They still go through the licence and quality filter. `--force-tone ID` additionally skips the
  recency and popularity checks (flag `forced`); the licence policy is never skipped.
* **Persistent sources:** `~/.config/sawblade/pool_sources.json` (override `SAWBLADE_POOL_SOURCES`),
  `{"searches": ["big muff"], "tones": [12345]}`, is merged into every `pull`; `pull` prints the path. It is
  user state and is not committed. A malformed file is an error, a missing one is empty.

### Rate limits and logging

100 requests/min client-side token bucket; 429 and 502/503/504 are retried with exponential backoff
(honouring `Retry-After`); a 401 triggers one refresh + retry. The `X-Tone3000-Deprecations` response header is
logged at WARNING. Use `-v` for INFO logs.

### `ladder` (gain ladders)

```
sawblade-t3k ladder TONE_ID [--size standard] [--architecture 1|2|custom] --json
```

Lists the tone's models and prints one JSON line `{"tone_id", "size", "rungs": [{"model_id", "gain", "name"}]}`
(ids are strings, rungs sorted by gain) or `"rungs": null` when there is no unambiguous ladder. A ladder exists
only if, among models of the requested size (and architecture; default = what `resolve` would pick, A2 then A1),
every model name yields exactly one gain number (`Gain 6`, `G6`, `gain=6`, `6 gain`, `Drive 7`, `@7`, `G 6.5`),
the names are identical once that number is removed, the gains are distinct and there are at least two. Anything
else is "no ladder" - it never guesses. Errors follow the usual `--json` `{"error","code"}` shape.
Code: `sawblade_match/t3k/ladder.py` (`parse_ladder`, `gain_ladder`).

### `suggest-body` (Task C pool rule)

```
sawblade-t3k suggest-body --a-title "<path A amp title>" [--cache-dir D] --json
```

Offline (reads `<cache>/pool_manifest.json` and the capture cache, no login). Prints one JSON line
`{"tone_id", "model_id", "title", "cached"}` (ids are strings) or `null` (no pool, or no high-gain amp in it). Candidates are the
pool's amp models whose `classify(...)` is `amp_high`. Order: a different amp family from path A first, then
already-cached captures, then pool order. Family key = an alias family found anywhere in the title (5150/5153/6505/evh -> `5150`, recto/rectifier/dual/triple ->
`recto`), else the leftmost other `_HIGH_AMPS` match (so `peavey` stays its own family), else the first alphabetic word; an empty or unknown
A title ranks nobody as "different". Code: `sawblade_match/t3k/suggest.py` (`suggest_body`, `pool_candidates`).

### `pack`, progress lines and exit codes

```
sawblade-t3k pack TONE_ID [--cache-dir D] -o manifest.json [--progress-json]
sawblade-t3k resolve PRESET [-o OUT] [--first-model] [--progress-json]
```

* `pack` downloads every model of an IR tone (one IR per model) into the cache and writes
  `{ "toneId", "title", "creator", "license", "url", "models": [ { "modelId", "name", "file" (absolute), "sha256" } ] }`.
  It refuses tones that are not IR tones. Licence rules are the same as for `resolve`.
* `--progress-json` makes stdout JSON lines only (one per item, flushed); the human summary goes to stderr.
  `pack`: `{"done": i, "total": n, "name": "<model name>"}` per model.
  `resolve`: `{"done": i, "total": n, "capture": "<json path>", "title": "<tone title>"}` per TONE3000 capture.
* **Exit codes (all commands):** `0` ok, `4` not logged in / re-auth required (no stored token, or the API
  rejected it after refresh; run `sawblade-t3k login`), `1` any other error. The plugin relies on 4.

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
has a `references` list and `report.png` one overlay + difference panel per ref. `gapNoiseDb` is measured in the DI's
real silences (phase 3.6): regions where the DI's 10 ms RMS stays below an absolute -50 dBFS for at least 120 ms, ignoring the first 50 ms of each so a ringing tail is not counted; the value is the output
RMS in those regions minus the output RMS over its playing frames, and `gapCount` / `gapTotalS` are reported. Null / rule
status `n/a` when there are fewer than 3 gaps or they total less than 3 s (so on a dense DI with no real silence, e.g. the Gatecreeper cover) or no DI is given (the earlier "quietest 5 % + 6 dB" frames were mostly palm
mutes and ring tails, which a gate never closes on). `diNoiseFloorDb` (5th-percentile 50 ms frame level, dBFS) is reporting only, for gate calibration. `lowDecayDbPerMs` is the regression slope
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
               [--matched left|right|mono] [--offset-ms N] [--ref-clean] [--ref-channel auto|side|left|right|mid] [--ref-section A:B ...]
               [--stems-dir DIR] [--profile derived|<id>|PATH] [--base-profile swedish_death_hm2] [--prescreen N]
               [--out DIR] [--budget 1.0] [--seed 0] [--excerpt-s 6] [--top-k 3] [--threads|--jobs 4]
               [--quick | --thorough] [--progress-json PATH] [--listen]
               [--ablate feel,boost,filters,irsweep,irblend,studio] [--trace-tones ID[,ID...]]
python -m sawblade_match.matcher.known_answer --pool ... --di Guitar_L.wav --out DIR [--seed 1] [--topology blend|single|single2]
python -m sawblade_match.matcher.recall --run RUN_DIR --di ... --ref ... [--matched left] --pool ... --ns 2,3,4,6,9
               [--quick [--coarse-s S]] [--old-pool] [--quick-run QUICK_RUN_DIR]
```

Needs the built `sawblade_core` (see "Core bindings"; `SAWBLADE_CORE_DIR` pins a build) and a pool whose captures are downloaded
(`sawblade-t3k pull`): only downloaded, commercially licensed models are candidates.

* **Slots are gear classes, not titles** (`matcher/classify.py`; to override a capture's class add `"classOverride": "fuzz"` (any valid class of its gear) to its tone or model entry in `pool_manifest.json`): pedals are `drive`, `distortion` (HM-2/chainsaw, RAT, DS-1, MT-2...),
  `fuzz`, `preamp` or `pedal_unknown`; amps `amp_low`/`amp_high`; `cab`. Every pedal slot may be "none" and accepts any pedal
  (unknown ones included); every amp slot accepts any amp. Profiles never choose gear: the HM-2 is just a `distortion` capture.
* **Three topologies**, all with one shared cab (live-compatible): `single` [pedal?] -> amp; `single2` pedal1 -> pedal2 -> amp;
  `blend` A [pedal?] -> amp + B [pedal?] -> amp. All are searched; the final choice prefers the simplest topology within 0.1 dB
  of the best loss (single < single2 < blend), then the lighter model set within 0.05 dB, and `result.json` reports the best of
  every topology (`topologies`).
* **Reference** (phase 3.4, fizz fix): `--ref-channel auto` uses a cached htdemucs `other` stem (`testdata/stems/`) if present: its
  side channel when the guitars are hard-panned (stem side/mid RMS >= -3 dB), else its mid (`calibrate/channels.py:stem_guitar_signal`;
  per-guitar level +3.01 dB for side, 0 for mid). The full-mix side channel `(L-R)/2` is a fallback only (and so is an explicit
  `--ref-channel side`): above 4.5 kHz it is dominated by cymbals, so the LTAS bands above 4.5 kHz are not fitted but get a
  one-sided ceiling (the render may be darker, never brighter; `--ref-hf-limit HZ`, 0 = off; explicit left/right/mid are taken as
  given), and the matched-pair STFT term stops at that limit too (the matched channel is a mix). On a stem basis a **texture term**
  is added: |median spectral flatness 5-10 kHz diff| x 25 + |8-12 kHz level (re 1-3 kHz) diff| x 0.12 per dB over the playing
  segments. `result.json -> reference` records `basis`, `stemChannel`, `bandLimitHz`, `textureTerm`, and the loss breakdown has `tex`.
  Stage 2 also searches a post-EQ high shelf (3-7 kHz, -8..0 dB) and a post-EQ low-pass (5-12 kHz, 12 dB/oct; 12 kHz = off).
  `sawblade-tonecheck` reports `fizz_texture` (flatness 5-10 kHz ceiling; value **pending lead approval**, reported as n/a until
  `metrics.fizzTexture.target` is set in the targets file) and the `fizzTexture` metric. The side-channel (+3 dB) basis is the
  fallback described above, otherwise unchanged. An explicit full-mix `left`/`right`/`mid` channel gets no HF limit, and the run
  notes and logs a suggestion to pass `--ref-hf-limit 4500`. `--matched left` makes the reference a time-aligned pair
  with the DI (STFT term against that mix channel, LTAS target = matching side segment). The DI->reference offset is searched
  within +-3 s (or only +-20 ms around `--offset-ms`, in both the excerpt and the final refinement) and refined (about +-1 ms; distorted renders vs a mix are not sample-exact).
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
  `sawblade-tonecheck` against the profile on the best and on the **generic starter baseline** (first amp + first cab of the pool, no pedals, no EQ; class-agnostic, it is also the render used for the first offset refinement and the 'before' numbers), clip guard on max(L, R).
* **Clean (isolated) references** (v0.4M): `--matched mono` (or `--ref-clean`, e.g. with `--matched left` on a stereo amp print) declares the
  reference file an isolated guitar track, not a mix. Its own signal is then the target (`ref-channel auto` = the matched channel, no
  stem lookup), there is **no HF limit** (nothing but the guitar above 5 kHz, so the LTAS is fitted up to 8 kHz and the STFT term is
  not cut at 4.5 kHz) and the fizz feel term is on. Before v0.4M a mono `--matched mono` file took the full-mix fallback (basis `mid`,
  4.5 kHz ceiling, fizz not measurable). A matched channel that is a full mix keeps the limit/fizz-off behaviour; `result.json ->
  reference.clean` and `referenceTarget.feel.fizzOn` say which applies.
* **Feel term** (v0.4M, `matcher/feel.py`, `feelTerms` in every `breakdown`): the LTAS finds the average spectrum, this finds how the tone
  behaves. `feel = 0.5 tight + 0.5 fizz + 0.25 polish` added to the total (initial weights, tuned in Task D). *tight*: per-note 60-250 Hz
  12 dB decay time (t12) and 40-120 ms sustain after the DI's palm-muted chugs (>= 3, else all notes; one-sided: floppier than the
  reference counts fully, tighter half), t12 / 20 ms + sustain / 3 dB. *fizz*: per 2048-pt frame 5-12 kHz re 1-4 kHz (dB), 5-10 kHz flatness
  and 5-12 kHz envelope modulation, W1 distance of the distributions / (1.5 dB, 0.03, 0.1). *polish*: W1 of the spectral flux (/ 0.5 dB) and
  of the per-400 ms crest (/ 1.5 dB) plus the inter-note floor re the active level (one-sided, / 6 dB). All gain invariant. Terms without
  enough data (< 3 notes, < 100 ms of DI gaps, ...) are dropped and listed in `feelTerms.dropped`. Without `--matched` the reference's own
  features are compared as distributions at half weight (no floor). Stage 1's pair x pair blend screen stays LTAS-only; every full-loss
  evaluation (re-score, cab sweep, stage 2, finals) includes it.
* **Loss** weights are in `matcher/loss.py` (A-weighted LTAS error after level-offset removal x1, buzz x0.5/dB, lowDecay x2 per
  dB/ms, STFT x0.25/dB for matched pairs, EQ-gain regulariser x0.02/dB). **Not searched by the optimiser**: gate (starts at the DI
  floor measured on the gate's own peak envelope +4 dB, hold 40 ms, release 150 ms, range -50 dB; matched to the reference after
  stage 2, see below), bus comp (off), alignment (probed once per blend combo, written as `manual`), output gain.
* **Search-space additions, all always on** (v0.4M Task B / B2.2, `--ablate` switches each one off for on/off pairs):
  * **Tight boost** (`Combo.boost`, `matcher/space.py`): every `single` combo re-scored in stage 1 also competes with the modeled
    `pedal.ts` ("green overdrive", slot `boost`, model version 1) directly in front of the amp (after any pedal). Its knobs are in
    stage 2's NAM-gain group: `boost.drive` 0-3, `boost.level` 6-10, `boost.tone` 3-8 (defaults 1 / 8 / 5). Stage 2 always refines the
    best boost variant and the best plain single. It costs like one extra block: it only wins if it beats the best plain single
    candidate by more than 0.1 dB (`choose`). The modeled pedal adds 50 samples of latency, which the renderer already advances out of the
    output, so the matcher's core stays sample-aligned. `result.json -> tightBoost {tried, refined, won, params, bestBoostLoss,
    bestPlainSingleLoss, occamDb, ablated}`; every candidate row has `tightBoost`. Single-path `single2` chains and blend paths get none (no variant there yet).
  * **Post-cab filters**: `post.hp` 60-140 Hz and `post.lp2` 6-11 kHz after the shared cab, each with a discrete slope parameter
    (`post.hp_slope`, `post.lp2_slope`: < 0.5 = 12 dB/oct, >= 0.5 = 24 dB/oct = two cascaded biquads with the 4th-order Butterworth Qs
    0.541 / 1.307). Off at the range edge (hp 60 Hz, lp2 11 kHz: the band is omitted, whatever the slope); they are not in the EQ-gain
    regulariser. The slopes are not CMA-ES dimensions: the frequencies are searched at 12 dB/oct, then each filter is tried at 24 dB/oct
    (`refine.pick_slopes`). The existing `post.lp` roll-off (5-12 kHz) stays; `post_filters_from_eq` reads the filters back from a preset
    (a single 12 dB low-pass cannot be told from `post.lp`). `result.json -> postFilters`.
  * **Cab breadth** (`matcher/cabsweep.py`): after stage 2, the top 3 refined candidates per topology are scored with **every** cab of the pool
    (full loss, the NAM cores come from the engine memo, so each IR costs two linear renders + the loss). When another cab wins, the last
    linear CMA-ES block is re-run on it (`refine.relinear`). `result.json -> cabSweep {poolCabs, candidates[...irs]}` lists every IR's
    loss, best / worst and whether the cab changed. `cab_sweep()` is a separate function with the contract candidate + cabs -> loss rows so the
    analytic IR screen (B3) can replace it for large pools.
  * **Gate matched to the reference** (`matcher/gatesweep.py`): on the final chain the threshold (DI floor + 4, 8, 12, 16, 20 dB) x release
    (80, 150, 250 ms) grid is rendered (15 NAM cores per path; the gate is part of the core memo key) and the cell with the lowest feel
    `floor` term (inter-note level re the reference's) is picked, subject to the A-weighted LTAS error rising by at most 0.05 dB and the
    tightness term not getting worse than at the default cell (4 dB, 150 ms), which is part of the grid. Without a matched pair the
    reference's own inter-note floor is the target (`reference_floor_db`; a mix without real silence has none and the sweep is skipped).
    `result.json -> gateSweep` (whole grid, baseline, picked, constraints), `gateDefault`, `gateFinal`. The final preset carries the picked gate.
  * **Why a DI's `gap_noise` stays high** (synthetic diagnosis): the fixed gate (floor + 4 dB,
    hold 40 ms) sits inside the DI's own noise-peak statistics and never closes; the sweep fixes that. What is left depends on the gate
    hold / range (not swept) and on what follows the gate (a high-gain chain and the IR tail).
* **`--ablate LIST`** (`feel, boost, filters, irsweep, irblend, studio`): switches suspects off for on/off pairs; `result.json -> ablate` echoes the
  list. `feel`: no feel term in the search's loss (it is still measured for the report and the gate sweep); `boost`: no boost variants;
  `filters`: no post-cab hp/lp2; `irsweep`: only the stage-1 cab sweep (the pre-v0.4M behaviour). `irblend` and `studio` are accepted and echoed
  but are no-ops until the two-IR blend (B2.1) and studio processing (B2.3) land. **`--trace-tones ID[,ID...]`** explains TONE3000 tones in
  `result.json -> trace[id]`: in the manifest / downloaded, models, gear class, pre-screen score / rank in class / survived, best stage-1
  pair (rank, LTAS error, best blend), its best candidate loss as the amp (the refined one, or the winner's pedals + cab + EQ with this amp
  rendered once and one linear block), and `vsWinner`: the weighted loss terms minus the winner's, largest first, with a one-line `why`. Cab tones
  list their IRs from the winner's cab sweep.
* **Profiles** (`profiles/`, schema `sawblade.profile`; see `profiles/README.md`): guardrail rules only, the reference LTAS is the
  target. `--profile derived` (default) derives a profile from the reference's isolated guitars with loosen-only tolerances from the
  rule skeleton of `--base-profile`; written to `<out>/profile.derived.json`; result.json lists the changes and the base rules'
  status on the best render. Hand-written profiles are optional; those other than `swedish_death_hm2` are flagged
  `"calibrated": false`.
* **Determinism**: `--seed` seeds subset sampling and CMA-ES; thread-pool results are order-independent.
* **Output** (`--out`, default `~/.cache/sawblade/match_runs/<timestamp>`, never in the repo): `best.preset.resolved.json`
  (absolute capture paths + TONE3000 `source` ids/modelIds), `best.preset.json` (portable names), `alt1..5`, `result.json` (loss
  breakdown, topology, captures with gear class and size category, offsets, before/after, plan, timings, profile),
  `tonecheck/*`, `render_*.wav`, and with `--listen` the `listen/` folder (without `--listen` no listening files are made, in
  either mode (intentional default change); the R render is still made with `--di-r`, since the clip guard uses max(L, R)).
  `listen/` is **loudness-matched, never peak-normalised** (BS.1770-4 integrated LUFS, `matcher/loudness.py`; float WAV so nothing
  clips): `ref.wav`, `render.wav` and (when the starter was rendered) `before.wav` are the same 30 s guitar-dominant section
  (the DI's densest window, moved into the reference when needed; the whole overlap if shorter), 48 kHz, mono for a matched
  pair, time-aligned with the final found offset (the starter's offset search when no final one exists), the render and the
  before each scaled to the reference's integrated loudness over that section. With a reference that is not a matched pair there
  is no alignment: `ref.wav` is the reference's own guitar-isolation section. The full-length stereo `cover_guitars_L-R.wav` /
  `guitar_L_mono.wav` (+ `.mp3`, attenuated only for the MP3 if it would clip; `mp3GainDb`) carries the render's gain.
  `result.json -> listening`: `section [s0, s1]`, `lufsRef`, `lufsRenderRaw`, `gainDb`, `lufsBefore`, `gainBeforeDb`, `offsetMs`,
  `truePeakDb {ref, render, before}` (4x oversampled), `fullLengthGainDb`; the run log says how much louder/quieter the render
  was than the reference before matching (a large number means the level match, not just the tone, is off).
  Exported/derived models from TONE3000 captures are for the user's own use only.

Cost model: one 4-NAM render runs at ~0.6x real time per core. The default plan (703 pair renders of a 6.5 s excerpt, 3 + 2 + 1
CMA-ES refined combos, 3 full-length renders) measured 20.7 min (original) and 25.9 min (cover mix) on 4 shared cores;
`--budget` scales every count (`--budget 0.05` ~ 4 min).

### Quick mode, progress file, timings (phase 6b)

`--thorough` (default) is the search above and the documented final match. `--quick` is PREVIEW quality (about 5x less CPU,
within 1 dB A-weighted of thorough on the two test references; the original 0.15 dB target was not met: 4 pedals per class in
the pre-screen drops the cover's HM-2+JMP winner and the excerpt loss predicts the full-song error poorly; see
`docs/specs/phase6b_matcher_speed_REPORT.md`). It omits two-pedal chains (`single2`) unless `--top-k 4`. It is the fast preset (spec `docs/specs/phase6b_matcher_speed.md`); the
levers that were measured and kept (`Plan.quick`, `matcher/run.py`):

* **Capped pre-screen, always on** (`capped_prescreen`): per-class quotas, pedals 4 per class and the rest of the pair cap (380) spent
  on amps, scored on a 2.5 s window (a 1.5 s window made the ranking unstable: single/blend recall collapsed on the old pool);
* **two-pass pair screen** (`coarse_s`, `coarse_keep`): every surviving pair is rendered on the coarse 2.5 s window (the densest part
  of the excerpt), singles and pair x pair blends are scored from it, and only the best 8 % (min 32; singles, and both members of
  the best blends) are rendered on the full 6 s excerpt and re-scored;
* **NAM-core memo** (`Engine.core_blocks`): a chain rendered by the pre-screen on an excerpt is not rendered again by the pair
  pass (bit-identical, LRU 400 MB; `result.json -> timings.coreCache`);
* **stage 2** (`gain_s`, `short_linear`, `patience*`): the first linear block and the gain block run on a 2.5 s window, the last
  linear block on the full excerpt; CMA-ES stops on a plateau (`cma.minimize(patience, tol)`); fewer re-scores (16/12) and cab
  sweeps (3/2); refined combos: 2 blend + 2 single, no two-pedal chains (`--top-k 4` brings one back);
* **no full-length "before" render and no listening files** unless `--listen` (the "before" is then `null`; the
  excerpt loss of the starter is still in `result.json -> starter`);
* tried and dropped: a blend-aware pre-screen (`Plan.blend_aware`, extras ranked by the best blend a capture takes part in: no recall
  gain at the same pair budget, kept as an option) and a process pool (`--jobs` is an alias of `--threads`: the C++ renderer
  releases the GIL and scales with the cores, so processes would only copy the models).

`result.json -> timings` has the per-stage seconds (reference load, excerpt, starter + offset, target + profile, stage 1 broken
down into pre-screen / coarse pass / full pass / scoring / re-score / two-pedal chains, stage 2 per combo, full-length renders,
tonecheck, final offsets, outputs, listening, `cpuSeconds`), and `mode`.

`--progress-json PATH` writes `{"stage", "fraction", "etaSeconds", "bestErrorDb", "message"}` (plus `elapsedSeconds`, `done`)
atomically (temp file + rename) on every state change and at least every 0.5 s; stages `prepare, prescreen, screen, refine,
finalize`, weighted by the measured time profile of the mode; `bestErrorDb` is the best LTAS error so far, then the final
A-weighted error; on failure `message` starts with `error:`.

## NAM export (`sawblade-export`, phase 4)

```
pip install -e 'match[export]' -c match/constraints-export.txt     # neural-amp-modeler 0.13.0; torch stays 2.5.1 (CPU)
sawblade-export PRESET.resolved.json [--mode nocab|withcab] [--size feather|lite|standard] [--epochs N] [--max-minutes M]
                [--seed 0] [--signal-seed 1] [--threads 4] [--allow-inexact] [--target-esr E] [--out DIR] [--name STEM]
                [--di Guitar_L.wav|builtin] [--no-validate] [--resume DIR|auto] [--keep-scratch]
                [--exports-root DIR] [--progress-json PATH] [--require-accept]
```

Trains one `.nam` (A1 WaveNet) of the preset, e.g. the matcher's `best.preset.resolved.json`. Default output
`~/.cache/sawblade/exports/<name>-<mode>-<size>-<timestamp>/` (never the repo): `<name>-<mode>-<size>.nam`, `<name>-nocab.ir.wav`
(nocab), `export_report.json`, `validation_renders/*.wav`, `listen/ab_original_then_export.{wav,mp3}`. Exit codes: 0 = finished (acceptance met, or not
judged for feather/lite), 2 = trained but acceptance NOT MET (only with `--require-accept`; the files and the full report are still
written), 1 = refused or error (message on stderr), 130 = interrupted.
`--exports-root DIR` puts the output directory at `DIR/<name>-<mode>-<size>-<ts>` (`--out` overrides it; `--resume auto` searches it).
When any capture is `cc-by-nc*`, the file stem (`--name` or the preset slug) gets `-nc` (`.nam`, IR and directory names).
`--di builtin` (also the fallback when the default test DI is missing, with a log line) validates and builds the listening file from
an excerpt of the built-in held-out signal; the report says `diExcerpt.excerpt.file = "builtin"`.
`--progress-json PATH` writes `{stage, fraction, etaSeconds, epoch, epochs, bestEsr, message, outDir, resumable, elapsedSeconds}`
atomically (stages plan, signal, render, train, validate, done / cancelled / error; `fraction` never decreases: train is 0.10-0.90,
validate 0.90-0.99; at most once a second while training, immediately on stage changes and epoch ends, once more at exit).
SIGINT (Ctrl-C) cancels: the trainer stops at the end of the current batch, the partial epoch writes no checkpoint (the last
complete epoch stays, `progress.json` gets `"interrupted": true`), validation and the report are skipped, the progress file says
`cancelled` with `resumable: true`, exit 130; continue with `--resume`. A second Ctrl-C raises the usual KeyboardInterrupt. Exported models are derived from TONE3000 captures: **personal use only**.

* **Modes.** `nocab` (default; needs `cab.mode == "shared"`): the model is everything from input gain to the blend, plus the
  output gain; the shared cab IR and the post EQ are *folded* into one IR (below). Requires the bus comp **off** (it sits after the cab
  and is nonlinear): otherwise refused, or with `--allow-inexact` dropped and the error it introduces shows up in the validation
  (the reference keeps the comp). `perPath` presets: `nocab` is refused with "only the with-cab export is exact for studio blends"
  (not overridable). `withcab` always works (cab, post EQ and bus comp are in the model); a bus comp with release > 150 ms is
  refused there.
* **Never trained.** The gate is always bypassed in the training chain and reported (with its original settings) in
  `export_report.json -> plan.bypassed`. Non-bypassed blocks that are not NAM-trainable (unknown type, or the core's
  `namTrainable == false` trait, detected from the core's render warnings) are refused. `cc-by-nc*` captures are allowed (policy in CLAUDE.md): attribution entries and the `.nam` `sawblade` block get `nonCommercial: true` and the licence note adds NON-COMMERCIAL plus the capture names (it appears in the `.nam`, `export_report.json` and the CLI's final print). CLAUDE.md supersedes the phase 4 spec's note string; the note now reads "Derived from TONE3000 captures; for the user's personal use only; sharing needs permission from the creators and TONE3000."
* **Export notes** (`export/notes.py`). Every export writes `<name>.export_notes.txt` next to the `.nam` (`<name>` = the `.nam`
  stem) and an `exportNotes` block into `export_report.json`: every enabled stage of the preset that is **not in the trained
  model**, in signal order (gate -> [pre-NAM] -> NAM -> cab IR -> post EQ -> bus comp), so it can be rebuilt around a loader
  pedal. Shape: `{"version", "mode", "file", "stages": [{"stage": "gate"|"cab"|"postEq"|"busComp", "position": "before NAM"|"after
  NAM", "inModel": false, "settings": {...}, "hardware": "<one line>"}], "loaderOrder": "<one line>", "message"?}`. The gate is
  always listed (before the NAM, keyed on the DI = put it first: threshold/close dB, attack/hold/release ms, range dB, expander
  ratio, key HPF). `nocab` additionally lists the cab (shared IR, or for `irMix` both IRs, mix, `offsetSamplesB`/`invertB` (v0.4M B2.1 core hook) when
  present, with file/title/creator/licence/mic when known), the post EQ (type, Hz, dB, Q; HP/LP 12 dB/oct; both are already
  folded into the exported `.ir.wav`, which the notes point to) and the bus comp (threshold dB re the chain's pre-headroom level
  and re 0 dBFS at the exported output, which includes the output gain the model already has; ratio, attack/release ms, knee dB,
  make-up dB), the last only with `--allow-inexact`. `withcab` lists only the gate (and a bypassed bus comp, if the plan ever
  drops one). Nothing outside the model: the file says "Nothing to add". The output gain is never listed (it is inside the
  model). The `.txt` ends with the licence note (NON-COMMERCIAL included).
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
  -18 dBFS with the export hook that restores the level, `LightningModule` + `pytorch_lightning.Trainer` (`--device auto|cpu|cuda|mps`, default auto = cuda > mps > cpu; the model is moved to CPU before export; the device is in the report), `net.export` with
  `other_metadata`), using the loss/optimiser/scheduler recipe from the trainer's **A2 packed-model default config**, applied here to an A1 net (ESR validation loss, MR-STFT 5e-4, Adam 4e-3, ExponentialLR 0.994 unless annealed, see below).
  `tkinter` is stubbed when absent. See `export/train.py`. Sizes are **Sawblade's own approximations of the community feather/lite/standard A1 sizes, recalled from memory, not NAM's official presets** (two layer arrays, 10 dilations
  1..512, kernel 3, Tanh): feather 8/4 channels (3 637 params), lite 12/6 (7 903 params), standard 16/8 (13 801 params);
  receptive field 4093. **A2:** 0.13.0 trains a packed A2 WaveNet by default (`PackedWaveNet`, `export_container`; the core is built
  with `NAM_ENABLE_A2_FAST`); that path is available in the pin but not enabled here, A1 being what loader pedals play.
* **Resume (phase 4.1).** After every epoch the trainer writes `<out>/checkpoint/` atomically (temp file + rename): `last.ckpt`
  (Lightning checkpoint: model, optimiser, scheduler, epoch, plus the training history, elapsed time and the torch/numpy/python/
  DataLoader RNG states), `best.ckpt` (best-so-far model) and `progress.json` (epoch, best val ESR, elapsed training seconds, preset /
  training-signal / validation-signal sha256, mode, size, seed, batch size, epochs, lr gamma; written last). An interrupted run is
  continued with the same command line plus `--resume`:
  `sawblade-export PRESET --mode nocab --size standard --epochs 250 --max-minutes 420 ... --resume <out dir>` (the run's output
  directory, e.g. `~/.cache/sawblade/exports/<name>-<mode>-<size>-<ts>`) or `--resume auto` (the newest unfinished run in the
  exports dir with the same preset, mode, size, signal and training settings; otherwise it starts fresh; either way the CLI prints
  which happened). It is refused (exit 1, message on stderr) if the preset sha, signal sha, size, mode, seed, batch size, epochs or
  lr gamma differ, or if `--out` names another directory. `--max-minutes` counts the training time of all sessions together. On CPU
  with the same thread count a resumed run is bit-identical to an uninterrupted one (test). The checkpoint dir is removed after a
  successful export; `--keep-scratch` keeps it (marked complete, so `auto` never picks it). Runs started before 4.1 have no checkpoint
  and cannot be resumed.
* **Determinism.** Everything is seeded (`--seed`: model init + batch order; `--signal-seed`). CPU training repeats bit-for-bit for the
  same seed, thread count, machine and library versions (smoke test); it is not guaranteed across thread counts/BLAS builds. The `.nam`
  carries a date stamp, so its bytes differ between runs.
* **Metadata** (`.nam` `metadata`): `name`, `modeled_by: "Sawblade"`, `gear_type` (`pedal_amp` for nocab, `amp_pedal_cab` for withcab),
  `tone_type: hi_gain`, `training.validation_esr` (the trainer's best validation ESR on level-normalised data, model output only; `validation_esr_source` says so), NAM's own `loudness`/`gain`, and `sawblade`: preset name + sha256 (canonical JSON without
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
  report has `validation.acceptance.status` (`met` / `NOT MET` / `not judged (non-standard size)`) and a one-line summary, which the CLI prints; `--require-accept` exits 2 when the status is `NOT MET`.
* **Listening file.** `listen/ab_original_then_export.mp3`: the DI excerpt through the original chain, 0.8 s gap, then the export
  (RMS-matched to the original; the gain is in the report). The gate is bypassed in both.
* **Budget and measured results (CPU only).** Defaults: feather 40 epochs / 15 min, lite 30 epochs / 30 min, standard 22 epochs /
  55 min (whichever comes first; the learning rate decays to 5 % over `--epochs`, `--lr-gamma` overrides), 188 s of training audio =
  1099 datums = 68 steps of batch 16 per epoch. Measured on the shared 4-core box with the matcher's best `original` preset
  (nocab, seed 0): uncontended ~53 s/epoch for lite (4 threads); with other jobs on the cores 100-170 s/epoch for lite and 100-165
  s/epoch for standard (3 threads, `OMP_WAIT_POLICY=PASSIVE`: with spinning OpenMP threads an epoch stalled for > 6 min as soon as
  another job took cores). At that budget the models are **under-trained**: lite 14 epochs (30.2 min): trainer validation ESR 0.359,
  held-out ESR 0.455 after the IR, DI-excerpt LTAS error 3.74 dB; standard 22 epochs (48.8 min): validation ESR 0.457, held-out ESR
  0.404, DI-excerpt ESR 0.517, LTAS error 4.23 dB. The spec's acceptance (standard: ESR <= 0.02, LTAS <= 0.5 dB) is **not met**; NAM
  models of heavy two-path high-gain chains normally need hundreds of epochs on a GPU. The same code trains on a GPU box unchanged
  with `--device cuda` or `--device mps` (default auto).
  `--batch-size 4` gave a better ESR per minute in a 10-minute trial (0.51 vs ~0.58 at the same time) but did not change the picture.

## Separation models (`sawblade-models`)

The C++ `ModelStore` (stem separation, phase 5.1b) expects the ONNX core of the separation models in the
per-user models dir. Nothing is committed or bundled; this tool builds them locally.

One-time venv setup (from the repo root). torch comes from PyPI (CUDA wheels, ~3 GB download; CPU is all that
is used). The `models` extra pins torch 2.5.1, demucs 4.0.1, onnx 1.23.1 and onnxruntime 1.30.0; the constraints
file pins the resolved set (linux x86_64):

```
python3.11 -m venv match/.venv
match/.venv/bin/pip install -e 'match[models]' -c match/constraints-separation.txt
match/.venv/bin/pip cache purge          # optional: reclaim pip's download cache
```

Then:

```
match/.venv/bin/sawblade-models fetch --model htdemucs_6s     # default; also: htdemucs | all
match/.venv/bin/sawblade-models status
```

* Models dir: `$SAWBLADE_MODELS_DIR`, else macOS `~/Library/Application Support/Sawblade/models/`, else Linux
  `$XDG_DATA_HOME/sawblade/models/` or `~/.local/share/sawblade/models/`. `--dir` overrides for one call.
* `fetch` downloads the official checkpoint from dl.fbaipublicfiles.com (sha256 pinned; cached under
  `<dir>/checkpoints/`, skipped when present and correct, partial downloads deleted on failure), exports
  `<dir>/<id>-core-opset17.onnx` (torch legacy exporter, opset 17, fixed shapes), checks onnxruntime against torch
  on a seeded synthetic segment (fails if the residual is worse than -60 dB), and only then writes
  `<id>-core-opset17.onnx.sha256`. The export is reproducible; its hash is compared with the pin from
  `spikes/separator/RESULTS.md` (a mismatch on another platform is a warning, not a failure).
* Never commit the checkpoints or ONNX files.
