# v0.1.1 report — capture-free launch, Classic presets on TONE3000 ids, CLIENT ID prefill

Branch `claude/sawblade-v0_1_1-init-classic` (from `claude/sawblade-plugin-setup-7k0b8q` at `ae39429`),
2026-10-05. Last code commit `eeb098c`; this report is the commit after it. Spec:
`docs/specs/v0_1_1-init_classic.md`.

**Status: all three tasks ACCEPT.** Validated on GitHub Actions at `eeb098c` (CI run 53): linux-gcc ctest 688 with
0 failed, pluginval VST3 strictness 10 SUCCESS; linux-clang `-Werror` build and ctest with 0 failed; python
`pytest match` 439 passed, 7 skipped (env-gated training/real-model tests). macOS: see "Known, not this phase".

## Task A — root cause of the red "file not found" on a clean launch

INIT itself is capture-free, but `makeInitPreset()` gives the disabled cab the placeholder file `"(none)"`
(`plugin/src/PresetMapping.cpp:92`; the schema requires a cab file). Nothing in the UI treated that placeholder as
"no capture":

- **The red text**: the mic page builds the IR response plot for the current cab.
  `MicPage::Impl::spectrumFor` (`plugin/src/mic/MicPage.cpp:568`) → `mic::spectrumOfCapture`
  (`plugin/src/mic/IrResponse.cpp:62`) → `loadIr("(none)")` fails → red
  `Cannot read the IR: WAV error ((none)): cannot open or not a valid WAV file`. It is set at launch even when the mic
  page is closed, and the preset browser overlay showed the same label.
- Also leaking the placeholder as a file: the About box (`about/CaptureList.cpp:16`, "Cab: (none) · file missing"),
  the rig Cab tab IR cards (`rig/RigModel.cpp` `captureTitle`/`captureCredit`), the mic page pack title and selected-IR
  name (`mic/MicPage.cpp:606`, `:718`), `IrPack::single` (a phantom 1-IR pack), the preset info panel
  (`presets/PresetLibrary.cpp` `summariseCaptures`), and saved state: `presetToStateJson` absolutised it to
  `"<cwd>/(none)"`, so states saved by v0.1 keep the bug alive after the fix unless normalised.

Not the cause (traced, ruled out): the engine load path (`core/src/chain.cpp:147` skips a disabled cab), the preset
browser auto-selecting a Classic entry (needs a click; Task B routes it through resolve), Standalone state restore
of a Classic preset.

Fix (`e25034a`, `55a7339`, `eeb098c`): one constant `kNoCaptureFile` and one predicate `isNoCapture(const Capture&)`
(`plugin/src/PresetMapping.h`), used by every consumer above; the placeholder is never listed, titled, read,
absolutised or exported ("No cab" is shown). `SawbladeProcessor::setStateInformation`
(`plugin/src/PluginProcessor.cpp:309-318`) rewrites a `cab.ir/irA/irB` file whose filename is `(none)` back to the
bare placeholder, so v0.1 saved states restore clean. No schema or `core/` change; the Init pass-through test is
unchanged and green.

Tests (`plugin/tests/test_integration.cpp`, `plugin/tests/test_mic.cpp`):
- `launch: a clean first Standalone run shows no missing-file or (none) text on any panel` — **reproduced first**:
  committed alone in `ca37ec4`; CI at `35403e9` failed it with 13 assertions (About box, rig tabs 3–5, mic page,
  preset browser, incl. the red "Cannot read the IR"), every other test green. Passes at `eeb098c`.
- `launch: a saved state whose captures no longer exist restores; only the load failure names the file` (regression
  guard; passed before and after).
- `launch: a state saved by an older build with the absolutised "(none)" cab file shows no cab and no error`.
- `no capture: the "(none)" placeholder is never a file` (isNoCapture truth table, state JSON, summariseCaptures,
  IrPack::single).

Reviewer: REVISE (old saved states with the absolutised placeholder; missing red-string phrases; unit pins; one
wording) → ACCEPT on `55a7339`; CI then showed one more consumer (`nameOf`, MicPage.cpp:718) → fixed in `eeb098c`,
reviewer ACCEPT.

## Task B — Classic presets resolve through TONE3000

