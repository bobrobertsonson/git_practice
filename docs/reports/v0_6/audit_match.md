# v0.6 Task A (trainer / match side): what A2 is in the pinned trainer, where `match/` assumes A1, official A1 presets, fixtures

Author: match-engineer, 2026-10-07. Audit plus A.3 (sizes) and the fixtures; no A2 export yet (that is Task C), no dependency bump.
Companion: `docs/reports/v0_6/audit_core.md` (dsp-engineer; the pinned C++ core, which agrees with everything below).

Sources, all read from the installed packages (venv: `pip install -e 'match[export]' -c match/constraints-export.txt`, Python 3.11,
`neural-amp-modeler 0.13.0`, `torch 2.5.1`, `pytorch-lightning 2.6.1`, nothing bumped):

* **T13** = `neural-amp-modeler==0.13.0`, the PINNED trainer. Paths below are relative to `site-packages/nam/`.
* **T12** = `neural-amp-modeler==0.12.3` (wheel downloaded only to read it, never installed or pinned). Needed because **0.13.0 no longer
  contains the official A1 presets** (section 1.5).

## 0. Findings in one screen

1. **The `.nam` file does not say "A2".** There is no A2 flag, version bump or special key. An A2 file is declared by *shape*:
   `architecture: "SlimmableContainer"` (the packed export, `config.submodels[] = {max_value, model}`) or a plain `WaveNet` whose
   config has one 23-layer array (LeakyReLU, kernels 6/15, dilations 1,3,7,17,41,101,239, head kernel 16). Model-file `version` is `"0.7.0"`
   for A2 *and* for an A1 exported by the same 0.13.0 trainer (`models/_constants.py:4`). "A2" vs "A1" is a TONE3000 `architecture_version`
   field on the API record, not something in the file.
2. **What the trainer offers for A2: one recipe, two sizes, one container.** `train/_resources/config_model_packed.json` defines a
   `PackedWaveNet` with submodels `channels_3` and `channels_8`; `export_container` writes one `SlimmableContainer` file with them at
   `max_value` 0.5 and 1.0. The trainer never says "A2 Full" or "A2 Lite". The mapping **Lite = 3 channels, Full = 8 channels** is the one
   the pinned core's fast path uses (`audit_core.md`) and is the only reading consistent with the trainer's config; the names themselves
   come from TONE3000 / the Anagram (not documented in the trainer).
3. **A2 vs A1 numbers (same trainer, measured):** A2 Full 12 145 params, A2 Lite 1 870, container 22 783; receptive field **6347** for every A2
   size, **4093** for every official A1 preset; A2 training costs about 1.5x A1 standard per step on CPU (the packed net trains Lite and Full together).
4. **A.3 surprise: the "official A1 presets" are not in the pinned trainer.** 0.13.0 deleted `Architecture` / `get_wavenet_config`; its
   `core.py` only builds the packed A2 net. The presets (standard / lite / feather / nano) exist byte-identically in 0.11.0 to 0.12.3. A.3
   copies them from 0.12.3 with file and line, and a test pins them (parameter counts and the 4093 receptive field are checked against the
   *pinned* 0.13.0 `WaveNet`). **Sawblade's `standard` was already the official preset; `lite` and `feather` had the right channels but the wrong
   layer split** (10 + 10 dilations instead of 7 + 13): params 7 903 -> 6 553 and 3 637 -> 3 025.
5. **`match/` has no A1-specific reader of `.nam` internals.** The only A1 assumptions are in the export trainer (`export/train.py`), its
   acceptance rule (`validate.py`), the report text, and the pool's size ranking; everything that reads captures goes through the C++
   core (`sawblade_core`). The pool's A2 preference already exists (`t3k/filter.py`, `t3k/fetch.py`). Table in section 2.
6. **Two Python tests fail on the unchanged baseline** (not caused by this task; see "Test results").

## 1. A.1 What A2 is, from the pinned trainer source

### 1.1 The packed / slimmable container

* `models/factory.py:22-29` registers the nets `ConvNet, Linear, LSTM, Sequential, WaveNet, PackedWaveNet` (`PackedWaveNet` imported at `:18`); `models/wavenet/__init__.py:51-52` exports
  `PackedWaveNet` and the classic `WaveNet`.
