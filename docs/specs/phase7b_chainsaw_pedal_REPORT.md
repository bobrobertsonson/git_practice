# Phase 7b report: the chainsaw pedal (deep `pedal.hm` v2, `pedal.muff`, pedal face, preset bank)

**Status: accepted by the lead** after two reviewer rounds (task A core: REVISE → fixed → accepted;
task B plugin: see §Review). Release ctest **295/295** (core, plugin, editor, pluginval);
Debug ASan/UBSan core tests **219/219**; `-Werror` clean; pluginval strictness 10 **SUCCESS**.
Spec: `docs/specs/phase7b_chainsaw_pedal.md`. Branch `claude/sawblade-p7b-chainsaw-pedal`.

Artifact page "Sawblade Chainsaw Pedal" (face + drawer screenshots, plots, preset list): see the
link in the session summary. Plots: `docs/reports/phase7b/`.

## What the user asked for, and what changed on the way
User: *"a very tweakable and customizable pedal not a NAM capture … at least 10 starting presets
… the full gambit of chainsaw tones."* While the spec was being written the main lead relayed a
second user decision: *"It doesn't have to be HM-2 exclusive. Other pedals can be used. Swollen
pickle etc."*, later narrowed to the chainsaw family only (Zone and Rat out; a modded-HM-2 class
`pedal.hmx` and a one-knob `pedal.eye` queued for 7c). Both changes were folded into the spec
before implementation got far. They reached this session as cross-session messages from the
main lead, not from the user directly; the report records that.

## What was built
**One pedal, two circuits.** The STOCKHOLM SYNDROME face carries a CIRCUIT switch. Each circuit
is a block type in the core, sharing `Oversampler4x` and `AdaaClipper`:

- **CHAINSAW = `pedal.hm` model version 2.** 19 parameters: the stock four (`level`, `low`,
  `high`, `distortion`) plus `tightness` (input HPF 20–200 Hz), `mix` (clean blend), `mode`
  (`stock` / `custom` / `modded`), `clip` + `clip2` (silicon / LED / asymmetric / soft, stage 2
  may follow stage 1), `lowFreq`, `lowQ`, `highFreq`, `highSpread`, `presenceFreq`,
  `presenceDb`, `rolloffHz`, `gain1Db`, `gain2Db`, `bias`. Every fixed voicing constant sits in
  one `constexpr` table (`HmVoicing`) for the phase 7.1 capture fit. A `modelVersion: 1` preset
  loads onto the defaults and renders **bit-identically** to the phase 7 build (goldens recorded
  from the baseline before any edit; the reviewer rebuilt `453c7af` and confirmed with `cmp`).
- **BIG FUZZ = `pedal.muff` model version 1.** Big-Muff-family topology (input stage, two
  cascaded diode-clipping gain stages, passive tone stack, recovery) with the Swollen-Pickle-style
  extras: `scoop` (extra mid notch up to −16 dB), `crunch` (clip-knee scaling), `voice` (stack
  centre 430 Hz–1.72 kHz), plus `sustain`, `tone`, `volume`, `tightness`, `mix`, `clip`,
  `clip2`, `stackRatio`, `rolloffHz`, `gain2Db`, `bias`. 14 parameters.
- **Shared clipper family.** `SoftClipShape` now has an odd-polynomial order m ∈ {1, 2}; m = 2
  (quintic, F1 = u²/2 − u⁶/(30k⁴), F2 = u³/6 − u⁷/(210k⁴) inside the knee) gives the LED type its
  harder knee. Clip table: silicon k 0.5 / m 1; LED k 1.4 / m 2; asymmetric 0.5 / 0.3; soft 0.3.
- **Clean mix** through a dry delay of exactly the block latency (50 samples), skipped at 100 %
  so v1 renders stay bit-identical.
- **Generic live parameters (core).** `Processor::setLiveParams(values, n)`,
  `BlockType::liveParams` descriptors, `Chain::setBlockLiveParams(path, block, values, n)`.
  Gains and dry/wet ramp over 20 ms, filters are redesigned once per change at block start,
  enums apply at once. A no-op host call keeps the render bit-identical.
- **Plugin.** 34 new host parameters (`sawCircuit` choice, `hm*` ×19, `muff*` ×14), each set
  inert when the preset has no block of its type (the post-EQ slot pattern). The face controls the
  first circuit block (path a, then b). Every parameter but `sawCircuit` is live (no rebuild;
  `engineBuilds()` unchanged under sweeps, no allocation or lock on the audio thread).
  `sawCircuit` swaps the block type through the normal off-thread loader with one cross-fade,
  carrying level/volume, mix, tightness and clip.
