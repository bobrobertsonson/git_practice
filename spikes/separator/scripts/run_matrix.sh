#!/usr/bin/env bash
# Produce every stem set the evaluation needs (C++ with THREADS threads, default 4; Python shifts=0 and shift=4033), outside the repo.
set -euo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
BIN="${SEPARATOR_SPIKE_BIN:-$DATA/build-spike/spikes/separator/separator_spike}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export TORCH_HOME="$CACHE/torchhome"
for d in real7 synth; do for m in 4s 6s; do
  model=htdemucs; [ $m = 6s ] && model=htdemucs_6s
  "$BIN" --model "$CACHE/ggml/ggml-model-htdemucs-$m-f16.bin" --in "$DATA/$d/mixture.wav" --out-dir "$DATA/out/cpp_${m}_$d" --threads "${THREADS:-4}" | grep RESULT
  "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$DATA/$d/mixture.wav" --out-dir "$DATA/out/py_${m}_${d}_shift0"
  "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$DATA/$d/mixture.wav" --out-dir "$DATA/out/py_${m}_${d}_shift4033" --shift-offset 4033
  "$VENV/bin/python" "$HERE/run_python.py" --model $model --in "$DATA/$d/mixture.wav" --out-dir "$DATA/out/py_${m}_${d}_cppmatch" --shift-offset 4033 --zero-pad-chunks
done; done
