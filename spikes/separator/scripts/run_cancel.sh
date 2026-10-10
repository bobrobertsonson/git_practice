#!/usr/bin/env bash
# Phase 5.1a acceptance 6 + 7. Cancel latency (flag set -> engine returns) on the 70 s loop, and the
# library-silence check (driver run with --quiet: stdout and stderr must both be empty).
set -uo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
BIN_EIGEN="${BIN_EIGEN:-$DATA/build-a-eigen/spikes/separator/separator_spike}"
BIN_BLAS="${BIN_BLAS:-$DATA/build-a-blas/spikes/separator/separator_spike}"
BIN_ONNX="${BIN_ONNX:-$DATA/build-a-eigen/spikes/separator/separator_onnx}"
IN="$DATA/loop70/mixture.wav"; LOG="$DATA/cancel_51a.log"
c() { echo "== $*" | tee -a "$LOG"; nice "$@" 2>&1 | grep -E "CANCEL|RESULT" | tee -a "$LOG"; }
for m in 4s 6s; do
  model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
  ggml="$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin"; onnx="$CACHE/onnx/$model-core-opset17.onnx"
  for t in 1 4; do
    c "$BIN_EIGEN" --model "$ggml" --in "$IN" --out-dir /tmp/cancel_out --threads $t --cancel-after 5
    c "$BIN_BLAS"  --model "$ggml" --in "$IN" --out-dir /tmp/cancel_out --threads $t --cancel-after 5
    c "$BIN_ONNX"  --model "$onnx" --in "$IN" --out-dir /tmp/cancel_out --threads $t --cancel-after 5
  done
done
echo "== library silence (--quiet): bytes on stdout / stderr" | tee -a "$LOG"
for b in "$BIN_EIGEN" "$BIN_BLAS"; do
  nice "$b" --model "$CACHE/ggml/ggml-model-htdemucs-6s-f16.bin" --in "$DATA/real7/mixture.wav" --out-dir /tmp/quiet_out --threads 4 --quiet >/tmp/q.out 2>/tmp/q.err
  echo "$(basename "$(dirname "$(dirname "$(dirname "$b")")")") exit=$? stdout=$(wc -c </tmp/q.out) stderr=$(wc -c </tmp/q.err)" | tee -a "$LOG"
done
echo CANCELDONE | tee -a "$LOG"
