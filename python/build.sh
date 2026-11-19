#!/bin/sh
# Builds the ddfast Python extension into python/ddfast/: libddfast.so (the solver), the module
# _ddfast.abi3.so on it (one file for CPython 3.12 and later, nanobind's stable-ABI build) and
# libdds.so (DDS's C interface on it; see below). Needs nanobind importable by
# $PYTHON (pip install nanobind) and the extern/dds submodule (DDS's PBN parser and par code
# are compiled in). Then: PYTHONPATH=python python3 -c "import ddfast"
#   PYTHON=python3   the Python whose headers and nanobind to use
#   ARCH=...         target flags (default -march=native, as scripts/build.sh)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PYTHON=${PYTHON:-python3}
NB=$($PYTHON -c "import nanobind, os; print(os.path.dirname(nanobind.__file__))")
PYINC=$($PYTHON -c "import sysconfig; print(sysconfig.get_paths()['include'])")
if [ "$(uname -s)" = Darwin ]; then
  # macOS (Apple silicon): the same three libraries as on Linux, as .dylib, with an exported
  # symbols list in place of the version scripts and @loader_path in place of $ORIGIN. Needs
  # Google Highway's headers (brew install highway; HWY_INC overrides).
  CXX=${CXX:-clang++}
  if [ -z "${ARCH:-}" ]; then
    ARCH="-mcpu=native"
    [ "$(sysctl -n hw.optional.arm.FEAT_CSSC 2>/dev/null)" = 1 ] && ARCH="-mcpu=native+cssc"
  fi
  HWY_INC=${HWY_INC:-$(brew --prefix 2>/dev/null)/include}
  [ -f "$HWY_INC/hwy/highway.h" ] || { echo "python/build.sh: Google Highway not found; brew install highway (or set HWY_INC)" >&2; exit 1; }
  OBJ=bin/py/obj
  LIB=python/ddfast
  mkdir -p $OBJ/core $OBJ/py $OBJ/capi
  rm -f $OBJ/core/*.o $OBJ/py/*.o $OBJ/capi/*.o
  BASE="-std=c++20 -O3 -DNDEBUG -fPIC $ARCH -I$HWY_INC"
  DDS="-Iextern/dds/library/src -Iextern/dds/library/src/api"
  PY="-I$PYINC -I$NB/include -DPy_LIMITED_API=0x030C0000"
  printf '%s\n' '__ZN6ddfast*' '__ZN13ddfast_compat*' > $OBJ/libddfast.exp
  printf '%s\n' _SetMaxThreads _SetResources _FreeMemory _SolveBoard _SolveBoardPBN _ErrorMessage > $OBJ/libdds.exp
  echo "python/build.sh: $CXX (macOS), nanobind $NB"
  # shellcheck disable=SC2086
  {
    for f in tables solver par_deal api play; do $CXX $BASE -Iinclude -Isrc -c src/$f.cpp -o $OBJ/core/$f.o & done
    $CXX $BASE $DDS -w -c extern/dds/library/src/pbn.cpp -o $OBJ/core/dds_pbn.o &
    $CXX $BASE $DDS -w -c extern/dds/library/src/par.cpp -o $OBJ/core/dds_par.o &
    $CXX $BASE $DDS -w -c extern/dds/library/src/dealer_par.cpp -o $OBJ/core/dds_dealer_par.o &
    $CXX $BASE $DDS -w -c extern/dds/library/src/utility/constants.cpp -o $OBJ/core/dds_constants.o &
    $CXX $BASE -Iinclude $DDS -DDDFAST_COMPAT_CALCDDTABLE -c python/src/dds_compat.cpp -o $OBJ/core/dds_compat.o &
    $CXX $BASE -fvisibility=hidden -Iinclude $DDS $PY -c python/src/module.cpp -o $OBJ/py/module.o &
    $CXX -std=c++17 -O3 -DNDEBUG -fPIC -fvisibility=hidden -fno-strict-aliasing -DNB_BUILD -DNB_COMPACT_ASSERTIONS \
      $PY -I$NB/ext/robin_map/include -c $NB/src/nb_combined.cpp -o $OBJ/py/nanobind.o &
    $CXX $BASE -Iinclude $DDS -c python/src/dds_capi.cpp -o $OBJ/capi/dds_capi.o &
    wait
  }
  # shellcheck disable=SC2086
  $CXX -dynamiclib -install_name @rpath/libddfast.dylib -Wl,-exported_symbols_list,$OBJ/libddfast.exp \
    -o $LIB/libddfast.dylib $OBJ/core/*.o -pthread
  # shellcheck disable=SC2086
  $CXX -shared -undefined dynamic_lookup -o $LIB/_ddfast.abi3.so $OBJ/py/*.o -L$LIB -lddfast \
    -Wl,-rpath,@loader_path -pthread
  # shellcheck disable=SC2086
  $CXX -dynamiclib -install_name @rpath/libdds.dylib -Wl,-exported_symbols_list,$OBJ/libdds.exp \
    -o $LIB/libdds.dylib $OBJ/capi/dds_capi.o -L$LIB -lddfast -Wl,-rpath,@loader_path -pthread
  echo "python/build.sh: $LIB/libddfast.dylib, $LIB/_ddfast.abi3.so, $LIB/libdds.dylib"
  exit 0
fi
if [ -z "${CXX:-}" ]; then
  CXX=g++
  if command -v clang++ >/dev/null; then
    CXX=clang++
    for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
      if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
    done
  fi
fi
ARCH=${ARCH:--march=native}
OBJ=bin/py/obj
mkdir -p "$OBJ"
BASE="-std=c++20 -O3 -DNDEBUG -fPIC -fvisibility=hidden $ARCH"
DDS="-Iextern/dds/library/src -Iextern/dds/library/src/api"
PY="-I$PYINC -I$NB/include -DPy_LIMITED_API=0x030C0000"
echo "python/build.sh: $CXX, nanobind $NB"
# Three libraries, all in python/ddfast/:
#   libddfast.so      the solver, the endgame tables, the thread pool and the DDS-compatible
#                     layer (ddfast_compat). Exports only ddfast:: and ddfast_compat:: (a version
#                     script keeps the DDS code compiled in - its PBN parser, par, CalcDDtable -
#                     private, so it cannot clash with another DDS in the process).
#   _ddfast.abi3.so   the Python module, linked against libddfast.so.
#   libdds.so         DDS's legacy C interface (SolveBoardPBN, SolveBoard, ErrorMessage,
#                     SetMaxThreads, ...) on libddfast.so, for programs that load DDS by name
#                     (python/src/dds_capi.cpp).
# The two last find libddfast.so beside themselves ($ORIGIN), and a process that loads both has
# one libddfast.so, so one copy of the tables. Linux only (macOS would need install names,
# @loader_path and an exported-symbols list in place of the version scripts).
LIB=python/ddfast
CORE="$BASE -fno-semantic-interposition"
CORE=$(echo "$CORE" | sed 's/-fvisibility=hidden//')
cat > $OBJ/libddfast.map <<'MAP'
{
  global:
    extern "C++" { ddfast::*; ddfast_compat::*; };
  local: *;
};
MAP
cat > $OBJ/libdds.map <<'MAP'
{
  global: SetMaxThreads; SetResources; FreeMemory; SolveBoard; SolveBoardPBN; ErrorMessage;
  local: *;
};
MAP
mkdir -p $OBJ/core $OBJ/py $OBJ/capi
rm -f $OBJ/*.o
# shellcheck disable=SC2086
{
  for f in tables solver par_deal api play; do $CXX $CORE -Iinclude -Isrc -c src/$f.cpp -o $OBJ/core/$f.o & done
  $CXX $CORE $DDS -c extern/dds/library/src/pbn.cpp -o $OBJ/core/dds_pbn.o &
  $CXX $CORE $DDS -c extern/dds/library/src/par.cpp -o $OBJ/core/dds_par.o &
  $CXX $CORE $DDS -c extern/dds/library/src/dealer_par.cpp -o $OBJ/core/dds_dealer_par.o &
  $CXX $CORE $DDS -c extern/dds/library/src/utility/constants.cpp -o $OBJ/core/dds_constants.o &
  $CXX $CORE -Iinclude $DDS -DDDFAST_COMPAT_CALCDDTABLE -c python/src/dds_compat.cpp -o $OBJ/core/dds_compat.o &
  $CXX $BASE -Iinclude $DDS $PY -c python/src/module.cpp -o $OBJ/py/module.o &
  $CXX -std=c++17 -O3 -DNDEBUG -fPIC -fvisibility=hidden -fno-strict-aliasing -DNB_BUILD -DNB_COMPACT_ASSERTIONS \
    $PY -I$NB/ext/robin_map/include -c $NB/src/nb_combined.cpp -o $OBJ/py/nanobind.o &
  $CXX $CORE -Iinclude $DDS -c python/src/dds_capi.cpp -o $OBJ/capi/dds_capi.o &
  wait
}
# shellcheck disable=SC2086
$CXX -shared -Wl,-soname,libddfast.so -Wl,--version-script=$OBJ/libddfast.map \
  -o $LIB/libddfast.so $OBJ/core/*.o -pthread
# shellcheck disable=SC2086
$CXX -shared -o $LIB/_ddfast.abi3.so $OBJ/py/*.o -L$LIB -lddfast -Wl,-rpath,'$ORIGIN' -pthread
# shellcheck disable=SC2086
$CXX -shared -Wl,-soname,libdds.so -Wl,--version-script=$OBJ/libdds.map \
  -o $LIB/libdds.so $OBJ/capi/dds_capi.o -L$LIB -lddfast -Wl,-rpath,'$ORIGIN' -pthread
echo "python/build.sh: $LIB/libddfast.so, $LIB/_ddfast.abi3.so, $LIB/libdds.so"
