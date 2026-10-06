# v0.2 — tweakable capture presets: REPORT

Spec: [`v0_2-tweakable_presets.md`](v0_2-tweakable_presets.md) (user decisions at its top; lead decisions per task
at its bottom). Branch `claude/sawblade-v0_2-tweakable-presets`, cut from `claude/sawblade-plugin-setup-7k0b8q`
at 4c9db8e; the v0.1.3 merge (559048d) was brought in at c8ec1f2. Last code commit bfa6b70; every task
reviewer-ACCEPTed; CI green on bfa6b70 (run 106).

## Outcome per task

| Task | What shipped | Key commits | Reviewer |
|---|---|---|---|
| A — per-path amp controls | `AmpStage` per path: GAIN (drive before the amp block), BASS 100 Hz shelf, MID 650 Hz peak, TREBLE 3 kHz shelf, PRESENCE 5.5 kHz shelf, LEVEL; ±12 dB (PRESENCE ±9); skipped exactly at neutral; coefficients redesigned on a 32-sample grid counted from `prepare()` (block-size independent); latency 0, NAM-trainable. Preset schema v2 (`paths.<a\|b>.ampControls`, v1 still read). 12 host params `ampA_*` / `ampB_*`. `ampIndex` moved to core. | d871376, 24ad00d, ef5eb39; follow-ups in fc89c6b, 51f0c62, 4f4b9ff | ACCEPT (724/724, ASan/UBSan clean) |
| B — gain steps | Python: `parse_ladder` / `sawblade-t3k ladder` (never guesses: one gain token per name, identical remainders, distinct gains, ≥ 2 rungs). Core: `model.ladder` in the preset, `LadderBlock` (rung positions, 0.15 hysteresis, residual drive ±12 dB, lock-free hand-over via `SwapSlot`, 10 ms warm-up + 20 ms equal-power fade, latency-mismatched rungs rejected). Plugin: ladder fetch once per tone, preload of the 8 nearest rungs, background fetch of missing rungs, `gainStep` write-back, offline render uses `gainStep`. | 3f1c688, 8d63e24, fc89c6b, 98e7e8c, 51f0c62, 4f4b9ff, 5edf0da, a3e5a7d | parser ACCEPT; plugin/core REVISE → ACCEPT |
| C — BLEND fills path B | Python: `suggest_body` / `sawblade-t3k suggest-body` (amp_high pool amps; different family from A first — 5150/6505/EVH and Recto/Dual/Rectifier aliases merge — then cached, then pool order). Plugin: BLEND on an empty path B adds `pedal.ts` (0/5/8) + the cached fallback amp (tone 88689) + level match auto / constant loudness in one edit; the async suggestion replaces the amp only if path B and the blend are untouched; one stale-safe undo entry; cached captures carry licence/creator/sha256 from the cache meta (-nc marks the rig non-commercial). | 416933c, 056909c, 62ec10e, e29b7cc, 0b23f64, 5edf0da | rule REVISE → ACCEPT; plugin REVISE → ACCEPT |
| D — UI wiring | `rig::AmpHead`: six `knob_amp` filmstrip knobs per head over the baked knob positions (no new art), bound to the Task A params (editor-test exemption removed); read-out `GAIN 7.0` / `· capture: <rung>` / `· drive only (fetching <rung>)`; `NO AMP IN THIS PATH`; `BODY PATH OFF — turn up BLEND to add one` (B empty) / `— turn up BLEND` (B has blocks); `CAPTURE · FIXED TONE` on capture slot cards; Cmd/Ctrl+Z → BLEND-fill undo (not under a text field or an overlay; works with the rig editor open, on purpose). | 214b34e, 9854ea2, 019f5e9, 975f230, bfa6b70 | ACCEPT (764/764); Cmd/Ctrl+Z follow-up REVISE ×2 → ACCEPT (9/9 overlay mutations caught) |

## Decisions and deviations (all reviewer-checked)

- Amp controls act even when the amp block is bypassed (path tone controls); documented in `PRESET_SCHEMA.md`.
- Shelf acceptance reading: RBJ shelves are half their gain at the corner, so tests assert ±6 / ±4.5 dB at the
  corner and the full ±12 / ±9 dB on the plateau (analytic response, within 0.1 dB); MID ±12 dB at 650 Hz.
