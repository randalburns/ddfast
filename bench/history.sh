#!/bin/sh
# Measures ddfast at past commits with one program (bench/bench_history.cpp, ddfast's public
# API only): for each NAME:COMMIT, a worktree under build/history with its submodules (taken
# from this checkout's copies), its library built for this machine (LIBONLY, -march=native,
# so the AVX-512 code on an AVX-512 CPU), and bench_history linked to it; then ROUNDS rounds
# over all points, pinned to CPU (default 0). Use the minimum of the rounds per point.
#   sh bench/history.sh [ROUNDS] NAME:COMMIT ...   (N problems and tables, default 300; SEED, default 7)
# THREADS (macOS only; default 1): the same work as parallel batches on that many threads, with
# problems and tables per second. REPS: passes per run (default 3).
# The table files come from $DDFAST_TABLES through a directory of links, so a commit that
# expects a file the directory lacks builds it there, not in $DDFAST_TABLES. Old commits
# need ddclaim's checkout at $DDCLAIM_REPO (default ../ddclaim) for their extern/ddclaim.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=$ROOT/build/history
ROUNDS=$1
shift
CPU=${CPU:-0}
mkdir -p "$H/tables"
for f in "${DDFAST_TABLES:?set DDFAST_TABLES to the table directory}"/*; do ln -sf "$f" "$H/tables/"; done
CXX=${CXX:-clang++}
# AArch64 (macOS, Linux Arm): -mcpu=native. macOS cannot pin a thread: bench_history asks for a
# performance core instead.
case "$(uname -m)" in x86_64|amd64) NATIVE=-march=native ;; *) NATIVE=-mcpu=native ;; esac
PIN=""
command -v taskset >/dev/null 2>&1 && PIN="taskset -c $CPU"
for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
  if [ "$CXX" = clang++ ] && [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
done
for p in "$@"; do
  name=${p%%:*} commit=${p#*:} w=$H/wt-${p%%:*}
  [ -d "$w" ] || git -C "$ROOT" worktree add -q --detach "$w" "$commit"
  (cd "$w"
   git -c protocol.file.allow=always submodule update -q --init --reference "$ROOT/.git/modules/extern/dds" extern/dds
   if git ls-tree HEAD extern/ddclaim | grep -q commit; then
     git -c protocol.file.allow=always -c submodule.extern/ddclaim.url="${DDCLAIM_REPO:-$ROOT/../ddclaim}" submodule update -q --init extern/ddclaim
   fi
   LIBONLY=1 OUT=bin sh scripts/build.sh > "$H/build-$name.log" 2>&1
   # shellcheck disable=SC2086
   $CXX -std=c++20 -O2 $NATIVE -Iinclude "$ROOT/bench/bench_history.cpp" bin/libddfast.a -pthread -o "$H/bench-$name")
  echo "history.sh: built $name ($commit)"
done
r=1
while [ "$r" -le "$ROUNDS" ]; do
  for p in "$@"; do
    printf '%s %-8s ' "$r" "${p%%:*}"
    DDCLAIM_DIR=$H/tables DDFAST_TABLES=$H/tables $PIN "$H/bench-${p%%:*}" "${N:-300}" "${SEED:-7}" "${REPS:-3}" "${THREADS:-1}"
  done
  r=$((r + 1))
done
