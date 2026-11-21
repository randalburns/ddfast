#!/bin/sh
# Fetches the bridge-solver libraries that bin/bench_libdds compares ddfast with, both behind
# DDS's C API:
#   build/bridge-solver/build/libdds.so   BSalita/bridge-solver, branch faster-search (a fork of
#                                         macroxue/bridge-solver), at BSOLVER_REF, built here
#                                         with its own script (-march=native, PGO)
#   build/ben-bsolver/libbsolver-dds.so   the copy BEN (lorserker/ben) ships and solves on by
#                                         default, at BEN_REF (portable: x86-64-v3)
# Then, for example:
#   taskset -c 0 bin/bench_libdds build/ben-bsolver/libbsolver-dds.so tables 100
#   taskset -c 0-3,12-15 bin/bench_libdds build/ben-bsolver/libbsolver-dds.so boards 200 8 0-3,12-15
# Environment: BSOLVER_REF (default 61a6e59, the commit BEN 0.9.0.4 ships), BEN_REF (default
# main). Linux (x86-64 or Arm) and macOS (Apple silicon).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BSOLVER_REF=${BSOLVER_REF:-61a6e59}
BEN_REF=${BEN_REF:-main}
mkdir -p build

# bridge-solver, built here.
if [ ! -d build/bridge-solver/.git ]; then
  git clone -q https://github.com/BSalita/bridge-solver.git build/bridge-solver
fi
git -C build/bridge-solver fetch -q origin faster-search
git -C build/bridge-solver checkout -q "$BSOLVER_REF"
# Its build script takes CXX as one word; some machines' clang needs --gcc-install-dir (as
# scripts/build.sh finds it), so pass that through a wrapper.
CXXW=""
if [ -z "${CXX:-}" ] && command -v clang++ >/dev/null 2>&1; then
  for d in /usr/lib/gcc/$(uname -m)-linux-gnu/14 /usr/lib/gcc/$(uname -m)-linux-gnu/13; do
    if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then
      CXXW="$ROOT/build/bridge-solver-cxx"
      printf '#!/bin/sh\nexec clang++ --gcc-install-dir=%s "$@"\n' "$d" > "$CXXW"
      chmod +x "$CXXW"
      break
    fi
  done
fi
if [ -n "$CXXW" ] && [ -z "${LLVM_PROFDATA:-}" ]; then
  LLVM_PROFDATA=$(command -v llvm-profdata || ls /usr/bin/llvm-profdata-* 2>/dev/null | sort -V | tail -1 || true)
  export LLVM_PROFDATA
fi
(cd build/bridge-solver && CXX=${CXXW:-${CXX:-}} bash build.sh) | tail -1
echo "get_bsolver.sh: build/bridge-solver/build/libdds.so ($(git -C build/bridge-solver rev-parse --short HEAD))"

# BEN's copy.
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) sub=linux-x64 name=libbsolver-dds.so ;;
  Linux-aarch64) sub=linux-arm64 name=libbsolver-dds.so ;;
  Darwin-arm64) sub=macos-arm64 name=libbsolver-dds.dylib ;;
  *) echo "get_bsolver.sh: BEN ships no bridge-solver for $(uname -s) $(uname -m)"; exit 0 ;;
esac
mkdir -p build/ben-bsolver
curl -fsSL -o "build/ben-bsolver/$name" "https://raw.githubusercontent.com/lorserker/ben/$BEN_REF/bin/bsolver/$sub/$name"
echo "get_bsolver.sh: build/ben-bsolver/$name (BEN $BEN_REF)"
