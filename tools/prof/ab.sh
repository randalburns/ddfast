#!/bin/sh
# Alternating A/B benchmark of two ddfast builds on one core.
#   sh tools/prof/ab.sh A B [pairs] [set]
# A and B are build directories (each with bench_solve; bprof too for branch-miss counts).
# Runs A/bench_solve and B/bench_solve in turn `pairs` times (default 5) on CPU $CPU (default 0;
# keep its SMT sibling idle) on the set (default tests/data/public.set), then prints each
# build's median, min and max cycles/deal, and, where OUT/bprof exists, its exact branch-miss
# counts per deal (nearly noise-free, so good for screening). Needs DDFAST_TABLES.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
A=$1 B=$2 P=${3:-5} SET=${4:-tests/data/public.set} CPU=${CPU:-0}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
i=0
while [ $i -lt "$P" ]; do
  for X in "$A" "$B"; do
    o=$(taskset -c "$CPU" "$X/bench_solve" set "$SET" 2>&1)
    echo "$o" | awk '/^cycles\/deal/{print $2, $4}' >> "$T/$(echo "$X" | tr / _)"
    echo "$o" | grep -q "all correct" || echo "ab.sh: $X WRONG or failed" >&2
  done
  i=$((i + 1))
done
for X in "$A" "$B"; do
  sort -n "$T/$(echo "$X" | tr / _)" | awk -v x="$X" '{c[NR]=$1; ins=$2}
    END {printf "%-16s cycles/deal median %.0f  min %.0f  max %.0f  (n=%d)  instructions/deal %.0f\n", x, c[int((NR+1)/2)], c[1], c[NR], NR, ins}'
  if [ -x "$X/bprof" ]; then taskset -c "$CPU" "$X/bprof" "$SET" none 2>&1 | grep "per deal" | sed "s|^|$X  |"; fi
done
