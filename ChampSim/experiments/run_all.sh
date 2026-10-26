#!/usr/bin/env bash
#
# Run every (variant, policy, workload) that has a built binary, in parallel.
#
# p1 runs the trace on one core. p2 and p2s run the SAME trace on both cores: in p2s the cores share an address
# space, so this creates (artificial) sharing; p2 is the identical load without sharing, as a control.
#
# Usage, from a WSL shell in the ChampSim directory:
#   bash experiments/run_all.sh
#   VARIANTS="p1" POLICIES="lru srrip" WORKLOADS="mcf lbm" JOBS=4 bash experiments/run_all.sh
#
# Environment (defaults):
#   BUILD_DIR=~/champsim-build  TRACE_DIR=~/traces  OUT=~/champsim-results
#   WARMUP=10000000  SIM=50000000  JOBS=8
#   VARIANTS, POLICIES, WORKLOADS   optional space-separated filters
#
# Logs go to $OUT/raw/<variant>/<policy>/<workload>.log. A job whose log already contains "Simulation complete"
# is skipped, so an interrupted sweep can be resumed by running this again.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$HOME/champsim-build}"
TRACE_DIR="${TRACE_DIR:-$HOME/traces}"
OUT="${OUT:-$HOME/champsim-results}"
WARMUP="${WARMUP:-10000000}"
SIM="${SIM:-50000000}"
JOBS="${JOBS:-8}"
VARIANTS="${VARIANTS:-p1 p2 p2s}"
POLICIES="${POLICIES:-}"
WORKLOADS="${WORKLOADS:-}"

# Without a policy filter, take every policy that has a binary for any variant.
if [ -z "$POLICIES" ]; then
  POLICIES="$(find "$BUILD_DIR/bin" -maxdepth 1 -name 'champsim_*_*' -printf '%f\n' 2>/dev/null \
    | sed -E 's/^champsim_[^_]+_//' | sort -u | tr '\n' ' ')"
fi

in_filter() {
  [ -z "$2" ] && return 0
  [[ " $2 " == *" $1 "* ]]
}

run_one() {
  local variant="$1" policy="$2" workload="$3" trace="$4"
  local bin="$BUILD_DIR/bin/champsim_${variant}_${policy}"
  local log="$OUT/raw/$variant/$policy/$workload.log"
  local traces=("$trace")
  if [ "$variant" != "p1" ]; then
    traces+=("$trace")
  fi

  mkdir -p "$(dirname "$log")"
  if "$bin" --warmup-instructions "$WARMUP" --simulation-instructions "$SIM" "${traces[@]}" > "$log.tmp" 2>&1 \
      && grep -q "Simulation complete" "$log.tmp"; then
    mv "$log.tmp" "$log"
    echo "done   $variant/$policy/$workload"
  else
    echo "FAILED $variant/$policy/$workload (see $log.tmp)" >&2
    return 1
  fi
}
export -f run_one
export BUILD_DIR OUT WARMUP SIM

jobs_file="$(mktemp)"
trap 'rm -f "$jobs_file"' EXIT
skipped=0

while read -r name trace _; do
  [ -z "$name" ] || [[ "$name" == \#* ]] && continue
  in_filter "$name" "$WORKLOADS" || continue
  trace_path="$TRACE_DIR/$trace.champsimtrace.xz"
  if [ ! -f "$trace_path" ]; then
    echo "warning: missing trace $trace_path, skipping $name" >&2
    continue
  fi
  for variant in $VARIANTS; do
    for policy in $POLICIES; do
      if [ ! -x "$BUILD_DIR/bin/champsim_${variant}_${policy}" ]; then
        echo "warning: missing binary champsim_${variant}_${policy}, skipping $variant/$policy/$name" >&2
        continue
      fi
      log="$OUT/raw/$variant/$policy/$name.log"
      if [ -f "$log" ] && grep -q "Simulation complete" "$log"; then
        skipped=$((skipped + 1))
        continue
      fi
      printf '%s %s %s %s\n' "$variant" "$policy" "$name" "$trace_path" >> "$jobs_file"
    done
  done
done < "$HERE/workloads.txt"

total="$(wc -l < "$jobs_file")"
echo "Running $total jobs ($JOBS at a time, warmup $WARMUP, sim $SIM); $skipped already complete"

status=0
if [ "$total" -gt 0 ]; then
  xargs -P "$JOBS" -L 1 bash -c 'run_one "$@"' _ < "$jobs_file" || status=$?
fi

failed="$(find "$OUT/raw" -name '*.log.tmp' 2>/dev/null | wc -l)"
complete="$(find "$OUT/raw" -name '*.log' 2>/dev/null | wc -l)"
echo
echo "Summary: $total run, $skipped skipped as complete, $failed failed/partial (*.log.tmp), $complete logs in $OUT/raw"
exit "$status"