`245f44e`, `147a07e`. The four Classic presets use the spec's mapping; every `source` object is identical to the
committed preset already using that (id, modelId) pair (checked programmatically by the reviewer);
`body_boost_ts_tight.nam` became the modeled `pedal.ts` (drive 0, tone 5, level 8); all other values unchanged
(parsed-JSON diff). `presets/README.md` carries the mapping and the resolve instruction; `docs/HANDOFF.md` open item
annotated. No code change was needed: `planPresetLoad` already sends any sourced, uncached capture to the resolve
flow.

Tests: `Factory presets: every capture reference has a TONE3000 source unless it points into tests/fixtures/`
(`tests/test_render.cpp`; reviewer revert check: fails on the old presets) and `Classic factory presets resolve
through TONE3000 like Matched ones: NeedsResolve when uncached, Direct and loadable when cached`
(`plugin/tests/test_presets.cpp`). Reviewer: ACCEPT (core ctest 309/309 locally; plugin test green in CI).

## Task C — CLIENT ID prefill

Python (`426caca`, `5fbf54b`): a successful `sawblade-t3k login` stores the publishable `client_id` in the token
file; `t3k_cs_` values are never stored or loaded; old token files stay valid; refresh preserves it. 6 tests in
`match/tests/test_auth.py` (5 fail on revert; the secret-refusal one guards pre-existing behaviour). Reviewer: ACCEPT
after one REVISE (the secret test registered respx routes it never calls).

C++ (`35403e9`, `f96e44d`): `Settings::resolveTone3000ClientId()` / `effectiveTone3000ClientId()` — stored →
`TONE3000_CLIENT_ID` → `client_id` from the token file (same path rules as Python `default_token_path`), read with a
SAX handler that keeps only the top-level `client_id`; a `t3k_cs_` value from any source is skipped; missing,
corrupt, oversized or unreadable files give empty. The Settings field shows the effective id with a dim
"from environment" / "from sawblade-t3k login" caption; showing it never stores it. Tests: 4 cases in
`test_settings.cpp`, 1 in `test_editor.cpp`. Reviewer: REVISE (env var not restored on a thrown test) → ACCEPT.

## Validation

| check | where | result |
|---|---|---|
| gcc Release ctest | CI linux-gcc, `eeb098c` | 688 tests, 0 failed (skips as on base) |
| clang `-Werror` build + ctest | CI linux-clang-werror, `eeb098c` | 0 failed |
| pluginval VST3 strictness 10 | CI linux-gcc, `eeb098c` | SUCCESS |
| Python suite | CI python, `eeb098c` | 439 passed, 7 skipped |
| macOS build + ctest | CI macos-arm64, `eeb098c` | build OK; ctest 45 failed — the same 45 tests fail on the base branch (`f9ac2ca`, run 52); none of this phase's tests fail |

This container could not build the plugin (no X11/ALSA dev headers; apt, PyPI and gitlab.com are blocked by the
environment's network policy), so GitHub CI was the build/test oracle; the subagents syntax-checked with g++ and
clang++ `-Werror` against the fetched JUCE sources before each push.

## Known, not this phase

- **macOS CI is red on the base branch** (runner/two-pass "Subprocess aborted", chain mute ramp, legacy goldens,
  save/load round trip, A/B compare, rig editor screenshots). Identical list here, so auval and macOS pluginval did
  not run. Likely related to the base branch's open v0.1.2 Mac work; not ported or fixed here.
- `runner: export progress (--progress-json)…` failed once on clang at `55a7339` and passed at `eeb098c`; this phase
  does not touch the runner. Watch it.
- CI on `55a7339` lost gcc and macOS at configure: gitlab.com refused the Eigen clone ("unable to handle this request
  due to load"). Eigen and NAM core's Eigen submodule come from gitlab; a GitHub mirror of the same pinned commit
  exists (`github.com/eigen-mirror/eigen`, `3147391d`).

## Proposals (not done)

1. A cab that is enabled while holding the placeholder (e.g. CAB on in INIT, or perPath with no IR) still reaches
   core and fails with a message naming `(none)`; guard it in the Cab tab or give a "no IR selected" message.
2. Move the Eigen FetchContent source to the GitHub mirror (same commit) so CI and cloud containers stop depending on
   gitlab.com.
3. Cap the token-file `client_id` length (e.g. 256) before showing it in the UI.
4. `docs/PLUGIN.md`: one line on the client-id fallback order.
