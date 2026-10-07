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
# blend references, blend runs (first), per-path check, held-out transfer, dynamics sweep, summary module
expect '^\+ match/\.venv/bin/python -m sawblade_match\.matcher\.refsum --a .*18\\ GTR\\ RHY\\ L\\ HM2\\ AMP\.wav --b .*19\\ GTR\\ RHY\\ L\\ UBR\\ AMP\.wav --out .*/out/refs/L_blend\.wav --blend-db 0\\?,0 --polarity auto --json .*/out/refs/L_blend\.json$'
expect '--out .*/out/refs/R_blend\.wav --blend-db 0\\?,0 --polarity auto --json .*/out/refs/R_blend\.json$'
expect '^\+ match/\.venv/bin/sawblade-match --di .*17\\ GTR\\ RHY\\ L\\ DI\.wav --ref .*/out/refs/L_blend\.wav .*--thorough --out .*/out/L_blend$'
expect '^\+ match/\.venv/bin/sawblade-match --di .*nested/20\\ GTR\\ RHY\\ R\\ DI\.wav --ref .*/out/refs/R_blend\.wav .*--thorough --out .*/out/R_blend$'
reject 'L_blend_quick'
first_run="$(grep -n -E '^run  ' <<<"$out" | head -n 1)"
[[ $first_run == *"run  L_blend" ]] || { echo "L_blend is not the first run: $first_run" >&2; fail=1; }
expect '^\+ match/\.venv/bin/python -m sawblade_match\.matcher\.pathcheck --result .*/out/L_blend/result\.json --di .*17\\ GTR\\ RHY\\ L\\ DI\.wav --ref-a .*18\\ GTR.* --ref-b .*19\\ GTR.* --ref-blend .*/out/refs/L_blend\.wav --blend-db 0\\?,0 --json .*/out/L_blend/pathcheck\.json$'
expect '--result .*/out/R_blend/result\.json --di .*20\\ GTR.*--ref-a .*21\\ GTR.*--ref-b .*22\\ GTR.*--json .*/out/R_blend/pathcheck\.json$'
expect '--result .*/out/L_blend/result\.json --di .*20\\ GTR\\ RHY\\ R\\ DI.*--ref-a .*21\\ GTR.*--ref-blend .*/out/refs/R_blend\.wav .*--json .*/out/L_blend_on_R\.pathcheck\.json$'
expect '^\+ match/\.venv/bin/python -m sawblade_match\.matcher\.dynsweep --result .*/out/L_blend/result\.json --di .*17.* --json .*/out/L_blend/dynsweep\.json$'
expect 'dynsweep\.py|matcher\.dynsweep --result .*/out/R_ubr/result\.json --di .*20.*/out/R_ubr/dynsweep\.json$'
reject 'dynsweep --result .*_no_'
expect '^\+ match/\.venv/bin/python -m sawblade_match\.matcher\.validation_summary '
# the listen pairs name render_live.wav; the held-out transfer passes --offset-ms 0
expect 'listen A/B .*ref\.wav vs render\.wav .*render_live\.wav'
expect 'L_blend_on_R\.pathcheck\.json$'
expect '--blend-db 0\\?,0 --offset-ms 0 --json .*/out/L_blend_on_R\.pathcheck\.json$'
# blend runs come first in the listen list
lo="$(grep -n -E '^open ' <<<"$out" | head -n 1)"
[[ $lo == *"L_blend/listen"* ]] || { echo "first listen line is not L_blend: $lo" >&2; fail=1; }
# --blend-db is passed on, validated as two numbers
out="$(bash "$script" "${args[@]}" --blend-db -3,2.5)"
expect 'refsum .*--blend-db -3\\?,2\.5 '
expect 'pathcheck .*--blend-db -3\\?,2\.5 '
for bad in "x,1" "1" "1,2,3" "" "a" "1,"; do
  set +e; bash "$script" "${args[@]}" --blend-db "$bad" >/dev/null 2>&1; rc=$?; set -e
  [[ $rc -eq 2 ]] || { echo "bad --blend-db '$bad': exit $rc, wanted 2" >&2; fail=1; }
