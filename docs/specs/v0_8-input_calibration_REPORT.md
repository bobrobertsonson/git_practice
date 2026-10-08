# v0.8 input calibration — REPORT

## Task A — audit

Audit only. No behaviour or code changed. Audited on 2026-10-07, branch `claude/sawblade-v0_8-input-cal`. Tags: **[verified]**
means read in source at the cited commit. **[search-only]** means the figure came from a search-engine extract of the vendor
page; the vendor domains (focusrite.com, darkglass.com, uaudio.com, audient.com) are blocked by this container's egress
proxy, so those pages could not be opened directly. **[unverified]** means no source settles it.

### A1. NAM calibration metadata in the pinned versions

**NeuralAmpModelerCore @ `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842`** (pinned at `cmake/Dependencies.cmake:19`; the
`build/_deps/nam_core-src` checkout is at the same commit) **[verified]**

| What | Where |
|---|---|
| JSON keys read from `.nam` `metadata`: **`input_level_dbu`**, **`output_level_dbu`** (and `loudness`) | `NAM/get_dsp.cpp:275-277` |
| Parsed into `ModelMetadata::input_level` / `output_level` (`std::optional<double>`) | `NAM/model_config.h:27-28` |
| Applied to the DSP on every `get_dsp` / `create_dsp` path (`SetInputLevel` / `SetOutputLevel`) | `NAM/get_dsp.cpp:232-240` |
| API: `HasInputLevel()`, `GetInputLevel()`, `HasOutputLevel()`, `GetOutputLevel()` (non-const) | decl. `NAM/dsp.h:125,142,148,158`; impl. `NAM/dsp.cpp:169-187` |
| Meaning, from the doc comment: "Input level is in **dBu RMS, corresponding to 0 dBFS peak for a 1 kHz sine wave**" (output: the same wording) | `NAM/dsp.h:120`, `NAM/dsp.h:137` |
| A missing key leaves `Has*Level()` false. Calling `Get*Level()` without checking returns 0.0 and does not throw. `GetLoudness()` throws instead. | `NAM/dsp.h:122,139`; `dsp.cpp:169-177` |

**Trainer:** pinned as `neural-amp-modeler==0.13.0` (`match/pyproject.toml:43`, `match/constraints-export.txt:3`). Inspected at tag
`v0.13.0`, commit `f26112906de06ec6b796ad6d1982e29eed83144e` **[verified]**.

| What | Where |
|---|---|
| `UserMetadata.input_level_dbu: Optional[float]`, `output_level_dbu: Optional[float]` | `nam/models/metadata.py:66-67` |
| Docstring: input = "what analog loudness, in dBu, corresponds to 0 dbFS input to the model"; output = "… 0 dbFS outputted by the model" | `nam/models/metadata.py:54-57` |
| Written flat into the `.nam` `metadata` object (`user_metadata.model_dump()`), so the core finds it at the key it reads | `nam/models/exportable.py:101` |
| GUI labels: "Reamp send level (dBu)" / "Reamp return level (dBu)". Measurement method: a 1 kHz sine at 0 dBFS **peak**, measured as **RMS** volts at the jack and converted to dBu | `nam/train/gui/__init__.py:1267-1300` |
| `GearType` values: `amp`, `pedal`, `pedal_amp`, `amp_cab`, `amp_pedal_cab`, `preamp`, `studio` | `nam/models/metadata.py:16-23` |

**Exact meaning:**
- `input_level_dbu` = the dBu RMS of a 1 kHz sine whose digital peak is 0 dBFS, measured at the **reamp send** that fed the gear.
- `output_level_dbu` = the same at the interface input that recorded the gear (the return path).

Both are a full-scale sine's RMS in dBu. This is the same convention interface makers use for "max input level", which
is a sine at clip. That last point is industry practice, not something stated in a NAM file **[unverified as a NAM statement]**.

**Reference plugin (sdatkinson/NeuralAmpModelerPlugin)**, inspected at release tag `v0.7.15` (`96337e9a`) and at HEAD
`16be8697` (2026-08-10). The relevant lines are identical in both **[verified]**.

