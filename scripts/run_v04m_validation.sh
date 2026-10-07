#!/usr/bin/env bash
# One command for the v0.4M validation on the Mac (docs/specs/v0_4m-matcher_feel_REPORT.md, "Mac validation commands").
# Run it AFTER `scripts/mac_update.sh --no-models`. Your shell is zsh, so invoke it with bash:
#
#   bash scripts/run_v04m_validation.sh [--quick-only] [--force] [--dry-run]
#        [--bb DIR] [--irs DIR] [--pool FILE] [--out DIR] [--r-search-dir DIR]
#        [--l-di F] [--l-hm2 F] [--l-ubr F] [--r-di F] [--r-hm2 F] [--r-ubr F]
#
# --quick-only  IR scan, quick L HM2, quick L UBR, summary, DI-tilt lines, open commands (a few minutes)
# --force       redo runs whose result.json already exists (default: skip them, so an interrupted run resumes)
# --dry-run     print the commands instead of running them (safe on any machine; inputs are still checked)
# --bb DIR      Bloodbath NTM folder (default $HOME/sawblade/testdata/ntm/bloodbath)
# --irs DIR     your IR folder (default /Users/notsch/Music/Studio_Notsch/_IRs/Guitar_Cabs)
# --pool FILE   TONE3000 pool manifest (default $HOME/.cache/sawblade/captures/pool_manifest.json)
# --r-search-dir DIR  where to look for the R files when they are not in --bb (recursive, exact file name; default
#               /Users/notsch/Desktop/NailTheMix/NailtheMix_March2023_Bloodbath_44k24b). Only the L files are required:
#               if an R file is found in neither place the held-out R runs are skipped with a message
# --out DIR     results folder (default $HOME/.cache/sawblade/match_runs/v04m)
# --l-di/--l-hm2/--l-ubr/--r-di/--r-hm2/--r-ubr  file names (relative to --bb, or absolute); defaults are
#               "17 GTR RHY L DI.wav", "18 GTR RHY L HM2 AMP.wav", "19 GTR RHY L UBR AMP.wav",
#               "20 GTR RHY R DI.wav", "21 GTR RHY R HM2 AMP.wav", "22 GTR RHY R UBR AMP.wav"
# Everything printed is also appended to ~/sawblade-work/v04m_validation.log (each run also to <out>/<run>.log).
#
set -euo pipefail

QUICK_ONLY=0
FORCE=0
DRY=0
BB="$HOME/sawblade/testdata/ntm/bloodbath"
IRS="/Users/notsch/Music/Studio_Notsch/_IRs/Guitar_Cabs"
POOL="$HOME/.cache/sawblade/captures/pool_manifest.json"
OUT="$HOME/.cache/sawblade/match_runs/v04m"
R_SEARCH_DIR="/Users/notsch/Desktop/NailTheMix/NailtheMix_March2023_Bloodbath_44k24b"
L_DI_F="17 GTR RHY L DI.wav"
L_HM2_F="18 GTR RHY L HM2 AMP.wav"
L_UBR_F="19 GTR RHY L UBR AMP.wav"
R_DI_F="20 GTR RHY R DI.wav"
R_HM2_F="21 GTR RHY R HM2 AMP.wav"
R_UBR_F="22 GTR RHY R UBR AMP.wav"

need_arg() { [[ $# -ge 2 && -n $2 ]] || { echo "run_v04m_validation: $1 needs a value (try --help)" >&2; exit 2; }; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick-only) QUICK_ONLY=1 ;;
    --force) FORCE=1 ;;
    --dry-run) DRY=1 ;;
    --bb) need_arg "$@"; BB="$2"; shift ;;
    --irs) need_arg "$@"; IRS="$2"; shift ;;
    --pool) need_arg "$@"; POOL="$2"; shift ;;
    --out) need_arg "$@"; OUT="$2"; shift ;;
    --r-search-dir) need_arg "$@"; R_SEARCH_DIR="$2"; shift ;;
    --l-di) need_arg "$@"; L_DI_F="$2"; shift ;;
    --l-hm2) need_arg "$@"; L_HM2_F="$2"; shift ;;
    --l-ubr) need_arg "$@"; L_UBR_F="$2"; shift ;;
    --r-di) need_arg "$@"; R_DI_F="$2"; shift ;;
    --r-hm2) need_arg "$@"; R_HM2_F="$2"; shift ;;
    --r-ubr) need_arg "$@"; R_UBR_F="$2"; shift ;;
    -h|--help) awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print } else exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "run_v04m_validation: unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LOG="$HOME/sawblade-work/v04m_validation.log"
