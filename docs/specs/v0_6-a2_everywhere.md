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

## Lead decisions (v0.6 lead, 2026-10-07)

1. **Base.** Branch `claude/sawblade-v0_6-a2` from `4d580a2`, with the reviewer-accepted v0.4M head (`bbae365`,
   `claude/sawblade-v0_4m-matcher-feel`) merged in (`6701294`), so the `anagram` profile extends the real
   `export/notes.py` (`NOTES_VERSION` 1) instead of a copy of its format. **Dependency:** v0.4M must be merged into
   the working branch before (or together with) v0.6. If v0.4M changes after `bbae365`, the integration lead
   merges v0.4M first; v0.6 adds nothing to `notes.py` that v0.4M's tests rely on.
2. **Who does what.** Task A is split: match-engineer audits the trainer side (A2 definition, sizes, presets,
   `match/` inventory) and generates the fixtures; dsp-engineer audits the core/plugin side (pinned core's A2
   support, `core/` + `plugin/` inventory). Task B = dsp-engineer; Task C = match-engineer (+ dsp-engineer for the
   export panel's three-way choice). Reviewer on every task.
3. **Fixtures.** Tiny A2 Full / A2 Lite / A1 `.nam` files are trained or initialised with the pinned trainer
   (`neural-amp-modeler==0.13.0`) from synthetic signals only, plus the trainer's own forward-pass output on a
   fixed synthetic input (the null-test reference). Generator script committed; the small fixtures are committed
   under `tests/fixtures/a2/` (synthetic, no TONE3000 data — allowed). Weights may be untrained/random-seeded:
   the null test checks the player, not tone quality.
4. **No dependency bump** in this phase unless Task A proves the pinned core cannot play an A2 variant the Anagram
   accepts; then it is a proposal in the REPORT + reviewer ACCEPT + THIRD_PARTY.md entry before any change.
5. **CPU numbers** come from a ctest-registered benchmark that prints a table (not a pass/fail on absolute time
   except the stated real-time margin for the two-A2-Full rig at 64 samples on macOS CI); numbers are copied from
   the CI logs into the REPORT.
6. **Anagram claims.** Only the published block list (Neural Amp / Neural Pedal / Neural Loader, IR, compressor,
   gate, EQ) and "up to three NAM blocks" are used; nothing about internals, CPU limits or firmware behaviour
   beyond the user's statement (KosmOS 1.16+ plays A2 Full / A2 Lite / A1).
7. **device-null** (Task D tool) is built in Task C by match-engineer as `sawblade-calibrate device-null`
   (align by cross-correlation + gain match, residual dB overall and per octave band, `listen/` A/B pair); it is
   tested on synthetic data (a known delay + gain + filter must be recovered and reported).
8. **A2 names (Task A result).** A `.nam` does not declare "A2"; an A2 file is a `SlimmableContainer` or a standalone
   23-layer WaveNet. Sawblade's names: **A2 Full = 8-channel submodel, A2 Lite = 3-channel submodel** (trainer
   `config_model_packed.json` `channels_8` / `channels_3`, container `max_value` 1.0 / 0.5; the core's fast-path shapes).
   That TONE3000 and the Anagram use the words Full/Lite for these is the user's statement, recorded as such.
9. **Official A1 presets** are cited from `neural-amp-modeler` 0.12.3 `nam/train/core.py:845-955` (identical since
   0.11.0; removed in 0.13.0) and pinned by a test against the 0.13.0 `WaveNet`. No vendoring, no pin change. `nano` is
   not offered. Sawblade keeps the 0.13.0 training recipe for A1 (no A/B in this phase; noted in the REPORT).
10. **A2 export files (Task C).** One A2 run trains the packed net (both sizes, per-submodel best checkpoints) and
    writes **three files**: the container plus standalone `…_a2_full.nam` and `…_a2_lite.nam` (extracted submodels,
    same weights). `--size full|lite` picks which standalone file is the "primary" output (shown first, validated
    against the acceptance rule, named in the export notes); both standalone files are always validated and their ESR
    reported (per-submodel `ESR_packed_i`, never the summed `ESR`). Reason: whether the Anagram accepts a container is
    not published, and the pinned core plays only the container's Full submodel. Task D uses the standalone Full file.
11. **No container size selector in the plugin** this phase (it needs an off-thread `SetSlimmableSize` + swap); A2 Lite
    playback uses a standalone Lite file. Proposal in the REPORT.
12. **Resume identity** gains the architecture/layout key in Task C (old lite/feather checkpoints refuse cleanly).
13. **TONE3000 A2 `size` strings** (pool ranking, ladder size "standard") cannot be verified offline: Task C keeps the
    current behaviour and the REPORT lists a one-line user check (`sawblade-t3k models <A2 tone>`).
14. **Task C interface contract** (so match and plugin halves can run in parallel):
    - CLI: `sawblade-export … --arch a2|a1` (default `a2`); `--size` = `full|lite` for a2 (default `full`),
      `standard|lite|feather` for a1 (default `standard`). Any other combination exits non-zero with a clear message.
      `--arch a1` without `--size` keeps today's behaviour exactly.
    - Files (a2): `<name>.a2.nam` (container), `<name>.a2_full.nam`, `<name>.a2_lite.nam`; the primary file is the
      one `--size` names. Export JSON gains `arch`, `size`, `files: {primary, container?, full?, lite?}` and
      `validation: {full: {...}, lite: {...}}` for a2 (one entry for a1). Progress/status JSON adds `arch`.
    - Export notes: the existing v0.4M `exportNotes` block is unchanged (generic profile). It gains
      `deviceProfiles.anagram` (structured, per stage: anagram block, position in chain, settings in hardware terms)
      and a second text file `<name>.anagram_notes.txt`; the generic `<name>.export_notes.txt` stays. Documented in
      `docs/PRESET_SCHEMA.md` next to the v0.4M format.
    - Plugin: export panel choice A2 Full (default) / A2 Lite / A1 (A1 keeps its standard/lite/feather sub-choice),
      passed as `--arch/--size`; wall-time history keyed by arch+size; the notes view can switch Generic / Anagram
      when `deviceProfiles.anagram` is present. Ladder fetch stays `standard` (decision 13).
15. **Plugin settings migration** (accepted as built in `26ba02d`): saved export settings without `arch` → A1 + saved
    size; no saved export settings → A2 Full (the new default; old builds never persisted the A1 Standard default, so
    such users also land on A2 Full — intended, the user decided "the whole project should work on A2").
16. **Anagram profile keys** follow the plugin reader built in `26ba02d` (`plugin/src/ExportNotes.cpp`
    `anagramProfileOf`): `deviceProfiles.anagram = {file, message, loaderOrder[], stages[{stage, block, position,
    settings{…}, hardware}]}`; match-engineer emits exactly these keys (settings as an ordered list of
    `{name, value, unit}` is NOT used — an object, the plugin sorts keys). A2 file names follow the plugin preview
    `<stem>-<mode>-<size>.a2_full.nam` / `.a2_lite.nam` / `.a2.nam` only if that matches the existing A1 naming
    pattern in `export/run.py`; otherwise match-engineer keeps the existing pattern and the plugin preview is adjusted
    (results are read from `files` either way). Correction (lead, after review): `loaderOrder` is a string (e.g. "Anagram chain: Gate -> Neural Amp -> IR -> Compressor"), and the profile also carries `device`; exporter and plugin agree.
17. **CI for the trainer** (Task A review finding): the `python` job installs only `match[dev]`, so nothing that needs
    `neural-amp-modeler` runs in CI. Nam-dependent tests use `pytest.importorskip("nam")`, and a new CI job
    `python-export` installs `match[dev,export]` with `constraints-export.txt` (CPU torch) and runs only the export /
    A2 tests (preset pins, fixture `generate.py --check`, A2 export smoke with a tiny step budget). Keeps the main
    `python` job's time and pip exposure unchanged.
18. **User decision (2026-10-07, relayed by the main lead): the hardware export is a standard NAM A2 file, loader-
    agnostic** ("usable on any A2 loader; can even use the regular steps of creating a capture in a DAW").
    - **The container is primary** (reverses decision 10's "standalone is primary"): `<stem>.a2.nam`, written by the
      pinned trainer's own `PackedWaveNet.export_container`, is what the standard workflow produces. `--size full|lite`
      now only selects which standalone extra is listed first and which size's acceptance gates `--require-accept`
      (the container plays Full in standard players). Standalone `.a2_full.nam` / `.a2_lite.nam` stay as extras.
      Whether every A2 loader accepts the container is unverified; Task D records what the Anagram accepts.
    - **Core-only load check:** `tests/tools/nam_load_check` (links only NeuralAmpModelerCore: `nam::get_dsp` +
      `process`, no Sawblade code) loads every exported `.nam`; the `python-export` job exports a tiny A2 and runs it on
      all three files (env `SAWBLADE_NAM_LOAD_CHECK`). Extra metadata keys (`metadata.sawblade`) must not break it.
19. **(Superseded by 22.)** ~~Built-in training keeps Sawblade's own seeded signal~~ (validated, reproducible, no third-party audio). Stated
    plainly in the REPORT and export notes: "built-in training uses Sawblade's own test signal; the file format is
    standard NAM A2; for a capture trained the standard NAM way, use the reamp pair."
20. **Reamp-pair export** (new, small): `sawblade-export … --reamp-pair NAM_INPUT.wav [--no-train]` renders the
    official NAM standard input file (supplied by the user; never committed, bundled or auto-downloaded until its
    licence is checked and recorded in `docs/THIRD_PARTY.md`) through the preset's exportable chain (same rules as the
    model export: no gate / time FX / long comp; no-cab or with-cab per `--mode`) and writes `<stem>.reamp_input.wav`
    (the user's file, copied unchanged) and `<stem>.reamp_output.wav` (48 kHz, same length, latency-compensated, a
    format the pinned trainer accepts), plus notes: personal use only, non-commercial when any `-nc` capture is involved,
    never upload or share (derived from TONE3000 captures). The input is validated (sample rate, length, and the hash /
    version check the pinned trainer applies to standard input files); anything else is rejected with a plain message.
    Report JSON: `files.reampPair{input, output}`; `--no-train` writes only the pair and the notes. Plugin: a Settings
    field for the NAM input file path and an "EXPORT REAMP PAIR" action (`--reamp-pair PATH --no-train`).
21. **Anagram notes are an optional generic hint**, not part of the file: "load the `.nam` into any NAM A2 block; on the
    Anagram that is a Neural Amp (or Neural Pedal) block, KosmOS 1.16+". The REPORT drops "KosmOS 1.17 assumed": standard
    A2 file; on the Anagram, A2 playback needs KosmOS 1.16+.
22. **User reversed 19 (relayed by the main lead): built-in training uses the official NAM standard input signal by
    default.** Licence finding: the pinned trainer (neural-amp-modeler 0.13.0, MIT — the licence covers the trainer
    code) ships no input file; its GUI links the files on Google Drive (`nam/train/gui/__init__.py:267-269`); no terms
    are stated for the audio. So the file is **user-supplied and never redistributed** (no commit, bundle or
    auto-download; recorded in `docs/THIRD_PARTY.md`).
    - Training goes through the pinned trainer's standard API (`nam.train.core.train` with the user's input file and
      the rendered output wav: its hash recognition, blip latency calibration and validation split); any step that
      needs the lower-level path is documented.
    - Every version the pinned trainer recognises is accepted; docs and prompts default to the trainer's current
      standard (v3.0.0 per `nam/train/_names.py:21` unless the pin says otherwise). The version used is recorded in each
      export's metadata (`metadata.sawblade.trainingSignal`) and notes.
    - Sawblade's synthetic signal is the labelled fallback ("trained on Sawblade's test signal, not the standard NAM
      signal"), used only when chosen explicitly. The plugin asks for the official file on the first export, with a
      "use Sawblade's test signal instead" option; it never falls back silently. CLI: `--nam-input PATH` (or the
      Settings value), `--signal sawblade` for the fallback; without either, the CLI exits with a plain message.
    - CI cannot have the file: CI trains on the fallback and tests the official path with a synthetic stand-in through
      an injectable recognition table. The official-signal acceptance numbers (held-out ESR / LTAS for A2 Full and Lite
      vs the Sawblade-signal numbers) are a user-run Mac step next to Task D; the exact command is in the REPORT.
    - The reamp pair (decision 20) uses the same file, unchanged.
23. **Signal on resume and precedence** (review of `ec7a32b`): a resumed export passes neither `--nam-input` nor
    `--signal`; the CLI exempts `--resume` from the signal requirement and reuses the signal recorded in the run's
    identity (a resume that does pass a different signal is refused). Precedence in the plugin: the latest explicit
    choice wins — turning on "train on Sawblade's test signal" wins over a stored file path; choosing a file turns the
    toggle off. The "use Sawblade's test signal" choice is remembered (explicit, revocable, shown in the checklist).
