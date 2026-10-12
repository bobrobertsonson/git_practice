#!/usr/bin/env bash
# Poor-man's sampling profiler (no perf here): attach gdb to a running process N times and record, per
# sample, the top frame and the first `demucscpp::` / `Eigen::` caller frame. usage: gdb_sample.sh PID N INTERVAL_S OUT
PID=$1; N=${2:-60}; DT=${3:-1}; OUT=${4:-samples.txt}
: > "$OUT"
for i in $(seq 1 "$N"); do
  kill -0 "$PID" 2>/dev/null || break
  gdb -p "$PID" -batch -ex "bt 14" 2>/dev/null | grep '^#' | sed -E 's/^#[0-9]+ +(0x[0-9a-f]+ in )?//; s/\(.*//' | tr '\n' '|' >> "$OUT"
  echo >> "$OUT"
  sleep "$DT"
done