if [[ $DRY -eq 0 ]]; then
  mkdir -p "$(dirname "$LOG")"
  exec > >(tee -a "$LOG") 2>&1
fi

PY="${V04M_PY:-match/.venv/bin/python}"          # the V04M_* overrides exist for the dry-run/stub test
M="${V04M_MATCH:-match/.venv/bin/sawblade-match}"
PIP="${V04M_PIP:-match/.venv/bin/pip}"
CMAKE="${V04M_CMAKE:-cmake}"
case "$PY" in /*) PY_ABS="$PY" ;; *) PY_ABS="$ROOT/$PY" ;; esac

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

# Run a command, or only print it under --dry-run.
run() {
  if [[ $DRY -eq 1 ]]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

abs() { case "$1" in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$BB" "$1" ;; esac; }
L_DI="$(abs "$L_DI_F")"; L_HM2="$(abs "$L_HM2_F")"; L_UBR="$(abs "$L_UBR_F")"

# The R files are optional: BB first, then a recursive search by exact file name under $R_SEARCH_DIR.
find_r() {
  local f="$1" cand
  if [[ $f == /* ]]; then
    [[ -f $f ]] && printf '%s' "$f"
    return 0
  fi
  if [[ -f "$BB/$f" ]]; then
    printf '%s' "$BB/$f"
    return 0
  fi
  if [[ -d $R_SEARCH_DIR ]]; then
    cand="$(find "$R_SEARCH_DIR" -type f -name "$f" 2>/dev/null | LC_ALL=C sort | head -n 1 || true)"
    [[ -n $cand ]] && printf '%s' "$cand"
  fi
  return 0
}
R_DI=""; R_HM2=""; R_UBR=""; R_OK=0; R_MISSING=""
if [[ $QUICK_ONLY -eq 0 ]]; then
  R_DI="$(find_r "$R_DI_F")"; R_HM2="$(find_r "$R_HM2_F")"; R_UBR="$(find_r "$R_UBR_F")"
  [[ -n $R_DI && -n $R_HM2 && -n $R_UBR ]] && R_OK=1
  R_MISSING=""
  [[ -n $R_DI ]] || R_MISSING+=" \"$R_DI_F\""
  [[ -n $R_HM2 ]] || R_MISSING+=" \"$R_HM2_F\""
  [[ -n $R_UBR ]] || R_MISSING+=" \"$R_UBR_F\""
fi

[[ $DRY -eq 1 ]] && say "run_v04m_validation: DRY RUN, commands are printed, not executed"
say "run_v04m_validation: repo $ROOT, results in $OUT, log $LOG"

# ---------------------------------------------------------------- 0. inputs
step "0 check inputs"
FILES=("$L_DI" "$L_HM2" "$L_UBR")           # only the L files are required
missing=0
if [[ ! -d $BB ]]; then
  say "run_v04m_validation: Bloodbath folder not found: $BB (use --bb DIR)" >&2
  exit 1
fi
for f in "${FILES[@]}"; do
  if [[ ! -f $f ]]; then
    say "run_v04m_validation: missing Bloodbath file: $f" >&2
    missing=1
  fi
done
if [[ $missing -eq 1 ]]; then
  say "Files in $BB:" >&2
  ls "$BB" >&2
  say "Pass the right names with --l-di, --l-hm2, --l-ubr (or --bb DIR)." >&2
  exit 1
fi
[[ -d $IRS ]] || { say "run_v04m_validation: IR folder not found: $IRS (use --irs DIR)" >&2; exit 1; }
[[ -f $POOL ]] || { say "run_v04m_validation: pool manifest not found: $POOL (run sawblade-t3k pull, or use --pool FILE)" >&2; exit 1; }
if [[ $DRY -eq 0 ]]; then
  for x in "$PY" "$PIP"; do
    [[ -x $x ]] || { say "run_v04m_validation: $x not found (set up match/.venv, see match/README.md)" >&2; exit 1; }
  done
fi
say "inputs ok: ${#FILES[@]} L files, $IRS, $POOL"
if [[ $QUICK_ONLY -eq 0 ]]; then
  if [[ $R_OK -eq 1 ]]; then
    say "R files: $R_DI | $R_HM2 | $R_UBR"
  else
    say "R side files not found in $BB or $R_SEARCH_DIR (missing:$R_MISSING); skipping held-out runs"
  fi
fi
run mkdir -p "$OUT"

# ---------------------------------------------------------------- 1. build + install
step "1 rebuild the core module and install the matcher"
START=$SECONDS
# always configure: a stale build-py cache may point at another Python
run "$CMAKE" -S . -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PYTHON=ON -DSAWBLADE_BUILD_TESTS=OFF \
  "-DPython_EXECUTABLE=$PY_ABS"
run "$CMAKE" --build build-py
export SAWBLADE_CORE_DIR="$ROOT/build-py/python"
say "SAWBLADE_CORE_DIR=$SAWBLADE_CORE_DIR"
run "$PIP" install -q -e match
say "Build took $((SECONDS - START)) s."

COMMON=(--pool "$POOL" --matched mono --offset-ms 0 --ir-dir "$IRS" --listen --trace-tones 57492,79751)
FAILED=()

# match_run NAME DI REF [extra args...]: skipped when $OUT/NAME/result.json exists (unless --force)
match_run() {
  local name="$1" di="$2" ref="$3"
  shift 3
  if [[ $FORCE -eq 0 && -f "$OUT/$name/result.json" ]]; then
    say "skip $name (result.json exists; --force to redo)"
    return 0
  fi
  say "run  $name"
  local t0=$SECONDS
  if [[ $DRY -eq 1 ]]; then
    printf '+'
    printf ' %q' "$M" --di "$di" --ref "$ref" "${COMMON[@]}" "$@" --out "$OUT/$name"
    printf '\n'
  elif "$M" --di "$di" --ref "$ref" "${COMMON[@]}" "$@" --out "$OUT/$name" 2>&1 | tee "$OUT/$name.log"; then
    say "done $name in $((SECONDS - t0)) s"
  else
    say "FAILED $name (see $OUT/$name.log); continuing" >&2
    FAILED+=("$name")
  fi
}

# ---------------------------------------------------------------- 2. IR scan
step "2 index the IR library"
run "$PY" -m sawblade_match.matcher.irlib --scan "$IRS" --json "$OUT/ir_scan.json"
say "Check the printout: accepted / unique counts, rejected by reason, near-duplicate pairs, truncated, tag coverage."

RUNS=()
if [[ $QUICK_ONLY -eq 1 ]]; then
  step "3 quick runs (left side)"
  match_run L_hm2_quick "$L_DI" "$L_HM2" --quick
  match_run L_ubr_quick "$L_DI" "$L_UBR" --quick
  RUNS=(L_hm2_quick L_ubr_quick)
else
  step "3 main runs: left side, quick timing + thorough HM2 and UBR"
  match_run L_hm2_quick "$L_DI" "$L_HM2" --quick
  match_run L_hm2 "$L_DI" "$L_HM2" --thorough
  match_run L_ubr "$L_DI" "$L_UBR" --thorough
  step "4 what each suspect buys (quick runs, HM2, one suspect off each)"
  for a in feel boost filters irsweep irblend studio preeq; do
    match_run "L_hm2_quick_no_$a" "$L_DI" "$L_HM2" --quick --ablate "$a"
  done
  RUNS=(L_hm2 L_ubr)
  step "5 held-out check: right side"
  if [[ $R_OK -eq 1 ]]; then
    match_run R_hm2 "$R_DI" "$R_HM2" --thorough
    match_run R_ubr "$R_DI" "$R_UBR" --thorough
    RUNS+=(R_hm2 R_ubr)
  else
    say "R side files not found in $BB or $R_SEARCH_DIR (missing:$R_MISSING); skipping held-out runs"
  fi
fi

# ---------------------------------------------------------------- summary
step "summary (paste this back to the lead)"
if [[ $DRY -eq 1 ]]; then
  say "+ $PY - $OUT   (summary printer over $OUT/*/result.json)"
