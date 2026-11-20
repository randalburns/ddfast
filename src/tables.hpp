// The endgame tables: solved endings of 2, 3 and 4 cards a hand, which ddfast's search uses to
// finish off the positions it reaches.
//
// What is here:
//   - the hand-word helpers: a hand is a 64-bit word, suit s in bits 16s .. 16s+12 (bit 12
//     the ace); in the searches the trump suit, if any, is lane 3;
//   - the layout of the two-card table, the three-card table and the 4-card pattern table,
//     loaded by init_tables() and read through attach() (below);
//   - the probes: inline functions that answer "does the leader's side of this ending take at
//     least `target` tricks", with the set of cards that decided it. They are in this header
//     so that they inline into the caller's search; the table builder uses the same code.
//
// Requires an x86-64 CPU with AVX-512 and BMI2 (build with -march=native or the equivalent),
// or AArch64 (Apple silicon; see port.hpp).
// tables.cpp holds the table data and init_tables().
#pragma once

#include "port.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ddcore {

// ---------------------------------------------------------------------------------------
// Hand words.
namespace bits {

typedef uint64_t u64;

constexpr u64 LANE0 = 0x1FFF;
constexpr u64 GUARD = 0x8000800080008000ull;
constexpr u64 ONES = 0x0001000100010001ull;
constexpr u64 ALL_LANES = 0x1FFF1FFF1FFF1FFFull;

constexpr u64 LANE(int s) { return LANE0 << (16 * s); }
inline u64 lane_of(u64 c) { return LANE0 << (__builtin_ctzll(c) & 48); }
inline int suit_of(u64 c) { return __builtin_ctzll(c) >> 4; }
inline u64 ge(u64 c) { return lane_of(c) & (0 - c); }       // c and everything above it in its suit
inline u64 above(u64 c) { return ~((c << 1) - 1); }        // all bits above c (any lane)
inline u64 lowest(u64 x) { return x & (0 - x); }
inline u64 highest(u64 x) { return 1ull << (63 - __builtin_clzll(x)); }
inline int popc(u64 x) { return __builtin_popcountll(x); }

// For every lane: all bits at and above the lowest set bit.
inline u64 fill_up(u64 x) {
  u64 y = x | GUARD;
  u64 low = y & ~(y - ONES);
  return (GUARD - low) & ALL_LANES;
}

// One card per run of touching cards (the top one). `mine` is a subset of `present`.
inline u64 heads(u64 mine, u64 present) { return ddport::run_heads(mine, present); }

// The lowest card of the run of touching cards that `c` heads; `rest` is the hand without c.
inline u64 run_bottom(u64 c, u64 rest, u64 present) { return ddport::run_bottom_of(c, rest, present); }

// Moves the lanes of x by a byte shuffle (g_shuf / g_unshuf below).
inline u64 shuffle_lanes(u64 x, const uint8_t* ctl) { return ddport::shuf8(x, ctl); }

}  // namespace bits