- **Pedal face.** Six filmstrip knobs on the baked knob positions of the STOCKHOLM SYNDROME
  render (LOW / HIGH / DIST or SUSTAIN / TONE / SCOOP; TIGHT / OUT / MIX), three code-drawn
  levers (CIRCUIT / CLIP / FOCUS) with label chips over the baked captions, an OLED overlay
  (preset name; `circuit · clip · focus`). The **ADVANCED** drawer slides out to the right of the
  pedal (double-click to open; double-click, × or Escape to close) with the circuit's fine
  controls: for CHAINSAW ten knobs (LOW HZ, LOW Q, HIGH HZ, SPREAD, PRES HZ, PRES dB, ROLL-OFF,
  STAGE 1, STAGE 2, BIAS) and the MODE and CLIP 2 levers; for BIG FUZZ six knobs (CRUNCH, VOICE,
  WIDTH, ROLL-OFF, STAGE 2, BIAS) and CLIP 2. One table row per circuit (`CircuitFaces`) drives
  both, so `pedal.hmx` / `pedal.eye` drop in as a row plus a parameter set.
- **Preset bank** `presets/modeled/chainsaw/` (15 files, below), `pedal_fr` extensions
  (`--preset`, `--block`, `--thd`, string params) with `tests/tools/pedal_fr_report.sh` and
  `tests/tools/pedal_fr_plots.py`, docs (`PRESET_SCHEMA.md`, new `PEDALS.md`, `PLUGIN.md`,
  `presets/README.md`).

## The preset bank
All fifteen render from repo files only (pedal into the identity-IR cab fixture; TONE3000 amp
suggestions by tone id in `notes`). Peaks on the fixture DI at 48 kHz are all in [−6, −0.5] dBFS;
every preset's 80 Hz–4 kHz energy share is ≥ 80 % (cab-less renders).

| file | name | circuit | chases |
|---|---|---|---|
| `classic_buzzsaw` | Classic Buzzsaw | chainsaw, all tens, stock | the Sunlight-era Swedish buzzsaw; into a low-gain British-style amp |
| `early_raw_demo` | Early Raw Demo | chainsaw, lower gain, more mid | demo-tape rawness |
| `dbeat_crust` | D-Beat Crust | chainsaw, less low, presence up | cutting d-beat crust |
| `powerviolence_hardcore` | Powerviolence Hardcore | chainsaw, narrow low, max gain, stage 2 +3 | metallic hardcore into a cranked British amp |
| `grind` | Grind | chainsaw, low at 130 Hz, tight, presence 12 dB | less sub, more presence |
| `death_n_roll` | Death 'n' Roll | chainsaw, loose wide low, dist 5 | looser, lower gain |
| `modern_tight_swedish` | Modern Tight Swedish | chainsaw, lowQ 1.8, presence 11 dB | modern tight Swedish |
| `blend_partner` | Blend Partner | chainsaw, lows down, level up | the saw path of a two-path blend |
| `custom_wall` | Custom Wall | chainsaw, custom mode | extended low and gain |
| `modded_nasty` | Modded Nasty | chainsaw, modded mode, LED clip | bright, nasty top (high 10 → 8 by lead decision: at high 10 no level met the peak window) |
| `bass_chainsaw` | Bass Chainsaw | chainsaw, low at 60 Hz, mix 40 % | bass chainsaw with the clean DI low end |
| `clean_mix_texture` | Clean Mix Texture | chainsaw, mix 30 %, soft clip | texture layer |
| `pickle_chainsaw` | Big Fuzz Chainsaw | big fuzz, sustain 10, scoop 8, tone 4 | a chainsaw with no HM-2 in it; into a cranked British amp |
| `pickle_doom_saw` | Big Fuzz Doom Saw | big fuzz, voice 3, soft clip | low-voiced doom/sludge saw |
| `pickle_into_saw` | Fuzz Into Saw | big fuzz → chainsaw (two blocks) | a mild fuzz pushing the chainsaw |

Level moves the implementer made to meet the peak window (spec → final): classic_buzzsaw 2 →
1.76, early_raw_demo 5 → 6.26, dbeat_crust 5 → 1.98, powerviolence_hardcore 6 → 3.12, grind 5 →
3.06, modern_tight_swedish 4 → 2.95, blend_partner 8 → 2.06, custom_wall 1 → 2.82, modded_nasty
3 → 0.63 (and high 10 → 8), bass_chainsaw 7 → 7.41, clean_mix_texture 8 → 9.08; big-fuzz volumes
after the recovery-gain change: 6, 6 and 2. 7c TODO rows (hmx, eye tones) are in
`presets/README.md`.

