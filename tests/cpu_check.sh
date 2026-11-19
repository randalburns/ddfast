#!/bin/sh
# System test of the run-time CPU check (src/cpu.cpp): every ddfast program must stop at load,
# with a message naming the missing instruction set and status 1, when the CPU lacks a set the
# build was compiled for; and must run when it has them.
#   sh tests/cpu_check.sh          (after scripts/build.sh; ARCH, CXX, HWY_INC as for it)
# 1. The check compiled for this build's target passes.
# 2. For each instruction set ddfast checks that this CPU lacks (from /proc/cpuinfo), the check
#    compiled with it added must stop the program with that set named, status 1. A CPU that has
#    them all (a recent Xeon) skips these; CI also runs bin/ddfast under Intel SDE as an older CPU.
# 3. If bin/ddfast exists: it passes too (bin/ddfast --cpu).
# Only the small src/cpu.cpp is compiled per case, so this takes seconds. x86-64 Linux only.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
case "$(uname -m)" in x86_64|amd64) ;; *) echo "cpu_check: x86-64 only; skipped"; exit 0 ;; esac
if [ -z "${CXX:-}" ]; then
  CXX=g++
  if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
    for d in /usr/lib/gcc/x86_64-linux-gnu/14 /usr/lib/gcc/x86_64-linux-gnu/13; do
      if [ -d "$d" ] && [ -d "/usr/include/c++/$(basename "$d")" ]; then CXX="clang++ --gcc-install-dir=$d"; break; fi
    done
  fi
fi
. "$ROOT/scripts/target.sh"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/main.cpp" <<'EOF'
#include <cstdio>
#include "port.hpp"
int main() {
  const char* m = ddport::cpu_check();
  printf("%s\n", m ? m : "ok");
  return m ? 2 : 0;  // 1 would mean the load-time check stopped the program, as it should
}
EOF
fail=0
# build_and_run NAME FLAGS: compiles src/cpu.cpp and main with $ARCH FLAGS, runs it; sets
# st (status) and out (output).
build_and_run() {
  # shellcheck disable=SC2086
  if ! $CXX -std=c++20 -O2 $ARCH $2 -Isrc "$T/main.cpp" src/cpu.cpp -o "$T/$1" 2> "$T/$1.err"; then
    echo "cpu_check: $1: does not compile:"; head -5 "$T/$1.err"; fail=1; st=-1; out=""; return
  fi
  set +e
  out=$("$T/$1" 2>&1)
  st=$?
  set -e
}

build_and_run target ""
if [ "$st" = 0 ] && [ "$out" = ok ]; then echo "cpu_check: this build's target ($ARCH): runs"
else echo "cpu_check: FAIL: this build's target ($ARCH): status $st: $out"; fail=1; fi

flags=$(grep -m1 '^flags' /proc/cpuinfo)
tested=0
# -m flag | /proc/cpuinfo flag | the name in ddfast's message
for c in "-mavx2|avx2|AVX2" "-mbmi2|bmi2|BMI2" "-mavx512f|avx512f|AVX512F" "-mavx512vbmi2|avx512_vbmi2|AVX512VBMI2" \
         "-mavx512vpopcntdq|avx512_vpopcntdq|AVX512VPOPCNTDQ" "-mgfni|gfni|GFNI" "-mavx512fp16|avx512_fp16|AVX512FP16"; do
  mflag=${c%%|*}; rest=${c#*|}; cpuflag=${rest%%|*}; name=${rest#*|}
  case " $flags " in *" $cpuflag "*) continue ;; esac
  tested=$((tested + 1))
  build_and_run "no_$cpuflag" "$mflag"
  if [ "$st" = 1 ] && echo "$out" | grep -q "this CPU lacks (.*$name"; then
    echo "cpu_check: built with $mflag, which this CPU lacks: stops at load naming $name (status 1)"
  else
    echo "cpu_check: FAIL: built with $mflag (this CPU lacks $cpuflag): status $st: $out"; fail=1
  fi
done
[ "$tested" = 0 ] && echo "cpu_check: this CPU has every instruction set ddfast checks: missing-set cases skipped"

if [ -x bin/ddfast ]; then
  if bin/ddfast --cpu > "$T/cpu.out" 2>&1; then echo "cpu_check: bin/ddfast --cpu: $(head -1 "$T/cpu.out")"
  else echo "cpu_check: FAIL: bin/ddfast --cpu:"; cat "$T/cpu.out"; fail=1; fi
fi
[ "$fail" = 0 ] && echo "cpu_check: all passed"
exit "$fail"
