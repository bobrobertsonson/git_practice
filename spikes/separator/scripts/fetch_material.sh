#!/usr/bin/env bash
# Build the test material OUTSIDE the repo ($SAWBLADE_SEP_DATA, default ~/sawblade-sep-data).
#  real7/  : MUSDB18-7 sample clip "Music Delta - 80s Rock" (6.8 s, ground-truth drums/bass/other/vocals),
#            taken from the sigsep-mus-eval git repo (zenodo is blocked on the dev box).
#  loop70/ : the same clip looped to 70 s (timing only; stems looped too).
#  synth/  : 36 s synthetic mixture (distorted guitar, pad, bass, drums, vocal-like) with exact stems.
set -euo pipefail
DATA="${SAWBLADE_SEP_DATA:-$HOME/sawblade-sep-data}"
VENV="${SAWBLADE_SEP_VENV:-$HOME/.venvs/sawblade-demucs}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PIN=2716132b2f4125d4174b99d01fb5ed2e9de17adb
mkdir -p "$DATA/probe"
if [ ! -d "$DATA/probe/sigsep-mus-eval/.git" ]; then
  git clone -q https://github.com/sigsep/sigsep-mus-eval "$DATA/probe/sigsep-mus-eval"
fi
git -C "$DATA/probe/sigsep-mus-eval" checkout -q "$PIN"
"$VENV/bin/python" "$HERE/make_material.py" "$DATA" \
  "$DATA/probe/sigsep-mus-eval/tests/data/MUSDB18-7-SAMPLE/train/Music Delta - 80s Rock.stem.mp4"