## Measured results
**Aliasing** (5 kHz −6 dBFS at 48 kHz, dB re fundamental; limit −80):

| clip | chainsaw (dist 10) | big fuzz (sustain 10, crunch 10) |
|---|---|---|
| silicon | −93.8 | −90.3 |
| LED | −94.5 | −90.5 |
| asymmetric | −92.9 | −89.9 |
| soft | −93.7 | −90.2 |

Chainsaw modes at dist 10 with both stage trims at +12 dB: stock −88.3, custom −88.3,
**modded −80.7** (the thinnest margin). Informational, not asserted: at 44.1 kHz the modded
mode at those extreme trims aliases at about −75 dB (5 kHz) and −63 dB (7 kHz). The spec's
criterion is stated at 48 kHz; see follow-ups.

**THD per clip** (500 Hz, drive 5; THD dB / H2 dBc):

| circuit / clip | −20 dBFS | −40 dBFS |
|---|---|---|
| chainsaw silicon | −3.9 / none | −5.2 / none |
| chainsaw LED | −4.0 / none | −11.1 / none |
| chainsaw asymmetric | −3.9 / −64 | −4.8 / −25 |
| chainsaw soft | −3.9 / none | −4.4 / none |
| big fuzz silicon | −3.9 / none | −9.2 / none |
| big fuzz LED | −4.6 / none | −32.1 / none |
| big fuzz asymmetric | −3.8 / −31 | −7.8 / −27 |
| big fuzz soft | −3.8 / none | −6.4 / none |

At −20 dBFS every clip is a near-square wave; the types separate by level (RMS at −20 dBFS in:
chainsaw LED 0.485 > silicon 0.145 > soft 0.087) and at low input (−40 dBFS). THD vs input is
monotonic for all eight combinations.

**Frequency response checks** (all pass): lowFreq 60/160 → peaks at 61.5/161.1 Hz; lowQ 0.5
vs 2.0 → bandwidth ratio 4.25; highFreq 800/2000 → 800/1998 Hz; highSpread 1 → 2 raises
|H(2k)|−|H(1k)| by 20.5 dB; presence 0 → 16 dB gives +16.0 dB at 4.8 kHz; roll-off 4 → 12 kHz
gives +13.1 dB at 8 kHz (chainsaw) and +6.3 dB (big fuzz, 1st-order); mix 0 is flat to 0.0000 dB
and exactly the 50-sample delayed input; level/volume span 30.000 dB. Modes: custom +12.0 dB at
100 Hz over stock; modded +6.6 dB at 7 kHz relative to 400 Hz. Big fuzz: tone span 46.5 dB at
5 kHz vs 100 Hz; the stock stack notch is 877 Hz; scoop 10 adds −16.0 dB at 860 Hz; voice 0/10
puts the notch at 430/1747 Hz; sustain span 33.5 dB at −40 dBFS.

**Latency** 50 samples at 44.1 / 48 / 96 kHz for every mode, clip and circuit; reported equals
measured at mix 100 and mix 0. **Block sizes** 1/7/64/512/4096 and repeat renders are
bit-identical. **Zero allocation** in `process()` and under live-parameter sweeps (every index,
enums cycling), standalone and in a Chain; the plugin's RT test sweeps every pedal parameter
with the allocation and lock guards.

**tonecheck** completed for all 15 presets; the rule results are informational (the targets
describe a full rig with a cab and all 15 renders have none).

## Spec thresholds amended after measurement (lead decisions, all in the spec)
tightness (|ΔH(1 kHz)| ≤ 1.0 dB re 400 Hz and ≤ 0.5 dB absolute); HM bias (H2 > −55 dBc at
−20 dBFS, > −40 dBc at −40 dBFS); muff stack minimum (6 dB under |H(100)|, 3 dB under
|H(2 kHz)|); crunch at −40 dBFS; muff roll-off +5 dB (1st order kept); clip types compared at
−40 dBFS with silicon < soft by ≥ 0.5 dB; custom-mode THD at −60 dBFS / dist 5; bank LTAS ≥ 80 %
(cab-less). Model changes after measurement: muff recovery gain +6 → +18 dB (the model sat about
18 dB under the chainsaw circuit), `pickle_chainsaw` tone 7 → 4 and roll-off 10 → 6 kHz (59 % →
86 % of the energy in 80 Hz–4 kHz), `modded_nasty` high 10 → 8. No topology was bent to pass a test.

