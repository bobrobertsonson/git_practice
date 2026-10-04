#!/usr/bin/env bash
# Speed / memory runs on the 70 s looped mixture (timing only). Raw RESULT lines go to $DATA/timing.log.
# Run on an otherwise idle machine. Stems are written to a scratch dir and discarded.
# usage: run_timing.sh [portable-binary]   (portable = build with -DSAWBLADE_SEPARATOR_NATIVE=OFF)
set -uo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export TORCH_HOME="$CACHE/torchhome"
IN="$DATA/loop70/mixture.wav"; TMP="$DATA/out/timing_scratch"; LOG="$DATA/timing.log"
NATIVE="$DATA/build-spike/spikes/separator/separator_spike"
PORT="${1:-$DATA/build-portable/spikes/separator/separator_spike}"
run_cpp() { # label binary model-size mode threads reps
  local label=$1 bin=$2 m=$3 mode=$4 thr=$5 reps=$6
  for r in $(seq 1 "$reps"); do
    echo "== $label m=$m mode=$mode threads=$thr rep=$r" | tee -a "$LOG"
    "$bin" --model "$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin" --in "$IN" --out-dir "$TMP" --threads "$thr" --mode "$mode" 2>&1 | grep RESULT | tee -a "$LOG"
  done
}
for m in 4s 6s; do
  model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
  for r in 1 2 3; do
    echo "== python m=$m threads=4(default) rep=$r" | tee -a "$LOG"
    "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$IN" --out-dir "$TMP/py" | grep RESULT | tee -a "$LOG"
  done
  for r in 1 2; do
    echo "== python m=$m threads=1 rep=$r" | tee -a "$LOG"
    "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$IN" --out-dir "$TMP/py" --threads 1 | grep RESULT | tee -a "$LOG"
  done
  run_cpp native-single "$NATIVE" $m single 4 2
  run_cpp native-split  "$NATIVE" $m split 4 3
done
for m in 4s 6s; do
  run_cpp native-single "$NATIVE" $m single 1 2
done
if [ -x "$PORT" ]; then
  for m in 4s 6s; do
    run_cpp portable-single "$PORT" $m single 1 2
    run_cpp portable-split  "$PORT" $m split 4 3
  done
fi
echo ALLDONE | tee -a "$LOG"