* `PackedWaveNet` (`models/wavenet/_packed_wavenet.py:28`) is **one wide network holding several submodels**. Its config is
  `{"submodels": [{"name", "config"}...], "export": {...}}` (`_packed_wavenet.py:41-50`). The submodels must be identical in layer-array count, depth,
  kernel sizes, dilations, activation, head kernel and `head_scale`; they may differ only in `channels` (and so bottleneck / head sizes)
  (`models/wavenet/_packed.py:232-359`, e.g. matching dilations `:286-287`, kernels `:284-285`). Unsupported: `condition_dsp`, top-level `head`, grouped
  convs, FiLM, gated/paired activations (`_packed.py:232-245, 362-381`), more than one output channel per submodel (`:319-322`). So A2 packed
  is mono-in, mono-out, ungated.
* Internally the submodels are packed into one `WaveNet` whose channel counts are the *sums* (`_packed.py:88-116`, `ref["channels"] = sum(...)`), with
  per-submodel block masks on the weights (`PackedConv1dBase.apply_mask`, `_packed_wavenet.py:181-189`; the Lightning callbacks `PackedMaskCallback`
  `train/lightning_module.py:568-573` and `optimizer_step` `:531-534` keep off-block weights at exactly 0). Forward returns `(B, P, L)`, one
  output channel per submodel (`_packed_wavenet.py:198-205`). The submodels share nothing at inference: each is cut out again with
  `extract_submodel(i)` into an ordinary `WaveNet` (`:93-111`).
* "Slimmable" in the trainer = this container only. The *other* trainer mechanism, `slimmable: {method: "slice_channels_uniform", kwargs.allowed_channels}`
  inside one `WaveNet` (`models/wavenet/_slimmable.py:10`, `_wavenet.py:22-119`), is mutually exclusive with packing (`_packed.py:362-364`), allowed only
  with one layer array and kernel-1 heads (`_wavenet.py:69-71, 108-111`), and is **not used by the trainer's default recipe**.
* Default recipe: `train/core.py:883-891` (`_get_packed_model_config`) and `:899-947` (`_get_configs`, `model_config = _get_packed_model_config()` at `:915`):
  **every** `nam.train.core.train` run in 0.13.0 trains the packed net; there is no switch back to A1 (`train/full.py:210-213` selects the Packed module only
  when the config says `PackedWaveNet`, so `nam-full` with a user config can still train A1).

### 1.2 How an exported `.nam` declares it

`PackedWaveNet.export_container` (`_packed_wavenet.py:129-179`) writes:

