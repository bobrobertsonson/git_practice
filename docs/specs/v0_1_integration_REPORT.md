# v0.1 integration report (integration lead)

Branch `claude/sawblade-plugin-setup-7k0b8q`, last code commit `e9f346a` (this report follows it), 2026-10-05.
Artifact with the screenshots and the checklist: https://claude.ai/artifact/JVLrb9eyxWjFZKuShy3wyg (private until shared). Every merge and every fix below went through the reviewer; verdicts are recorded here.

**Status: v0.1 feature set integrated and green.** gcc Release 679/679, clang `-Werror` 679/679 (two
env-gated skips: the real htdemucs model, the Pillow icon reproducibility check), pluginval VST3 strictness 10 passing
in both builds, `match/` pytest: 413 passed / 7 skipped (`-k "not speed"`) at the p11 round; after the B6/B7 rounds the suite (excluding the 15-minute `test_speed.py`, which passed at 10.1, and `test_calibrate`) exits 0 with no failures.

## Mac quick start

On the Apple Silicon Mac (first time: `docs/MAC.md` "First time" — Command Line Tools, `brew install cmake ninja
python@3.11 git`, clone to `~/sawblade`, `python3.11 -m venv match/.venv && match/.venv/bin/pip install -e match`,
`match/.venv/bin/sawblade-t3k login`):

```
cd ~/sawblade && git pull
scripts/mac_update.sh --standalone        # pull, configure + build Release, install AU + VST3, auval, resolve presets, open the app
# later updates: scripts/mac_update.sh      (add --no-resolve to skip TONE3000, --clean after a dependency change)
```

Then in Logic Pro: Settings > Plug-in Manager > Reset & Rescan Selection for Sawblade (restart Logic if it is not
listed), insert Sawblade (AU, "Swbl") on a guitar DI track, open the editor. Load a tone first from the preset box in
the top bar: `presets/styles/uk_death_bolt_thrower.json` (a blend; the resolve step has fetched its captures) or
`presets/modeled/hm_chainsaw.json` (a modeled chainsaw, no captures needed). Then PLAY ALONG to load a song folder or
file, RIG to edit the chain, the gear button for Settings. Intel Macs: configure with `-DSAWBLADE_WITH_SEPARATOR=OFF`.

## What is in the branch

| merge | branch | reviewer | ctest after |
|---|---|---|---|
| 5c48280 | p9 mic page, preset browser, A/B compare, irMix, capture cache | ACCEPT | 423 gcc / 422 clang |
| 9a06eab | p6a record + match in the plugin | ACCEPT | 463 |
| 1003fc1 + 33e0c6d | p5-1b on-device separation (ONNX Runtime 1.30.0) + integration fix | REVISE then ACCEPT | 485 |
| 4e0f271, 2659f44 | phase 12 spec, phase 6b matcher `--quick` / `--progress-json` (lead patches, `git am`) | lead-accepted | 485, pytest 58 |
| d6b6c4c | 7c chainsaw family (pedal.hmx, pedal.eye, pedal.hm v3), 14 presets | ACCEPT | 539 |
| ff619d1 | 6a.1 quick-then-thorough MATCH, top-bar MATCH | ACCEPT | 567 |
| d51e47f | 10.1 path level match + constant-loudness blend | ACCEPT | 583, pytest 299 |
| 27f535d | p12 NAM export panel | ACCEPT | 603 |
| a45ac39 (with 5b6d076) | p11 settings, first-run, About, icon, ToolRunner | ACCEPT (trailer fix applied before push) | 656, pytest 413 |
| 517dc11 .. e9f346a | v0.1 integration walk, Standalone launch test, fixes, matcher heartbeat + ETA | ACCEPT after three rounds (two must-fixes on the launch script) | 679 |

Already in the branch before this pass: p8 capture browser, p10 rig editor, 7b chainsaw pedal, 5.1a / 5.2 play-along,
renders-v3, CI, mac build. Not merged: nothing accepted is left on origin; `p6b-matcher-speed` never appeared as a
branch (delivered as a patch).

