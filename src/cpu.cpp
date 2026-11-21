// The run-time CPU check: does this CPU have every instruction set the build was compiled for
// (its -march flags)? Each such set is checked with __builtin_cpu_supports. The check runs as
// the first static constructor (priority 101, before any code that might use those sets): on a
// CPU that lacks one, every program linked with ddfast stops at load with a message naming
// what is missing (status 1) instead of failing on an illegal instruction later. ddfast::init()
// checks again. tests/cpu_check.sh tests it by compiling this file for sets the CPU lacks.
#include "port.hpp"

#include <cstddef>
#include <string>

#include <unistd.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

namespace ddport {
// Compiled for the baseline x86-64, so that it runs on any x86-64 CPU. The sets are also turned
// off by name: clang keeps a set given as its own flag (-mavx512f) through arch=, and would
// then clear cpu_check's buffers with AVX-512 stores.
#if DDPORT_X86
#define DDPORT_BASELINE \
  __attribute__((target("arch=x86-64,no-sse3,no-avx,no-popcnt,no-lzcnt,no-bmi,no-bmi2,no-movbe")))
#else
#define DDPORT_BASELINE
#endif
#if DDPORT_X86
// Appends z to the string in buf (of size n). Plain loops: GCC cannot inline the fortified
// strncat or snprintf into a baseline-target function.
DDPORT_BASELINE static void append(char* buf, size_t n, const char* z) {
  size_t i = 0;
  while (buf[i]) i++;
  while (*z && i + 1 < n) buf[i++] = *z++;
  buf[i] = 0;
}
#endif
#if DDPORT_X86
// A CPUID feature bit, for the sets clang's __builtin_cpu_supports does not name. reg: 2 ecx,
// 3 edx.
DDPORT_BASELINE static bool cpuid_bit(unsigned leaf, int reg, int bit) {
  unsigned r[4] = {};
  if (!__get_cpuid(leaf, &r[0], &r[1], &r[2], &r[3])) return false;
  return (r[reg] >> bit) & 1;
}
#endif
DDPORT_BASELINE const char* cpu_check() {
#if DDPORT_X86
  static char msg[512];
  static int state = 0;  // 0 not checked, 1 fine, 2 missing (msg filled)
  if (state) return state == 1 ? nullptr : msg;
  __builtin_cpu_init();
  char miss[256] = "";  // no std::string or lambdas here: they would not be baseline code
#define DDPORT_NEED(feature, name) \
  if (!__builtin_cpu_supports(feature)) append(miss, sizeof miss, *miss ? ", " name : name)
#ifdef __SSSE3__
  DDPORT_NEED("ssse3", "SSSE3");
#endif
#ifdef __SSE4_1__
  DDPORT_NEED("sse4.1", "SSE4.1");
#endif
#ifdef __SSE4_2__
  DDPORT_NEED("sse4.2", "SSE4.2");
#endif
#ifdef __POPCNT__
  DDPORT_NEED("popcnt", "POPCNT");
#endif
#ifdef __AVX__
  DDPORT_NEED("avx", "AVX");
#endif
#ifdef __AVX2__
  DDPORT_NEED("avx2", "AVX2");
#endif
#ifdef __FMA__
  DDPORT_NEED("fma", "FMA");
#endif
#ifdef __F16C__
  if (!cpuid_bit(1, 2, 29)) append(miss, sizeof miss, *miss ? ", F16C" : "F16C");
#endif
#ifdef __MOVBE__
  if (!cpuid_bit(1, 2, 22)) append(miss, sizeof miss, *miss ? ", MOVBE" : "MOVBE");
#endif
#ifdef __LZCNT__
  if (!cpuid_bit(0x80000001u, 2, 5)) append(miss, sizeof miss, *miss ? ", LZCNT" : "LZCNT");
#endif
#ifdef __BMI__
  DDPORT_NEED("bmi", "BMI1");
#endif
#ifdef __BMI2__
  DDPORT_NEED("bmi2", "BMI2");
#endif
#ifdef __AVX512F__
  DDPORT_NEED("avx512f", "AVX512F");
#endif
#ifdef __AVX512VL__
  DDPORT_NEED("avx512vl", "AVX512VL");
#endif
#ifdef __AVX512BW__
  DDPORT_NEED("avx512bw", "AVX512BW");
#endif
#ifdef __AVX512DQ__
  DDPORT_NEED("avx512dq", "AVX512DQ");
#endif
#ifdef __AVX512CD__
  DDPORT_NEED("avx512cd", "AVX512CD");
#endif
#ifdef __AVX512VBMI__
  DDPORT_NEED("avx512vbmi", "AVX512VBMI");
#endif
#ifdef __AVX512VBMI2__
  DDPORT_NEED("avx512vbmi2", "AVX512VBMI2");
#endif
#ifdef __AVX512VPOPCNTDQ__
  DDPORT_NEED("avx512vpopcntdq", "AVX512VPOPCNTDQ");
#endif
#ifdef __AVX512BITALG__
  DDPORT_NEED("avx512bitalg", "AVX512BITALG");
#endif
#ifdef __GFNI__
  DDPORT_NEED("gfni", "GFNI");
#endif
#ifdef __AVX512FP16__
  DDPORT_NEED("avx512fp16", "AVX512FP16");
#endif
#undef DDPORT_NEED
  if (!*miss) {
    state = 1;
    return nullptr;
  }
  append(msg, sizeof msg, "ddfast: this build uses instructions this CPU lacks (");
  append(msg, sizeof msg, miss);
  append(msg, sizeof msg,
         "). Build on this machine (scripts/build.sh uses -march=native), or set ARCH, e.g. "
         "ARCH=-march=x86-64-v3 for any x86-64 CPU with AVX2 and BMI2, ARCH=-march=x86-64-v2 "
         "for older ones.");
  state = 2;
  return msg;
#else
  return nullptr;
#endif
}

const char* build_paths() {
  static std::string s;
  if (s.empty()) {
#if DDPORT_AVX512
    s = "AVX-512";
#else
    s = std::string("Highway (") + hwy::TargetName(HWY_TARGET) + ")";
#endif
    s += DDPORT_PEXT ? ", BMI2 pext/pdep" : ", table pext/pdep";
  }
  return s.c_str();
}

namespace {
DDPORT_BASELINE __attribute__((constructor(101))) void cpu_check_at_load() {
  if (const char* m = cpu_check()) {
    size_t n = 0;
    while (m[n]) n++;
    if (write(2, m, n) >= 0 && write(2, "\n", 1) >= 0) {}
    _exit(1);
  }
}
}  // namespace
}  // namespace ddport
