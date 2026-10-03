# Phase 1.5 spec — TONE3000 access + tone check

Runs after phase 1 (T4) is accepted. Owner: **match-engineer** (Python, `match/`), reviewed
by **reviewer**. Uses the `tonerender` CLI as a subprocess (pybind11 comes in phase 3).

## Context / constraints (lead)

- TONE3000 API v1, base `https://www.tone3000.com` (docs: tone3000.com/api; integration
  guide: github.com/tone-3000/api). Every endpoint needs an end-user OAuth 2.0 + PKCE token;
  the publishable key `t3k_pub_…` is the OAuth `client_id`. Never use or store a `t3k_cs_…`
  secret key.
- Business: Sawblade is **commercial**. Development and evaluation are free. Until an
  agreement is signed the default candidate pool is the user's own **favorited / created /
  downloaded** tones (bounded list endpoints). `tones/search` is implemented but must be
  opted into explicitly (`--search`), and its docstring says a commercial agreement is
  required before shipping.
- Rate limit: 100 requests/min default; search tighter. Client-side token bucket + retry
  with backoff on 429; retry once on 401 after refresh.
- Pass `architecture` explicitly on model queries (omitting it returns the legacy A1 set).
  Sawblade loads A1 and A2 (NAM core pin supports both) — request both and prefer standard
  size A1 unless configured.
- Log the `X-Tone3000-Deprecations` header when present.
- Attribution: store title, creator, license, tone URL for every capture (preset `source`).

## Credentials in the cloud container (device flow — verified against tone3000.com/api)

- Login uses the **OAuth 2.0 Device Authorization Grant** (RFC 8628), built for headless
  boxes: `POST /api/v1/oauth/device_authorization` (form or JSON: `client_id`, optional
  `scope`, default `read`) → show the user code + `https://www.tone3000.com/activate` →
  poll `POST /api/v1/oauth/token` at the returned interval, handling `authorization_pending`,
  `slow_down`, `expired_token`, `access_denied`. No redirect URI or PKCE in this flow (the
  server rejects them).
- Session = `{access_token, refresh_token, expires_in, token_type}`; refresh with
  `grant_type=refresh_token` (+ `client_id`) ~60 s before expiry, one refresh at a time,
  retry once on 401; 400/401 on refresh = session over → tell the user to re-login.
- Secrets: `TONE3000_CLIENT_ID` (publishable `t3k_pub_…` key) required. Optional
  `TONE3000_REFRESH_TOKEN` seed so a fresh container skips the device login.
- Persist the newest tokens to `~/.config/sawblade/t3k_tokens.json` (0600) and prefer them over
  the env seed. Containers are ephemeral, so `sawblade-t3k login` prints the refresh token
  once with a note that the user may save it as the `TONE3000_REFRESH_TOKEN` secret.
- Never print access tokens; never log Authorization headers.

## A — TONE3000 client + capture cache (`match/sawblade_match/t3k/`)

1. Package layout: `match/pyproject.toml` (Python ≥ 3.11; deps: `httpx`, `numpy`, `scipy`,
   `soundfile`, `matplotlib`; dev: `pytest`, `respx` or a local fake server). Pin versions.
2. `client.py`: token management (above), `get_user()`, `list_favorited()`, `list_created()`,
   `list_downloaded()`, `get_tone(id)`, `list_models(tone_id, architecture=...)`,
   `download_model(model_url, dest)` (Bearer auth; streams to a temp file, then renames),
   `search(...)` (opt-in). Pagination handled. Typed dataclasses for Tone/Model.
3. `cache.py`: `~/.cache/sawblade/captures/<tone_id>/<model_id>.<nam|wav>` plus
   `meta.json` (tone + model JSON, sha256, fetched_at). Cache hits never hit the network.
4. CLI `sawblade-t3k`:
   - `login` (device flow, above); `whoami`;
   - `pull --favorites [--gear amp|pedal|ir ...]` → downloads into the cache, prints a table;
   - `resolve PRESET.json [-o OUT.json]` → for every capture whose `source.provider ==
     "tone3000"` with `id` (+ optional `modelId`), fetch into the cache, set `file` to the
     cached path, fill `sha256`, `title`, `creator`, `license`, `url`.
5. Schema change (lead approves): `source.modelId` (optional string) — which of the tone's
   models (size/architecture variant) is used. Update `docs/PRESET_SCHEMA.md` and the C++
   parser (small dsp-engineer follow-up, reviewed separately).
6. Tests: no network. Fake API covering auth refresh (incl. rotation persisted), 401 →
   refresh → retry, 429 backoff, pagination, deprecation header logging, download with
   Bearer header, cache hit, `resolve` rewriting a preset. Token file permissions 0600.

## A2 — Candidate quality filter (lead rule: newer + well reviewed only)

`sawblade-t3k pull` builds the candidate pool from `tones/favorited` (always included, the
user's own picks) plus the free-tier `trending` and `latest` lists, then filters using the real
Tone fields (`published_at`, `updated_at`, `downloads_count`, `favorites_count`,
`a2_models_count`, `a1_models_count`, `gear`, `license`, `user.is_verified`). Defaults
(configurable, recorded in the pool manifest):
- `a2_models_count > 0` preferred; A1 only if a tone has no A2 models (query models with
  `architecture=2` first, then `1`);
- `published_at` (fallback `updated_at`) within the last 18 months;
- popularity: `favorites_count` and `downloads_count` at or above the 75th percentile within
  the same `gear` across the fetched set, with absolute floors (configurable); favorited
  tones bypass the popularity floor but are flagged when below it;
- slot fit by `gear`: pedal slots ← `pedal`; amp slots ← `amp` (amp-only); IRs ← `cab`
  (format ir). `amp-cab` ("full rig") tones are excluded from slot candidates and kept only as
  references.
- `calibrated` is only a catalog filter in the docs (not a Model field) — record it if the
  JSON exposes it, otherwise skip.

## B — Tone check (`match/sawblade_match/tonecheck/`)

Goal: an objective report of how a rendered preset compares to `docs/TONE_TARGETS.md`
(and, optionally, to a reference recording).

1. Machine-readable targets: `docs/tone_targets.json` (lead writes it from TONE_TARGETS.md;
   band edges, relative-level rules, tolerances).
2. `sawblade-tonecheck PRESET.json --di DI.wav [--ref REF.wav] [--out DIR]`:
   - renders via `tonerender` (path from `--tonerender` or `build/cli/tonerender`);
   - analyses the output: 1/3-octave long-term average spectrum (LTAS, Welch, Hann,
     energy-gated to exclude gate-closed silence), normalized to the 1 kHz band;
   - metrics: per-band target rules pass/fail with margin; "buzz" = spectral flatness
     1–3 kHz; low-end tightness = 80–150 Hz energy decay time after detected onsets;
     crest factor; short-term loudness range; noise floor in gated gaps;
   - `--ref`: same analysis on the reference + per-band LTAS difference (A-weighted error
     80 Hz–8 kHz);
   - outputs `report.json` + `report.png` (LTAS vs. target bands vs. reference).
3. Batch mode: `--presets presets/*.json` → one summary table.
4. Tests: synthetic signals with known spectra (pink noise through known filters) give the
   expected band levels ±0.5 dB; onset/decay metric on synthetic decays; rule evaluation
   unit tests; an end-to-end test with phase-1 golden fixtures (no network).

## Inputs the user provides

- A real DI recording of the riff style (≥ 30 s, 48 kHz, peaking around −6 dBFS), and
  optionally a reference song clip (isolated or stem-separated guitars preferred).
- Favorited candidate captures on TONE3000 per the shopping list in `presets/README.md`.
