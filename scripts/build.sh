#!/bin/sh
# Builds ddfast. Run from anywhere; everything goes to bin/ (or $OUT):
#   bin/libddfast.a     the library: ddfast and ddclaim (link it with -pthread)
#   bin/ddfast          the command-line tool (tools/ddfast_cli.cpp)
#   bin/test_ddfast     the exactness tests against DDS (tests/)
#   bin/make_ref         writes a test set with DDS's answers (tests/make_ref.cpp)
#   bin/bench_solve      single-thread and parallel benchmark (bench/bench_solve.cpp)
#   bin/bench_tables     full-table benchmark against DDS CalcDDtable (bench/bench_tables.cpp)
#   bin/libdds.a         DDS (extern/dds), for the tests and benchmarks only
#
# Needs an x86-64 CPU with AVX-512 and BMI2 (-march=native) on Linux, or Apple silicon on
# macOS (-mcpu=native; NEON in place of AVX-512, see extern/ddclaim/include/ddclaim/port.hpp),
# and the submodules
# (git clone --recursive, or git submodule update --init). Uses clang++ if present, else g++;
# set CXX to choose. Environment:
#   XFLAGS="..."  more flags for ddfast's own sources, e.g. -DSTATS (prints search counters,
#                 counts nodes), -DVERIFY_ORDER -DVERIFY_BOUNDS (checks the bounds and move
#                 ordering against the DDS port on every call; aborts on any difference),
#                 -DSELFTEST=10 (re-solves every 10th lead node of 4 to SELFTEST_MAX cards,
#                 default 13, with the cards outside its relevant set re-dealt; aborts if
#                 the value changes).
#                 Use a separate OUT for such builds.
#   OUT=dir       output directory (default bin)
#   LIBONLY=1     build only the library and the tool (no DDS, tests or benchmarks)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${OUT:-bin}
mkdir -p "$OUT/obj" "$OUT/dds"
if [ ! -f extern/ddclaim/src/ddclaim.cpp ] || [ ! -d extern/dds/library/src ]; then
  echo "build.sh: submodules missing; run: git submodule update --init" >&2
  exit 1
fi
if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then
    CXX="clang++"
    # Some machines' clang picks a GCC without C++ headers unless told otherwise.
    for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
      if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then
        CXX="clang++ --gcc-install-dir=$d"
        break
      fi
    done
  else
    CXX="g++"
  fi
fi
XFLAGS=${XFLAGS:-}
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
case "$(uname -m)" in
  arm64|aarch64)
    ARCH="-mcpu=native"
    # Apple clang may know the CPU only as an older one (an M5 as apple-m3); add the scalar
    # popcount and count-trailing-zeros instructions (CSSC) when the CPU has them.
    [ "$(sysctl -n hw.optional.arm.FEAT_CSSC 2>/dev/null)" = 1 ] && ARCH="-mcpu=native+cssc" ;;
  *) ARCH="-march=native" ;;
esac
BASE="-std=c++20 -O3 -DNDEBUG $ARCH"
INC="-Iinclude -Iextern/ddclaim/include"
DDSINC="-Iextern/dds/library/src -Iextern/dds/library/src/api"
echo "build.sh: $CXX, output $OUT${XFLAGS:+, XFLAGS=$XFLAGS}"

# The library: ddclaim (the tables, compiled without XFLAGS) and ddfast.
rm -f "$OUT/obj/ddclaim.o" "$OUT/obj/solver.o" "$OUT/obj/par_deal.o" "$OUT/obj/api.o" "$OUT/obj/play.o"
# shellcheck disable=SC2086
{
  $CXX $BASE -Iextern/ddclaim/include -c extern/ddclaim/src/ddclaim.cpp -o "$OUT/obj/ddclaim.o" &
  $CXX $BASE $XFLAGS $INC -c src/solver.cpp -o "$OUT/obj/solver.o" &
  $CXX $BASE $XFLAGS $INC -Isrc -c src/par_deal.cpp -o "$OUT/obj/par_deal.o" &
  $CXX $BASE $XFLAGS $INC -c src/api.cpp -o "$OUT/obj/api.o" &
  $CXX $BASE $XFLAGS $INC -c src/play.cpp -o "$OUT/obj/play.o" &
  wait
}
[ -f "$OUT/obj/ddclaim.o" ] && [ -f "$OUT/obj/solver.o" ] && [ -f "$OUT/obj/par_deal.o" ] && [ -f "$OUT/obj/api.o" ] && [ -f "$OUT/obj/play.o" ]
rm -f "$OUT/libddfast.a"
ar rcs "$OUT/libddfast.a" "$OUT/obj/solver.o" "$OUT/obj/par_deal.o" "$OUT/obj/api.o" "$OUT/obj/play.o" "$OUT/obj/ddclaim.o"
# shellcheck disable=SC2086
$CXX $BASE $INC tools/ddfast_cli.cpp "$OUT/libddfast.a" -pthread -o "$OUT/ddfast"
[ "${LIBONLY:-0}" = 1 ] && { echo "build.sh: done (library and tool)"; exit 0; }

# DDS, for reference answers. Built once (an object is rebuilt when its source is newer).
find extern/dds/library/src -name '*.cpp' > "$OUT/dds/sources"
# shellcheck disable=SC2016
xargs -P "$JOBS" -I{} sh -c 'o="$1/$(echo "$2" | tr / _).o"; if [ ! -f "$o" ] || [ "$2" -nt "$o" ]; then $0 $3 -c "$2" -o "$o" || exit 255; fi' \
  "$CXX" "$OUT/dds" {} "$BASE -w $DDSINC" < "$OUT/dds/sources"
rm -f "$OUT/libdds.a"
ar rcs "$OUT/libdds.a" "$OUT"/dds/*.o

# Tests and benchmarks.
for b in test_ddfast test_play make_ref bench_solve bench_tables bench_positions; do rm -f "$OUT/$b"; done
# shellcheck disable=SC2086
{
  $CXX $BASE $INC $DDSINC -Itests tests/test_ddfast.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/test_ddfast" &
  $CXX $BASE $INC $DDSINC tests/test_play.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/test_play" &
  $CXX $BASE $INC $DDSINC -Itests tests/make_ref.cpp "$OUT/libdds.a" -pthread -o "$OUT/make_ref" &
  $CXX $BASE $INC $DDSINC -Itests bench/bench_solve.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/bench_solve" &
  $CXX $BASE $INC $DDSINC -Itests bench/bench_tables.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/bench_tables" &
  $CXX $BASE $INC $DDSINC bench/bench_positions.cpp "$OUT/libddfast.a" "$OUT/libdds.a" -pthread -o "$OUT/bench_positions" &
  wait
}
for b in test_ddfast test_play make_ref bench_solve bench_tables bench_positions; do [ -x "$OUT/$b" ] || { echo "build.sh: $b failed" >&2; exit 1; }; done
echo "build.sh: done"
