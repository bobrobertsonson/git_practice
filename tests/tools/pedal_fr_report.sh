#!/bin/sh
# Writes the CSVs for the phase 7b report plots with the pedal_fr developer tool.
#   tests/tools/pedal_fr_report.sh [PEDAL_FR_BINARY] [OUT_DIR]
# Defaults: build/tests/pedal_fr and ./pedal_fr_csv. Run from the repository root.
set -eu
FR=${1:-build/tests/pedal_fr}
OUT=${2:-pedal_fr_csv}
mkdir -p "$OUT"

# 1. FR of the first circuit block of every bank preset.
for f in presets/modeled/chainsaw/*.json; do
  "$FR" --preset "$f" --out "$OUT/preset_$(basename "$f" .json).csv"
done
# 2. THD vs input level for the four clip types on each circuit (drive knob 5, others default).
for c in silicon led asymmetric soft; do
  "$FR" --type pedal.hm   --param distortion=5 --param clip=$c --thd --out "$OUT/clip_hm_${c}_thd.csv"
  "$FR" --type pedal.muff --param sustain=5    --param clip=$c --thd --out "$OUT/clip_muff_${c}_thd.csv"
done
# 3. The three HM modes at low = high = dist = 10.
for m in stock custom modded; do
  "$FR" --type pedal.hm --param low=10 --param high=10 --param distortion=10 --param mode=$m --out "$OUT/mode_$m.csv"
done
# 4. The big-fuzz tone control and scoop (tone 5 for the scoop pair).
for t in 0 5 10; do "$FR" --type pedal.muff --param tone=$t --out "$OUT/muff_tone$t.csv"; done
for s in 0 10; do "$FR" --type pedal.muff --param tone=5 --param scoop=$s --out "$OUT/muff_scoop$s.csv"; done
echo "wrote $(ls "$OUT" | wc -l) CSVs to $OUT"