done
out="$(bash "$script" "${args[@]}")"

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
expect 'sawblade-match --di .*17.* --ref .*/out/refs/L_blend\.wav .*--quick --out .*/out/L_blend_quick$'
expect 'refsum .*--out .*/out/refs/L_blend\.wav'
reject 'R_blend|refs/R_'
expect 'pathcheck --result .*/out/L_blend_quick/result\.json .*--json .*/out/L_blend_quick/pathcheck\.json$'
expect 'dynsweep --result .*/out/L_ubr_quick/result\.json'
reject 'L_blend_on_R'
expect '^\(no held-out transfer'
[[ "$(grep -n -E '^open ' <<<"$out" | head -n 1)" == *"L_blend_quick/listen"* ]] || { echo "quick listen: blend not first" >&2; fail=1; }

# polarity: auto by default, --blend-polarity maps to refsum's --polarity (hm2 = track a, body = track b); bad value -> exit 2
out="$(bash "$script" "${args[@]}")"
expect 'refsum .*--blend-db 0\\?,0 --polarity auto --json .*/out/refs/L_blend\.json$'
for pair in "asis:asis" "invert-hm2:invert-a" "invert-body:invert-b" "auto:auto"; do
  out="$(bash "$script" "${args[@]}" --blend-polarity "${pair%%:*}")"
  expect "refsum .*--polarity ${pair##*:} --json .*/out/refs/L_blend\\.json\$"
done
for bad in "sideways" "invert" "" "INVERT-BODY"; do
  set +e; bash "$script" "${args[@]}" --blend-polarity "$bad" >/dev/null 2>&1; rc=$?; set -e
  [[ $rc -eq 2 ]] || { echo "bad --blend-polarity '$bad': exit $rc, wanted 2" >&2; fail=1; }
