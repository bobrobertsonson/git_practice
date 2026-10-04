#!/usr/bin/env bash
# Phase 5.1a acceptance 4: same engine + same threads twice (bit-identical?) and 1 vs 4 threads (residual),
# on real7, both models. Stems go to $DATA/out/det/<engine>_<m>_<tag>; compare with
#   eval.py --det <A>:<B> ...   (paths relative to $DATA/out)
set -euo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
BIN_EIGEN="${BIN_EIGEN:-$DATA/build-a-eigen/spikes/separator/separator_spike}"
BIN_BLAS="${BIN_BLAS:-$DATA/build-a-blas/spikes/separator/separator_spike}"
BIN_ONNX="${BIN_ONNX:-$DATA/build-a-eigen/spikes/separator/separator_onnx}"
in="$DATA/real7/mixture.wav"
for m in 4s 6s; do
  model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
  ggml="$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin"; onnx="$CACHE/onnx/$model-core-opset17.onnx"
  for t in 4 1; do for rep in a b; do
    [ $t = 1 ] && [ $rep = b ] && continue
    nice "$BIN_EIGEN" --model "$ggml" --in "$in" --out-dir "$DATA/out/det/eigen_${m}_t${t}${rep}" --threads $t --quiet
    nice "$BIN_BLAS"  --model "$ggml" --in "$in" --out-dir "$DATA/out/det/blas_${m}_t${t}${rep}"  --threads $t --quiet
    nice "$BIN_ONNX"  --model "$onnx" --in "$in" --out-dir "$DATA/out/det/onnx_${m}_t${t}${rep}"  --threads $t > /dev/null
  done; done
done
echo DETDONE
