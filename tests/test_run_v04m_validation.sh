#!/usr/bin/env bash
# Checks scripts/run_v04m_validation.sh under --dry-run with fake inputs (bash only; nothing runs).
set -euo pipefail
script="$1"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bb" "$T/irs" "$T/rs/Bloodbath/nested"
for f in "17 GTR RHY L DI.wav" "18 GTR RHY L HM2 AMP.wav" "19 GTR RHY L UBR AMP.wav"; do : >"$T/bb/$f"; done
: >"$T/pool.json"
fail=0
args=(--dry-run --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/out" --r-search-dir "$T/rs")
expect() {
  if ! grep -qE -- "$1" <<<"$out"; then echo "MISSING: $1" >&2; fail=1; fi
}
reject() {
  if grep -qE -- "$1" <<<"$out"; then echo "UNEXPECTED: $1" >&2; fail=1; fi
}

# R files only in a nested folder of the search dir: found by exact name, the held-out runs are listed
for f in "20 GTR RHY R DI.wav" "21 GTR RHY R HM2 AMP.wav" "22 GTR RHY R UBR AMP.wav"; do : >"$T/rs/Bloodbath/nested/$f"; done
out="$(bash "$script" "${args[@]}")"
expect '^\+ cmake -S \. -B build-py -G Ninja .* -DPython_EXECUTABLE=.*/match/\.venv/bin/python$'
expect '^\+ cmake --build build-py$'
expect '^\+ match/\.venv/bin/pip install -q -e match$'
expect '^\+ match/\.venv/bin/python -m sawblade_match\.matcher\.irlib --scan .*/irs --json .*/out/ir_scan\.json$'
expect '^\+ match/\.venv/bin/sawblade-match --di .*17\\ GTR\\ RHY\\ L\\ DI\.wav --ref .*18\\ GTR\\ RHY\\ L\\ HM2\\ AMP\.wav .*--matched mono --offset-ms 0 --ir-dir .*--listen --trace-tones 57492\\?,79751 --quick --out .*/out/L_hm2_quick$'
expect '--thorough --out .*/out/L_hm2$'
expect '--thorough --out .*/out/L_ubr$'
for a in feel boost filters irsweep irblend studio preeq; do expect "--quick --ablate $a --out .*/out/L_hm2_quick_no_$a$"; done
expect 'nested/20\\ GTR\\ RHY\\ R\\ DI\.wav.*--thorough --out .*/out/R_hm2$'
expect '--thorough --out .*/out/R_ubr$'
expect "^\\+ grep -H 'pre-EQ: DI tilt'"
expect '^open ".*/out/L_hm2/listen"$'
expect '^open ".*/out/R_ubr/listen"$'

# resumable: a run with a result.json is skipped, --force redoes it
mkdir -p "$T/out/L_hm2" && : >"$T/out/L_hm2/result.json"
out="$(bash "$script" "${args[@]}")"
expect '^skip L_hm2 '
reject 'out/L_hm2$'
out="$(bash "$script" "${args[@]}" --force)"
expect 'thorough --out .*/out/L_hm2$'
rm -rf "$T/out"

# only some R files found: the missing one is named
rm "$T/rs/Bloodbath/nested/22 GTR RHY R UBR AMP.wav"
out="$(bash "$script" "${args[@]}")"
expect 'skipping held-out runs'
expect '\(missing: "22 GTR RHY R UBR AMP\.wav"\)'
reject '"20 GTR RHY R DI\.wav"\)'
: >"$T/rs/Bloodbath/nested/22 GTR RHY R UBR AMP.wav"
rm -rf "$T/out"

# R files nowhere: the held-out runs are skipped with a message, no failure
rm -rf "$T/rs/Bloodbath"
out="$(bash "$script" "${args[@]}")"
expect 'R side files not found in .* or .*; skipping held-out runs'
reject '/out/R_hm2$'
expect '^\+ match/.*--out .*/out/L_ubr$'

# --quick-only: IR scan, the two quick left runs, no thorough / ablation / right runs
out="$(bash "$script" "${args[@]}" --quick-only)"
expect '--quick --out .*/out/L_hm2_quick$'
expect '--quick --out .*/out/L_ubr_quick$'
reject 'thorough|--ablate'
expect '^open ".*/out/L_ubr_quick/listen"$'

# a missing L file: ls of the Bloodbath folder and exit 1
rm "$T/bb/19 GTR RHY L UBR AMP.wav"
set +e
out="$(bash "$script" "${args[@]}" 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || { echo "missing L file: exit code $rc, wanted 1" >&2; fail=1; }
expect 'missing Bloodbath file: .*19 GTR RHY L UBR AMP\.wav'
expect '^17 GTR RHY L DI\.wav$'          # the ls listing
# --help ends at the header (no code lines)
out="$(bash "$script" --help)"
reject 'set -euo pipefail'
expect '^Run it AFTER `scripts/mac_update\.sh --no-models`'
# real (non-dry) run with stubs: a failing run is reported and the rest continues, a failing summary printer does not
# stop the open lines / FAILED report; HOME is a temp dir so the log goes there
: >"$T/bb/19 GTR RHY L UBR AMP.wav"
S="$T/stub"; mkdir -p "$S" "$T/home"
printf '#!/usr/bin/env bash\nexit 0\n' >"$S/cmake"
printf '#!/usr/bin/env bash\nexit 0\n' >"$S/pip"
printf '#!/usr/bin/env bash\ncase "$1" in -) cat >/dev/null; exit 1 ;; *) exit 0 ;; esac\n' >"$S/python"
cat >"$S/match" <<'STUB'
#!/usr/bin/env bash
out=""
while [[ $# -gt 0 ]]; do [[ $1 == --out ]] && out="$2"; shift; done
case "$out" in *L_ubr_quick) echo "stub failure" >&2; exit 1 ;; esac
mkdir -p "$out/listen"; : >"$out/result.json"
echo "[   1.0s] pre-EQ: DI tilt -3.00 dB/oct, low excess +1.00 dB; widened: none"
STUB
chmod +x "$S"/*
set +e
out="$(HOME="$T/home" V04M_CMAKE="$S/cmake" V04M_PIP="$S/pip" V04M_PY="$S/python" V04M_MATCH="$S/match" \
       bash "$script" --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/out2" --quick-only 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || { echo "failing run: exit code $rc, wanted 1" >&2; fail=1; }
expect '^FAILED L_ubr_quick'
expect '^done L_hm2_quick in '
expect 'summary printer failed'
expect "L_hm2_quick\.log:.*pre-EQ: DI tilt -3\.00"
expect '^open ".*/out2/L_hm2_quick/listen"$'
reject 'open ".*L_ubr_quick/listen"'
expect '^FAILED runs: L_ubr_quick'
[[ -f "$T/home/sawblade-work/v04m_validation.log" ]] || { echo "no log in HOME" >&2; fail=1; }
# rerun: finished runs are skipped
out="$(HOME="$T/home" V04M_CMAKE="$S/cmake" V04M_PIP="$S/pip" V04M_PY="$S/python" V04M_MATCH="$S/match" \
       bash "$script" --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/out2" --quick-only 2>&1 || true)"
expect '^skip L_hm2_quick '
# unknown flag
if bash "$script" --bogus >/dev/null 2>&1; then echo "unknown flag accepted" >&2; fail=1; fi
exit $fail
