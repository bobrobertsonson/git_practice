# v0.4a: fit the five pedals against TONE3000 captures (user's Mac)

Spec: `docs/specs/v0_4a-pedal_accuracy.md`. One run, about 3-4 hours on 4 cores for the five pedals (hm alone is about
90 minutes: 20 captures x ~400 renders). Needs what the cloud container lacks: your TONE3000 OAuth login, the guitar
DI, scipy/soundfile. Nothing here is committed except the JSON, MD and PNG named in step 7. **Never commit captures
(`~/.cache/sawblade/captures`), the DI, tokens or the `--work` directory.**

## 0. Setup (skip what is done)

```
cd ~/sawblade
git fetch origin && git checkout claude/sawblade-v0_4a-pedal-accuracy && git pull --ff-only
python3 -m venv match/.venv && match/.venv/bin/pip install -e 'match[dev]'
source match/.venv/bin/activate
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=OFF -DSAWBLADE_BUILD_TESTS=OFF
cmake --build build --target tonerender
export SAWBLADE_TONERENDER=$PWD/build/cli/tonerender
export TONE3000_CLIENT_ID=t3k_pub_xxxxxxxx        # the publishable key only, never t3k_cs_...
mkdir -p ~/sawblade-work/pedalfit                 # scratch renders, outside the repo
```

The DI must be at `testdata/gatecreeper_cover/Guitar_L.wav` (user-supplied, see `docs/TEST_MATERIAL.md`; untracked).

## 1. Check the harness on known answers (about 5 minutes)

```
pytest match/tests/test_pedal_fit.py -q
sawblade-calibrate pedal-fit --known-answers --work ~/sawblade-work/pedalfit --out docs/reports/v0_4 --refine-generations 30
```

All tests must pass before any capture number is trusted. The second command rewrites
`docs/reports/v0_4/known_answers.json` (pedal rendered at known params, fitted back).

## 2. Login and resolve the unknown ids

```
sawblade-t3k login                                 # open the URL, enter the code
sawblade-t3k whoami
sawblade-t3k search "peterny HM-2" --gear pedal    # HM-2 MiJ v2.0 (cc-by-nc): note its tone id
sawblade-t3k search "big muff" --gear pedal        # then: "russian big muff", "ram's head", "muff fuzz"
sawblade-t3k search "TS808" --gear pedal           # confirm 30104, 92212; find the 70280 pack's TS-style models
```

Edit `docs/reports/v0_4/targets.json`: add the muff tone ids to `pedals.muff.tones` (`{"tone_id": N, "unit": "...",
"group": "...", "models": []}`), the peterny id to `pedals.hm.tones`, and any labelled knob settings you can read from
the model names to `assumed` (exact model name -> knob values) or `label_regex`. Licences: `cc-by-nc*` captures are
fine for this personal project; their fits are marked non-commercial automatically.

## 3. Pull the captures (one pull: it rewrites `pool_manifest.json` each time)

```
sawblade-t3k pull --no-trending --no-latest --gear pedal --max-models-per-tone 12 \
  --force-tone 58569 --force-tone 74487 --force-tone 78122 --force-tone 88604 --force-tone 6778 \
  --force-tone 72990 --force-tone 60618 --force-tone 62523 \
  --force-tone 30104 --force-tone 92212 --force-tone 70280 \
  --force-tone <peterny id> --force-tone <each muff id>
```

Captures land in `~/.cache/sawblade/captures/<tone_id>/<model_id>.nam`. The 70280 boost pack has non-TS models:
list only the TS-style model ids in `pedals.ts.tones[70280].models` of `targets.json` (an empty list fits every cached model of the tone).

## 4. Fit each pedal

Each command writes `docs/reports/v0_4/fits_<pedal>.json` (licence + creator per capture, free and knob-constrained
fits, `harm_rms_db` with even/odd parts, `harm_rms_db_legacy`, the lag, the capture spread) and per-capture PNGs.
It is crash-safe: a re-run with `--merge` skips captures already in the file.

```
W=~/sawblade-work/pedalfit; O=docs/reports/v0_4
sawblade-calibrate pedal-fit --pedal hm   --work $W --out $O --jobs 4      # modelVersion 3, the current voicing
sawblade-calibrate pedal-fit --pedal hmx  --work $W --out $O --jobs 4
sawblade-calibrate pedal-fit --pedal eye  --work $W --out $O --jobs 4
sawblade-calibrate pedal-fit --pedal ts   --work $W --out $O --jobs 4
sawblade-calibrate pedal-fit --pedal muff --work $W --out $O --jobs 4
```

Optional, to compare with phase 7.1 (hm v1 with the old -70 dB floor and the old search):

```
sawblade-calibrate pedal-fit --pedal hm --model-version 1 --harm-floor -70 --refine-generations 0 \
  --work $W --out docs/reports/v0_4/hm_v1_replay --fits-name fits_hm_v1.json
```

`--merge` refuses a phase 7.1 `fits.json` (schema 1): fit into a new file instead.

## 5. Generate the report

```
sawblade-calibrate pedal-accuracy --out docs/reports/v0_4/accuracy.md \
  --known-answers docs/reports/v0_4/known_answers.json \
  --fits $O/fits_hm.json --fits $O/fits_hmx.json --fits $O/fits_eye.json --fits $O/fits_ts.json --fits $O/fits_muff.json
```

Pedals without a fits file stay `PENDING USER RUN` in the table; the status line at the top says so.

## 6. Check before committing

* `git status`: only `docs/reports/v0_4/` (JSON, MD, PNG) and `targets.json` changed; no `.nam`, `.wav`, token or
  `testdata/` file is staged (`git diff --cached --stat`).
* Skim `accuracy.md`: the known-answer rows are ~0, each capture row has licence + creator, `assumed` pins are not
  counted for the v0.4 target.

## 7. Commit and push to the report branch

```
git add docs/reports/v0_4
git commit -m "v0.4a: pedal accuracy fits and report (user run)"
git push origin claude/sawblade-v0_4a-pedal-accuracy
```