- **Bit-identity of legacy presets rests on structure, not on a per-preset golden.** The neutral stage is skipped
  (asserted bitwise at stage and chain level for block sizes 1…100000); existing render goldens pass; the
  committed per-preset test proves parse-equivalence and a render through the new chain at defaults. The
  comparison against a pre-v0.2 build was an ad-hoc 77-render byte compare (all identical) at 304280b, not in CI.
- Rung swap = 10 ms silent warm-up of the incoming model + 20 ms equal-power fade (≤ 30 ms total, as specified);
  without the warm-up a cold model breaks the −60 dBFS click limit.
- Ladders are fetched for size `standard` (presets do not record capture size); a ladder that does not contain
  the block's own model is ignored and reported in `ladderMessages()`.
- Fallback body amp has no fixed model id: any cached model of tone 88689 (lowest id with a meta entry), else
  `fetch 88689`. Uncached captures are never written into the preset, so path B is "TS only" until the amp file
  exists.
- Undo covers BLEND fills only (there was no undo infrastructure); one entry, refused if the rig changed since.
- `SAWBLADE_NO_NETWORK=1` disables every network tool the plugin starts on its own (ladder fetch, rung fetch,
  BLEND suggestion/fetch); set for every ctest test and the pluginval steps (ci.yml: env only). Default on for
  real use.
- Knob order on the heads follows the baked art (GAIN BASS MID TREBLE LEVEL PRESENCE), drawn at 30 design px; the
  read-out pill covers the lower "SAW"/"BODY" lettering (no free strip in the head art).
- Ladder parser fixtures are hand-written from real naming patterns (no TONE3000 login in the container).

## Validation

- Local (gcc Release, clang -Werror, Linux): ctest 774/774 at a7d542e (2 env-gated skips); ASan/UBSan clean on
  the amp, ladder and body-fill tests.
- CI (GitHub, validation of record): **run 106 on bfa6b70 — all four jobs green**: linux-gcc (ctest + pluginval
  VST3 level 10), linux-clang -Werror, python (pytest), macos-arm64 (ctest + auval + pluginval AU and VST3 level 10).
  macOS: no failures (the v0.1.3 baseline is now 0, so this is stricter than "no new failures"). The report commit
  on top is docs-only.
- Known base-branch issue, not v0.2: `plugin:runner: export progress (--progress-json)` failed once on the base
  branch's linux-gcc job (run 72); it passed on every v0.2 run.
- macOS: two v0.2 CI failures were test timing assumptions, fixed in the tests, not the product:
  the ladder race test (needed > 20 producer hand-overs within a fixed 3000 blocks; got 3 on arm64 — allocation
  check passed) → a3e5a7d; the pre-existing pedal-face circuit-timer test (fixed 250 ms wait) → a7d542e.
- pluginval: not available locally; CI runs VST3 level 10 (Linux, macOS) and AU level 10 + auval (macOS).

## Screenshots

Rig page at 1280×800 from the `[ampd]` snapshot test (`SAWBLADE_SCREENSHOT_DIR`), not committed: defaults, knobs
moved, ladder read-out, body off (B empty), body off (B has blocks). Attached to the lead's final message (session scratchpad `shots/`).

## Proposals (not done — need a user decision or art)

1. **Art:** amp-head art with six labelled knob positions in parameter order and a free strip for the GAIN
   read-out (today: caption patches over the baked LOW/HIGH labels, pill over the SAW/BODY lettering).
2. **Record capture size** (`standard` / `lite` / …) in the schema so ladders are fetched for the right size.
3. **General rig undo** (Cmd/Ctrl+Z for every rig edit, not only BLEND fills).
4. **T3kTool watchdog**: a hung `sawblade-t3k` call blocks the one in-flight slot for the session.
5. **Controls next:** per-head DEPTH/RESONANCE (low-end tightness), a BRIGHT switch, and a ladder rung list the
   user can click instead of only GAIN.
6. Matcher: the shared `_HIGH_AMPS` regex has no word boundaries ("Slovenia" → `slo`); 5150/6505 match inside
   longer numbers. Tighten with care — it feeds `classify`.
7. Run TSan on the ladder hand-over tests in CI (not run in this phase).
8. `closeAllOverlays()` / `anyOverlayOpen()` / the overlay test are three hand-kept lists — derive them from one.
9. Python CI: the container could not run the full match suite (no respx/numpy); CI did (green).
