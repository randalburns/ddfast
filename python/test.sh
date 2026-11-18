#!/bin/sh
# Tests of the Python interface: builds and runs the C++ comparison of the DDS-compatible layer
# with DDS itself (python/tests/compat_test.cpp: solve_board on random positions at every stage
# of play, error cases, tables, par text), then the Python binding tests. Needs scripts/build.sh
# run first (bin/libddfast.a, bin/libdds.a), the module built (python/build.sh) and the tables
# ($DDFAST_TABLES).
#   sh python/test.sh [positions] [seed]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PYTHON=${PYTHON:-python3}
if [ -z "${CXX:-}" ]; then
  CXX=g++
  if command -v clang++ >/dev/null; then
    CXX=clang++
    for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
      if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
    done
  fi
fi
mkdir -p bin/py
# shellcheck disable=SC2086
$CXX -std=c++20 -O2 -march=native -Iinclude -Ipython/src -Iextern/dds/library/src -Iextern/dds/library/src/api \
  python/src/dds_compat.cpp python/tests/compat_test.cpp bin/libddfast.a bin/libdds.a -pthread -o bin/py/compat_test
(cd bin/py && ./compat_test "${1:-2000}" "${2:-1}")  # in bin/py: DDS writes dump.txt for the error cases
PYTHONPATH=python $PYTHON python/tests/test_dds3.py
