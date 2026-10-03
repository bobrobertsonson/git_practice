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
                  [--no-download] [--manifest pool.json] [--cache-dir DIR]
                  [--max-age-months 18] [--percentile 75] [--min-favorites 10] [--min-downloads 200]
                  [--prefer-size standard] [--no-a1-fallback] [--favorites-bypass-recency]
sawblade-t3k resolve presets/chainsaw_body.json -o presets/chainsaw_body.resolved.json
```

* `pull` builds the candidate pool from **favorited** (always) + **trending** + **latest**, applies the
  quality filter (A2 preferred, <= 18 months old, >= per-gear 75th-percentile favorites/downloads with
  absolute floors; favorited tones bypass the popularity floor but are flagged; `amp-cab` rigs are
  references only), downloads the chosen model of each survivor into the cache and writes
  `<cache>/pool_manifest.json` (decisions and reasons for every tone, including exclusions).
* `resolve` finds every capture whose `source.provider == "tone3000"` (needs `source.id`, optional
  `source.modelId`), fetches it (cache hit = no network), and sets `file` (absolute cache path),
  `sha256`, `source.modelId`, `url`, `title`, `creator`, `license`. Without `-o` the preset is rewritten in place.
* Cache: `~/.cache/sawblade/captures/<tone_id>/<model_id>.<nam|wav>` + `meta.json` (override with
  `SAWBLADE_CACHE_DIR`). Files are sha256-verified on every hit.

### `--search` (opt-in, commercial)

`pull --search "query"` adds `tones/search` results to the pool. That endpoint is outside TONE3000's
free tier: **a commercial agreement with TONE3000 is required before shipping anything that uses it**
(Sawblade is commercial). It is off by default and uses a separate, tighter client-side rate bucket.

### Rate limits and logging

100 requests/min client-side token bucket; 429 and 502/503/504 are retried with exponential backoff
(honouring `Retry-After`); a 401 triggers one refresh + retry. The `X-Tone3000-Deprecations` response header is
logged at WARNING. Use `-v` for INFO logs.
