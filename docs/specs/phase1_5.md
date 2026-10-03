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

## Credentials in the cloud container

- Secrets (environment variables): `TONE3000_CLIENT_ID`, `TONE3000_REFRESH_TOKEN`.
- Refresh tokens may rotate: on refresh, persist the newest token to
  `~/.config/sawblade/t3k_tokens.json` (mode 0600) and prefer it over the env seed.
  Warn clearly when the env seed is stale and the user must update the secret.
- `sawblade-t3k login` runs on the **user's own machine**: PKCE (S256) + `state`,
  localhost redirect `http://localhost:3001/callback` (must be registered on the key),
  prints the refresh token with instructions to paste it into the environment secret.

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
   - `login` (above); `whoami`;
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

`sawblade-t3k pull` builds the candidate pool from favorites plus the free-tier `trending`
and `latest` lists, then filters. Defaults (configurable, recorded in the pool manifest):
- architecture **A2** preferred; A1 only if no A2 model exists for the tone;
- **calibrated** models preferred;
- created/updated within the last 18 months (by the API's date fields);
- popularity at or above the 75th percentile of favorites/downloads within the same gear
  type across the fetched set, with an absolute floor; tones without counts are excluded;
- amp tones must be amp-only/DI (no cab); "full rig" models excluded from slot candidates.
First task: inspect the real Tone/Model JSON and map these rules onto the actual fields;
report any rule the API can't support instead of guessing.

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