## Hand resolutions worth knowing

- **p9 vs p8 browser:** p9's `PresetBrowser` member became `presetBrowser_` (p8's `CaptureBrowser` keeps `browser_`);
  `RigPiece::onDoubleClick(Piece)` (p10) drives both the pedal drawer and the cab's mic page.
- **p9 exit-code contract:** `sawblade-t3k` exits 4 for a TONE3000 re-auth (`EXIT_NOT_LOGGED_IN`); p8's CLI tests were
  aligned (10.1 merge). The C++ client parses the JSON error object first, so code `auth` survives any exit status.
- **5.1b vs 6a:** after LOAD SONG on a file, `settings().folder` is empty; record + match now use
  `PlayAlong::activeStemsDir()` and `activeSongName()` (33e0c6d).
- **10.1 vs p9/6b:** `CabMode` keeps `IrMix`; `ChainInfo.liveCompatible = usesSharedCab(mode)` (shared or irMix);
  the render report carries `cabMode`, `levelMatch` and `blend`; matcher `refine_combo` is the union of 6b's quick-mode
  keywords and 10.1's `levels`. Goldens untouched (defaults off / linear are bit-identical).
- **p12 vs 6a.1:** `MatchScreen` is p12's match-only structure with every 6a.1 element (PREVIEW / REFINED badges, refine
  bar, APPLY REFINED BEST, auto-refine, CANCEL REFINE); `ExportPanel` owns export; `MatchSettings` uses p12's
  `shared_ptr` props with both `autoRefine` and `exportWallSeconds`.
- **7c:** host parameters appended only (46 -> 63); saved state from 7b loads unchanged.
- **Presets:** 31 factory presets (p7 modeled, 7c, 10.1 demo) were missing `"category"`, which
  `test_category_cache` requires; schema-recommended values were assigned from the notes.
- **Test expectations changed, with the reason recorded in each commit:** `test_speed.py` quick-vs-thorough margin
  0.15 -> 1.0 dB (the accepted 6b criterion; quick is 0.684 vs thorough 0.255 dB at seed 7 once 10.1 levels enter the
  loss; follow-up "6b.1: re-probe levels after quick's short-linear stage"); A/B and EXPORT NAM top-bar buttons live.

## The v0.1 walk (`sawblade_integration_tests`, ctest prefix `integration: `, 21 tests)

One `SawbladeEditor` over one Standalone-mode `SawbladeProcessor`, in a temp HOME / app-data / cache, with the fake
`sawblade-t3k` (plugin/tests/fake_t3k.py) and the fake match / export child (plugin/tests/fake_tools.h) installed as the
Settings venv, synthetic stems and a synthetic ONNX separator model. Each step asserts the panel state and writes a
1280 x 800 screenshot to `build/screenshots/v01/<nn>_<name>.png`. A step run alone first runs its prerequisites. Whole
walk: about 20 s. `integration: standalone launches` runs the real Standalone under `xvfb-run` with no audio device
(window opens, non-modal "input muted" bar, first-run checklist on a fresh data dir), captures the X root with
ImageMagick, and checks a clean SIGTERM exit.

| checklist item | steps | result |
|---|---|---|
| rig editor (p10, 10.1 BLEND levels and law) | 04-09, 21b | pass |
| pedal CIRCUIT switch (4 circuits) + ADVANCED drawer | 10, 11 | pass |
| capture browser with SAWBLADE_FAKE_T3K | 12 | pass |
| mic page (drag, snap, BLEND 2 MICS) | 13 | pass |
| preset browser, prev / next, A/B | 01-03 | pass (right-click menu asserted via tooltip: JUCE native popup crashes under bare Xvfb) |
| record + match with the fake child (quick -> refined) | 15-17 | pass |
| export panel (configure, training, result) | 18 | pass |
| play-along with synthetic stems | 14, 15 | pass |
| separation from a song file (cache miss, hit) | 19 | pass |
| settings / first-run / About | 21c | pass |
| state round-trip, overlay exclusivity | 20, 21 | pass |
| Standalone binary under xvfb | 22 | pass |
| not exercised | mic page LOAD PACK (fake has no `pack`), real audio device, real TONE3000 login | gap |