| Key | Value | Where |
|---|---|---|
| `version` | `"0.7.0"` (`MODEL_VERSION`, same as for A1 exports) | `_packed_wavenet.py:152`, `models/_constants.py:4` |
| `architecture` | **`"SlimmableContainer"`** (an A1 / standalone model says `"WaveNet"`) | `_packed_wavenet.py:154` |
| `config.submodels` | list of `{"max_value": float, "model": <a complete standalone .nam dict>}`, one per size, ascending | `:155-160` |
| `max_value` | `container_max_values` = `"uniform"` -> `(i+1)/N`, last forced to 1.0: **[0.5, 1.0]** for the 2-submodel default; or an explicit sorted list | `:207-221`, `config_model_packed.json:147-149` |
| `weights` | `[]` (all weights live in the submodels) | `:161` |
| `sample_rate` | top level, when the net has one (always set by the trainer: 48000.0) | `:164-165` |
| `metadata` | top level: `date`, `loudness`, `gain` **copied from the highest-quality submodel**, then `UserMetadata` and `other_metadata` merged in (so Sawblade's `sawblade` block can sit on the container) | `:163, 166-169, 223-234` |
| each `model` | full standalone file: `version`, own `metadata` (date/loudness/gain), `architecture: "WaveNet"`, `config`, `weights`, `sample_rate` | `models/exportable.py:187-194`, `models/base.py:108-119` |
| export hook | the `Dataset` output-scale hook is applied to the container (every submodel's `head_scale` and last weight, plus loudness) | `_packed_wavenet.py:170`, `data.py:338-388` (`SlimmableContainer` handler `:350`, docstring `:377`) |

A standalone A2 `WaveNet` entry has `config = {"layers": [one array], "head": null, "head_scale": 0.01}`; the array has `channels` 3 or 8, 23 `kernel_sizes`
(6 x 14, 15, 15, 6 x 7), 23 `dilations` (1,3,7,17,41,101,239 / 1,3,7,17,41,101,239 / 1,13 / 1,3,7,17,41,101,239), 23 `LeakyReLU` (`negative_slope` 0.01) activation objects,
`head: {out_channels 1, kernel_size 16, bias true}`, `bottleneck = channels`, `head1x1.active false`, `layer1x1.active true`, `gating_mode` all `"none"`,
`slimmable: null` (`config_model_packed.json:9-74`, verified on the generated files, `tests/fixtures/a2/`).

**Note for the A1 path:** the A1 `WaveNet` that Sawblade exports today with 0.13.0 already uses this same layer schema (`kernel_sizes`, activation objects,
`gating_mode`, `bottleneck`, `head1x1`, `layer1x1`, `slimmable: null`; `models/wavenet/_layer_array.py:964-1030`), not the 0.5.x `kernel_size`/`gated` keys. The
fixtures confirm the A1 and A2 standalone files have identical layer-key sets. The pinned core reads both (`audit_core.md` 1.1).

### 1.3 What "A2 Full" and "A2 Lite" are

* The trainer has **no names** for them. The only identifiers are the submodel names `channels_3` and `channels_8` (`config_model_packed.json:7, 77`) and the
  `max_value`s 0.5 / 1.0 derived from the order (`_packed_wavenet.py:207-221`).
* So, by what the trainer *does*: **A2 Lite = the 3-channel submodel (1 870 params, `max_value` 0.5); A2 Full = the 8-channel submodel (12 145 params,
  `max_value` 1.0)**; both 23 layers, receptive field 6347, header-identical otherwise. The container holds both in one file (22 783 params in the packed net).
  This matches the core's fast-path detector (A2 Full = 8 ch, A2 Lite = 3 ch, `audit_core.md` header table), so the three sources agree on the shapes.
  That "Full" and "Lite" are the words TONE3000 and the Anagram use for these two sizes is **not in the trainer**; it is the user's statement (spec
  Lead decision 6) plus the core.
* One trainer run therefore produces both sizes; the trainer offers **no way to train Full alone or Lite alone** other than a custom config with one submodel
  (`validate_and_build_packed_spec` accepts `len(submodels) >= 1`, `_packed.py:123-124`) or exporting `extract_submodel(i)` as a standalone `WaveNet`
  (`_packed_wavenet.py:93`).
* `container_max_values` is configurable (`_packed_wavenet.py:213-221`: list must match the submodel count, sorted, last forced to 1.0). Selecting a size at
  playback is the player's job (`audit_core.md` 1.2).

### 1.4 What A2 training changes for Sawblade (Task C input)

* Lightning module class: `PackedLightningModule` (`train/lightning_module.py:484-565`). `_shared_step` uses `pad_start=False` and splits the `(B, P, L)` prediction
  into one loss per submodel (`:536-552`); training loss = sum of every submodel's weighted losses (`:493-500`).
* **Metric keys change.** Validation logs `ESR_packed_i`, `val_loss_packed_i` per submodel, and the *aggregate* `ESR` and `val_loss` are **sums over
  submodels** (`:507-527`). `export/train.py:340-341` reads `m["ESR"]` and `m["val_loss"]` as single-model numbers; for A2 it must read the per-submodel keys
  (Full = `ESR_packed_1`), or the reported ESR is Lite + Full.
* Best checkpoint: A1 keeps one best epoch (Sawblade's `Run` callback, `export/train.py:344-345`). The packed trainer keeps one best checkpoint **per submodel**
  (`PackedBestCheckpoint`, `lightning_module.py:576-630`, files `packed_best_submodel_{i}.ckpt` + `packed_best.json`) and exports with
  `export_container(checkpoint_paths_by_submodel=...)` (`_packed_wavenet.py:129-149`; wiring in `train/core.py:1286-1330, 1482-1486`). Resume (`export/resume.py`)
  must carry those checkpoints too.
* Export: A1 uses `net.export(...)` (`export/train.py:436`); `PackedWaveNet.export` simply calls `export_container` (`_packed_wavenet.py:113-127`), so the same call
  writes the container, including `user_metadata` and `other_metadata` (`:166-169`). `Dataset.NormalizeJointDatasetOutput` works unchanged (hook supports the container).
* Cost (CPU, 4 threads, batch 16, ny 8192, loss incl. MR-STFT, this box, +-10 %): A1 standard 0.58 s/step, A1 lite (official) 0.52, **A2 packed 0.90 s/step
  (about 1.5x standard, 1.7x lite)**. The receptive field grows 4093 -> 6347, so datums need 2254 more samples of lead-in; `Dataset(nx=rf)` handles that, but the
  training-signal length check (`export/train.py:256`) should be re-derived for A2 (188 s training audio is far above both).
* The trainer's loss/optimiser recipe that `export/train.py` already borrows (`config_model_packed.json:152-165`: ESR validation, MR-STFT weight 5e-4, Adam lr 4e-3,
  weight decay 3.17e-7, ExponentialLR 0.994) is the A2 recipe. For A1 the 0.12.3 recipe was different (T12 `core.py:1052-1067`: plain ESR validation loss, Adam lr from the `train` argument, `gamma = 1 - lr_decay` with `lr_decay=0.007` at `core.py:1391` i.e. 0.993, no weight decay, MR-STFT off unless `fit_mrstft`);
  Sawblade keeps the 0.13.0 recipe for A1 (unchanged by this task; flagged as a question).

### 1.5 The official A1 presets (not in 0.13.0)

* T13: `train/core.py` has no `Architecture`, no `get_wavenet_config`, no `get_lstm_config` (grep for `feather|nano|lite|Architecture` over the whole 0.13.0 package: no hit
  outside `data.py` / `exportable.py` unrelated uses of the word "architecture"). The GUI/Colab/`nam-full` trainers in 0.13.0 always build the packed net (`train/core.py:915`).
* T12 (and 0.11.0, 0.12.0, 0.12.2: the block is byte-identical, md5 of the function text equal; at `nam/train/core.py:797`, `:814`, `:845`, `:845`): `class Architecture(Enum)`
  `core.py:59-63`; `get_wavenet_config(architecture)` `core.py:845-955`; `get_lstm_config` `core.py:605-631`. Every layer: kernel_size 3, activation Tanh, `gated` False,
  `condition_size` 1, `head_scale` 0.02; array 1 `input_size` 1, `head_bias` False; array 2 `input_size` = channels of array 1, `head_size` 1, `head_bias` True.

| Preset | T12 lines | Array 1: channels, head_size, dilations | Array 2: channels, dilations | Params | Receptive field |
|---|---|---|---|---|---|
| `standard` | 847-873 | 16, 8, `1,2,4,8,16,32,64,128,256,512` | 8, `1,2,4,8,16,32,64,128,256,512` | 13 801 | 4093 |
| `lite` | 874-900 | 12, 6, `1,2,4,8,16,32,64` | 6, `128,256,512,1,2,4,8,16,32,64,128,256,512` | 6 553 | 4093 |
| `feather` | 901-927 | 8, 4, `1,2,4,8,16,32,64` | 4, `128,256,512,1,2,4,8,16,32,64,128,256,512` | 3 025 | 4093 |
| `nano` | 928-954 | 4, 2, `1,2,4,8,16,32,64` | 2, `128,256,512,1,2,4,8,16,32,64,128,256,512` | 841 | 4093 |

Parameter counts are the sum of `numel()` of the 0.13.0 `WaveNet` built from the preset (the way the README quoted them before); `nano` is official but Sawblade does not
offer it (see questions). LSTM presets (`get_lstm_config`, 1 layer / hidden 24, 2 / 8, 1 / 16, 1 / 12) are listed for completeness; Sawblade does not train LSTMs.
Two further observations: the official layouts keep the receptive field equal at 4093 across sizes (all three Sawblade sizes did too), and the default epoch budget
of the trainer is 100 epochs (T12 `core.py:1384`, T13 `core.py:1325`), versus Sawblade's CPU budget of 22-40 epochs.

## 2. A.2 Inventory of A1 assumptions in `match/` (match-engineer scope)

Line numbers after this task's edits (`export/train.py` moved by about 40 lines). "Change" = what Task C / later work needs; nothing below is changed by this task except
where stated.

| File:line | Assumption | What changes for A2 |
|---|---|---|
| `export/train.py:1-24` (module doc), `:157-165` (`model_config`), `:237` (`lm.LightningModule`) | the net is the classic `WaveNet`, trained with `LightningModule`; recipe borrowed from the packed default config | A2: `PackedLightningModule` + `PackedMaskCallback` + `PackedBestCheckpoint`, config straight from `config_model_packed.json` (optionally one-submodel variants), `train/core.py:1286-1330` wiring |
| `export/train.py:98-109` `A1_PRESETS` / `SIZES` / `DEFAULT_EPOCHS` / `DEFAULT_MAX_MINUTES` | size keys are A1 names (`feather`/`lite`/`standard`); epochs/minute caps tuned for A1 CPU speed | add `--arch a1|a2`; A2 sizes `full`/`lite` (both produced by ONE run, section 1.3) and an A2 default budget (about 1.5x the time per epoch; see section 3) |
| `export/train.py:143-154` `wavenet_config` | A1 two-array config with the nested 0.13.0 `head` dict | A2 has one array, kernels/dilations lists, LeakyReLU (copy from `config_model_packed.json`, not hand-built) |
| `export/train.py:238, 244-246, 256` | receptive field read from the net and passed as `nx` (generic); error message mentions it | works for 6347; the minimum training-signal length check is generic. No hard-coded 4093 in code (only in README text) |
| `export/train.py:249-253` | `NormalizeJointDatasetOutput(-18 dBFS)` + `net.handshake(ds)` | works for the container (hook supports `SlimmableContainer`, `data.py:350`); verify with a smoke test in Task C |
| `export/train.py:340-345` | one `ESR` and one `val_loss` per epoch; best epoch by `val_loss` | A2: aggregate `ESR`/`val_loss` are sums over submodels (`lightning_module.py:507-527`): read `ESR_packed_i`; best epoch per submodel |
| `export/train.py:389` | log line says "A1 WaveNet" | name the architecture |
| `export/train.py:416-436` | one `best.ckpt`; `net.export(outdir, basename, user_metadata, other_metadata)` | A2: per-submodel best checkpoints -> `export_container(checkpoint_paths_by_submodel=...)`; `other_metadata` lands on the container top level (what the core reads) |
| `export/train.py:437-445`, `TrainResult.params/receptive_field` (`:200`) | single-net numbers; `config["net"]` = `wavenet_config(size)`; `namVersion` = 0.13.0 | per-submodel params, container params; `receptiveField` same for all submodels |
| `export/resume.py:20` `IDENTITY_KEYS` | identity = preset/signal sha, mode, `size` only | add the architecture (A1 vs A2): an A1 and an A2 run with the same `size` string must not resume each other. **Also**: a lite/feather checkpoint written before this task has the old layer layout and would fail to load (opaque shape error) |
| `export/run.py:178, 242, 247, 252` | `size` is the only model-shape field in identity, `sawblade` block and file stem (`<name>-<mode>-<size>.nam`) | include arch in the stem (`...-a2-full`) and the identity; the stem is also used by tests (`test_export.py:742, 872-897`) |
| `export/run.py:246-250` | `UserMetadata(name=f"... ({mode}, {size})", gear_type, tone_type=HI_GAIN)` | unchanged; for a container this goes to the top-level metadata (what the player reads) |
| `export/run.py:265-267` | report says `a2Export: ... not enabled: A1 only` | becomes the real architecture/sizes; keep the report key but fill it |
| `export/run.py:272-283` | reads the exported `.nam` back, writes `metadata.training.validation_esr` and `metadata.sawblade.validation` on the **top-level** `metadata`, rewrites the file | works unchanged for a container (top-level metadata exists, `_packed_wavenet.py:151-153`); `validation_esr` must be the Full submodel's (and Lite's separately). The submodels' own metadata is not touched (the core only reads top-level, `audit_core.md` 1.1) |
| `export/run.py:241` | `inputLevelDbu`/`outputLevelDbu` left empty; note says so | unchanged (the top-level metadata keys exist as `null`; the core reads them, `audit_core.md` 1.1) |
| `export/plan.py:251-262` (`sawblade_block`), `:247` (`gear_type`) | block records `size`; no architecture key | add `arch`/`submodels`; keep key order stable (tests pin the block) |
| `export/validate.py:83-97` (`acceptance`, `status_fields`) | acceptance (ESR <= 0.02, LTAS <= 0.5 dB) applies to `size == "standard"` only; others "not judged (non-standard size)" | decide the A2 Full / A2 Lite acceptance (spec Task C: "validation numbers per size"); today Lite/Full would read "not judged" |
| `export/validate.py` (render of the exported model) | validates through `sawblade_core` (`NamBlock`) | for a container the core plays the Full submodel by default (`audit_core.md` 1.2): validating A2 Lite needs a standalone Lite file or a size selector |
| `export/cli.py:22` | `--size` choices feather/lite/standard | `--arch a1|a2`; `--size` per arch; keep A1 default until Task C flips the default |
| `export/notes.py` (v0.4M notes) | text speaks of "the model" / NAM generic, no A1 wording | the `anagram` device profile (Task C) sits here; no A1-only claim found |
| `README.md:516-619` (NAM export) | sizes, 4093 receptive field, budgets, "A1 WaveNet" wording | updated for the sizes in this task; the budget paragraph (22-40 epochs, minutes per epoch) is A1-measured and stays A1 until Task C measures A2 |
| `t3k/filter.py:138-146`, `t3k/fetch.py:15-45` | pool prefers A2 models, falls back to A1 (`allow_a1_fallback`) | already A2-first. Open: nothing in the filter reads the file; it trusts the API's `architecture_version` / counts |
| `t3k/client.py:26`, `t3k/types.py:119-131` | `architecture` params "1" / "2" / "custom"; `Model.size` is a free string | none; but see the next row |
| `matcher/pool.py:36-50, 148-154` (`size_label`, `size_rank`) | size vocabulary `feather < lite < standard < xstandard < custom` parsed from the API `size` or the model name, default `standard` | for A2 captures the API's `size` values are unknown to me (the repo's own t3k fixtures pair `architecture_version "2"` with `standard/lite/feather`, `tests/fixtures/t3k_ladder/*.json`); an A2 "Lite" would rank like an A1 "lite" although it is a different net (3 channels, 23 layers). Verify the live `size` strings for A2 models (needs a TONE3000 token; not available offline) before relying on the ranking |
| `matcher/run.py:219-221` | records `architecture: c.arch` (the API's string) in the match result | none; arch is carried through as a string |
| `calibrate/pedal_fit.py:619` | loads cached `{mid}.nam` through the core | none (no architecture reading in Python) |
| `tests/test_export.py:325-345` (+ new `test_a1_sizes_are_nams_official_presets`) | pinned the old sizes through `SIZES` tuples | updated in this task |
| `tests/test_export.py:381` (`a["architecture"] == "WaveNet"`) | asserts the exported A1 file is a `WaveNet` | stays for A1; an A2 test asserts `SlimmableContainer` |
| `tests/test_export.py:500-508` | mock `.nam` dicts with `"architecture": "WaveNet"` | none |
| `tests/conftest.py:52, 99-111` | fake TONE3000 server: `architecture_version` per model, arch filter | none |
| Nothing found | receptive-field math, latency compensation, loudness metadata reading, `sawblade` metadata *reading* in Python | `match/` computes no receptive field itself and never reads `loudness`/`gain` from a `.nam`; the only `sawblade` metadata code is the writer above. (Core side: `audit_core.md`) |

## 3. A.3 Old -> new sizes

Source of the new layouts: T12 `nam/train/core.py:845-955` (section 1.5). Implementation: `match/sawblade_match/export/train.py` (`A1_PRESETS`, `wavenet_config`);
tests: `match/tests/test_export.py::test_a1_sizes_are_nams_official_presets` (new) and `::test_train_config_defaults_and_architectures` (adapted to the new table).

| Size | Old (Sawblade, "recalled from memory") | New (official) | Params | Receptive field | Train step (CPU, 4 thr, +-10 %) |
|---|---|---|---|---|---|
| `standard` | 16/8 ch, 10 + 10 dilations (1..512) | **identical** | 13 801 -> 13 801 | 4093 -> 4093 | 0.58 s -> 0.58 s |
| `lite` | 12/6 ch, 10 + 10 dilations | 12/6 ch, **7 (1..64) + 13 (128..512, 1..512)** dilations | **7 903 -> 6 553 (-17 %)** | 4093 -> 4093 | 0.59 -> 0.52 s (about -10 %) |
| `feather` | 8/4 ch, 10 + 10 dilations | 8/4 ch, **7 + 13** dilations | **3 637 -> 3 025 (-17 %)** | 4093 -> 4093 | 0.59 -> 0.38-0.42 s (about -30 %; fewer wide layers) |

Effects: `lite` and `feather` get about 17 % fewer parameters and train 10-30 % faster per step, so the existing `DEFAULT_EPOCHS` / `DEFAULT_MAX_MINUTES` (lite 30 epochs / 30 min,
feather 40 / 15) are a bit conservative now and were left alone. ESR expectation: the official layout puts the capacity in the 128..512 dilation stack of the narrow array 2
(13 layers there vs 10 before), which is where long-memory content (the low end, cab-less ringing) lives; no ESR measurement was made (no GPU/long run here), and the README's earlier
under-trained numbers (lite ESR 0.36-0.45 at 14 epochs) were for the old layout, so they are stale for lite/feather. `standard` (the only size judged by the acceptance rule) is unchanged, so
its numbers and acceptance are unaffected. Old half-finished lite/feather resume checkpoints no longer load (shape mismatch); see the `resume.py` row in section 2. Reports written
by earlier runs keep their own `config.net`.

## 4. Fixtures (Lead decision 3): `tests/fixtures/a2/`

Generator: `tests/fixtures/a2/generate.py` (committed; run with the `match[export]` venv; `--check` regenerates to a temp dir and compares byte for byte: passes). Seeds, versions and hashes are
in `manifest.json`; `README.md` next to them says how to use them.

| File | What | Receptive field | Latency | Params | Bytes |
|---|---|---|---|---|---|
| `a2_container.nam` | `SlimmableContainer`, submodels Lite (`max_value` 0.5) + Full (1.0), from `PackedWaveNet.export_container`; carries a `metadata.sawblade` fixture block | 6347 | 0 | 22 783 | 308 787 |
| `a2_full.nam` | standalone `WaveNet`, 8 ch, 23 layers (= the container's Full submodel, asserted) | 6347 | 0 | 12 145 | 265 284 |
| `a2_lite.nam` | standalone `WaveNet`, 3 ch (= the container's Lite submodel, asserted) | 6347 | 0 | 1 870 | 42 846 |
| `a1_standard.nam` | `WaveNet` A1 standard, official preset through `sawblade_match.export.train.wavenet_config` | 4093 | 0 | 13 801 | 300 634 |
| `input.wav` | 1 s, 48 kHz, mono, float32: 0.4 s seeded noise + 0.6 s log sweep 50 Hz-16 kHz | | | | 192 080 |
| `ref_a2_full.wav`, `ref_a2_lite.wav`, `ref_a1_standard.wav` | the trainer's forward pass of the exported files (read back with `nam.models.init_from_nam`, float32, zero start-up history) | | | | 3 x 192 080 |
| `manifest.json`, `README.md`, `generate.py` | index, usage, generator | | | | 4 330 + 4 800 + 13 000 |

Total about **1.7 MB**. Weights are the trainer's seeded random initialisation (seeds 20261007 input, 20261008 A2, 20261009 A1), untrained; reference RMS -26.8 dBFS (Full), -31.3 (Lite),
-37.3 (A1), and the nets are strongly non-linear (halving the input changes the output shape by 36 %), so the null test is sensitive.

How a "Lite" file is produced: the trainer exports Full and Lite together as one container; there is no separate Lite training. The standalone `a2_lite.nam` / `a2_full.nam` are
`extract_submodel(i).export(...)` of the same packed net, with the same weights as the container submodels; a player that never selects a size plays the container's last (Full) submodel,
so `ref_a2_full.wav` is also the container's reference.

Expected latency of each fixture: **0 samples** (NAM models are causal; the trainer aligns capture latency out at training time; `audit_core.md` 1.5). Receptive field / warm-up: 6347 samples (132 ms) for A2,
4093 (85 ms) for A1. The generator pins the metadata `date` (the trainer would stamp "now") so regeneration is byte-identical; that is the only edit after the trainer's export.

## 4b. Test results

Run in this container (Python 3.11 venv with `match[dev,export]` + constraints, `sawblade_core` built from this branch): **2 failed / 713 passed / 7 skipped**.

* The two failures are `test_matcher_v04m.py::test_a_strongly_post_eqd_reference_fires_eqd_and_the_wider_post_eq_is_kept` and `test_matcher.py::test_stage2_first_linear_block_is_ltas_only`.
  They also fail on the merge base `6701294`. They are **not container-specific** (an earlier version of this section said so, wrongly): CI run 238 failed them too. The v0.4M
  merge in `6701294` was the stale head `bbae365`; v0.4M's `582ca3d` (in `bafcada`) fixes both, and the v0.6 branch now includes it (merge of `bafcada`).
* `tests/fixtures/a2/generate.py --check` regenerates the fixtures byte-identically.
* `test_a1_sizes_are_nams_official_presets` failed in a `[dev]`-only install (it imports `nam`); fixed in the review round by splitting it into a pure layout test and a `_need_nam` (skip without the trainer) parameter-count test, plus the `python-export` CI job (decision 17).

## 5. Decisions / questions for lead

1. **Where the "official presets" are cited from.** The pinned 0.13.0 does not contain them; I cite 0.12.3 (the last release with them, identical since 0.11.0) and verified parameter counts and receptive field
   against the pinned 0.13.0 `WaveNet`. Fine, or do you want `A1_PRESETS` frozen differently (e.g. a vendored copy under `third_party/` with MIT notice)? Nothing is vendored: the numbers are plain layer sizes.
2. **`nano`.** Official A1 preset (4/2 channels, 841 params), not offered. Add it to `--size` (small, one table row and an epoch budget) or leave it out? I left it out (scope).
3. **A1 training recipe.** Sawblade trains A1 with the 0.13.0 *packed A2 recipe* (ESR + MR-STFT 5e-4, Adam 4e-3, weight decay, ExponentialLR) rather than the 0.12.3 A1 recipe (plain ESR, no weight decay or MR-STFT).
   Unchanged here; flag only. Worth an A/B in Task C's validation numbers?
4. **A2 size names in Sawblade.** The trainer only has `channels_3` / `channels_8`. Proposal for Task C: `--arch a2 --size full|lite|both` where one run always trains the container (both sizes), `full`/`lite` choose
   which file(s) are written (container, plus standalone Full or Lite for loaders that do not accept a container). Is the Anagram known to accept a container file? (I could not check; spec decision 6 allows only the published block list.)
5. **TONE3000 `size` strings for A2 models** (`matcher/pool.py` size ranking): not verifiable offline. Needs one live `sawblade-t3k models <tone>` on an A2 tone with a token.
6. **Old resume checkpoints** of lite/feather runs no longer load. Add `arch`/layout to `IDENTITY_KEYS` now (refuses cleanly) or in Task C together with the architecture key? Proposed: Task C.
7. **Environment.** The Python suite needs `sawblade_core`: I built it in a scratch dir (`cmake -DSAWBLADE_BUILD_PYTHON=ON`) and pointed `SAWBLADE_CORE_DIR` at it; nothing in the repo changed for that.
