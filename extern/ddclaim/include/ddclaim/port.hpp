// Portability layer: the bit and vector operations ddclaim (and ddfast) use, for x86-64 (BMI2
// and AVX-512, the original instructions) and for AArch64 (Apple silicon: NEON, and table
// lookups or bit tricks in place of pext/pdep, which Arm lacks).
#pragma once

#include <bit>
#include <cstdint>

#if defined(__x86_64__)
#include <immintrin.h>
#define DDCLAIM_X86 1
#define DDCLAIM_ARM 0
#elif defined(__aarch64__)
#include <arm_neon.h>
#define DDCLAIM_X86 0
#define DDCLAIM_ARM 1
#else
#error "ddclaim needs x86-64 (AVX-512, BMI2) or AArch64"
#endif

namespace ddport {

typedef uint64_t u64;

inline int clz64(u64 x) { return std::countl_zero(x); }  // 64 for 0, like lzcnt
inline int ctz64(u64 x) { return std::countr_zero(x); }  // 64 for 0, like tzcnt
#ifdef ABL_NOPREFETCH  // leave-one-out: no software prefetch
inline void prefetch(const void*) {}
#else
inline void prefetch(const void* p) { __builtin_prefetch(p, 0, 3); }
#endif
// The pause in a spin-wait loop.
inline void cpu_relax() {
#if DDCLAIM_X86
  _mm_pause();
#else
  __builtin_arm_yield();
#endif
}
// A cheap, steadily increasing tick count (for ratios of time spent, not for wall time).
inline u64 ticks() {
#if DDCLAIM_X86
  return __rdtsc();
#else
  return __builtin_arm_rsr64("cntvct_el0");
#endif
}

// Leave-one-out switches (bench/loo.sh; the defaults are the normal build): ABL_SOFT_BITS
// replaces BMI2 pext/pdep on x86 by the byte-table code used on Arm; ABL_SCALAR_TT,
// ABL_SCALAR_PMATCH and ABL_SCALAR_PICK replace the AVX-512 match8, pmatch8 and best_key
// by scalar loops.
#if DDCLAIM_X86 && !defined(ABL_SOFT_BITS)

inline u64 pext64(u64 x, u64 m) { return _pext_u64(x, m); }
inline u64 pdep64(u64 x, u64 m) { return _pdep_u64(x, m); }
inline unsigned pext32(unsigned x, unsigned m) { return _pext_u32(x, m); }
inline unsigned pdep32(unsigned x, unsigned m) { return _pdep_u32(x, m); }
inline unsigned pext16(unsigned x, unsigned m) { return _pext_u32(x, m); }  // m < 2^16
// pext(x, 0x000F000F000F000F << 4k): nibble k of each 16-bit lane.
inline u64 lane_nibbles(u64 x, int k) { return _pext_u64(x, 0x000F000F000F000Full << (4 * k)); }

// A mask prepared for several pext/pdep calls (free on x86).
struct BitMask {
  u64 m;
  explicit BitMask(u64 mask) : m(mask) {}
  u64 ext(u64 x) const { return _pext_u64(x, m); }
  u64 dep(u64 x) const { return _pdep_u64(x, m); }
};

#else  // software pext/pdep: AArch64, or x86 with ABL_SOFT_BITS

// pext/pdep a byte at a time through 256 x 256 tables (64 KB each, so both stay in a
// performance core's 128 KB L1). The eight bytes are independent: each one's output offset
// is a prefix sum of the mask's byte popcounts, found with one NEON cnt and a multiply.
struct ByteTables {
  uint8_t ext[256][256];  // [mask][x] -> x's bits under mask, packed low
  uint8_t dep[256][256];  // [mask][v] -> the low bits of v spread over mask
  ByteTables() {
    for (unsigned m = 0; m < 256; m++)
      for (unsigned x = 0; x < 256; x++) {
        unsigned e = 0, d = 0;
        for (unsigned b = 0, k = 0; b < 8; b++)
          if (m >> b & 1) {
            e |= (x >> b & 1) << k;
            d |= (x >> k & 1) << b;
            k++;
          }
        ext[m][x] = uint8_t(e);
        dep[m][x] = uint8_t(d);
      }
  }
};
inline const ByteTables g_bytes;

// Byte k: the number of mask bits in bytes 0 .. k-1.
__attribute__((always_inline)) inline u64 byte_offsets(u64 m) {
#if DDCLAIM_ARM
  const u64 c = vget_lane_u64(vreinterpret_u64_u8(vcnt_u8(vcreate_u8(m))), 0);
#else  // the popcount of each byte, by halves (x86 with ABL_SOFT_BITS)
  u64 c = m - ((m >> 1) & 0x5555555555555555ull);
  c = (c & 0x3333333333333333ull) + ((c >> 2) & 0x3333333333333333ull);
  c = (c + (c >> 4)) & 0x0F0F0F0F0F0F0F0Full;
#endif
  return (c * 0x0101010101010101ull) << 8;
}

struct BitMask {
  u64 m, off;
  explicit BitMask(u64 mask) : m(mask), off(byte_offsets(mask)) {}
  __attribute__((always_inline)) u64 ext(u64 x) const {
    u64 r = 0;
#pragma clang loop unroll(full)
    for (int k = 0; k < 8; k++) {
      const unsigned mk = unsigned(m >> (8 * k)) & 0xFF;
      r |= u64(g_bytes.ext[mk][unsigned(x >> (8 * k)) & 0xFF]) << ((off >> (8 * k)) & 0xFF);
    }
    return r;
  }
  __attribute__((always_inline)) u64 dep(u64 x) const {
    u64 r = 0;
#pragma clang loop unroll(full)
    for (int k = 0; k < 8; k++) {
      const unsigned mk = unsigned(m >> (8 * k)) & 0xFF;
      const unsigned o = unsigned(off >> (8 * k)) & 0xFF;
      r |= u64(g_bytes.dep[mk][unsigned(o < 64 ? x >> o : 0) & 0xFF]) << (8 * k);
    }
    return r;
  }
};

__attribute__((always_inline)) inline u64 pext64(u64 x, u64 m) { return BitMask(m).ext(x); }
__attribute__((always_inline)) inline u64 pdep64(u64 x, u64 m) { return BitMask(m).dep(x); }
inline unsigned pext32(unsigned x, unsigned m) { return unsigned(pext64(x, m)); }
inline unsigned pdep32(unsigned x, unsigned m) { return unsigned(pdep64(x, m)); }
// m < 2^16: two bytes.
__attribute__((always_inline)) inline unsigned pext16(unsigned x, unsigned m) {
  const unsigned m0 = m & 0xFF, m1 = (m >> 8) & 0xFF;
  return unsigned(g_bytes.ext[m0][x & 0xFF]) | unsigned(g_bytes.ext[m1][(x >> 8) & 0xFF]) << std::popcount(m0);
}
inline u64 lane_nibbles(u64 x, int k) {
  x >>= 4 * k;
  return (x & 0xF) | (x >> 12 & 0xF0) | (x >> 24 & 0xF00) | (x >> 36 & 0xF000);
}

#endif

#if DDCLAIM_X86

// Bytes 0..7 of x reordered by ctl[0..7] (an index 0..7, or 0x80 for a zero byte). ctl is
// 16-byte aligned.
inline u64 shuf8(u64 x, const uint8_t* ctl) {
  return u64(_mm_cvtsi128_si64(
      _mm_shuffle_epi8(_mm_cvtsi64_si128((long long)x), _mm_load_si128((const __m128i*)ctl))));
}

// Bit i set when (mask[i] & own) == val[i], for i < 8. mask and val are 64-byte aligned.
inline unsigned match8(const u64* mask, const u64* val, u64 own) {
#ifdef ABL_SCALAR_TT
  unsigned k = 0;
  for (int i = 0; i < 8; i++) k |= unsigned((mask[i] & own) == val[i]) << i;
  return k;
#else
  return _mm512_cmpeq_epi64_mask(_mm512_and_si512(_mm512_load_si512(mask), _mm512_set1_epi64((long long)own)),
                                 _mm512_load_si512(val));
#endif
}

// Pattern-table group: bit i set when entry i matches (b[i] & own) == a[i] and its bounds
// (bits 56-58 low, 59-61 high) decide target tl.
inline unsigned pmatch8(const u64* a, const u64* b, u64 own, int tl) {
#ifdef ABL_SCALAR_PMATCH
  unsigned k = 0;
  for (int i = 0; i < 8; i++) {
    const u64 lo = (b[i] >> 56) & 7, hi = (b[i] >> 59) & 7;
    k |= unsigned((b[i] & own) == a[i] && (lo >= u64(tl) || hi < u64(tl))) << i;
  }
  return k;
#else
  const __m512i vo = _mm512_set1_epi64((long long)own), vt = _mm512_set1_epi64(tl);
  const __m512i vb = _mm512_load_si512(b);
  const unsigned k = _mm512_cmpeq_epi64_mask(_mm512_and_si512(vb, vo), _mm512_load_si512(a));
  const __m512i vlo = _mm512_and_si512(_mm512_srli_epi64(vb, 56), _mm512_set1_epi64(7));
  const __m512i vhi = _mm512_and_si512(_mm512_srli_epi64(vb, 59), _mm512_set1_epi64(7));
  return k & (_mm512_cmpge_epi64_mask(vlo, vt) | _mm512_cmplt_epi64_mask(vhi, vt));
#endif
}

// The highest key[i] among the slots i < 16 with cards[i] & cand nonzero (0 if none).
// cards is 64-byte aligned, key 32-byte aligned.
inline unsigned best_key(const u64* cards, const uint16_t* key, u64 cand) {
#ifdef ABL_SCALAR_PICK
  unsigned m = 0;
  for (int i = 0; i < 16; i++)
    if ((cards[i] & cand) && key[i] > m) m = key[i];
  return m;
#else
  const __mmask16 live = __mmask16(_mm512_test_epi64_mask(_mm512_load_si512(cards), _mm512_set1_epi64((long long)cand)) |
                                   _mm512_test_epi64_mask(_mm512_load_si512(cards + 8), _mm512_set1_epi64((long long)cand)) << 8);
  const __m256i kv = _mm256_maskz_mov_epi16(live, _mm256_load_si256((const __m256i*)key));
  const __m128i mx = _mm_max_epu16(_mm256_castsi256_si128(kv), _mm256_extracti128_si256(kv, 1));
  return 0xFFFF - unsigned(_mm_extract_epi16(_mm_minpos_epu16(_mm_xor_si128(mx, _mm_set1_epi16(-1))), 0));
#endif
}

#else  // DDCLAIM_ARM

inline u64 shuf8(u64 x, const uint8_t* ctl) {
  return vget_lane_u64(vreinterpret_u64_u8(vtbl1_u8(vcreate_u8(x), vld1_u8(ctl))), 0);
}

// NEON: two 64-bit lanes at a time; the masks come out as one bit per lane.
inline unsigned mask2(uint64x2_t v) {  // v lanes all-ones or zero
  return unsigned(vgetq_lane_u64(v, 0) & 1) | unsigned(vgetq_lane_u64(v, 1) & 2);
}
inline unsigned movemask8x64(uint64x2_t a, uint64x2_t b, uint64x2_t c, uint64x2_t d) {
  // Narrow 8 lanes of all-ones/zero to bytes and pick one bit per byte.
  const uint32x4_t ab = vcombine_u32(vmovn_u64(a), vmovn_u64(b));
  const uint32x4_t cd = vcombine_u32(vmovn_u64(c), vmovn_u64(d));
  const uint8x8_t n = vmovn_u16(vcombine_u16(vmovn_u32(ab), vmovn_u32(cd)));
  const uint8x8_t bitv = vcreate_u8(0x8040201008040201ull);
  return vaddv_u8(vand_u8(n, bitv));
}

inline unsigned match8(const u64* mask, const u64* val, u64 own) {
  const uint64x2_t vo = vdupq_n_u64(own);
  uint64x2_t r[4];
  for (int i = 0; i < 4; i++) r[i] = vceqq_u64(vandq_u64(vld1q_u64(mask + 2 * i), vo), vld1q_u64(val + 2 * i));
  return movemask8x64(r[0], r[1], r[2], r[3]);
}

inline unsigned pmatch8(const u64* a, const u64* b, u64 own, int tl) {
  const uint64x2_t vo = vdupq_n_u64(own), vt = vdupq_n_u64(u64(tl)), seven = vdupq_n_u64(7);
  uint64x2_t r[4];
  for (int i = 0; i < 4; i++) {
    const uint64x2_t vb = vld1q_u64(b + 2 * i);
    const uint64x2_t k = vceqq_u64(vandq_u64(vb, vo), vld1q_u64(a + 2 * i));
    const uint64x2_t lo = vandq_u64(vshrq_n_u64(vb, 56), seven), hi = vandq_u64(vshrq_n_u64(vb, 59), seven);
    r[i] = vandq_u64(k, vorrq_u64(vcgeq_u64(lo, vt), vcltq_u64(hi, vt)));
  }
  return movemask8x64(r[0], r[1], r[2], r[3]);
}

inline unsigned best_key(const u64* cards, const uint16_t* key, u64 cand) {
  const uint64x2_t vc = vdupq_n_u64(cand);
  uint16x8_t best = vdupq_n_u16(0);
  for (int h = 0; h < 16; h += 8) {
    // 0xFFFF in lane i where cards[h + i] & cand is nonzero.
    const uint32x4_t t0 = vcombine_u32(vmovn_u64(vtstq_u64(vld1q_u64(cards + h), vc)),
                                       vmovn_u64(vtstq_u64(vld1q_u64(cards + h + 2), vc)));
    const uint32x4_t t1 = vcombine_u32(vmovn_u64(vtstq_u64(vld1q_u64(cards + h + 4), vc)),
                                       vmovn_u64(vtstq_u64(vld1q_u64(cards + h + 6), vc)));
    const uint16x8_t live = vcombine_u16(vmovn_u32(t0), vmovn_u32(t1));
    best = vmaxq_u16(best, vandq_u16(live, vld1q_u16(key + h)));
  }
  return vmaxvq_u16(best);
}
#endif

// Hand-word run helpers. A hand word has suit s in bits 16s .. 16s+12 and a guard bit at
// 16s+15. `present` is the set of cards still in play; `mine` is a subset of it.
constexpr u64 GUARD_BITS = 0x8000800080008000ull;

#if DDCLAIM_X86
// One card per run of touching cards (the top one), and the heads with another of mine
// directly below in play (the run has more cards).
inline u64 run_heads(u64 mine, u64 present, u64* seq = nullptr) {
  const u64 p = present | GUARD_BITS;
  const u64 pm = pext64(mine, p), hc = pm & ~(pm >> 1);
  if (seq) *seq = pdep64(hc & (pm << 1), p);
  return pdep64(hc, p);
}
inline u64 run_bottom_of(u64 c, u64 rest, u64 present) {
  const u64 p = present | GUARD_BITS;
  const u64 pm = pext64(rest, p), bc = pext64(c, p);
  const u64 t = ~pm & (bc - 1);
  const u64 bb = 1ull << (64 - _lzcnt_u64(t));  // just above the highest gap below c, or bit 0
  return pdep64(bb, p);
}
#else
// The present cards whose next card in play below (same suit) is in `seeds` (a subset of the
// cards in play): a carry started just above each seed runs up through the cards no longer
// in play and stops at the next one in play (or the guard).
inline u64 next_up(u64 seeds, u64 present) {
  const u64 gaps = ~present & ~GUARD_BITS;
  const u64 x = seeds << 1;
  return (((x & gaps) + gaps) & ~gaps) | (x & ~gaps);
}
// A card heads its run when the next card in play above it (or the top of the suit) is not
// mine. With the bits reversed, "above" becomes "below": the same carry from every other
// card in play and from each suit's guard.
inline u64 run_heads(u64 mine, u64 present, u64* seq = nullptr) {
  const u64 gaps = __builtin_bitreverse64(~present & ~GUARD_BITS);
  const u64 seeds = __builtin_bitreverse64((present & ~mine) | GUARD_BITS) << 1;
  const u64 stop = (((seeds & gaps) + gaps) & ~gaps) | (seeds & ~gaps);
  const u64 h = __builtin_bitreverse64(__builtin_bitreverse64(mine) & stop);
  if (seq) *seq = h & next_up(mine, present);
  return h;
}
// The lowest card of the run of touching cards that `c` heads; `rest` is the hand without c.
// Below c in its suit, the run reaches down to just above the highest card in play that is
// not `rest`'s; its bottom is the lowest of `rest`'s cards in that span, or c itself.
inline u64 run_bottom_of(u64 c, u64 rest, u64 present) {
  const u64 below = (u64(0x1FFF) << (ctz64(c) & 48)) & (c - 1);
  const u64 block = present & ~rest & below;
  const u64 span = block ? below & ~((u64(2) << (63 - clz64(block))) - 1) : below;
  const u64 mine = rest & span;
  return mine ? (mine & (0 - mine)) : c;
}
#endif

}  // namespace ddport

