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
