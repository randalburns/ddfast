#!/bin/sh
# Links bprof against an existing ddfast build: OUT/bprof from OUT/libddfast.a and OUT/libdds.a.
#   sh tools/prof/build.sh [OUT]        (default bin)
# For source lines in sym.py, build the library with -g first (it does not change the code):
#   XFLAGS=-g OUT=bin-prof sh scripts/build.sh && sh tools/prof/build.sh bin-prof
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=${1:-bin}
[ -f "$OUT/libddfast.a" ] && [ -f "$OUT/libdds.a" ] || { echo "tools/prof/build.sh: build $OUT first (scripts/build.sh)" >&2; exit 1; }
if [ -z "${CXX:-}" ]; then
  CXX=clang++
  for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
    if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
  done
  command -v clang++ >/dev/null || CXX=g++
fi
# -no-pie: sampled addresses are then the binary's own, as addr2line expects.
# shellcheck disable=SC2086
$CXX -std=c++20 -O2 -g -march=native -no-pie -Iinclude -Iextern/ddclaim/include -Itests \
  -Iextern/dds/library/src -Iextern/dds/library/src/api \
  tools/prof/bprof.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/bprof"
echo "tools/prof/build.sh: $OUT/bprof"
