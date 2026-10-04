#!/usr/bin/env bash
# One command to get the latest Sawblade plugin on the Mac (Apple Silicon, Homebrew,
# cmake + ninja, Command Line Tools only). See docs/MAC.md.
#
#   scripts/mac_update.sh [--no-resolve] [--clean] [--standalone] [--dry-run]
#
# --no-resolve  skip the TONE3000 preset resolve step
# --clean       wipe build-mac/ first (full reconfigure + rebuild)
# --standalone  open the Standalone app at the end
# --dry-run     print the commands instead of running them (safe on any machine)
set -euo pipefail

NO_RESOLVE=0
CLEAN=0
STANDALONE=0
DRY=0
for arg in "$@"; do
  case "$arg" in
    --no-resolve) NO_RESOLVE=1 ;;
    --clean) CLEAN=1 ;;
    --standalone) STANDALONE=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "mac_update: unknown option: $arg (try --help)" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR=build-mac
LAST_FILE=.mac_update_last
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
ART_DIR="$BUILD_DIR/plugin/SawbladePlugin_artefacts/Release"
T3K=match/.venv/bin/sawblade-t3k
TOKEN_FILE="${SAWBLADE_T3K_TOKEN_FILE:-$HOME/.config/sawblade/t3k_tokens.json}"

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

[[ $DRY -eq 1 ]] && say "mac_update: DRY RUN, commands are printed, not executed"
say "mac_update: repo $ROOT"

# ---------------------------------------------------------------- 1. pull
step "1/5 git pull"
PREV=""
[[ -f $LAST_FILE ]] && PREV="$(tr -d '[:space:]' <"$LAST_FILE")"
run git pull --ff-only
HEAD_SHA="$(git rev-parse HEAD)"
if [[ -n $PREV ]] && git cat-file -e "${PREV}^{commit}" 2>/dev/null; then
  if [[ $PREV == "$HEAD_SHA" ]]; then
    say "No new commits since the last update (${HEAD_SHA:0:7})."
  else
    say "Commits since last update (${PREV:0:7}..${HEAD_SHA:0:7}):"
    git log --oneline "${PREV}..${HEAD_SHA}"
  fi
else
  PREV=""
  say "No previous update recorded; now at $(git log --oneline -1)"
fi

# ---------------------------------------------------------------- 2. build
step "2/5 build"
if [[ $CLEAN -eq 1 ]]; then
  run rm -rf "$BUILD_DIR"
fi
START=$SECONDS
if [[ ! -f $BUILD_DIR/build.ninja || $CLEAN -eq 1 ]]; then
  run cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON
fi
run cmake --build "$BUILD_DIR"
say "Build took $((SECONDS - START)) s."

# ---------------------------------------------------------------- 3. install + auval
step "3/5 install + auval"
if [[ $DRY -eq 0 ]]; then
  for f in "$ART_DIR/AU/Sawblade.component" "$ART_DIR/VST3/Sawblade.vst3"; do
    [[ -d $f ]] || { echo "mac_update: build artefact missing: $f" >&2; exit 1; }
  done
fi
run mkdir -p "$AU_DIR" "$VST3_DIR"
run rm -rf "$AU_DIR/Sawblade.component" "$VST3_DIR/Sawblade.vst3"
run cp -R "$ART_DIR/AU/Sawblade.component" "$AU_DIR/"
run cp -R "$ART_DIR/VST3/Sawblade.vst3" "$VST3_DIR/"
if [[ $DRY -eq 1 ]]; then
  say "+ killall -9 AudioComponentRegistrar (failure ignored)"
  say "+ auval -v aufx Swb1 Swbl | tail -n 1"
else
  killall -9 AudioComponentRegistrar 2>/dev/null || true
  sleep 1
  AUVAL_LAST="$(auval -v aufx Swb1 Swbl 2>&1 | tail -n 1 || true)"
  say "$AUVAL_LAST"
  if [[ $AUVAL_LAST != *"SUCCEEDED"* ]]; then
    say "WARNING: auval did not report success; run 'auval -v aufx Swb1 Swbl' for details." >&2
  fi
fi

# ---------------------------------------------------------------- 4. resolve presets
step "4/5 TONE3000 presets"
resolve_presets() {
  local p out n_ok=0 n_fail=0 n_skip=0 reason
  while IFS= read -r p; do
    out="${p%.json}.resolved.json"
    if [[ $DRY -eq 0 && -f $out && $out -nt $p ]]; then
      say "  skip   $p (resolved file is newer)"
      n_skip=$((n_skip + 1))
      continue
    fi
    if [[ $DRY -eq 1 ]]; then
      say "+ $T3K resolve $p"
      continue
    fi
    if reason="$("$T3K" resolve "$p" 2>&1)"; then
      say "  ok     $p"
      n_ok=$((n_ok + 1))
    else
      reason="$(printf '%s' "$reason" | tail -n 1)"
      say "  failed $p: ${reason:-unknown error}"
      n_fail=$((n_fail + 1))
    fi
  done < <(find presets -name '*.json' ! -name '*.resolved.json' | LC_ALL=C sort)
  [[ $DRY -eq 1 ]] || say "  resolve: $n_ok ok, $n_fail failed, $n_skip up to date"
}

if [[ $NO_RESOLVE -eq 1 ]]; then
  say "Skipped (--no-resolve)."
elif [[ $DRY -eq 0 && ! -x $T3K ]]; then
  say "Skipped: $T3K not found (see match/README.md to set up the venv)."
elif [[ $DRY -eq 1 || -n ${TONE3000_CLIENT_ID:-} || -f $TOKEN_FILE ]]; then
  resolve_presets
else
  say "Not logged in to TONE3000. Run this once, then re-run mac_update.sh:"
  say "  export TONE3000_CLIENT_ID=t3k_pub_xxxxxxxx && $ROOT/$T3K login"
fi

# ---------------------------------------------------------------- 5. what's new
step "5/5 what's new"
if [[ -n $PREV ]]; then
  say "Changed under plugin/ and presets/ since ${PREV:0:7}:"
  CHANGED="$(git diff --name-status "$PREV" "$HEAD_SHA" -- plugin presets | grep -v '\.resolved\.json$' || true)"
  say "${CHANGED:-  (nothing)}"
else
  say "Changed files: (no previous update recorded, nothing to compare)"
fi
APP="$ROOT/$ART_DIR/Standalone/Sawblade.app"
say "Standalone app: $APP"
say "Installed: $AU_DIR/Sawblade.component, $VST3_DIR/Sawblade.vst3"
say "Restart your DAW (or rescan plugins) to load the new build."

if [[ $DRY -eq 0 ]]; then
  printf '%s\n' "$HEAD_SHA" >"$LAST_FILE"
fi
if [[ $STANDALONE -eq 1 ]]; then
  run open "$APP"
fi
