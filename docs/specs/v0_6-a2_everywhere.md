# v0.6 — A2 everywhere (playback, matching, export), proven on the user's Darkglass Anagram

Source: user decision 2026-10-07: "the anagram runs A2 — this whole project should work on A2." The user's loader
is a **Darkglass Anagram** (KosmOS 1.16+ runs NAM A2 Full, A2 Lite and A1 in Neural Amp / Neural Pedal / Neural
Loader blocks, up to three at once; it also has IR and compressor blocks). Owners: match-engineer (training, export),
dsp-engineer (core/plugin playback, CPU), reviewer on every task. Hard rules in CLAUDE.md apply.

## What already exists (lead audit, 2026-10-07)

- Playback: NeuralAmpModelerCore is pinned (cmake/Dependencies.cmake) and built with `NAM_ENABLE_A2_FAST`.
- Pool: `sawblade-t3k` prefers A2 models of a tone and falls back to A1 (`t3k/fetch.py`, `t3k/filter.py`).
- Export: `match/sawblade_match/export/train.py` trains **A1 only**, with sizes that are Sawblade's own approximations
  "recalled from memory, NOT NAM's official presets". The pinned trainer (`neural-amp-modeler==0.13.0`) can train the
  packed A2 WaveNet (`PackedWaveNet` + `export_container`, `config_model_packed.json`), but it is not enabled.

## Task A — audit and pin (no behaviour change)

1. Confirm from the pinned sources (core commit and trainer 0.13.0) exactly what A2 is: the packed/slimmable
   container, which sizes TONE3000 and the Anagram call "A2 Full" and "A2 Lite", how a `.nam` declares it, and
   whether the pinned core plays every A2 variant the Anagram accepts. If the pins are too old for current A2 files,
   propose a bump (exact commit/tag, licence, changelog delta) — do not bump without a reviewer ACCEPT.
2. Inventory every place in core / plugin / match that assumes A1 (architecture strings, latency/receptive-field
   math, loudness metadata, `sawblade` metadata block, export validation). Report it as a table.
3. Replace the "recalled from memory" A1 sizes with the official NAM presets from the pinned trainer, cited by file
   and line. Keep A1 export available (older loaders).

## Task B — A2 playback, measured

- Load and play A2 Full and A2 Lite captures (fixtures generated with the pinned trainer, never TONE3000 files in
  git): null test against the trainer's own forward pass (tolerance stated), latency reported, zero allocations in
  `process()`, block-size independence.
- CPU: per-instance cost of A1 standard / A2 Full / A2 Lite on the CI Mac and Linux runners, reported in the
  REPORT; the plugin's rig with two A2 Full amps + pedals must stay real-time at 64-sample blocks on the macOS CI
  runner (state the margin).

## Task C — A2 export (default) with Anagram-ready notes

- `sawblade-export` trains **A2** by default (`--arch a2` with `--size full|lite`), A1 kept as `--arch a1`
  (`--size` = the official presets from Task A). The export panel offers A2 Full (default) / A2 Lite / A1.
- Validation numbers per size (ESR on the held-out segment, the existing acceptance tests) for A2 Full and Lite;
  the existing A1 acceptance stays.
- Export notes (v0.4M Task E format) gain a **device profile**: `anagram` maps stages to Anagram blocks — the model
  to a Neural Amp block (or Neural Pedal for a drive-only export), the cab to the IR block (no-cab export), bus comp
  settings to the compressor block, gate to the gate block, with where each goes in the chain. The generic profile
  stays. Wording in plain hardware terms; no claims about Anagram internals beyond its published block list.

## Task D — proof on the user's Anagram (user-run)

The lead hands the user exact steps: export one matched preset (A2 Full, no-cab, shared IR), load the `.nam` into
a Neural Amp block and the IR into the IR block, re-amp the same DI through the Anagram (interface out → Anagram →
interface in), and run a provided `sawblade-calibrate device-null` command that aligns the recording with the
plugin's render and reports the residual (dB) and an A/B `listen/` pair. Success = the user hears no difference that
matters and the residual is within a stated tolerance; otherwise the report says what differs (level, latency,
filtering in the device path).

## Acceptance

Tasks A–C reviewer-ACCEPTed with tests; full suite green (gcc + clang `-Werror`, ctest, Python, pluginval 10,
macOS auval + pluginval); REPORT `docs/specs/v0_6-a2_everywhere_REPORT.md` with the audit table, CPU table,
validation numbers, and the user's Task D result once run.