else
  "$PY" - "$OUT" <<'PY' || say "summary printer failed (the run results in $OUT/*/result.json are still there)" >&2
import json, sys, pathlib
for d in sorted(pathlib.Path(sys.argv[1]).iterdir()):
    f = d / "result.json"
    if not f.exists(): continue
    r = json.loads(f.read_text())
    def g(*ks):
        v = r
        for k in ks:
            v = v.get(k) if isinstance(v, dict) else None
        return v
    b = r.get("best", {}); bd = b.get("breakdown", {}) or {}; ft = bd.get("feelTerms") or {}
    print(f"== {d.name}  wall {r.get('wallSeconds', 0)/60:.1f} min")
    print("  A-weighted dB:", (r.get("after") or [{}])[0].get("aWeightedErrorDb"), "| loss", bd.get("total"),
          "ltas", bd.get("ltas"), "feel", bd.get("feel"))
    print("  feel: tight", ft.get("tight"), "fizz", ft.get("fizz"), "polish", ft.get("polish"), "dropped", ft.get("dropped"))
    print("  chain:", b.get("topology"), {k: (v or {}).get("title") for k, v in (b.get("captures") or {}).items()})
    print("  boost won:", g("tightBoost", "won"), "| IR pair won:", g("irBlend", "won"), "| IR winner:", g("irPool", "winner"))
    print("  pre-EQ:", g("preEq", "chosen"), "| studio:", {k: g("studio", k) for k in ("compressed", "eqd", "busCompUsed")})
    print("  gate:", r.get("gateFinal"), "| post filters:", r.get("postFilters"))
    print("  listening gain dB:", g("listening", "gainDb"), "| IR pool:", {k: g("irPool", k) for k in ("total", "screened", "prefiltered")})
    tc = r.get("tonecheck", {}).get("best_L", {})
    print("  guardrails:", [(x.get("id"), x.get("status")) for x in tc.get("rules", []) if x.get("status") != "pass"])
    if r.get("trace"): print("  trace:", {k: (v or {}).get("why") for k, v in r["trace"].items()})
PY
fi

step "pre-EQ DI tilt lines (one per run; they calibrate the guitar-difference thresholds)"
if [[ $DRY -eq 1 ]]; then
  say "+ grep -H 'pre-EQ: DI tilt' $OUT/*.log"
else
  grep -H 'pre-EQ: DI tilt' "$OUT"/*.log 2>/dev/null || say "(none found in the run logs)"
fi

step "listen A/B (level-matched ref.wav vs render.wav, same 30 s)"
for r in "${RUNS[@]}"; do
  if [[ $DRY -eq 0 ]]; then
    case " ${FAILED[*]-} " in *" $r "*) continue ;; esac
    [[ -d "$OUT/$r/listen" ]] || { say "(no listen folder for $r)"; continue; }
  fi
  say "open \"$OUT/$r/listen\""
done

if [[ ${#FAILED[@]} -gt 0 ]]; then
  say "FAILED runs: ${FAILED[*]}" >&2
  exit 1
fi
say "run_v04m_validation: done."