| What | `NeuralAmpModeler/NeuralAmpModeler.cpp` |
|---|---|
| Params `CalibrateInput` (bool, default **off**) and `InputCalibrationLevel` (dBu, default **+12.0**, range −60..+60) | `:72-75`, `:93-95` |
| Input gain: `inputGainDB = Input knob + (InputCalibrationLevel − model->GetInputLevel())`, only when the model `HasInputLevel()` and calibration is on | `:690-699` (formula `:696`) |
| Output mode "Calibrated": `gain += output_level_dbu − InputCalibrationLevel`. Output mode "Normalized" uses `−18 − loudness` | `:716-723`, `:708-715` |
| Calibration controls are disabled when the model has no input level | `:949-951` |

Our planned `gainIn = deviceDbu − captureInputDbu` is the same formula. The plugin's `InputCalibrationLevel` is the
interface's dBu at 0 dBFS.

### A1b. Default interface level and interface presets

**The spec's +9 dBu has no source.** None of the sources below give +9.

| Source | Value (dBu at 0 dBFS / max input, instrument input) | Condition | Cite | Tag |
|---|---|---|---|---|
| NAM plugin default `InputCalibrationLevel` | **+12.0** | — | `NeuralAmpModeler.cpp:75` @ v0.7.15 | verified |
| Focusrite Scarlett 4i4 **3rd gen**, Inst | **+12.5** no PAD / **+14** PAD | "measured at minimum gain" | https://userguides.focusrite.com/hc/en-gb/articles/23031514701842 ; https://fael-downloads-prod.focusrite.com/customer/prod/downloads/Scarlett%204i4%203rd%20Gen%20User%20Guide%20V2.pdf ; https://eu.focusrite.com/products/scarlett-4i4-3rd-gen | search-only |
| Focusrite Scarlett 4i4 **4th gen**, Inst | **+12** | "at minimum gain" | https://fael-downloads-prod.focusrite.com/customer/prod/downloads/scarlett_4i4_4th_gen_user_guide_v2-pdf-en.pdf ; https://userguides.focusrite.com/hc/en-gb/articles/18676425695890-Scarlett-4i4-Specifications ; https://us.focusrite.com/products/scarlett-4i4 | search-only |
| UA Volt 2 (Gen 1), Inst | +12.5 (Volt 2 Gen 2: +13) | — | https://help.uaudio.com/hc/en-us/articles/4409183769620-Volt-2-Hardware-Manual ; https://www.uaudio.com/products/volt-2-gen-2 | search-only |
| Audient iD4 MKII, DI | "Lineup 12 dBu = 0 dBFS" (original iD4: +8 dBu max) | — | https://support.audient.com/hc/en-us/articles/360055048851-iD4-MKII-Technical-Specifications | search-only |
| Darkglass Anagram (as USB interface) | **unpublished, measure** (see note) | — | https://www.darkglass.com/pages/anagram-manual ; https://api.darkglass.com/static/Anagram-Manual.pdf | unverified |

- **The lead's figures for the 4i4 check out.** The extracts from the Focusrite user guide pages match the lead's figures exactly: 3rd gen
  +12.5 dBu without PAD and +14 dBu with PAD, 4th gen +12 dBu, all "at minimum gain". I could not open the PDFs themselves
  (blocked), so the user should confirm the figures against their own unit's guide.
- **Darkglass Anagram.** One search summary listed "Instrument input … Max Input Level: +12 dBu". It cited no
  identifiable Darkglass page, and I could not open the Darkglass manual (blocked). Even if the figure is right, it is the
  analog jack's clip point. The Anagram's own documentation does not say how its USB send maps dBFS to that jack, and the
  send may sit after its input DSP or trim. Write: **"unpublished, measure"** (guided measurement).
