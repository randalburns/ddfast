#!/bin/sh
# ddfast against ddclaim's standalone solver (3-7 cards a hand) and DDS (3-13) on the same
# random positions, one size at a time. Every ddfast answer is checked against DDS, and
# against ddclaim where ddclaim can solve the size.
#
# usage: sh bench/positions_vs_ddclaim.sh OUTDIR [DDCLAIM_REPO] [SEED]
#   DDCLAIM_REPO: a clone of github.com/randalburns/ddclaim (its main has the standalone
#   solver); default /home/randal/active/ddclaim. Run from ddfast's root, after build.sh,
#   with DDCLAIM_DIR set. ddclaim uses the 4- and 5-card pattern tables (p4_v5, p5_v5).
set -eu
O=${1:?usage: positions_vs_ddclaim.sh OUTDIR [DDCLAIM_REPO] [SEED]}
REPO=${2:-/home/randal/active/ddclaim}
SEED=${3:-21}
mkdir -p "$O"
O=$(cd "$O" && pwd)
CXX="clang++"
[ -d /usr/lib/gcc/x86_64-linux-gnu/13 ] && CXX="clang++ --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13"
if [ ! -x "$O/ddclaim_positions" ]; then
  [ -d "$O/ddclaim" ] || git -C "$REPO" worktree add -q --detach "$O/ddclaim" main
  F="-std=c++20 -O3 -DNDEBUG -march=native -w -I$O/ddclaim/include"
  $CXX $F -c "$O/ddclaim/src/ddclaim.cpp" -o "$O/ddclaim.o"
  $CXX $F -Ibench bench/ddclaim_positions.cpp "$O/ddclaim.o" -pthread -o "$O/ddclaim_positions"
fi
export DDCLAIM_LEVELS=2
for c in 3 4 5 6 7 8 9 10 11 12 13; do
  case $c in 3|4|5|6) n=20000 ;; 7) n=10000 ;; 8) n=5000 ;; 9) n=3000 ;; 10) n=2000 ;; 11) n=1000 ;; 12) n=500 ;; 13) n=300 ;; esac
  taskset -c 0 bin/bench_positions $c $n $SEED "$O/ddfast_$c.bin" dds
  if [ $c -le 7 ]; then
    taskset -c 0 "$O/ddclaim_positions" $c $n $SEED "$O/ddclaim_$c.bin"
    cmp -s "$O/ddfast_$c.bin" "$O/ddclaim_$c.bin" && echo "  ddfast = ddclaim on all $n" || echo "  ddfast and ddclaim DIFFER"
  fi
done
