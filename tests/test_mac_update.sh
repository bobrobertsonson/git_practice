#!/usr/bin/env bash
# Checks the commands scripts/mac_update.sh prints under --dry-run.
set -euo pipefail
script="$1"
out="$(bash "$script" --dry-run --standalone)"
fail=0
expect() {
  if ! grep -qE -- "$1" <<<"$out"; then echo "MISSING: $1" >&2; fail=1; fi
}
expect '^\+ git pull --ff-only$'
expect '^\+ cmake -S \. -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON$'
expect '^\+ cmake --build build-mac$'
expect '^\+ cp -R .*/Release/AU/Sawblade\.component .*/Library/Audio/Plug-Ins/Components/$'
expect '^\+ cp -R .*/Release/VST3/Sawblade\.vst3 .*/Library/Audio/Plug-Ins/VST3/$'
expect '^\+ killall -9 AudioComponentRegistrar'
expect '^\+ auval -v aufx Swb1 Swbl \| tail -n 1$'
expect '^\+ match/\.venv/bin/sawblade-t3k resolve presets/chainsaw_body\.json$'
expect '^\+ open .*/Standalone/Sawblade\.app$'
expect '^Standalone app: '
if grep -q 'resolved\.json$' <<<"$(grep -E 'sawblade-t3k resolve' <<<"$out")"; then
  echo "resolve was invoked on a .resolved.json file" >&2; fail=1
fi
# --no-resolve and --clean
out="$(bash "$script" --dry-run --no-resolve --clean)"
expect '^\+ rm -rf build-mac$'
expect '^Skipped \(--no-resolve\)\.$'
if grep -q 'sawblade-t3k resolve' <<<"$out"; then echo "--no-resolve still resolved" >&2; fail=1; fi
# dry-run must not write the last-built marker
if bash "$script" --bogus >/dev/null 2>&1; then echo "unknown flag accepted" >&2; fail=1; fi
exit $fail