- **Focusrite Control 2 gain readout (4i4 4th gen).**
  - The app shows preamp gain as a number in dB. Focusrite's 2i2 4th-gen guide says the slider "can be set in steps from 0 to
    70 dB". In Inst mode the minimum gain becomes **+7 dB** (Focusrite hardware-features pages,
    https://userguides.focusrite.com/hc/en-gb/articles/18676425481362-Scarlett-4i4-Hardware-Features ; 2i2 guide
    https://fael-downloads-prod.focusrite.com/customer/prod/downloads/scarlett_2i2_4th_gen_user_guide_v4-pdf-en.pdf)
    **[search-only; the 0–70 range is quoted from the 2i2 guide, not confirmed for the 4i4]**.
  - So the lead's formula **"12 − N" would be wrong.** At minimum Inst gain the readout already shows N = 7, not 0. The
    closest form the documents support is 12 − (N − 7).
  - That formula is still **unverified**. No Focusrite document I found says that the gain is applied in the analog domain
    before the ADC, that a 1 dB readout step is exactly 1 dB, or what the max input is above minimum gain. (Reviews call
    the preamp "digitally controlled analogue". That is a review, not Focusrite documentation.) Do not compute the max input
    from the readout. Use minimum gain, or measure.
  - Air mode is not covered by the max-input spec either **[unverified]**. Store "Air off" as part of the assumption.

**Design inputs for Task B / C** (from the lead, checked against the evidence above):
1. **Recommended setup: interface gain at minimum, with Sawblade supplying all gain internally.** Every published figure
   holds only at minimum gain. The device step must say "set the instrument gain to minimum (PAD and Air as chosen)", or
   offer the guided measurement. The calibration record stores `{dBu, method: preset|manual|measured, source/model,
   gainAtMinimum: true, pad, air, date}`.
2. **Interface presets, each with its citation:** Scarlett 4i4 3rd gen Inst = +12.5 dBu (no PAD) / +14 dBu (PAD);
   Scarlett 4i4 4th gen Inst = +12 dBu. Then "Enter dBu" and "Measure". Optional extras with citations: Volt 2 +12.5,
   iD4 MKII +12. The Anagram gets no preset; it routes to "Measure".
