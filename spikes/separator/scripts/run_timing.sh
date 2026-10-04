#!/usr/bin/env bash
# Phase 5.1a speed / memory table: 70 s loop, load time separate, raw RESULT lines in $DATA/timing_51a.log.
# Run on an otherwise idle machine. Stems go to a scratch dir and are discarded.
# Engines: onnx (ORT CPU EP), python (stock torch, shifts=0), blas (OpenBLAS), eigen (Eigen GEMM).
# usage: run_timing.sh [engine ...]   (default: onnx python blas eigen)
set -uo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
BIN_EIGEN="${BIN_EIGEN:-$DATA/build-a-eigen/spikes/separator/separator_spike}"
BIN_BLAS="${BIN_BLAS:-$DATA/build-a-blas/spikes/separator/separator_spike}"
BIN_ONNX="${BIN_ONNX:-$DATA/build-a-eigen/spikes/separator/separator_onnx}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export TORCH_HOME="$CACHE/torchhome"
IN="$DATA/loop70/mixture.wav"; TMP="$DATA/out/timing_scratch"; LOG="$DATA/timing_51a.log"
ENGINES=("$@"); [ ${#ENGINES[@]} -eq 0 ] && ENGINES=(onnx python blas eigen)
run() { # label reps cmd...
  local label=$1 reps=$2; shift 2
  for r in $(seq 1 "$reps"); do
    echo "== $label rep=$r" | tee -a "$LOG"
    nice "$@" 2>&1 | grep RESULT | tee -a "$LOG"
  done
}
for e in "${ENGINES[@]}"; do
  for m in 4s 6s; do
    model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
    ggml="$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin"; onnx="$CACHE/onnx/$model-core-opset17.onnx"
    for t in 1 4; do
      case $e in
        onnx)   run "onnx m=$m threads=$t" 3 "$BIN_ONNX" --model "$onnx" --in "$IN" --out-dir "$TMP" --threads $t ;;
        python) run "python m=$m threads=$t" 3 "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$IN" --out-dir "$TMP/py" --threads $t ;;
        blas)   run "blas-v3 m=$m threads=$t" 2 "$BIN_BLAS" --model "$ggml" --in "$IN" --out-dir "$TMP" --threads $t ;;
        eigen)  run "eigen-v3 m=$m threads=$t" 2 "$BIN_EIGEN" --model "$ggml" --in "$IN" --out-dir "$TMP" --threads $t ;;
      esac
    done
  done
done
echo ALLDONE | tee -a "$LOG"
