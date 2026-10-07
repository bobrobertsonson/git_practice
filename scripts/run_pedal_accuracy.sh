#!/usr/bin/env bash
# v0.4A pedal accuracy run on the user's Mac (docs/runbooks/v0_4a_pedal_accuracy.md, condensed).
# Fits the modeled pedals against TONE3000 captures and writes docs/reports/v0_4/accuracy.md.
# Long (about 3 hours). Safe to re-run: finished captures are skipped (--merge).
#
#   scripts/run_pedal_accuracy.sh --di PATH/TO/DI.wav [--pedals "hm hmx eye ts"] [--no-pull]
#
# Never commits anything. Captures stay in ~/.cache/sawblade/captures; renders in ~/sawblade-work/pedalfit.
# The pull writes its own manifest (~/sawblade-work/pedal_pool.json) so the matcher's pool_manifest.json is untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DI=""
PEDALS="hm hmx eye ts"
PULL=1
MANIFEST="$HOME/sawblade-work/pedal_pool.json"   # the pull manifest: names, licences, creators of the pedal captures
while [[ $# -gt 0 ]]; do
  case "$1" in
    --di) DI="$2"; shift 2 ;;
    --pedals) PEDALS="$2"; shift 2 ;;
    --no-pull) PULL=0; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$DI" && -f "$DI" ]] || { echo "need --di PATH to a mono guitar DI wav (30 s or longer)" >&2; exit 2; }

VENV="$ROOT/match/.venv/bin"
export SAWBLADE_TONERENDER="$ROOT/build-mac/cli/tonerender"
[[ -x "$SAWBLADE_TONERENDER" ]] || { echo "tonerender missing: run scripts/mac_update.sh first" >&2; exit 2; }
W="$HOME/sawblade-work/pedalfit"
O="$ROOT/docs/reports/v0_4"
mkdir -p "$W"
LOG="$HOME/sawblade-work/pedal_accuracy.log"
echo "log: $LOG"

{
  echo "== $(date) start; DI=$DI; pedals=$PEDALS"
  "$VENV/pip" install -q -e 'match[dev]' || echo "WARN: pip install of match[dev] failed; continuing"
  echo "== known-answer self-check"
  "$VENV/sawblade-calibrate" pedal-fit --known-answers --work "$W" --out "$O" --refine-generations 30
  if [[ $PULL -eq 1 ]]; then
    echo "== pulling pedal captures (needs your TONE3000 login)"
    "$VENV/sawblade-t3k" pull --no-trending --no-latest --gear pedal --max-models-per-tone 12 \
      --manifest "$MANIFEST" \
      --force-tone 58569 --force-tone 74487 --force-tone 78122 --force-tone 88604 --force-tone 6778 \
      --force-tone 72990 --force-tone 60618 --force-tone 62523 \
      --force-tone 30104 --force-tone 92212 --force-tone 70280
  fi
  FITS=()
  for p in $PEDALS; do
    echo "== $(date) fitting $p"
    rc=0
    "$VENV/sawblade-calibrate" pedal-fit --pedal "$p" --di "$DI" --work "$W" --out "$O" --jobs 4 --merge --manifest "$MANIFEST" \
      || rc=$?
    if [[ $rc -eq 4 ]]; then
      echo "WARN: $p fit finished with ERRORS (see the report's Errors section)"
    elif [[ $rc -ne 0 ]]; then
      echo "WARN: $p fit failed (see above); continuing"
    fi
    [[ -f "$O/fits_$p.json" ]] && FITS+=(--fits "$O/fits_$p.json")
  done
  echo "== report"
  "$VENV/sawblade-calibrate" pedal-accuracy --manifest "$MANIFEST" --out "$O/accuracy.md" --known-answers "$O/known_answers.json" "${FITS[@]}"
  echo "== $(date) done. Report: $O/accuracy.md"
} 2>&1 | tee -a "$LOG"
