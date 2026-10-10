#!/usr/bin/env bash
# Phase 5.1a: produce every stem set the evaluation needs, outside the repo ($SAWBLADE_SEP_DATA/out):
#   eigen_<m>_<d>, blas_<m>_<d>, onnx_<m>_<d>   (THREADS threads, default 4)
#   py_<m>_<d>_shift0                           (stock demucs 4.0.1 apply_model, shifts=0, split, overlap 0.25)
# for m in 4s 6s, d in real7 synth. Binaries: $BIN_EIGEN / $BIN_BLAS (separator_spike), $BIN_ONNX (separator_onnx).
set -euo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
BIN_EIGEN="${BIN_EIGEN:-$DATA/build-a-eigen/spikes/separator/separator_spike}"
BIN_BLAS="${BIN_BLAS:-$DATA/build-a-blas/spikes/separator/separator_spike}"
BIN_ONNX="${BIN_ONNX:-$DATA/build-a-eigen/spikes/separator/separator_onnx}"
HERE="$(cd "$(dirname "$0")" && pwd)"
THREADS="${THREADS:-4}"
export TORCH_HOME="$CACHE/torchhome"
for d in real7 synth; do for m in 4s 6s; do
  model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
  in="$DATA/$d/mixture.wav"
  ggml="$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin"
  onnx="$CACHE/onnx/$model-core-opset17.onnx"
  echo "== $d $m"
  nice "$BIN_EIGEN" --model "$ggml" --in "$in" --out-dir "$DATA/out/eigen_${m}_$d" --threads "$THREADS" | grep RESULT
  nice "$BIN_BLAS"  --model "$ggml" --in "$in" --out-dir "$DATA/out/blas_${m}_$d"  --threads "$THREADS" | grep RESULT
  nice "$BIN_ONNX"  --model "$onnx" --in "$in" --out-dir "$DATA/out/onnx_${m}_$d"  --threads "$THREADS" | grep RESULT
  [ -f "$DATA/out/py_${m}_${d}_shift0/drums.wav" ] || \
    nice "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$in" --out-dir "$DATA/out/py_${m}_${d}_shift0" | grep RESULT
done; done
echo MATRIXDONE
