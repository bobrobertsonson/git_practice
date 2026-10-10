#!/usr/bin/env bash
# Download the official htdemucs / htdemucs_6s PyTorch checkpoints, verify sha256, and convert them
# to demucs.cpp's ggml format with demucs.cpp's own convert-pth-to-ggml.py.
# Output goes OUTSIDE the repo: $SAWBLADE_SEP_CACHE (default ~/.cache/sawblade/separator).
# Needs: the venv from setup_venv.sh ($SAWBLADE_SEP_VENV, default ~/.venvs/sawblade-demucs) and
# the demucs.cpp checkout at the pinned commit ($SAWBLADE_SEP_CACHE/src, or pass DEMUCS_CPP_SRC).
set -euo pipefail
CACHE="${SAWBLADE_SEP_CACHE:-$HOME/.cache/sawblade/separator}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
PIN=f1206e9adeea103aef4a636b9e62297cf1f8e34e
SRC="${DEMUCS_CPP_SRC:-$CACHE/src}"
BASE=https://dl.fbaipublicfiles.com/demucs/hybrid_transformer
# name  sha256
CKPTS=(
  "955717e8-8726e21a.th 8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"
  "5c90dfd2-34c22ccb.th 34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd"
)
mkdir -p "$CACHE"
if [ ! -d "$SRC/.git" ]; then
  git clone https://github.com/sevagh/demucs.cpp "$SRC"
fi
git -C "$SRC" checkout -q "$PIN"   # no submodules needed for the converter

# torch.hub layout so demucs.pretrained.get_model() finds the files without downloading again
HUB="$CACHE/torchhome/hub/checkpoints"
mkdir -p "$HUB"
for e in "${CKPTS[@]}"; do
  set -- $e
  f="$HUB/$1"
  [ -f "$f" ] || curl -fSL --cacert "${SSL_CERT_FILE:-/root/.ccr/ca-bundle.crt}" -o "$f" "$BASE/$1"
  echo "$2  $f" | sha256sum -c -
done

export TORCH_HOME="$CACHE/torchhome"
mkdir -p "$CACHE/ggml"
"$VENV/bin/python" "$SRC/scripts/convert-pth-to-ggml.py" "$CACHE/ggml"               > "$CACHE/ggml/convert-4s.log" 2>&1
"$VENV/bin/python" "$SRC/scripts/convert-pth-to-ggml.py" "$CACHE/ggml" --six-source > "$CACHE/ggml/convert-6s.log" 2>&1
ls -l "$CACHE/ggml"
sha256sum "$CACHE"/ggml/*.bin
