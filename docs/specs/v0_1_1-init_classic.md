# v0.1.1 — capture-free launch, Classic presets on TONE3000 ids, CLIENT ID prefill

Source: HANDOFF.md open item 0 (first Mac run of v0.1 at `db0813e`). Owner: dsp-engineer (C++,
presets) + match-engineer (task C Python part only). Reviewer audits each task.

## Problem as reported, and what the code says

- User: the Standalone opens on `INIT` and shows a red "file not found" on launch; the four
  "Classic" presets (Chainsaw + Body, Studio Split, Swedeath Saw, Tight Body) reference
  placeholder files `presets/captures/*.nam|wav` that are not in the repo.
- Code: `makeInitPreset()` (`plugin/src/PresetMapping.cpp:87`) has no blocks and the cab is
  disabled with file `"(none)"`, so INIT itself references no capture. The red error therefore
  comes from somewhere else on the launch path. Candidates (unverified): the Standalone
  restoring a saved state that pointed at a Classic preset; the preset browser / prev-next
  auto-selecting the first Classic entry; the cab `"(none)"` sentinel being shown as a missing
  file by a UI panel (rig editor, info panel, cab view).

## Task A — find and fix the launch error

1. Reproduce headless: a test that constructs the processor + editor exactly as the
   Standalone does on a clean first run (no settings file, no saved state) and asserts that no
   UI status / message / info row reports a missing or not-found file. Also cover: a saved
   state whose capture paths no longer exist (must restore without a red error on an
   unrelated panel; the load-failure message for that state is allowed and must name the file).
2. Fix the root cause found. If it is the `"(none)"` sentinel, nothing may display it as a
   file. Record the root cause in the report with file:line.
3. INIT stays a clean pass-through (existing test `Processor: starts as a zero-latency
   pass-through (Init preset)` must stay green, unchanged).

## Task B — Classic presets resolve through TONE3000

Lead decision: keep the four Classic presets in the library, converted to TONE3000 ids, reusing
only (tone id, model id) pairs already verified in committed presets. No API calls are needed
to write them. Mapping:

| Placeholder file | Replacement | Source pair (title, licence) |
|---|---|---|
| `saw_pedal_hm2_maxed.nam` | NAM pedal | 58569 / 496942 Boss HM-2 1985 MIJ TTSV10, t3k (as in `matched/barbaric_v4.json`) |
| `saw_amp_lowgain.nam` | NAM amp | 86089 / 731435 Marshall JCM 800 2203, t3k |
| `body_boost_ts_tight.nam` | **modeled `pedal.ts` block** (drive 0, tone 5, level 8; copy the block shape from `presets/modeled/ts_boost.json`) | no verified TS capture pair exists |
| `body_amp_highgain.nam` | NAM amp | 70977 / 584871 6505+ FULL Pack, t3k (as in `matched/nails_v1.json`) |
| `cab_4x12_v30.wav` | IR | 84863 / 721117 Mesa Oversized SM57 and VR2 5150 Power, t3k |
| `cab_4x12_greenback.wav` (studio_split) | IR | 75087 / 656946 UK Greenback 1960TV M201, t3k |

- Copy each `source` object (provider, id, modelId, url, title, creator, license) verbatim from
  the committed preset that already uses that pair; `file` becomes `captures/<id>_<modelId>.<ext>`
  exactly like the matched presets. Keep every other value (EQ, blend, gate, comp, levels).
- Add one line to each `notes`: which TONE3000 captures it uses and that the body boost is the
  modeled TS.
- `presets/README.md`: replace the "Captures to fetch" placeholder table with the mapping
  above and the resolve instruction (`sawblade-t3k resolve <preset>` / the browser's resolve
  flow). Remove the dead placeholder names everywhere they appear (README, tests,
  `tests/test_mac_update.sh` expectations stay valid or are updated to match).
- Loading a Classic preset whose captures are not cached must go through the same resolve
  flow as the Matched presets (PresetLoadFlow), not a red "file not found". Test it with the
  fake t3k tool the preset-browser tests already use.
- Existing test: `presets` schema validation over every preset file stays green; add an
  assertion that **no file under `presets/` references a capture without a TONE3000 `source`
  unless it points into `tests/fixtures/`** (guards against placeholder regressions).

## Task C — Settings CLIENT ID prefill

GUI apps on macOS do not inherit `.zshrc`, so `TONE3000_CLIENT_ID` is usually absent.

1. Python (`match/sawblade_match/t3k/`): on a successful `login`, store the publishable client id
   used (never a `t3k_cs_` value) in the token file as `client_id`. Existing token files without
   it stay valid. Tests in `match/tests`.
2. C++ `Settings::effectiveTone3000ClientId()`: stored setting → env `TONE3000_CLIENT_ID` →
   `client_id` from `~/.config/sawblade/t3k_tokens.json` (read with the same path rules as the
   Python `default_token_path`). Never read or expose any token field. A `t3k_cs_` value from
   any source is refused.
3. Settings panel: when the stored id is empty, the field shows the effective id (as the
   field's text, with a dim caption "from environment" / "from sawblade-t3k login") so the user
   sees it; editing it stores it as usual. Tests: each source, precedence, secret refusal,
   unreadable/corrupt token file → empty, no crash.

## Acceptance

- Full suite green on gcc and clang `-Werror`: `ctest`, the Python suite, pluginval level 10
  (VST3) on Linux.
- New tests above exist and fail when the fix is reverted (reviewer checks one revert per task).
- No capture or IR file added to git. No change to `core/` DSP.
- Report `docs/specs/v0_1_1-init_classic_REPORT.md`: root cause of the launch error, the
  reviewer verdict per task, test counts.