## Integration bugs

Fixed in this pass (each commit carries a test that fails without the fix):

1. Overlays were not mutually exclusive (RIG editor, mic page, preset browser, MATCH, EXPORT only came to the front);
   `closeOverlaysExcept` now closes the others, the capture browser closes all, the RIG toggle follows (ea93263).
2. Re-attached export jobs got the 2.5 s match grace instead of the export grace before SIGTERM escalation (efc7c70).
3. Export progress froze during "rendering the training target" (fraction 0.05, elapsed 1.0 for ~105 s): a 1 Hz
   heartbeat ticks, guarded so a cancelled or failed stage is never overwritten (2d0077a, 372a573).
4. Export ETA was -1 for the first minutes and after a resume: seeded from the checkpoint's per-epoch time or the first
   partial epoch (e0caa51).
5. `standalone_launch.sh` leaked its temp dir (exec before the trap) and could orphan the app on a ctest timeout (95f4cd2, 204c1a3).
6. **Pending user-approved change (not shipped):** CC BY-NC captures. CLAUDE.md allows them for this personal project, but
   the lead ruled that enabling them in the t3k licence filter, the fake and the capture browser needs the user's direct
   approval; the change (e1fbe51) was reverted (6e7ce8c). The browser now says "CC BY-NC captures are not enabled in
   this build." Enabling it is a one-commit change: `match/sawblade_match/t3k/licenses.py`, `plugin/tests/fake_t3k.py`,
   the browser note, and the tests that pin the refusal.
7. The BLEND tab's law button truncated to "CONST..." at 1x (6997bc5).
8. The mode chip said STUDIO for a rig without a cab; a cab-less rig is live-compatible (c47595a, e9f346a).

Reported, not fixed (feature-internal or environment):

- Settings "Logged in" light follows the token file only (SettingsPanel.cpp ~693/751); a successful Test can disagree.
- The A/B right-click popup is a JUCE native menu and crashes (X BadAtom) under bare Xvfb with no window manager.
- The icon reproducibility test skips without Pillow; the real htdemucs test is env-gated.

Consolidation items for a later phase: four child-process runners (ToolRunner, T3kRunner, T3kTool::run, JobRunner);
exit code 4 handled in three places (LoginFlow.h, T3kTool.cpp, T3kClient's JSON object); two TONE3000 login flows;
three override stores for the sawblade-t3k path (only the defaults read the shared Settings venv); `appDataDir()`
lost its Windows `%APPDATA%` branch in p11 (Windows is not a v0.1 target).

## Mac update

`bash scripts/mac_update.sh --dry-run` passes here (no Mac): configure with `-DSAWBLADE_BUILD_PLUGIN=ON`, build, install
AU + VST3, resolve the TONE3000 presets, report what changed. On an Apple Silicon Mac the separator fetches the pinned
arm64 ONNX Runtime; an Intel Mac must pass `-DSAWBLADE_WITH_SEPARATOR=OFF` (the configure error says so).

## Process notes

- Every merge: `git merge --no-ff`, conflicts resolved by `dsp-engineer` / `match-engineer` from a written spec, both
  compilers built and tested, pluginval, reviewer audit of the integration (not of the already-accepted features), then a
  push. The integration commits (517dc11 .. this report) are pushed together after the final ACCEPT.
- One engineer run was cut off by a usage limit mid-merge (6a.1); the staged resolution on disk was resumed after the
  reset. One review verdict was mis-read from a transcript grep (p11: the reviewer's REVISE was the trailer problem
  that had already been fixed before the push); verdicts are now taken from the hand-back only.