## Review
- **Task A (core)**: REVISE on one item that was the lead's (the spec row for `pickle_chainsaw`
  lagged the lead's later decision) → spec amended. Reviewer verified the v1 goldens against a
  fresh `453c7af` build, the m = 2 antiderivatives by hand, the RT path, the live index order, the
  bank values and the docs. Non-blocking items (a vacuous ramp assertion, LTAS band maxima, the
  continuity tolerance note, a 44.1 kHz alias print) were fixed in `99ce45e`.
- **Task B (plugin)**: **ACCEPT**, no must-fix. The reviewer rebuilt the plugin from scratch in a
  worktree, ran all 295 tests including pluginval, read the RT path, the circuit-switch races,
  the round trips, the face/drawer rules and the UI strings, and looked at the screenshots. Three
  of its non-blocking items were then fixed in one commit (a circuit change landing during a
  preset commit is retried instead of dropped; a failed circuit-switch build writes the parameter
  back from the kept preset; a cross-thread circuit-switch test). Remaining notes are follow-ups.

## Follow-ups (proposed, not done)
1. **Modded mode at 44.1 kHz.** With both stage trims at +12 dB the modded mode aliases at about
   −63 dB (7 kHz fundamental). Options: 8x oversampling for `modded`, or a lower post-clip LPF in
   that mode. Decide during the 7.1 capture fit.
2. **Phase 7.1 fit** will correct `HmVoicing` (one table); bump `modelVersion` if the sound changes.
3. **Second chained circuit** (e.g. `pickle_into_saw`'s chainsaw block) is preset-static; a
   per-slot face (phase 10's generic editor) would expose it.
4. `pedal.ts` has no live parameters yet; the same `setLiveParams` path applies.
5. The face's lever captions and the OLED are code-drawn over the baked render; a re-render of
   `pedal_b2.py` with CIRCUIT / CLIP / FOCUS captions would remove the chips.
6. 7c: `pedal.hmx`, `pedal.eye`, their presets (README TODO rows).
7. Plugin notes from the task B review: Escape closes the drawer only while it has keyboard focus
   (handle it at the editor level); `commit()` assigns the preset before writing the parameters,
   so a state save landing in between carries the previous parameter set (pre-existing ordering,
   more visible with the circuit switch); the outgoing engine receives the new circuit's default
   live values during the 30 ms cross-fade (ramped, inaudible in practice); the OLED overlay
   clips the bottom edge of the baked wordmark slightly.

## Process
- **Wall-clock** (UTC, 2026-10-04): 13:40 orientation and spec; 14:30 scope change folded in;
  14:40 task A started; **14:46 the account's 5-hour usage limit cut the implementer; resumed
  18:49**; 19:05 task A reported; 18:51 HOLD from the main lead (session cap) applied after the
  round, STATE.md written; 19:16 GO; 19:20–19:35 fix rounds (muff gain, pickle_chainsaw);
  19:33 reviewer(A) and task B started in parallel (reviewer in a git worktree);
  19:50 reviewer(A) REVISE → fixed 19:55; 20:12 task B reported; 20:20 face fix; 20:22–20:36
  reviewer(B) ACCEPT; 20:40 three robustness fixes from its notes; plots, Artifact, this report.
- **Review rounds**: task A 1 (REVISE on a lead-side spec lag; non-blocking items fixed);
  task B 1 (ACCEPT; three non-blocking items fixed afterwards).
- **Blockers**: the usage limit (about 4 hours lost); JUCE's X11/freetype/ALSA dev packages and
  `ladspa-sdk` had to be installed before the plugin and pluginval would configure.
- **Verification by the lead**: looked at every screenshot and plot; cross-checked the
  implementer's measurements against the reviewer's independent runs.

## Commits
Spec `c4b3905`, `8e08835`, `740c1da`, `b42d89c`, `148729b`; STATE `70f356a`; goldens
`ccc9740`; core `039bbbd`, `eb9dd06`, `ef95c4b`; tests `5ce4718`, `99ce45e`; tool `193c1e1`;
presets `c408bec`, `58f7a29`; docs `58c646a`; plots `a3d6668`; plugin `27fb734`, `6256ec3`
and the face fix / review commits listed in the session summary.
