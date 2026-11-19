#!/bin/sh
# Builds the ddfast Python extension into python/ddfast/ (python/ddfast/_ddfast.abi3.so: one
# file for CPython 3.12 and later, nanobind's stable-ABI build). Needs nanobind importable by
# $PYTHON (pip install nanobind) and the extern/dds submodule (DDS's PBN parser and par code
# are compiled in). Then: PYTHONPATH=python python3 -c "import ddfast"
#   PYTHON=python3   the Python whose headers and nanobind to use
#   ARCH=...         target flags (default the build machine's CPU; see scripts/target.sh)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PYTHON=${PYTHON:-python3}
NB=$($PYTHON -c "import nanobind, os; print(os.path.dirname(nanobind.__file__))")
PYINC=$($PYTHON -c "import sysconfig; print(sysconfig.get_paths()['include'])")
if [ -z "${CXX:-}" ]; then
  CXX=g++
  if command -v clang++ >/dev/null; then
    CXX=clang++
    for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
      if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
    done
  fi
fi
. "$ROOT/scripts/target.sh"  # ARCH, with Highway when the target needs it
OBJ=bin/py/obj
mkdir -p "$OBJ"
BASE="-std=c++20 -O3 -DNDEBUG -fPIC -fvisibility=hidden $ARCH"
DDS="-Iextern/dds/library/src -Iextern/dds/library/src/api"
PY="-I$PYINC -I$NB/include -DPy_LIMITED_API=0x030C0000"
echo "python/build.sh: $CXX, nanobind $NB"
# shellcheck disable=SC2086
{
  for f in cpu tables solver par_deal api play; do $CXX $BASE -Iinclude -Isrc -c src/$f.cpp -o $OBJ/$f.o & done
  $CXX $BASE $DDS -c extern/dds/library/src/pbn.cpp -o $OBJ/dds_pbn.o &
  $CXX $BASE $DDS -c extern/dds/library/src/par.cpp -o $OBJ/dds_par.o &
  $CXX $BASE $DDS -c extern/dds/library/src/dealer_par.cpp -o $OBJ/dds_dealer_par.o &
  $CXX $BASE $DDS -c extern/dds/library/src/utility/constants.cpp -o $OBJ/dds_constants.o &
  $CXX $BASE -Iinclude $DDS -DDDFAST_COMPAT_CALCDDTABLE -c python/src/dds_compat.cpp -o $OBJ/dds_compat.o &
  $CXX $BASE -Iinclude $DDS $PY -c python/src/module.cpp -o $OBJ/module.o &
  $CXX -std=c++17 -O3 -DNDEBUG -fPIC -fvisibility=hidden -fno-strict-aliasing -DNB_BUILD -DNB_COMPACT_ASSERTIONS \
    $PY -I$NB/ext/robin_map/include -c $NB/src/nb_combined.cpp -o $OBJ/nanobind.o &
  wait
}
# shellcheck disable=SC2086
$CXX -shared -o python/ddfast/_ddfast.abi3.so $OBJ/*.o -pthread
echo "python/build.sh: python/ddfast/_ddfast.abi3.so"
