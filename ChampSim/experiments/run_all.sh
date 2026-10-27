#!/usr/bin/env bash
#
# Run every (variant, policy, workload) that has a built binary, in parallel.
#
# Cores per variant: p1=1, p2/p2s=2, p4/p4s=4 (the "s" variants share one address space).
# A workload whose trace column contains "{core}" is per-core (e.g. CloudSuite): core i runs the trace with {core}
# replaced by i, for i = 0..N-1. Otherwise (SPEC) the SAME trace runs on every core: in p2s/p4s that creates
# (artificial) sharing; p2/p4 are the identical load without sharing, as a control.
# Trace paths are $TRACE_DIR/<trace>.champsimtrace.xz, or $TRACE_DIR/<trace> if the column already ends in .xz.
# The optional 4th column of workloads.txt is the trace format: champsim (default) or cloudsuite (passes -c).
#
# Usage, from a WSL shell in the ChampSim directory:
#   bash experiments/run_all.sh
#   VARIANTS="p1" POLICIES="lru srrip" WORKLOADS="mcf lbm" JOBS=4 bash experiments/run_all.sh
#   VARIANTS="p4 p4s" WORKLOADS="cassandra nutch" BUILD_DIR=~/champsim-cs bash experiments/run_all.sh
#
# Environment (defaults):
#   BUILD_DIR=~/champsim-build  TRACE_DIR=~/traces  OUT=~/champsim-results
#   WARMUP=10000000  SIM=50000000  JOBS=8
#   VARIANTS="p1 p2 p2s" (add p4 p4s explicitly); POLICIES, WORKLOADS   optional space-separated filters
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

num_cores() {
  case "$1" in
    p1) echo 1 ;;
    p2|p2s) echo 2 ;;
    p4|p4s) echo 4 ;;
    *) echo "unknown variant $1" >&2; return 1 ;;
  esac
}

# Path of the trace that core $2 runs for trace column $1.
trace_path() {
  local trace="${1//\{core\}/$2}"
  case "$trace" in
    *.xz) echo "$TRACE_DIR/$trace" ;;
    *) echo "$TRACE_DIR/$trace.champsimtrace.xz" ;;
  esac
}

# Job line: variant policy workload format trace_for_core0 [trace_for_core1 ...]
run_one() {
  local variant="$1" policy="$2" workload="$3" format="$4"
  shift 4
  local traces=("$@")
  local bin="$BUILD_DIR/bin/champsim_${variant}_${policy}"
  local log="$OUT/raw/$variant/$policy/$workload.log"
  local flags=()
  if [ "$format" = "cloudsuite" ]; then
    flags+=(--cloudsuite)
  fi

  mkdir -p "$(dirname "$log")"
  if "$bin" "${flags[@]}" --warmup-instructions "$WARMUP" --simulation-instructions "$SIM" "${traces[@]}" \
      > "$log.tmp" 2>&1 && grep -q "Simulation complete" "$log.tmp"; then
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

while read -r name trace _behavior format _; do
  [ -z "$name" ] || [[ "$name" == \#* ]] && continue
  in_filter "$name" "$WORKLOADS" || continue
  format="${format:-champsim}"
  if [ "$format" != "champsim" ] && [ "$format" != "cloudsuite" ]; then
    echo "warning: unknown trace format '$format' for $name, skipping" >&2
    continue
  fi
  for variant in $VARIANTS; do
    cores="$(num_cores "$variant")"
    paths=()
    missing=""
    for ((core = 0; core < cores; core++)); do
      path="$(trace_path "$trace" "$core")"
      [ -f "$path" ] || missing="$path"
      paths+=("$path")
    done
    if [ -n "$missing" ]; then
      echo "warning: missing trace $missing, skipping $variant/$name" >&2
      continue
    fi
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
      printf '%s %s %s %s %s\n' "$variant" "$policy" "$name" "$format" "${paths[*]}" >> "$jobs_file"
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
