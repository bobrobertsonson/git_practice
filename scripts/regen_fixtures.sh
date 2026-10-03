#!/usr/bin/env bash
# Regenerates the deterministic audio fixtures (tests/fixtures/di_riff.wav, ir/ir_a.wav,
# ir/ir_b.wav). With --goldens, also rewrites tests/golden/*.wav (SAWBLADE_UPDATE_GOLDEN=1).
#
#   scripts/regen_fixtures.sh [--goldens] [build-dir]
#
# Review `git diff --stat` afterwards: regenerated fixtures change the goldens.
set -euo pipefail
goldens=0
if [[ "${1:-}" == "--goldens" ]]; then goldens=1; shift; fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
[[ -f "$build/build.ninja" ]] || cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target make_fixtures
"$build/tests/make_fixtures" "$root/tests/fixtures"
if [[ $goldens -eq 1 ]]; then
  cmake --build "$build" --target sawblade_tests
  SAWBLADE_UPDATE_GOLDEN=1 "$build/tests/sawblade_tests" "[golden]"
fi