// ---------------------------------------------------------------------------------------
// The tables. All of them are leader-relative: a probe is given the four hands H[0..3]
// (seats N E S W), the leader L and the cards present `all`, and answers for the leader's
// side. The result of every probe is bool "North-South reach `target` tricks" (target counted
// for North-South, as in the search) and R, the cards that decided it, filled up to the
// top of each suit (fill_up).
namespace tables {

using bits::u64;

// Two-card endings: [composition][nt][arrangement][target-1] -> bit 8: the leader's side
// reaches the target; low 8 bits: the cards that matter. Built in memory by init_tables()
// (a fraction of a second).

// Three-card endings, side suits sorted by length: [config][nt][arrangement] -> 5 bytes:
// bits 0-1 the leader's side's tricks, then for each target 1..3 the 12 cards (in compressed
// order) that matter for that answer. The file t3_v2.bin (377 MB).
constexpr size_t T3_ARR = 369600, T3_CFG = 102, T3_BYTES = T3_CFG * 2 * T3_ARR * 5;

// The pattern table for endings of 4 cards a hand (level 0; level = cards - 4), with the suits
// sorted: the three side suits in a trump contract (the trump lane stays last), all four at no
// trumps, where lane 3 is a suit like the others. A directory maps the suit lengths of all
// four hands (and the strain) to a run of
// groups of 8 entries. An entry is a pattern and bounds: a = the owners of the cards that
// matter (two bits per card, split in two bit strings in compressed order), b = those cards
// (the same mask under both bit strings), bits 56-58 and 59-61 bounds on the leader's side's
// tricks, bit 63 set. An unused slot has a = ~0, b = 0. A run stores its a's and then its b's
// (64 bytes per group each). Files p4_v6.{idx,ent}. The table is filled by
// solving random six-card endings, then frozen.
constexpr int P_LEVELS = 1;
constexpr int P_CHUNK = 8;  // groups prefetched at a time
struct PDir {
  u64 key;       // 0 marks an empty slot
  uint32_t off;  // of the run, in units of 64 bytes
  uint32_t groups;
};
struct PRec {
  u64 key, a;
  u64 b;
};
struct PTable {
  bool use = false;
  int dir_bits = 0;
  PDir* dir = nullptr;
  const u64* ent = nullptr;  // runs, in memory
  size_t lines = 0;          // 64-byte units in ent
  uint16_t* cnt = nullptr;   // hit counts while filling (a mapped file): 4 per 64-byte unit
};

// The state of the loaded tables: the small lookup arrays and the pointers to the tables.
struct Snapshot {
  bool use2;
  uint8_t r70[256], r6[16], comp[729];
  uint16_t* t2;
  bool use3;
  uint16_t r924[4096];
  uint8_t r20[64], cfg3[2197];
  uint8_t* t3;
  alignas(16) uint8_t shuf[64][16], unshuf[64][16];
  uint8_t lrank[64][4];
  PTable pt[P_LEVELS];
};

// Where the table files live: the directory given to init_tables(), else $DDFAST_TABLES, else
// build/tables under the current directory. Read when the tables are loaded, not before.
std::string table_dir();

// Loads the tables, building any that are missing (and writing them to table_dir()): the
// two-card table (always, in memory), the three-card table and, if pattern_levels >= 1,
// the 4-card pattern table. `dir` may be null (see
// table_dir()). Building the three-card table takes about a minute on many threads; the
// pattern table is filled for up to DDFAST_TABLE_SECONDS (170 by default). Call once, from one
// thread, before any probe.
// map_files (or DDFAST_TABLE_MAP set to anything but 0): map the two files read-only instead
// of reading them into this process's memory, so that all processes using the same files share
// one copy; see ddfast::init().
void init_tables(const char* dir, int pattern_levels, bool map_files = false);

// Copies the state of the tables loaded by init_tables() (about 17 KB; the tables themselves
// are pointed to, not copied).
void snapshot(Snapshot& out);

// The table state the probes read. Every file that includes this header has its own copy,
// with internal linkage: the compiler then knows that nothing outside the file changes it, and
// keeps table pointers and flags in registers across calls (global variables visible to other
// files cost about 2% in the table builder's search). tables.cpp fills its copy in
// init_tables(); any other file calls attach() once after init_tables().
namespace {

[[maybe_unused]] bool g_use2 = false;
[[maybe_unused]] uint8_t g_r70[256], g_r6[16], g_comp[729];
[[maybe_unused]] uint16_t* g_t2;
[[maybe_unused]] bool g_use3 = false;
[[maybe_unused]] uint16_t g_r924[4096];
[[maybe_unused]] uint8_t g_r20[64], g_cfg3[2197];
[[maybe_unused]] uint8_t* g_t3;
// Byte shuffles that sort lanes 0-2 by length (index: the three length comparisons), and back.
// Lane orders: pid has a bit for each pair of lanes, set when the first sorts after the second
// (bits 0-2: lanes 0-1, 0-2, 1-2; bits 3-5: lanes 0-3, 1-3, 2-3, zero when lane 3 stays last:
// the 3-card table always, the pattern table in trump contracts).
// g_lrank[pid][i] is the place lane i moves to.
[[maybe_unused]] alignas(16) uint8_t g_shuf[64][16];
[[maybe_unused]] alignas(16) uint8_t g_unshuf[64][16];
[[maybe_unused]] uint8_t g_lrank[64][4];
[[maybe_unused]] PTable g_pt[P_LEVELS];

// This file's copy of the table state <- the tables loaded by init_tables().
[[maybe_unused]] void attach() {
  static Snapshot s;  // large: not on the stack
  snapshot(s);
  g_use2 = s.use2;
  memcpy(g_r70, s.r70, sizeof g_r70), memcpy(g_r6, s.r6, sizeof g_r6), memcpy(g_comp, s.comp, sizeof g_comp);
  g_t2 = s.t2;
  g_use3 = s.use3;
  memcpy(g_r924, s.r924, sizeof g_r924), memcpy(g_r20, s.r20, sizeof g_r20), memcpy(g_cfg3, s.cfg3, sizeof g_cfg3);
  g_t3 = s.t3;
  memcpy(g_shuf, s.shuf, sizeof g_shuf), memcpy(g_unshuf, s.unshuf, sizeof g_unshuf);
  memcpy(g_lrank, s.lrank, sizeof g_lrank);
  for (int i = 0; i < P_LEVELS; i++) g_pt[i] = s.pt[i];
}

// ---------------------------------------------------------------------------------------
// Probes. H: the four hands; L: the leader; all = H[0] | H[1] | H[2] | H[3]; every hand
// holds n cards; target in 1..n is North-South's. `pause` is called once a table entry has
// been prefetched (an interleaved search switches to another search there; pass [] {}
// otherwise).

// n == 2; needs g_use2.
template <bool NT>
inline bool probe2(const u64* H, int L, u64 all, int target, u64& R) {
  using namespace bits;
  const bool cut = !(L & 1);
  const u64 h3 = H[(L + 3) & 3];
  const ddport::BitMask am(all);
  const unsigned lo = unsigned(am.ext(H[(L + 1) & 3] | h3)), hi = unsigned(am.ext(H[(L + 2) & 3] | h3));
  const unsigned rank = g_r70[lo] * 36 + g_r6[ddport::pext16(hi, lo)] * 6 + g_r6[ddport::pext16(hi, lo ^ 0xFF)];
  const unsigned comp = g_comp[popc(all & LANE(0)) + 9 * popc(all & LANE(1)) + 81 * popc(all & LANE(2))];
  const int tl = cut ? target : 3 - target;
  const uint16_t e = g_t2[((comp * 2 + NT) * 2520 + rank) * 2 + (tl - 1)];
  R = fill_up(am.dep(e & 0xFF));
  return bool(e >> 8) == cut;
}

// n == 3; needs g_use3.
template <bool NT, class Pause>
inline bool probe3(const u64* H, int L, u64 all, int target, u64& R, Pause&& pause) {
  using namespace bits;
  const bool cut = !(L & 1);
  const int p0 = popc(all & LANE(0)), p1 = popc(all & LANE(1)), p2 = popc(all & LANE(2));
  const int pid = (p0 < p1) | (p0 < p2) << 1 | (p1 < p2) << 2;
  const uint8_t* ctl = g_shuf[pid];
  const u64 h3 = H[(L + 3) & 3];
  const u64 alls = ddport::shuf8(all, ctl);
  const u64 ews = ddport::shuf8((H[(L + 1) & 3] | h3), ctl);
  const u64 sws = ddport::shuf8((H[(L + 2) & 3] | h3), ctl);
  const ddport::BitMask am(alls);
  const unsigned lo = unsigned(am.ext(ews)), hi = unsigned(am.ext(sws));
  const size_t rank = size_t(g_r924[lo]) * 400 + g_r20[ddport::pext16(hi, lo)] * 20 + g_r20[ddport::pext16(hi, lo ^ 0xFFF)];
  const size_t cfg = g_cfg3[p0 + 13 * p1 + 169 * p2];
  const uint8_t* addr = g_t3 + ((cfg * 2 + NT) * T3_ARR + rank) * 5;
  ddport::prefetch(addr);
  ddport::prefetch(addr + 4);
  pause();
  u64 e;
  memcpy(&e, addr, 8);
  const int tl = cut ? target : 4 - target;
  const u64 m = am.dep((e >> (12 * tl - 10)) & 0xFFF);
  R = fill_up(shuffle_lanes(m, g_unshuf[pid]));
  return (int(e & 3) >= tl) == cut;
}

// The leader-relative key of a pattern-table lookup, kept by a search that fills the table.
struct PatternQuery {
  u64 key = 0, alls = 0, own = 0;
  int pid = 0;
};

// 4 <= n < 4 + P_LEVELS; needs g_pt[n - 4].use. lens: nibble 4 * seat + suit holds that
// suit's length in that hand. Returns -1 if no entry decides the question, else the answer
// (0 or 1) with R set and `hit` the entry's index in PTable::cnt. q is filled either way.
template <bool NT, class Pause>
inline int probe_pattern(const u64* H, u64 lens, int n, int L, u64 all, int target, PatternQuery& q, u64& R, size_t& hit,
                         Pause&& pause) {
  using namespace bits;
  const bool cut = !(L & 1);
  const PTable& pt = g_pt[n - 4];
  const u64 rot = std::rotr(lens, 16 * L);
  const u64 t0 = ddport::lane_nibbles(rot, 0), t1 = ddport::lane_nibbles(rot, 1);
  const u64 t2 = ddport::lane_nibbles(rot, 2), t3 = ddport::lane_nibbles(rot, 3);
  const int c01 = t0 < t1, c02 = t0 < t2, c12 = t1 < t2;
  q.pid = c01 | c02 << 1 | c12 << 2;
  if (NT) {  // no trump lane: all four suits take part in the order
    q.pid |= (t0 < t3) << 3 | (t1 < t3) << 4 | (t2 < t3) << 5;
    const uint8_t* rk = g_lrank[q.pid];
    q.key = t0 << (16 * rk[0]) | t1 << (16 * rk[1]) | t2 << (16 * rk[2]) | t3 << (16 * rk[3]) | u64(NT) << 63;
  } else {
    q.key = t0 << (16 * (c01 + c02)) | t1 << (16 * (!c01 + c12)) | t2 << (16 * (!c02 + !c12)) | t3 << 48 | u64(NT) << 63;
  }
  size_t h = (q.key * 0x9E3779B97F4A7C15ull) >> (64 - pt.dir_bits);
  ddport::prefetch(&pt.dir[h]);
  pause();
  const uint8_t* ctl = g_shuf[q.pid];
  const u64 h3 = H[(L + 3) & 3];
  q.alls = ddport::shuf8(all, ctl);
  const u64 ews = ddport::shuf8((H[(L + 1) & 3] | h3), ctl);
  const u64 sws = ddport::shuf8((H[(L + 2) & 3] | h3), ctl);
  const ddport::BitMask am(q.alls);
  q.own = am.ext(ews) | am.ext(sws) << (4 * n);
  while (pt.dir[h].key != q.key && pt.dir[h].key) h = (h + 1) & ((size_t(1) << pt.dir_bits) - 1);
  if (!pt.dir[h].key) return -1;
  const u64* ea = pt.ent + size_t(pt.dir[h].off) * 8;
  const int groups = int(pt.dir[h].groups);
  const u64* eb = ea + 8 * groups;
  const int tl = cut ? target : n + 1 - target;
  for (int j = 0; j < groups; j++) {
    if (j % P_CHUNK == 0) {
      const int je = j + P_CHUNK < groups ? j + P_CHUNK : groups;
      for (int i = j; i < je; i++) {
        ddport::prefetch((ea + 8 * i));
        ddport::prefetch((eb + 8 * i));
      }
      pause();
    }
    // Entries that match and whose bounds decide the question, found without branches.
    const unsigned kd = ddport::pmatch8(ea + 8 * j, eb + 8 * j, q.own, tl);
    if (kd) {
      const int i = __builtin_ctz(kd);
      const u64 bb = eb[8 * j + i];
      const int blo = int(bb >> 56) & 7;
      const u64 mk = am.dep(bb & ((1ull << (4 * n)) - 1));
      R = fill_up(shuffle_lanes(mk, g_unshuf[q.pid]));
      hit = size_t(pt.dir[h].off) * 4 + size_t(8 * j + i);
      return (blo >= tl) == cut;
    }
  }
  return -1;
}

}  // namespace
}  // namespace tables
}  // namespace ddcore
