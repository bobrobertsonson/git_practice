# Phase 4 spec — NAM export ("one model of the blend for a loader pedal")

Owner: match-engineer → reviewer. Python in `match/sawblade_match/export/`, CLI `sawblade-export`.
Uses the core via `sawblade_core` (render) and the NAM trainer `neural-amp-modeler` (MIT, pin a
version, optional extra `match[export]` — torch already pinned in `match[separation]`; reuse it).

## Goal
Given a resolved preset (e.g. the matcher's `best.preset.resolved.json`), train a single `.nam`
model that reproduces the chain, so a NAM loader pedal plays the Sawblade blend.

## Modes (from the original brief)
- `nocab` (default; requires `cab.mode == "shared"`, i.e. live-compatible): model = everything
  from input gain to the blend **before** the cab; export the shared cab IR as a `.wav` alongside.
  Exactness condition: post EQ and bus comp must be expressible after the IR — post EQ (linear)
  is folded into the exported IR (IR ⊛ post-EQ impulse response); bus comp must be **off**
  (it sits after the cab and is nonlinear) — otherwise refuse with a clear message, or allow
  `--allow-inexact` which reports the error introduced.
- `withcab`: model = the whole chain including cab, post EQ and bus comp. Always allowed.
- `perPath` cab presets ("studio blend"): `nocab` is refused with the message the UI shows
  ("only the with-cab export is exact for studio blends").

## What is never trained
- The gate is always bypassed in the training chain (and reported). Delay/reverb/modulation
  blocks (none exist yet) and bus comp with release > 150 ms are refused (registry traits
  `namTrainable == false` → refuse). Report every bypass/refusal in `export_report.json`.

## Training signal
- Prefer a Sawblade-owned deterministic capture signal (documented generator, seeded): level-
  stepped white/pink noise, log sweeps, chord-like synthetic plucks, silence segments, ≥ 3 min
  at 48 kHz, plus a held-out validation segment. Rationale: no third-party audio licence. If
  NAM's standard `v3_0_0.wav` input is used instead, record its source and licence in
  docs/THIRD_PARTY.md and fetch it at runtime (never commit it).
- Render the training target through the chain with `sawblade_core.render` at 48 kHz (latency
  trimmed). Record input/output levels so the model's `input_level_dbu`-style calibration
  metadata can be filled honestly or left empty (document the choice).

## Training
- Architecture options mapped to NAM presets: `feather`, `lite`, `standard` (default standard,
  A1 WaveNet as supported by the pinned trainer; report whether A2 export is possible).
- CPU training with `--epochs` (default sized so `lite` finishes in ≤ 30 min on 4 cores; report
  measured times). Seeded; deterministic where torch allows (document).
- Output `.nam` metadata: name, modeled_by "Sawblade", gear_type, tone_type, training ESR, and
  a `sawblade` block: preset name + hash, export mode, and the full capture attribution list
  (title, creator, licence, TONE3000 URL) — plus a licence note: "Derived from TONE3000 captures;
  personal use only unless permitted by the creators and TONE3000."

## Validation (acceptance)
- Render a held-out signal AND the cover DI excerpt (testdata/, git-ignored) through (a) the
  original chain and (b) the exported model (+ IR for nocab) via the core; report ESR and
  A-weighted 1/3-oct LTAS error. Acceptance for `standard`: ESR ≤ 0.02 on the validation
  segment and LTAS error ≤ 0.5 dB on the DI excerpt; report `lite`/`feather` numbers too.
- The exported `.nam` must load in the C++ core (`NamBlock`) and in the plugin engine.
- Listening file: DI excerpt through original vs export, level-matched, as MP3 for the user.

## Outputs
Default `--out ~/.cache/sawblade/exports/<preset>-<mode>-<size>-<timestamp>/`: `.nam`, IR `.wav`
(nocab), `export_report.json`, validation renders. Never write into the repo.

## Tests (no long training in CI)
Mode/refusal logic; IR ⊛ post-EQ folding equals the chain's cab+post-EQ to −100 dB; gate
bypass; metadata/attribution; training-signal determinism; a tiny 1–2 epoch `feather` smoke
training on a 2 s signal (marked slow, skipped unless `SAWBLADE_TEST_TRAIN=1`).
