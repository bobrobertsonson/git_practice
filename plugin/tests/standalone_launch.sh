#!/bin/bash
# integration: standalone launches. Starts the built Standalone app under xvfb for 6 s with its data in a temp dir, captures
# the X root with ImageMagick, checks the process is still alive at capture time and that SIGTERM ends it.
# usage: standalone_launch.sh <xvfb-run> <import> <Sawblade executable> <out.png>
XVFB=$1; IMPORT=$2; EXE=$3; OUT=$4
[ -x "$EXE" ] || { echo "no executable: $EXE"; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$(dirname "$OUT")" "$tmp/home"
export HOME="$tmp/home" SAWBLADE_DATA_DIR="$tmp/data" SAWBLADE_APPDATA="$tmp/data" SAWBLADE_CACHE_DIR="$tmp/cache" \
       SAWBLADE_SETTINGS_FILE="$tmp/settings.json" SAWBLADE_STEMS_DIR="$tmp/stems"
export XVFB EXE OUT IMPORT
exec "$XVFB" -a -s "-screen 0 1400x900x24" bash -c '
  "$EXE" > "'"$tmp"'/app.log" 2>&1 &
  pid=$!
  sleep 6
  if ! kill -0 $pid 2>/dev/null; then echo "FAIL: the app exited early"; cat "'"$tmp"'/app.log"; exit 1; fi
  "$IMPORT" -window root "$OUT" || { echo "FAIL: capture"; kill $pid; exit 1; }
  kill -TERM $pid
  for i in $(seq 1 50); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
  if kill -0 $pid 2>/dev/null; then echo "FAIL: still running after SIGTERM"; kill -KILL $pid; exit 1; fi
  wait $pid; rc=$?
  echo "exit status after SIGTERM: $rc"
  [ $rc -eq 0 ] || [ $rc -eq 143 ] || { echo "FAIL: unexpected exit status"; cat "'"$tmp"'/app.log"; exit 1; }
  [ -s "$OUT" ] || { echo "FAIL: empty screenshot"; exit 1; }
  echo "app log:"; cat "'"$tmp"'/app.log"
'