3. **Default: recommendation.**
   - **Use an assumed +12 dBu internally, and flag it.** That is the NAM plugin default, Focusrite 4th gen, and Audient
     iD4 MKII. The 3rd gen and Volt are +12.5. No sourced value is +9.
   - Until the user picks a preset or measures, show the input as **uncalibrated**. This matches the lead's "no default" in
     the UI.
   - Why not the neutral fallback (gainIn = 0 dB, today's behaviour)? With 0 dB, swapping two captures whose
     `input_level_dbu` differ still changes the drive by the full difference. That is exactly the problem v0.8 exists to
     fix.
   - Under any assumed value, `gainIn_k = assumed − captureIn_k` keeps the *differences* between captures correct. The
     device value only shifts every capture's drive by the same amount. Across the sourced interfaces that shift is within
     about ±1 dB of +12, compared with about 3 dB for the spec's +9.
   - The neutral 0 dB fallback should apply only per block, when a *capture* lacks `input_level_dbu` (Task B's
     missing-metadata rule). It should not apply when the device is uncalibrated.

### A2. TONE3000 coverage of calibration fields

**What the API exposes.** In our client, neither the API's tone record (`Tone`, `match/sawblade_match/t3k/types.py:59-110`)
nor its model record (`Model`, `types.py:113-133`) has any level or calibration field. The fields are: id, title, gear,
format, license, sizes, counts, `model_url`, `architecture_version`. Unknown keys are kept in `.raw` (`types.py:79,132`), so a
field added to the API later would still be visible there. **Calibration lives only inside the `.nam` file's
`metadata`**, and it is optional (it is blank unless the creator typed it into the trainer GUI). So coverage has to be counted from
downloaded `.nam` files. The repo references no API documentation that mentions calibration fields.

**TONE3000 gear values** (`filter.py:66-75`): `amp` (amp only, no cab; this is the "IR-less amp"), `pedal`, `amp-cab` /
`full-rig` (used only as a reference, never a slot), `cab` / `ir` (IR, not NAM). The `.nam` file also carries the trainer's own
`gear_type` (see A1), which can disagree with the TONE3000 `gear`.

**Counts: none available here.**
- This container has no capture cache (`~/.cache/sawblade/captures` absent, `SAWBLADE_CACHE_DIR` unset; root from
  `cache.py:22-24`).
- It has no OAuth tokens (`~/.config/sawblade/t3k_tokens.json` absent; `auth.py:28-32`). Only `TONE3000_CLIENT_ID` is set.
- So there are no counts, and no numbers are invented.
- The only `.nam` files on disk are test fixtures. `tests/fixtures/nam/wavenet.nam` and `lstm.nam` (from the NAM core
  examples) carry `input_level_dbu: 18.3` and `output_level_dbu: 12.3`, which is useful for Task B tests.

**How to count (for the lead to run on the user's machine).** This is read-only and writes counts only, no capture files:

```
# python -I count_cal.py [cache_root]   (default: sawblade_match.t3k.cache.default_cache_root())
import json, sys, collections
from pathlib import Path
from sawblade_match.t3k.cache import default_cache_root
root = Path(sys.argv[1]) if len(sys.argv) > 1 else default_cache_root()
c = collections.Counter()
for meta_p in root.glob("*/meta.json"):
    meta = json.loads(meta_p.read_text()); tone = meta.get("tone", {})
    gear = tone.get("gear") or "?"
    for mid, m in (meta.get("models") or {}).items():
        f = meta_p.parent / f"{mid}.nam"
        if not f.exists(): continue                       # IRs (.wav) and missing files skipped
        md = json.loads(f.read_text()).get("metadata") or {}
        arch = "A2" if str(m.get("architecture_version") or "").startswith("2") else "A1"   # adjust to how the cache records A1/A2
        key = (gear, arch)
        c[key + ("total",)] += 1
        c[key + ("in",)]   += md.get("input_level_dbu")  is not None
        c[key + ("out",)]  += md.get("output_level_dbu") is not None
        c[key + ("both",)] += (md.get("input_level_dbu") is not None and md.get("output_level_dbu") is not None)
        c[key + ("loud",)] += md.get("loudness") is not None
for k in sorted({k[:2] for k in c}):
    t = c[k + ("total",)]; print(k, t, *(f"{n}={c[k+(n,)]}({100*c[k+(n,)]/t:.0f}%)" for n in ("in","out","both","loud")))
```

How the cache records A1 vs A2 is **[unverified]**: check `meta["models"][id]` and `Model.architecture_version` before trusting
the `arch` split. Also run it split by the `.nam`'s own `gear_type` to catch disagreements with TONE3000's `gear`. Report the
table in this file. Do not commit the script output if it includes titles; counts only.

### A3. Every level applied to a NAM block today

Signal order (`Chain::processChunk` `core/src/chain.cpp:913-959`, `renderPath` `:705-724`): INPUT → gate → per path: pre-EQ →
for each block [amp GAIN before the amp block → block in-gain → NAM → block out-gain/normalise/make-up → amp tone stack and
LEVEL] → path EQ → path level × level-match trim → cab → align delay → blend → shared cab → post EQ → bus comp → OUTPUT →
auto trim.

"Drive" below means: does it change the level going into a NAM (or other non-linear) block?

| # | file:line | What | Before/after the NAM | Changes drive? | Set by |
|---|---|---|---|---|---|
| 1 | `chain.cpp:307`, `:927`; plugin `Engine.cpp:164` | INPUT gain (`inputGainDb`) | before (all NAMs) | **yes**, all blocks of both paths | user / preset |
| 2 | `chain.cpp:928` | gate (keyed on DI) | before | yes, dynamically (attenuates while closed); not a static level | preset / matcher (DI noise floor, `matcher/run.py:135`) |
| 3 | `chain.cpp:711-712`, `:261` | path pre-EQ | before | **yes**, frequency-dependent | user / preset / matcher (`space.py:13`) |
| 4 | `nam_block.cpp:101-102`, `block_registry.cpp:20,34` | block `inputGainDb` (live: `nam_block.cpp:108-127`, `chain.cpp:855`) | before (that block) | **yes** | preset / matcher (searched ±12 dB, `matcher/space.py:18,134,202-214`) |
| 5 | `nam_block.cpp:103`, `block_registry.cpp:21,35` | block `outputGainDb` | after | **yes for the next block** (pedal NAM → amp NAM); no if last | preset (matcher fixes 0, `space.py:20`) |
| 6 | `nam_block.cpp:104`, `:111`; `block_registry.cpp:22` | `normalizeLoudness`: `out += −18 − loudness` (`.nam` `loudness`) | after | **yes for the next block**. The matcher turns it on only for amps (`space.py:213-214`), so the pedal→amp hop is not normalised there; a user preset may normalise a pedal | preset / matcher |
| 7 | `auto_trim.cpp:151-170`, `block_registry.cpp:23,35`; plugin `rig/Pedalboard.cpp:689,741`, `browser/BrowserController.cpp:286` | capture-swap make-up `makeupDb` = path LUFS before − after, added to the swapped block's out-gain (±24 dB) | after the swapped block | **yes when the swapped block feeds another NAM** (pedal swap → amp drive changes); no for the last block. Measured on the whole path, so it is only approximate through a compressing amp | auto (on swap) |
| 8 | `chain.cpp:714`, `amp_controls.cpp:167`, `amp_controls.h:38-40` | amp GAIN knob, (k−5)·2.4 dB before the path's amp block | before amp | **yes** | user / preset |
| 9 | `gain_ladder.h:38-39`, `chain.cpp:409-420`, `:913-921`; `gain_ladder.cpp:83-85` | gain ladder: rung = another capture of the same amp. Residual GAIN (knob − rung pos)·2.4, ±12 dB. Rungs share the block's in/out gains | before amp; rung swap | **yes** (residual is pre-amp; the rung itself is a different capture whose `input_level_dbu` may differ, and that is not compensated) | user GAIN knob → auto |
| 10 | `chain.cpp:716`, `amp_controls.cpp:169-` | amp tone stack + LEVEL | after amp | no, unless another NAM block follows the amp | user / preset |
| 11 | `pedal_ts.cpp:41,92`, `pedal_hm.cpp:178`, `pedal_hmx.cpp:93`, `pedal_muff.cpp:57`, `pedal_params.h:89` | modelled pedal LEVEL/VOLUME `3·k − 24` dB (Muff + recovery) | after the pedal, before the next NAM | **yes** (drives the amp NAM) | user / preset |
| 12 | `chain.cpp:719-720` | path EQ | after | no | user / preset / matcher |
| 13 | `chain.cpp:270`, `:439-442`, `:721`, `:482-489` | path `levelDb` + level-match trim `trimDb` (auto/manual, 10.1) | after | no | user / preset / auto |
| 14 | `chain.cpp:445-459` | blend weights + ConstantLoudness make-up (±12 dB) | after | no | user / preset / auto |
| 15 | `chain.cpp:953-955` | post EQ, bus comp | after | no NAM drive (the level does change the comp's behaviour) | preset |
| 16 | `chain.cpp:956`, `:802-811`, `:957`; `auto_trim.h:13-26` | OUTPUT gain, auto trim (−18 LUFS) | after | no | user / auto |
| 17 | `reference_di.h:22`; `chain.h:241,246` | measurement signals: reference DI (−10 dBFS peak) for the auto trim / make-up; level-match probe (−18 dBFS RMS noise, −12 dBFS peak) | probe only | no effect on playing. These fix the level the *measurements* drive the NAMs at, so trims are drive-dependent | fixed constants |
| 18 | `match/sawblade_match/matcher/run.py:272`, `:487-516` | matcher: reference/user DI read as-is (no scaling); output gain fitted after render | before (DI level) / after | DI level: **yes, implicitly** (whatever level the DI file has drives every capture; a NailTheMix DI is not the user's guitar) | file |
| 19 | `match/sawblade_match/export/train.py:83,226`, `export/signal.py:41`, `export/run.py:237-240` | NAM export: training signal at pink levels −48…−12 dBFS; trainer output normalised to −18 dBFS RMS; `input_level_dbu`/`output_level_dbu` written **empty** | n/a (training) | the exported model's drive reference is digital only | fixed |

**19 rows.**
- **Rows that change NAM drive:** 1 INPUT, 2 gate (dynamic), 3 pre-EQ, 4 block in-gain, 5 block out-gain (next block),
  6 normalizeLoudness (next block), 7 swap make-up (when it feeds another NAM), 8 amp GAIN, 9 gain ladder residual/rung,
  11 modelled pedal LEVEL, 18 matcher DI level.
- **Biggest gap for v0.8:** rows 6 and 7 set the pedal→amp hop from *loudness*, not from dBu. A pedal swap can change amp drive
  by up to ±24 dB with no calibration behind it.

**Does core read `input_level_dbu` / `output_level_dbu` today?** **No.**
- `git grep` over the tracked `core/ plugin/ cli/ bindings/ match/ tests/` finds the fields only in two places: the test
  fixtures, and the export, which writes them as `None` (`export/run.py:239-240`, `match/README.md:441`).
- `NamBlock` reads only `loudness`, `name`, `gear_type`, `modeled_by` (`nam_block.cpp:43-52`). It never calls
  `HasInputLevel` or `GetInputLevel`.
- The untracked `core/include/sawblade/calibration.h` and `core/src/calibration.cpp` are Task B work in progress by another
  agent. They were not audited.

## I1 — chain wiring

Branch `claude/sawblade-v0_8-input-cal`, core only (plugin, preset schema, `match/` and the export are untouched). Calibration is
opt-in and off by default: with it off the full ctest (415 existing tests) is unchanged, no golden was regenerated.
Numbers below come from `tests/test_chain_calibration.cpp` (the two `[report]` test cases print them; run
`build-i1/tests/sawblade_tests "[report]"`). Build: Release, warnings-as-errors, no warnings.

**Fixture fact the spec got wrong.** `wavenet.nam` and `lstm.nam` carry the same metadata: `input_level_dbu 18.3`, `output_level_dbu
12.3`. The 6.0 dB in the spec is output-minus-input, not a wavenet-vs-lstm input difference. So a wavenet <-> lstm swap plans a 0 dB change
(asserted), and the "exactly 6.0 dB" swap and ladder tests use new synthetic fixtures `tests/fixtures/nam/cal_*.nam` (Linear identity
models with invented dBu, written by `make_cal_fixtures.py`, noted in `NOTICE`).

### Acceptance 8 — SYNTHETIC — redo on the user's L_ubr_quick preset

Stand-in chain: `wavenet.nam` as the front (pedal) stage -> `lstm.nam` as the amp, both capture levels 18.3 in / 12.3 out. No
normalise, no make-up, so on and off differ only by calibration. Device: the assumed +12 dBu (`deviceAssumed` true), because the
user's interface is not calibrated yet. These are the NeuralAmpModelerCore example nets, not the user's captures; they are small and
their output sits near -60 dBFS, so treat the sweep as a wiring check, not as tone information.

| Block | Planned gain, on | Offset re the capture's calibrated drive, off -> on |
|---|---|---|
| pedal stage (wavenet) | -6.30 dB (12 - 18.3) | +6.30 -> 0.00 dB |
| amp (lstm) | -6.00 dB (12.3 - 18.3, the hop) | +6.00 -> 0.00 dB |

"Offset" = the gain the block applies minus the gain that puts the signal exactly at the capture's input level (`refBefore - input_level_dbu`).
Off, both blocks run 6.3 / 6.0 dB hotter than their captures expect. On, both are at 0.

Amp input level, measured by rendering a 220 Hz sine at -24 dBFS RMS through the pedal stage and applying the amp's input gain:

| | amp input level |
|---|---|
| calibration off | -39.24 dBFS |
| calibration on | -51.39 dBFS |
| moved | -12.16 dB |

In this SYNTHETIC stand-in (assumed +12 dBu device, example nets with 18.3 dBu input), the amp drive moved by 12.16 dB (>= 3 dB). This is
fixed by the fixtures' metadata and is not evidence about the user's chain; redo on the real preset. (The planned gains sum to -12.30 dB; the
pedal stage's own nonlinearity accounts for the other 0.14 dB.)

Dynamics sweep, 220 Hz sine, -30 to -12 dBFS RMS in 3 dB steps (18 dB), output RMS in dBFS:

| in (dBFS) | -30 | -27 | -24 | -21 | -18 | -15 | -12 |
|---|---|---|---|---|---|---|---|
| off | -59.55 | -58.92 | -57.91 | -56.48 | -54.69 | -52.80 | -51.10 |
| on | -60.40 | -60.35 | -60.25 | -60.07 | -59.73 | -59.15 | -58.27 |

Least-squares slope (dB out per dB in): off 0.486, on 0.111. In this SYNTHETIC stand-in (assumed +12 dBu device, example nets with 18.3 dBu
input) these figures are fixed by the fixtures' metadata and are not evidence about the user's chain; redo on the real preset. With
calibration the stand-in runs ~12 dB lower, close to the nets' floor near -60 dBFS, which is why the slope drops. The matched preset's values were
tuned without calibration (spec decision 1), which is why the default stays off. The tests assert these numbers, so this section cannot drift from
the code.

### Acceptance 9 — gate check at a -49.5 dBFS floor

Gaussian noise, -49.5 dBFS RMS, 10 s, seed 17. Gate = the v0.4M Task H record cell as the matcher builds it: `peakFloorDb` (the 92.5th
percentile of the gate's own peak envelope) = -42.32 dBFS for this noise; open = peakFloor + 10 = -32.32 dBFS, hysteresis 6 dB (close -38.32).
Chain: linear identity `cal_amp_hi` (input 12 dBu) with device 24 dBu, so calibration plans +12 dB. A gate keyed after that gain would see a floor
near -30 dBFS, above the open threshold. The open fraction is the share of 10 ms windows after 0.5 s in which the chain passes the noise (window
gain, with the known gain divided out, above -3 dB).

| | gate open fraction on noise only |
|---|---|
| calibration off | 0.0000 |
| calibration on (+12 dB planned) | 0.0000 |
| control: INPUT +12 dB (before the gate), calibration off | 1.0000 |

The control shows the test can fail: the same +12 dB placed before the gate opens it on every window. Calibration, applied after the gate's key,
does not.

### Design as built (for I2-I4)

- `ChainCalibration {enabled, device, defaults}`; `Chain::planCalibration` (any thread, allocates) -> `CalibrationPlan`;
  `Chain::setCalibration` (plan + apply, not RT-safe); `Chain::publishCalibration` (producer thread; the audio thread takes the plan from a
  `SwapSlot` at the start of `process()` and ramps over `kLiveRampMs`). `RenderOptions::calibration` carries it for offline renders; the report
  gains a `calibration` object (always present, `enabled:false` when off).
- Each NAM block (each gain-ladder rung too) computes its own planned gain from its own metadata with `calibration::planBlock`, so a rung
  swap uses the rung's levels. The planned gain is added to the block's intent (`inputGainDb`, live or preset).
- A NAM block that feeds another NAM block in the path drops `normalizeLoudness` and the make-up; `outputGainDb` stays. Only the last NAM
  block of a path keeps both. Bypassed blocks and disabled paths are not in the plan. `eq` and every `pedal.*` block are `LevelKind::Neutral`.

### Decisions made during review

- **Make-up and live edits.** `LiveBlock` now carries `makeupDb` apart from `outputGainDb`, and `Processor::setLiveGainsDb(in, out, makeup, ramp)`
  has a 4-argument form (the 3-argument one stays; it means "output includes any make-up"). A calibrated hop block drops the CURRENT make-up, so a
  live make-up edit leaves the amp's input gain unchanged (tested). With calibration off the sum is outputGain + makeup + normalise in the same
  order as before: all goldens and live tests unchanged. `LiveParams::fromPreset` now stores the make-up in `makeupDb` instead of in
  `outputGainDb`; the plugin does not touch `LiveBlock`.
- **A hop into a block with no usable input metadata (no `input_level_dbu`, no gear default) is not planned.** The upstream block keeps its
  normalise and make-up: today's behaviour, from the "missing = neutral" rule (spec decision 3a). Such a hop is flagged by the downstream block's
  `inputMissing` in the report. Tested with a hop into `cal_amp_nometa`.
- **Ladder limit.** A downstream NAM block is planned from the ladder block's STARTING rung's output level and does not follow a rung swap (the
  ladder's own input gain does). Pinned by a test: ladder of two pedals with different output levels, the amp after it keeps its gain across the swap.
- **Mid-stream plan publish.** A plan published while audio runs applies at the start of the next `process()` call, so the sample it starts on
  depends on the host block size. The ramp itself is sample-accurate.
- **Comparing hashes across branches.** After the v0.4M merge, `preset_hash` in `match/export/run.py` is computed over the flattened preset for presets
  with `dynamicsMode`, so hashes from before and after the merge are not comparable.

## I2 — device step

Branch `claude/sawblade-v0_8-input-cal`. Core: calibration-aware auto trim / path measurement / swap make-up (`skippedHop`), live-gate floor seed and learned-floor atomic.
Plugin: device record in Settings, "Calibrated input levels (beta)" toggle (default off), UNCAL badges, uncalibrated notice, hop make-up 0, floor persistence.
See `docs/PLUGIN.md` "Device step and calibrated input levels".

### Proposed guided Measure (not built in I2)
1. The app plays a 1 kHz sine of known digital level through its own output; the user loops the interface output into the instrument input at minimum gain.
2. The app reads the captured RMS in dBFS and, with the user-entered dBu of the source (from a meter or a known-level tone generator), derives the dBu at 0 dBFS.
3. Repeat at two levels and check the slope is 1 dB per dB, to catch input DSP and any gain-readout trap.
4. Store `method: measured`.
It needs a reference signal of known dBu from the user, which is why I2 only has the stub.

### Decisions (implementer)
1. The gate floor seed is used only with the toggle on (toggle off stays bit-identical); the learned floor is persisted whenever a record exists.
2. The "not calibrated" notice shows only with the toggle on and no record (the +12 dBu assumption is only true then).
3. No existing audio-to-UI channel fits a single value (InputMeter is a ring of peaks): a relaxed `atomic<float>` in the chain, read on the message thread via `published_`.
4. With calibration on, the trim cache key gets a `|cal:<dBu>` suffix; the stored trim is not read from the preset and the calibrated trim is not written back (I4 decides the schema).
5. On a hop block the make-up is set to 0 for the new capture (the old one is wiped); adding a pedal stores none.
6. `Settings::shared()` is now touched in `submit()` and `levelOnLoad`; the test Host harness isolates the settings file unless a test already did.
7. UNCAL badge on capture pedal tiles and on the amp head pill (same `paintBadge` style); no tooltip yet.
8. The PREVIEW audition still renders uncalibrated (out of the I2 list).
9. Choosing a new device entry clears the learned floor (the record is the key); re-choosing the same preset also clears it.

### Unverified until CI
Everything under `plugin/` was only syntax-checked (`g++ -fsyntax-only -Wall -Wextra -Wpedantic -Werror`) here; no X11 build. Run locally in scratch binaries: the record, presets,
validation and Settings tests, and the Engine tests (off bit-identical, summary, seed and learned floor with no allocation). CI must prove: all processor-level cases in
`test_device_calibration_engine.cpp` (rebuild counts, trim keying, hop skip, floor persistence), the SettingsPanel layout and the interface step, the editor / pedalboard /
browser tests (UNCAL badges, notice label, hop make-up 0), and the effect of the Host harness settings isolation on the other processor tests.
