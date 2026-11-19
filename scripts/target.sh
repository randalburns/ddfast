# Sourced by scripts/build.sh and python/build.sh, with ROOT and CXX set: sets ARCH, the target
# flags for every ddfast source. ARCH from the environment chooses the target (default: the
# build machine's own CPU); this adds what the target needs:
#   - Google Highway's headers when the target lacks AVX-512's Ice Lake set (any CPU other than
#     Zen 4/5 and Intel Ice Lake and later, or FORCE_HWY=1): $HWY_INC, else build/highway,
#     Homebrew's prefix, /usr/local/include, /usr/include; checked to be recent enough;
#   - -DDDPORT_SOFT_BITS on AMD Zen 1 and 2 (-march=native only), whose pext/pdep are slow.
# CI sets ARCH (for example -march=icelake-server) to build the AVX-512 path on a runner
# without AVX-512 and run it under Intel SDE.
if [ -z "${ARCH:-}" ]; then case "$(uname -m)" in
  arm64|aarch64)
    ARCH="-mcpu=native"
    # Apple clang may know the CPU only as an older one (an M5 as apple-m3); add the scalar
    # popcount and count-trailing-zeros instructions (CSSC) when the CPU has them.
    [ "$(sysctl -n hw.optional.arm.FEAT_CSSC 2>/dev/null)" = 1 ] && ARCH="-mcpu=native+cssc" ;;
  *) ARCH="-march=native" ;;
esac; fi
# shellcheck disable=SC2086
TARGET_MACROS=$($CXX $ARCH -dM -E -x c++ /dev/null 2>/dev/null || true)
NEED_HWY=1
case "$(uname -m)" in x86_64|amd64)
  NEED_HWY=0
  for m in __AVX512F__ __AVX512VL__ __AVX512BW__ __AVX512DQ__ __AVX512VBMI2__ __AVX512VPOPCNTDQ__ __AVX512BITALG__ __GFNI__; do
    echo "$TARGET_MACROS" | grep -q "define $m " || NEED_HWY=1
  done
  [ "${FORCE_HWY:-0}" = 1 ] && { NEED_HWY=1; ARCH="$ARCH -DDDPORT_FORCE_HWY"; }
  # AMD Zen 1 and 2 (family 23) run pext/pdep as slow microcode: use the table version.
  if [ "$ARCH" = "-march=native" ] && grep -q '^vendor_id.*AuthenticAMD' /proc/cpuinfo 2>/dev/null &&
     grep -q '^cpu family[[:space:]]*: 23$' /proc/cpuinfo; then
    ARCH="$ARCH -DDDPORT_SOFT_BITS"
  fi ;;
esac
if [ -z "${HWY_INC:-}" ]; then
  for d in "$ROOT/build/highway" "$(brew --prefix 2>/dev/null)/include" /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "$d/hwy/highway.h" ] && { HWY_INC=$d; break; }
  done
fi
if [ "$NEED_HWY" = 1 ]; then
  HWY_GET="git clone --depth 1 --branch 1.4.0 https://github.com/google/highway build/highway"
  if [ -z "${HWY_INC:-}" ] || [ ! -f "$HWY_INC/hwy/highway.h" ]; then
    echo "ddfast: this target has no AVX-512 (Ice Lake set), so ddfast needs Google Highway's headers" >&2
    echo "  (a recent release, tested with 1.4.0): brew install highway, or $HWY_GET, or set HWY_INC" >&2
    exit 1
  fi
  # Older Highway releases (Ubuntu 24.04's libhwy-dev is 1.0.7) lack operations ddfast uses.
  # shellcheck disable=SC2086
  if ! printf '#include "port.hpp"\nunsigned f(hwy::HWY_NAMESPACE::Full128<uint8_t> d) { return unsigned(hwy::HWY_NAMESPACE::BitsFromMask(d, hwy::HWY_NAMESPACE::FirstN(d, 3))) + hwy::HWY_NAMESPACE::ReduceMax(d, hwy::HWY_NAMESPACE::Zero(d)); }\n' |
      $CXX -std=c++20 $ARCH -I"$HWY_INC" -Isrc -fsyntax-only -x c++ - 2>/dev/null; then
    echo "ddfast: the Highway headers in $HWY_INC are too old (ddfast is tested with 1.4.0):" >&2
    echo "  $HWY_GET   (looked at first), or set HWY_INC" >&2
    exit 1
  fi
  [ "$HWY_INC" = /usr/include ] || ARCH="$ARCH -I$HWY_INC"
fi