done
# a reference built with other settings is rebuilt, never reused; its blend runs are redone; same settings: reused
SD="$T/stubdry"; mkdir -p "$SD" "$T/refs_out/refs" "$T/refs_out/L_blend"
cat >"$SD/python" <<'STUB'
#!/usr/bin/env bash
if [[ "$*" == *--settings-key* ]]; then echo "KEY[$*]"; fi
exit 0
STUB
chmod +x "$SD/python"
: >"$T/refs_out/refs/L_blend.wav"; : >"$T/refs_out/L_blend/result.json"
printf '{"polarity": {"mode": "auto"}, "settingsKey": "KEY[-m sawblade_match.matcher.refsum --settings-key --blend-db 0,0 --polarity auto]"}\n' >"$T/refs_out/refs/L_blend.json"
sargs=(--dry-run --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/refs_out" --r-search-dir "$T/none")
out="$(V04M_PY="$SD/python" bash "$script" "${sargs[@]}")"
expect '^skip L_blend reference \(exists with the same settings'
reject 'matcher\.refsum --a .*L_blend\.wav'
expect '^skip L_blend '
out="$(V04M_PY="$SD/python" bash "$script" "${sargs[@]}" --blend-polarity invert-body)"      # polarity mode changed
expect 'L_blend reference exists but was built with other settings'
expect 'refsum --a .*--polarity invert-b --json .*/refs/L_blend\.json$'
expect '--thorough --out .*/refs_out/L_blend$'                          # redone although result.json exists
reject '^skip L_blend '
out="$(V04M_PY="$SD/python" bash "$script" "${sargs[@]}" --blend-db 0,-2)"          # faders changed
expect 'built with other settings'
printf '{"gainsDb": [0, 0], "polarity": 1}\n' >"$T/refs_out/refs/L_blend.json"      # a reference from before the polarity option
out="$(V04M_PY="$SD/python" bash "$script" "${sargs[@]}")"
expect 'built with other settings \(or none recorded\)'
expect '--thorough --out .*/refs_out/L_blend$'

# real (non-dry) runs with stubs and a temp $OUT: stale / failed blend references remove what was derived from them, per side
R3="$T/real"; mkdir -p "$R3/stub" "$R3/home" "$R3/rs"
for f in "20 GTR RHY R DI.wav" "21 GTR RHY R HM2 AMP.wav" "22 GTR RHY R UBR AMP.wav"; do : >"$R3/rs/$f"; done
printf '#!/usr/bin/env bash\nexit 0\n' >"$R3/stub/cmake"; cp "$R3/stub/cmake" "$R3/stub/pip"
cat >"$R3/stub/python" <<'STUB'
#!/usr/bin/env bash
# --settings-key echoes the args; a refsum building R_blend fails when STUB_FAIL_R is set; everything else succeeds
if [[ "$*" == *--settings-key* ]]; then echo "KEY[$*]"; exit 0; fi
if [[ "$*" == *matcher.refsum* && "$*" == *R_blend.wav* && -n ${STUB_FAIL_R:-} ]]; then exit 2; fi
exit 0
STUB
cat >"$R3/stub/match" <<'STUB'
#!/usr/bin/env bash
out=""
while [[ $# -gt 0 ]]; do [[ $1 == --out ]] && out="$2"; shift; done
mkdir -p "$out/listen"; : >"$out/result.json"
STUB
chmod +x "$R3/stub"/*
LKEY='KEY[-m sawblade_match.matcher.refsum --settings-key --blend-db 0,0 --polarity auto]'
mk_state() {      # every blend artefact of both sides present; the L reference current, the R one built with other settings
  rm -rf "$R3/out"; mkdir -p "$R3/out/refs"
  for r in L_blend R_blend; do
    mkdir -p "$R3/out/$r"
    : >"$R3/out/$r/result.json"; : >"$R3/out/$r/pathcheck.json"; : >"$R3/out/$r/dynsweep.json"
    : >"$R3/out/refs/$r.wav"
  done
  : >"$R3/out/L_blend_on_R.pathcheck.json"
  printf '{"settingsKey": "%s"}\n' "$LKEY" >"$R3/out/refs/L_blend.json"
  printf '{"settingsKey": "OLD"}\n' >"$R3/out/refs/R_blend.json"
}
real() {          # real RUN_ARGS...: the full (non-quick) script on the temp $OUT, stubs for every tool
  HOME="$R3/home" V04M_CMAKE="$R3/stub/cmake" V04M_PIP="$R3/stub/pip" V04M_PY="$R3/stub/python" V04M_MATCH="$R3/stub/match" \
    bash "$script" --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$R3/out" --r-search-dir "$R3/rs" "$@" 2>&1
}
gone() { [[ ! -e "$1" ]] || { echo "NOT REMOVED: $1" >&2; fail=1; }; }
kept() { [[ -e "$1" ]] || { echo "WRONGLY REMOVED: $1" >&2; fail=1; }; }
# (1) only the R reference is stale: R's results and the held-out transfer go, the L side stays
mk_state
set +e; out="$(real)"; rc=$?; set -e
[[ $rc -eq 0 ]] || { echo "stale-R run: exit $rc, wanted 0" >&2; fail=1; }
expect 'R_blend reference exists but was built with other settings'
expect '^skip L_blend reference \(exists with the same settings'
expect '^skip L_blend '
expect '^done R_blend in '                       # redone although result.json existed
gone "$R3/out/R_blend/pathcheck.json"; gone "$R3/out/R_blend/dynsweep.json"; gone "$R3/out/L_blend_on_R.pathcheck.json"
kept "$R3/out/L_blend/pathcheck.json"; kept "$R3/out/L_blend/dynsweep.json"; kept "$R3/out/L_blend/result.json"; kept "$R3/out/refs/L_blend.wav"
grep -qF "$LKEY" "$R3/out/refs/L_blend.json" || { echo "L reference JSON changed" >&2; fail=1; }
# (2) a failing refsum for R: its old reference, results and the transfer are removed (not presented as current); L stays
mk_state
set +e; out="$(STUB_FAIL_R=1 real)"; rc=$?; set -e
[[ $rc -eq 1 ]] || { echo "failed-refsum run: exit $rc, wanted 1" >&2; fail=1; }
expect '^FAILED R_blend reference'
expect '^skip R_blend \(no R blend reference\)'
gone "$R3/out/R_blend/result.json"; gone "$R3/out/R_blend/pathcheck.json"; gone "$R3/out/R_blend/dynsweep.json"
gone "$R3/out/refs/R_blend.wav"; gone "$R3/out/refs/R_blend.json"; gone "$R3/out/L_blend_on_R.pathcheck.json"
kept "$R3/out/L_blend/result.json"; kept "$R3/out/L_blend/pathcheck.json"; kept "$R3/out/refs/L_blend.wav"; kept "$R3/out/refs/L_blend.json"
# (3) the L reference stale: L's results and the transfer go; R stays
mk_state
printf '{"settingsKey": "OLD"}\n' >"$R3/out/refs/L_blend.json"
printf '{"settingsKey": "%s"}\n' "$LKEY" >"$R3/out/refs/R_blend.json"
set +e; out="$(real)"; rc=$?; set -e
expect '^done L_blend in '
gone "$R3/out/L_blend/pathcheck.json"; gone "$R3/out/L_blend_on_R.pathcheck.json"
kept "$R3/out/R_blend/pathcheck.json"; kept "$R3/out/R_blend/dynsweep.json"

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
cat >"$S/python" <<'STUB'
#!/usr/bin/env bash
# fails the summary printer; fails refsum when STUB_REFSUM_FAIL is set; logs every module call
if [[ $1 == -m ]]; then
  echo "$*" >>"${STUB_CALLS:-/dev/null}"
  case "$2" in
    *validation_summary) exit 1 ;;
    *refsum) [[ -n ${STUB_REFSUM_FAIL:-} ]] && exit 2 ;;
  esac
fi
exit 0
STUB
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
out="$(HOME="$T/home" V04M_CMAKE="$S/cmake" V04M_PIP="$S/pip" V04M_PY="$S/python" V04M_MATCH="$S/match" STUB_CALLS="$T/calls" \
       bash "$script" --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/out2" --quick-only 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || { echo "failing run: exit code $rc, wanted 1" >&2; fail=1; }
expect '^FAILED L_ubr_quick'
expect '^done L_blend_quick in '
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
# the blend tools are called for the finished blend run only (not for the failed run), and the summary module is the failing printer
grep -q 'sawblade_match.matcher.pathcheck --result .*/out2/L_blend_quick/result.json' "$T/calls" || { echo "pathcheck not called for L_blend_quick" >&2; fail=1; }
grep -q 'sawblade_match.matcher.dynsweep --result .*/out2/L_hm2_quick/result.json' "$T/calls" || { echo "dynsweep not called for L_hm2_quick" >&2; fail=1; }
if grep -q 'dynsweep --result .*L_ubr_quick' "$T/calls"; then echo "dynsweep called for the failed run" >&2; fail=1; fi
# a failing refsum: reported, the blend run is skipped, exit 1, the single-amp runs still go
set +e
out="$(HOME="$T/home" V04M_CMAKE="$S/cmake" V04M_PIP="$S/pip" V04M_PY="$S/python" V04M_MATCH="$S/match" STUB_REFSUM_FAIL=1 \
       bash "$script" --bb "$T/bb" --irs "$T/irs" --pool "$T/pool.json" --out "$T/out3" --quick-only 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || { echo "failing refsum: exit code $rc, wanted 1" >&2; fail=1; }
expect '^FAILED L_blend reference'
expect '^skip L_blend_quick \(no L blend reference\)'
expect '^done L_hm2_quick in '
# unknown flag
if bash "$script" --bogus >/dev/null 2>&1; then echo "unknown flag accepted" >&2; fail=1; fi
exit $fail