// Stack switching for the coroutines (ddclaim's batch search, ddfast's NCTX > 1 build):
// DDPORT_DEFINE_CTX_SWITCH(name) defines
//   extern "C" void name(void** save_sp, void* load_sp);
// which saves the callee-saved registers on the current stack, stores the stack pointer in
// *save_sp, switches to load_sp and restores the registers saved there. ddport::init_stack
// prepares a new stack whose first switch to it enters `entry`.
#define DDPORT_STR2(x) #x
#define DDPORT_STR(x) DDPORT_STR2(x)
#if DDCLAIM_X86
#define DDPORT_DEFINE_CTX_SWITCH(name)                                                          \
  extern "C" void name(void** save_sp, void* load_sp);                                          \
  asm(".text\n.p2align 4\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"            \
      "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"     \
      "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"                                                \
      "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"   \
      ".size " #name ", .-" #name "\n");
#else
#if defined(__APPLE__)
#define DDPORT_SYM(name) "_" #name
#define DDPORT_TYPE(name) ""
#else
#define DDPORT_SYM(name) #name
#define DDPORT_TYPE(name) ".type " #name ",%function\n"
#endif
// AArch64: x19-x30 and d8-d15 in a 160-byte frame; returns through the restored x30.
#define DDPORT_DEFINE_CTX_SWITCH(name)                                                          \
  extern "C" void name(void** save_sp, void* load_sp);                                          \
  asm(".text\n.p2align 4\n.globl " DDPORT_SYM(name) "\n" DDPORT_TYPE(name) DDPORT_SYM(name) ":\n" \
      "  sub sp, sp, #160\n"                                                                    \
      "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"        \
      "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"       \
      "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n"       \
      "  stp d14, d15, [sp, #144]\n"                                                            \
      "  mov x9, sp\n  str x9, [x0]\n  mov sp, x1\n"                                            \
      "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"        \
      "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"       \
      "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n"       \
      "  ldp d14, d15, [sp, #144]\n"                                                            \
      "  add sp, sp, #160\n  ret\n");
#endif

namespace ddport {
// `top` is the (16-byte aligned) end of a new stack; returns its first saved stack pointer.
inline void* init_stack(void* top, void (*entry)()) {
  void** t = static_cast<void**>(top);
#if DDCLAIM_X86
  t[-2] = reinterpret_cast<void*>(entry);  // the return address, 16-byte aligned
  for (int k = 3; k <= 8; k++) t[-k] = nullptr;
  return t - 8;
#else
  void** f = t - 20;  // the frame the switch restores
  for (int k = 0; k < 20; k++) f[k] = nullptr;
  f[11] = reinterpret_cast<void*>(entry);  // x30
  return f;
#endif
}

// Thread placement. Linux pins to a CPU; macOS cannot pin, so it asks for the performance
// cores through the thread's QoS class instead (returns false: not pinned).
bool pin_to_cpu(int cpu);
}  // namespace ddport
