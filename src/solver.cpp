// ddfast's search: an exact double-dummy solver for full deals, a lean scalar zero-window
// search over 64-bit hand words, finishing endings of 2, 3 and 4 cards in the endgame tables
// (tables.hpp). See docs/NOTES.md for the design and its history.
//
// Everything here is internal (an anonymous namespace); the entry points are in namespace
// ddfast::detail at the end, used by api.cpp.
// DDF_PAR: this file compiled a second time inside src/par_deal.cpp, for the parallel search
// of one deal (its own thread-local state, abort checks, no STATS counters, no entry points).
#ifdef DDF_PAR
#undef STATS
#endif
#include <unordered_map>
#include "port.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <bit>
#include <thread>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "position.hpp"
#include "tables.hpp"
#include "dds_port.hpp"
#include "internal.hpp"

#ifndef DDF_PAR
namespace ddfast::detail {
const uint8_t* g_first_target;
bool g_sure_trick_bounds = true;
int g_cutoff_min = CUTC_MIN;  // set_options() writes CUTC_MIN or 14
}
#endif

namespace {

// The hand-word helpers (a hand is a 64-bit word, suit s in bits 16s .. 16s+12, the trump
// suit, if any, moved to lane 3) and the endgame tables with their probes are in tables.hpp.
using namespace ddcore::bits;
using namespace ddcore::tables;
using ddcore::Position;

// The suit lengths of seats 0..3 in suits 1..3, packed (pext of lens by 0xFFF0FFF0FFF0FFF0).
inline u64 lens48(u64 x) {
  return ((x >> 4) & 0xFFF) | ((x >> 20) & 0xFFF) << 12 | ((x >> 36) & 0xFFF) << 24 | ((x >> 52) & 0xFFF) << 36;
}
// The 16 nibbles of x as bytes.
inline void spread_nibbles(u64 x, uint8_t* out) {
#if DDPORT_AVX512
  _mm_storeu_si128((__m128i*)out, _mm_and_si128(_mm_set1_epi8(15), _mm_unpacklo_epi8(_mm_cvtsi64_si128((long long)x), _mm_cvtsi64_si128((long long)(x >> 4)))));
#else
  namespace hn = hwy::HWY_NAMESPACE;
  const hn::Full128<uint8_t> d8;
  const auto v = hn::BitCast(d8, hn::Set(hn::Full128<uint64_t>(), x));
  hn::StoreU(hn::And(hn::InterleaveLower(d8, v, hn::ShiftRight<4>(v)), hn::Set(d8, uint8_t(15))), d8, out);
#endif
}

#ifdef STATS
}
int g_why; unsigned long long g_whyc[8][8], g_whyw[8][8], g_whyn[8];
unsigned long long g_plook[P_LEVELS], g_phit[P_LEVELS], g_etc[2][16], g_etch[2][16], g_etcn[2][16];
int g_rootL;
unsigned long long g_cnS[2][16], g_c1S[2][16], g_wS[2][16], g_allS[2][16], g_allN[2][16];
namespace {
u64 g_cat[4][8][8], g_catw[4][8][8], g_fw[4][16], g_fwn[4][16], g_ff[4][16], g_blk[16], g_blkhit[16], g_tmiss[16], g_tnoslot[16], g_evict[16], g_store[16], g_n[16][5], g_fn[16], g_nodes, g_leads, g_queries, g_tthit, g_qt, g_lt, g_bsb, g_ttlook, g_first[4], g_tried[4], g_hx[2][2], g_cs[16], g_cs1[16], g_wmax[16], g_c1[16], g_cn[16], g_w[16], g_feat[2][16], g_wfeat[2][16];
struct StatPrinter {
  ~StatPrinter() {
    fprintf(stderr, "queries %llu nodes %llu (%.1f/query) leads %llu qt %llu lt %llu ttlook %llu tthit %llu bsb %llu\n",
            (unsigned long long)g_queries, (unsigned long long)g_nodes, double(g_nodes) / double(g_queries),
            (unsigned long long)g_leads, (unsigned long long)g_qt, (unsigned long long)g_lt,
            (unsigned long long)g_ttlook, (unsigned long long)g_tthit, (unsigned long long)g_bsb);
    for (int i = 0; i < P_LEVELS; i++) fprintf(stderr, "p%d: look %llu hit %llu\n", i + 4, g_plook[i], g_phit[i]);
    for (int n = 4; n < 14; n++) fprintf(stderr, "  n=%d: new entries %llu, blocks scanned %llu, blocks on hits %llu, misses %llu (no bound decides %llu, no slot %llu)\n", n, (unsigned long long)g_store[n], (unsigned long long)g_blk[n], (unsigned long long)g_blkhit[n], (unsigned long long)g_tmiss[n], (unsigned long long)g_evict[n], (unsigned long long)g_tnoslot[n]);
    for (int n = 1; n < 14; n++)
      fprintf(stderr, "  n=%d: leads %llu qt %llu lt %llu tthit %llu expanded %llu follow-nodes %llu\n", n, (unsigned long long)g_n[n][0], (unsigned long long)g_n[n][1], (unsigned long long)g_n[n][2], (unsigned long long)g_n[n][3], (unsigned long long)g_n[n][4], (unsigned long long)g_fn[n]);
    for (int n = 4; n < 14; n++) fprintf(stderr, "  n=%d: cutter size when late %llu, waste %llu, largest wasted lead %llu, cutter size when first %llu\n", n, (unsigned long long)g_cs[n], (unsigned long long)g_w[n], (unsigned long long)g_wmax[n], (unsigned long long)g_cs1[n]);
    for (int sd = 0; sd < 2; sd++) for (int n = 5; n < 14; n++) fprintf(stderr, "  SIDE %s n=%d: cut %llu first %.1f%% waste %llu | all-nodes %llu nodes-under %llu\n", sd ? "opp" : "rootleader", n, g_cnS[sd][n], 100.0 * double(g_c1S[sd][n]) / double(g_cnS[sd][n] + 1), g_wS[sd][n], g_allS[sd][n], g_allN[sd][n]);
    for (int n = 4; n < 14; n++) fprintf(stderr, "  n=%d: lead cut nodes %llu first %.1f%% waste/node %.0f\n", n, (unsigned long long)g_cn[n], 100.0 * double(g_c1[n]) / double(g_cn[n] + !g_cn[n]), double(g_w[n]) / double(g_cn[n] + !g_cn[n]));
    for (int sm = 0; sm < 2; sm++) {
      fprintf(stderr, "  %s suit: failed first [top ptop pruff low mid]:", sm ? "same" : "other");
      for (int i = 0; i < 5; i++) fprintf(stderr, " %llu", (unsigned long long)g_feat[sm][i]);
      fprintf(stderr, "  trump first x cutter [nn nt tn tt]:");
      for (int i = 8; i < 12; i++) fprintf(stderr, " %llu", (unsigned long long)g_feat[sm][i]);
      fprintf(stderr, "\n     cutter side [top ptop pruff low mid]:");
      for (int i = 0; i < 5; i++) fprintf(stderr, " %llu", (unsigned long long)g_wfeat[sm][i]);
      fprintf(stderr, "  cutter trump:");
      for (int i = 8; i < 13; i++) fprintf(stderr, " %llu", (unsigned long long)g_wfeat[sm][i]);
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "  expanded without hint %llu (cut %llu); with hint %llu (cut %llu)\n", (unsigned long long)g_hx[0][0], (unsigned long long)g_hx[0][1], (unsigned long long)g_hx[1][0], (unsigned long long)g_hx[1][1]);
    for (int k = 1; k < 4; k++) for (int a = 0; a < 6; a++) {
      fprintf(stderr, "  K=%d first-cat %d -> cutter cat [low lowwin highwin hiother ruff disc]:", k, a);
      for (int b = 0; b < 6; b++) fprintf(stderr, " %llu/%llu", (unsigned long long)g_cat[k][a][b], (unsigned long long)g_catw[k][a][b]);
      fprintf(stderr, "\n");
    }
    for (int k = 1; k < 4; k++) for (int n = 4; n < 14; n++)
      fprintf(stderr, "  follow K=%d n=%d: cut-late nodes %llu wasted nodes %llu fail-all nodes %llu\n", k, n, (unsigned long long)g_fwn[k][n], (unsigned long long)g_fw[k][n], (unsigned long long)g_ff[k][n]);
    for (int a = 0; a < 6; a++) { fprintf(stderr, "  K=1 why %d (n %llu) -> cutter cat:", a, g_whyn[a]); for (int b = 0; b < 6; b++) fprintf(stderr, " %llu/%llu", g_whyc[a][b], g_whyw[a][b]); fprintf(stderr, "\n"); }
    for (int k = 0; k < 2; k++) for (int n = 4; n < 14; n++) if (g_etcn[k][n])
      fprintf(stderr, "  ETC %s n=%d: nodes %llu probes %llu hits %llu\n", k ? "hand4-other" : "lead/hand4-first", n, g_etcn[k][n], g_etc[k][n], g_etch[k][n]);
    for (int k = 0; k < 4; k++)
      fprintf(stderr, "  hand %d: cut nodes %llu, first-move cuts %.1f%%\n", k, (unsigned long long)g_tried[k],
              100.0 * double(g_first[k]) / double(g_tried[k] ? g_tried[k] : 1));
  }
} g_stat_printer;
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

// ---------------------------------------------------------------------------------------
// Transposition table: buckets keyed on all suit lengths and the leader; each entry is a
// pattern (which top cards mattered and who owns them) with bounds on North-South's tricks.
struct alignas(64) Bucket {
  u64 mask[8];
  u64 val[8];
  u64 tag;
  uint32_t gen;
  uint8_t cnt, nxt, hint, pad;
  uint8_t lo[8], hi[8];
  uint8_t pad2[32];
};
static_assert(sizeof(Bucket) == 192);

constexpr int TT_BITS = 10;
constexpr int TT_MIN = 4;
// Interleaving (NCTX > 1): which table probes hand over to the next search after their
// prefetch. YIELD3: the three-card table; YIELDP: the four-card pattern table (1: after the
// directory prefetch only, 2: also after each chunk of entry groups).
#ifndef YIELD3
#define YIELD3 1
#endif
#ifndef YIELDP
#define YIELDP 2
#endif
#ifndef NCTX
#define NCTX 1
#endif
thread_local Bucket g_tt[1 << TT_BITS];

// Wide table for 8 or more cards per hand: ownership of up to 52 cards. A bucket holds eight
// entries, stored as four vectors. Entry: w0 = the cards that mattered (compressed over the
// cards present, up to 52 bits); w1, w2 = the two owner bit strings under that mask; the
// top 12 bits of w0..w2 and the low 18 of w3 hold the exact key (suit lengths of lanes 1-3,
// the leader, the card count); w3 also has lo (4 bits), hi (4), hint (6) and the generation.
struct alignas(64) WBucket {
  u64 w0[8], w1[8], w2[8], w3[8];
};
WBucket* g_wtt;
thread_local unsigned g_wrr;
constexpr u64 W52 = (1ull << 52) - 1, WKM = 0xFFFFFFFF0003FFFFull;

// The position being searched and the search's other per-thread scalars. Thread-local so
// that table building can use several threads. They are kept in one thread-local struct and
// named through macros: on macOS every thread-local variable costs a call to find its address
// (once per function that uses it), so one variable instead of several saves calls in every
// node; elsewhere it makes no difference.
struct SState;
struct Hot {
  u64 H[4] = {0, 0, 0, 0};
  u64 LENS = 0;  // nibble 4*seat+suit: the length of that holding
  SState* S = nullptr;  // set on first use (see ensure_state)
  uint32_t gen = 1;
  bool skiptt = false;  // the next lead node's table lookup is known to miss
  bool rvalid = false;
  int snapdepth = 0;
  u64 skiphint = 0;     // and this is the hint it would have found
  u64 mnodes = 0;
#ifdef DDF_PAR
  const std::atomic<uint8_t>* stop = nullptr;  // the running work item's abort flag (never null while searching)
  u64 mask2 = ~0ull;                            // hand 2's replies allowed at trick 1 (the root's follow node)
  bool ybw = false;                             // stop at trick 1 after hand 2's first reply fails ...
  u64 rest = 0;                                 // ... leaving these replies
#endif
};
thread_local Hot g_hot;
#ifdef DDF_PAR
#define PAR_STOP() (g_hot.stop->load(std::memory_order_relaxed) != 0)
#else
#define PAR_STOP() false
#endif
#define H (g_hot.H)
#define LENS (g_hot.LENS)
#define S (g_hot.S)
#define g_gen (g_hot.gen)
#define g_skiptt (g_hot.skiptt)
#define g_skiphint (g_hot.skiphint)
#define g_rvalid (g_hot.rvalid)
#define g_snapdepth (g_hot.snapdepth)
#define g_mnodes (g_hot.mnodes)

inline Bucket& bucket_for(u64 tag) { return g_tt[((tag ^ g_gen) * 0x9E3779B97F4A7C15ull) >> (64 - TT_BITS)]; }

inline int len_of(int seat, int suit) { return int(LENS >> (16 * seat + 4 * suit)) & 15; }

template <bool NT, int K>
bool follow(int n, int L, int target, u64 all, u64 lane, u64 best, int bw, bool ranked, u64& R);

// ---------------------------------------------------------------------------------------
// DDS-quality bounds and ordering for the upper levels (n >= DMIN), through the scalar ports
// in dds_port_core.hpp. Lanes are used as DDS suits; trumps are lane 3.
#ifndef DMIN
#define DMIN 4
#endif
#ifndef DFOL
#define DFOL 99
#endif
// S->trk (the cards played to the current trick) is read only by the port's follow ordering
// (dds_order with K >= 1, from DFOL cards); the lead card reaches follow 1 as its best card.
// Without DFOL the stores are dead, and as stores through a pointer they made the compiler
// reload H and LENS.
#define TRK_NEEDED (DFOL <= 13)
namespace port = ddfast::port;
// The per-deal transposition table. A directory maps the exact key (suit lengths of lanes
// 1-3, leader, card count) to a chain of blocks of 8 patterns; blocks come from a pool that
// is handed out in order and reset for every deal, so a deal's table is one compact region.
// Up to 7 cards per hand a pattern is 16 bytes: a = owners of the cards that mattered (two
// 28-bit strings), b = that mask under both strings, bit 55 + t set if the entry decides
// target t (1..7), bit 63 the answer;
// a block is a[8], b[8]. From 8 cards: w0 = mask (52 bits), lo in bits 52-55, hi in 56-59;
// w1, w2 = the owner strings; a block is w0[8], w1[8], w2[8]. An unused slot never matches.
#ifndef DIR_MIN
#define DIR_MIN 15  // was 11: the regrowths (11, 13, 15) cost about 12k branch misses a deal
#endif
#ifndef DIR_MAX
#define DIR_MAX 20
#endif
#ifndef DIR_LSH
#define DIR_LSH 1  // grow when the directory is 2^-DIR_LSH full
#endif
#ifndef POOL_BITS
#define POOL_BITS 22
#endif
constexpr uint32_t NIL = 0xFFFFFFFFu;
struct DirSlot {
  u64 key;  // key54 | hint << 54 | count in the head block << 60
  uint32_t gen, head;
};
struct TT {
  DirSlot* dir = nullptr;    // 2^bits slots in use; grows with the number of keys
  u64* pool = nullptr;       // 64-byte lines
  uint32_t* next = nullptr;  // per line: the next block of the chain
  DirSlot* tmp = nullptr;
  uint32_t used = 0, keys = 0, gen = 1;
  int bits = DIR_MIN;
  void alloc() {
    const size_t db = sizeof(DirSlot) << DIR_MAX, pb = size_t(64) << POOL_BITS, nb = size_t(4) << POOL_BITS;
    dir = static_cast<DirSlot*>(mmap(nullptr, db, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    tmp = static_cast<DirSlot*>(mmap(nullptr, db / 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    pool = static_cast<u64*>(mmap(nullptr, pb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    next = static_cast<uint32_t*>(mmap(nullptr, nb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    // LEAN_NOHUGE: 4 KB pages everywhere (to measure what huge pages give).
#ifdef MADV_HUGEPAGE
    if (!getenv("LEAN_NOHUGE") && !getenv("LEAN_NOHUGE_TT")) madvise(dir, db, MADV_HUGEPAGE), madvise(pool, pb, MADV_HUGEPAGE), madvise(next, nb, MADV_HUGEPAGE);
#endif
  }
  void new_gen() {
    if (++gen == 0) memset(dir, 0, sizeof(DirSlot) << DIR_MAX), gen = 1;
  }
  void reset() {
    used = 0, keys = 0, bits = DIR_MIN;
    new_gen();
  }
  inline void prefetch(u64 key54) const { ddport::prefetch(&dir[(key54 * 0x9E3779B97F4A7C15ull) >> (64 - bits)]); }
  inline DirSlot* find(u64 key54) {
    size_t h = (key54 * 0x9E3779B97F4A7C15ull) >> (64 - bits);
    for (;;) {
      DirSlot& d = dir[h];
      if (d.gen != gen) return nullptr;
      if ((d.key & ((1ull << 54) - 1)) == key54) return &d;
      h = (h + 1) & ((size_t(1) << bits) - 1);
    }
  }
  __attribute__((noinline)) void grow() {
    size_t k = 0;
    // Without a branch: whether a slot is in use is unpredictable.
    for (size_t i = 0; i < (size_t(1) << bits); i++) tmp[k] = dir[i], k += dir[i].gen == gen;
    new_gen();
    bits += 2;
    for (size_t i = 0; i < k; i++) {
      size_t h = ((tmp[i].key & ((1ull << 54) - 1)) * 0x9E3779B97F4A7C15ull) >> (64 - bits);
      while (dir[h].gen == gen) h = (h + 1) & ((size_t(1) << bits) - 1);
      dir[h] = tmp[i];
      dir[h].gen = gen;
    }
  }
  // The slot of the key, made if need be. May grow the directory, or reset the table when it is full.
  inline DirSlot* obtain(u64 key54) {
    for (;;) {
      size_t h = (key54 * 0x9E3779B97F4A7C15ull) >> (64 - bits);
      for (;;) {
        DirSlot& d = dir[h];
        if (d.gen != gen) break;
        if ((d.key & ((1ull << 54) - 1)) == key54) return &d;
        h = (h + 1) & ((size_t(1) << bits) - 1);
      }
      if (keys >= (1u << (bits - DIR_LSH))) {
        if (bits + 2 <= DIR_MAX) grow();
        else reset();
        continue;
      }
      keys++;
      dir[h] = DirSlot{key54 | 63ull << 54 | 8ull << 60, gen, NIL};
      return &dir[h];
    }
  }
};
struct SState {
  TT tt;
  port::Pos pos[14];   // per trick (index: cards per hand)
  int removed[14][4];  // ranks in no hand when the trick started
  u64 trk[14][4];      // the cards played to the trick
  u64 killer[14][4];   // the card that last cut off, per trick and hand
  uint32_t htried[4][64], hcut[4][64];  // leads: how often each card was tried, and cut off
  int rootL;                            // the leader at the root of the current deal
  // MEMN: per card count and card, the result (1 cut, 2 failed) and cost (lead nodes) of the
  // card's last search as a lead of the root leader's side, within the deal.
  uint8_t mres[14][64];
  u64 mcost[14][64];
};
thread_local SState g_sdef;
inline void ensure_state() { if (!S) [[unlikely]] S = &g_sdef; }

inline void fill_holdings(port::Pos& t) {
  memcpy(t.rank_in_suit, H, 32);
  const u64 all = H[0] | H[1] | H[2] | H[3];
  memcpy(t.aggr, &all, 8);
  // LENS has a nibble per holding; spread to a byte each.
  spread_nibbles(LENS, reinterpret_cast<uint8_t*>(t.length));
}
inline void fill_tops(int n, port::Pos& t) {
  for (int l = 0; l < 4; l++) {
    unsigned x = t.aggr[l];
    S->removed[n][l] = 0xffff ^ x;
    port::HighCardType hc[3];
    for (int j = 0; j < 3; j++) {
      const bool has = x != 0;
      const int r = 31 - __builtin_clz(x | 1);
      x &= ~(1u << r);
      const int bit = 16 * l + r;
      const int q = int((H[1] >> bit) & 1) | int((H[2] >> bit) & 1) << 1 | (int((H[3] >> bit) & 1) * 3);
      hc[j].rank = has ? r + 2 : 0;
      hc[j].hand = has ? q : -1;
    }
    t.winner[l] = hc[0], t.second_best[l] = hc[1], t.third[l] = hc[2];
  }
}
inline u64 win_ranks(const port::Pos& t) {
  return fill_up(u64(t.win_ranks[0]) | u64(t.win_ranks[1]) << 16 | u64(t.win_ranks[2]) << 32 | u64(t.win_ranks[3]) << 48);
}
inline void clear_wr(port::Pos& t) { t.win_ranks[0] = t.win_ranks[1] = t.win_ranks[2] = t.win_ranks[3] = 0; }

inline int tops(u64 ml, u64 xs, u64& topset);

// ---------------------------------------------------------------------------------------
// DDS's lead-node QuickTricks, LaterTricksMIN and LaterTricksMAX on the hand words (the port's
// code mirrored case by case, tricks_max = 0). Relevant cards are kept as one word WR with
// the port's win_ranks in lanes. -DPORT_BOUNDS uses the port; -DVERIFY_BOUNDS runs both and
// aborts on any difference.
struct NB {
  u64 h[4], lens;
  u64 all;
  inline int owner_of(u64 c) const {
    return c ? (int((h[1] & c) != 0) | int((h[2] & c) != 0) << 1 | int((h[3] & c) != 0) * 3) : -1;
  }
  inline int ris(int x, int s) const { return int((h[x] >> (16 * s)) & LANE0); }
#ifdef OLD_QT
  inline int len(int x, int s) const { return int(lens >> (16 * x + 4 * s)) & 15; }
  u64 wb[4];  // the top card of each suit (0 if none)
  int wh[4];  // its hand (-1 if none)
  inline u64 sb(int s) const { u64 x = (all & LANE(s)) ^ wb[s]; return x ? highest(x) : 0; }
  inline u64 tb(int s) const { u64 x = (all & LANE(s)) ^ wb[s]; if (x) x ^= highest(x); return x ? highest(x) : 0; }
  inline int sbh(int s) const { return owner_of(sb(s)); }
  inline int tbh(int s) const { return owner_of(tb(s)); }
  inline void init(u64 a) {
    h[0] = H[0], h[1] = H[1], h[2] = H[2], h[3] = H[3], lens = LENS;
    all = a;
    for (int s = 0; s < 4; s++) {
      const u64 x = a & LANE(s);
      wb[s] = x ? highest(x) : 0;
      wh[s] = owner_of(wb[s]);
    }
  }
#else
  // The top three cards of each suit and their hands, computed for all suits at once (one
  // 64-bit lane per suit), and the lengths as bytes (-DOLD_QT: computed on demand).
  alignas(32) u64 wb[4], sbv[4], tbv[4];
  int wh[4], sbo[4], tbo[4];
  uint8_t ln[16];
  inline int len(int x, int s) const { return ln[4 * x + s]; }
  inline u64 sb(int s) const { return sbv[s]; }
  inline u64 tb(int s) const { return tbv[s]; }
  inline int sbh(int s) const { return sbo[s]; }
  inline int tbh(int s) const { return tbo[s]; }
  inline void init(u64 a) {
    h[0] = H[0], h[1] = H[1], h[2] = H[2], h[3] = H[3], lens = LENS;
    all = a;
    spread_nibbles(LENS, ln);
#if DDPORT_AVX512
    __m256i x = _mm256_and_si256(_mm256_set1_epi64x((long long)a), _mm256_set_epi64x((long long)LANE(3), (long long)LANE(2), (long long)LANE(1), (long long)LANE(0)));
    const __m256i top = _mm256_set1_epi64x((long long)(1ull << 63));
    const __m256i v1 = _mm256_set1_epi64x((long long)H[1]), v2 = _mm256_set1_epi64x((long long)H[2]), v3 = _mm256_set1_epi64x((long long)H[3]);
    auto level = [&](u64* c, int* o) {
      const __m256i t = _mm256_srlv_epi64(top, _mm256_lzcnt_epi64(x));
      x = _mm256_xor_si256(x, t);
      _mm256_store_si256((__m256i*)c, t);
      const unsigned k1 = _mm256_test_epi64_mask(t, v1), k2 = _mm256_test_epi64_mask(t, v2), k3 = _mm256_test_epi64_mask(t, v3);
      __m256i w = _mm256_maskz_mov_epi64(__mmask8(~_mm256_test_epi64_mask(t, t)), _mm256_set1_epi64x(-1));
      w = _mm256_mask_mov_epi64(w, k1, _mm256_set1_epi64x(1));
      w = _mm256_mask_mov_epi64(w, k2, _mm256_set1_epi64x(2));
      w = _mm256_mask_mov_epi64(w, k3, _mm256_set1_epi64x(3));
      _mm_storeu_si128((__m128i*)o, _mm256_cvtepi64_epi32(w));
    };
    level(wb, wh);
    level(sbv, sbo);
    level(tbv, tbo);
#else
    for (int s = 0; s < 4; s++) {
      u64 x = a & LANE(s);
      u64* const cs[3] = {wb, sbv, tbv};
      int* const os[3] = {wh, sbo, tbo};
      for (int j = 0; j < 3; j++) {
        const u64 t = x ? highest(x) : 0;
        x ^= t;
        cs[j][s] = t;
        os[j][s] = owner_of(t);
      }
    }
#endif
  }
#endif
};
#define NLEN(x, s) b.len(x, s)
inline void set_lane(u64& WR, int s, u64 v) { WR = (WR & ~LANE(s)) | v; }

template <bool NT>
__attribute__((always_inline)) inline int nq_quick(const NB& b, int hand, int n, int target, bool& result, u64& WR) {
  constexpr int trump = NT ? 4 : 3, T = 3;
  const int P = hand ^ 2, LH = (hand + 1) & 3, RH = (hand + 3) & 3;
  u64 commBit = 0;
  int commSuit = -1;
  bool commPartner = false;
  int lhoTR = 0, rhoTR = 0, lowestQ = 0, qtricks = 0;
  result = true;
  int cutoff = (hand & 1) == 0 ? target : n + 1 - target;
#if defined(OLD_QT) || defined(OLD_COMM)
  for (int s = 0; s < 4; s++) {
    if (!NT && s != T) {
      if (b.wh[s] == P) {
        if (b.ris(hand, s) != 0 && ((b.ris(LH, s) != 0 || b.ris(LH, T) == 0) && (b.ris(RH, s) != 0 || b.ris(RH, T) == 0))) {
          commPartner = true, commSuit = s, commBit = b.wb[s];
          break;
        }
      } else if (b.wh[s] == hand && NLEN(hand, s) >= 2 && NLEN(P, s) >= 2 && b.sbh(s) == P) {
        if ((b.ris(LH, s) != 0 || b.ris(LH, T) == 0) && (b.ris(RH, s) != 0 || b.ris(RH, T) == 0)) {
          commPartner = true, commSuit = s, commBit = b.sb(s);
          break;
        }
      }
    } else if (NT) {
      if (b.wh[s] == P) {
        if (b.ris(hand, s) != 0) {
          commPartner = true, commSuit = s, commBit = b.wb[s];
          break;
        }
      } else if (b.wh[s] == hand && NLEN(hand, s) >= 2 && NLEN(P, s) >= 2 && b.sbh(s) == P) {
        commPartner = true, commSuit = s, commBit = b.sb(s);
        break;
      }
    }
  }
  if (!NT && !commPartner && b.ris(hand, T) != 0 && b.wh[T] == P) commPartner = true, commSuit = T, commBit = b.wb[T];
#else
  // The same with suit masks: the first suit where the leader reaches partner's top card, or
  // a second card of partner's under the leader's top (the opponents following or out of trumps).
  {
    auto q = [](unsigned m, int x) { return (m >> (4 * x)) & 15u; };
#if DDPORT_AVX512
    const __m128i lv = _mm_loadu_si128((const __m128i*)b.ln);
    const unsigned nzm = _mm_test_epi8_mask(lv, lv), g2m = _mm_cmpgt_epi8_mask(lv, _mm_set1_epi8(1));
    const __m256i vw = _mm256_load_si256((const __m256i*)b.wb), vP = _mm256_set1_epi64x((long long)b.h[P]);
    const unsigned topP = _mm256_test_epi64_mask(vw, vP), topH = _mm256_test_epi64_mask(vw, _mm256_set1_epi64x((long long)b.h[hand]));
    const unsigned sbP = _mm256_test_epi64_mask(_mm256_load_si256((const __m256i*)b.sbv), vP);
#else
    // The lengths (byte 4 * hand + suit) that are nonzero, and at least 2, as bit masks.
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::Full128<uint8_t> d8;
    const auto lv = hn::LoadU(d8, b.ln);
    const unsigned nzm = unsigned(hn::BitsFromMask(d8, hn::Ne(lv, hn::Zero(d8))));
    const unsigned g2m = unsigned(hn::BitsFromMask(d8, hn::Gt(lv, hn::Set(d8, uint8_t(1)))));
    const unsigned topP = unsigned((b.wb[0] & b.h[P]) != 0) | unsigned((b.wb[1] & b.h[P]) != 0) << 1 | unsigned((b.wb[2] & b.h[P]) != 0) << 2 | unsigned((b.wb[3] & b.h[P]) != 0) << 3;
    const u64 hh = b.h[hand];
    const unsigned topH = unsigned((b.wb[0] & hh) != 0) | unsigned((b.wb[1] & hh) != 0) << 1 | unsigned((b.wb[2] & hh) != 0) << 2 | unsigned((b.wb[3] & hh) != 0) << 3;
    const unsigned sbP = unsigned((b.sbv[0] & b.h[P]) != 0) | unsigned((b.sbv[1] & b.h[P]) != 0) << 1 | unsigned((b.sbv[2] & b.h[P]) != 0) << 2 | unsigned((b.sbv[3] & b.h[P]) != 0) << 3;
#endif
    unsigned safe = 15;
    if (!NT) {
      const unsigned l = q(nzm, LH), r = q(nzm, RH);
      safe = (l | ((l & 8) ? 0u : 15u)) & (r | ((r & 8) ? 0u : 15u));
    }
    const unsigned candA = topP & q(nzm, hand) & safe;
    const unsigned candB = topH & q(g2m, hand) & q(g2m, P) & sbP & safe;
    const unsigned cand = (candA | candB) & (NT ? 15u : 7u);
    if (cand) {
      const int s = __builtin_ctz(cand);
      commPartner = true, commSuit = s, commBit = ((candA >> s) & 1) ? b.wb[s] : b.sbv[s];
    }
  }
  if (!NT && !commPartner && b.ris(hand, T) != 0 && b.wh[T] == P) commPartner = true, commSuit = T, commBit = b.wb[T];
#endif
  int suit;
  if (!NT) suit = T, lhoTR = NLEN(LH, T), rhoTR = NLEN(RH, T);
  else suit = 0;
  auto next = [](int s) { if (!NT && s == T) return 0; s++; if (!NT && s == T) s++; return s; };

  // QtricksLeadHandTrump
  auto lead_trump = [&](int countLho, int countRho, int countOwn, int countPart, int qt, int& res) {
    res = 1;
    if ((countLho != 0 || lhoTR == 0) && (countRho != 0 || rhoTR == 0)) {
      WR |= b.wb[suit];
      if (++qt >= cutoff) return qt;
      if (countLho <= 1 && countRho <= 1 && countPart <= 1 && lhoTR == 0 && rhoTR == 0) {
        qt += countOwn - 1;
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    }
    const u64 s2 = b.sb(suit);
    const int h2 = b.sbh(suit);
    if (h2 == hand) {
      if (lhoTR == 0 && rhoTR == 0) {
        WR |= s2;
        if (++qt >= cutoff) return qt;
        if (countLho <= 2 && countRho <= 2 && countPart <= 2) {
          qt += countOwn - 2;
          if (qt >= cutoff) return qt;
          res = 2;
          return qt;
        }
      }
    } else if (h2 == P && countOwn > 1 && countPart > 1) {
      if (lhoTR == 0 && rhoTR == 0) {
        WR |= s2;
        if (++qt >= cutoff) return qt;
        if (countLho <= 2 && countRho <= 2 && (countPart <= 2 || countOwn <= 2)) {
          qt += std::max(countOwn - 2, countPart - 2);
          if (qt >= cutoff) return qt;
          res = 2;
          return qt;
        }
      }
    }
    res = 0;
    return qt;
  };
  // QtricksLeadHandNT (also the trump suit in a trump game)
  auto lead_nt = [&](int countLho, int countRho, int countOwn, int countPart, int qt, int& res) {
    res = 1;
    WR |= b.wb[suit];
    if (++qt >= cutoff) return qt;
    if (trump == suit && (!commPartner || suit != commSuit)) lhoTR = std::max(0, lhoTR - 1), rhoTR = std::max(0, rhoTR - 1);
    if (countLho <= 1 && countRho <= 1 && countPart <= 1) {
      qt += countOwn - 1;
      if (qt >= cutoff) return qt;
      res = 2;
      return qt;
    }
    const u64 s2 = b.sb(suit);
    const int h2 = b.sbh(suit);
    if (h2 == hand) {
      WR |= s2;
      if (++qt >= cutoff) return qt;
      if (trump == suit && (!commPartner || suit != commSuit)) lhoTR = std::max(0, lhoTR - 1), rhoTR = std::max(0, rhoTR - 1);
      if (countLho <= 2 && countRho <= 2 && countPart <= 2) {
        qt += countOwn - 2;
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    } else if (h2 == P && countOwn > 1 && countPart > 1) {
      WR |= s2;
      if (++qt >= cutoff) return qt;
      if (trump == suit && (!commPartner || suit != commSuit)) lhoTR = std::max(0, lhoTR - 1), rhoTR = std::max(0, rhoTR - 1);
      if (countLho <= 2 && countRho <= 2 && (countPart <= 2 || countOwn <= 2)) {
        qt += std::max(countOwn - 2, countPart - 2);
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    }
    res = 0;
    return qt;
  };
  // QuickTricksPartnerHandTrump
  auto part_trump = [&](int countLho, int countRho, int countOwn, int countPart, int qt, int& res) {
    res = 1;
    if ((countLho != 0 || lhoTR == 0) && (countRho != 0 || rhoTR == 0)) {
      WR |= b.wb[suit] | commBit;
      if (++qt >= cutoff) return qt;
      if (countLho <= 1 && countRho <= 1 && countOwn <= 1 && lhoTR == 0 && rhoTR == 0) {
        qt += countPart - 1;
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    }
    const u64 s2 = b.sb(suit);
    const int h2 = b.sbh(suit);
    if (h2 == P) {
      if (lhoTR == 0 && rhoTR == 0) {
        WR |= s2 | commBit;
        if (++qt >= cutoff) return qt;
        if (countLho <= 2 && countRho <= 2 && countOwn <= 2) {
          qt += countPart - 2;
          if (qt >= cutoff) return qt;
          res = 2;
          return qt;
        }
      }
    } else if (h2 == hand && countPart > 1 && countOwn > 1) {
      if (lhoTR == 0 && rhoTR == 0) {
        WR |= s2 | commBit;
        if (++qt >= cutoff) return qt;
        if (countLho <= 2 && countRho <= 2 && (countOwn <= 2 || countPart <= 2)) {
          qt += std::max(countPart - 2, countOwn - 2);
          if (qt >= cutoff) return qt;
          res = 2;
          return qt;
        }
      }
    } else if (suit == commSuit && h2 == LH && (countLho >= 2 || lhoTR == 0) && (countRho >= 2 || rhoTR == 0)) {
      const u64 s3 = b.tb(suit);
      if (b.tbh(suit) == P) {
        WR |= s3 | commBit;
        if (++qt >= cutoff) return qt;
        if (countOwn <= 2 && countLho <= 2 && countRho <= 2 && lhoTR == 0 && rhoTR == 0) {
          qt += countPart - 2;
          if (qt >= cutoff) return qt;
        }
      }
    }
    res = 0;
    return qt;
  };
  // QuickTricksPartnerHandNT
  auto part_nt = [&](int countLho, int countRho, int countOwn, int countPart, int qt, int& res) {
    res = 1;
    WR |= b.wb[suit] | commBit;
    if (++qt >= cutoff) return qt;
    if (countLho <= 1 && countRho <= 1 && countOwn <= 1) {
      qt += countPart - 1;
      if (qt >= cutoff) return qt;
      res = 2;
      return qt;
    }
    const u64 s2 = b.sb(suit);
    const int h2 = b.sbh(suit);
    if (h2 == P) {
      WR |= s2;
      if (++qt >= cutoff) return qt;
      if (countLho <= 2 && countRho <= 2 && countOwn <= 2) {
        qt += countPart - 2;
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    } else if (h2 == hand && countPart > 1 && countOwn > 1) {
      WR |= s2;
      if (++qt >= cutoff) return qt;
      if (countLho <= 2 && countRho <= 2 && (countOwn <= 2 || countPart <= 2)) {
        qt += std::max(countPart - 2, countOwn - 2);
        if (qt >= cutoff) return qt;
        res = 2;
        return qt;
      }
    } else if (suit == commSuit && h2 == LH) {
      const u64 s3 = b.tb(suit);
      if (b.tbh(suit) == P) {
        WR |= s3;
        if (++qt >= cutoff) return qt;
        if (countOwn <= 2 && countLho <= 2 && countRho <= 2) {
          qt += countPart - 2;
          if (qt >= cutoff) return qt;
        }
      }
    }
    res = 0;
    return qt;
  };

  while (suit <= 3) {
    const int countOwn = NLEN(hand, suit), countLho = NLEN(LH, suit), countRho = NLEN(RH, suit), countPart = NLEN(P, suit);
    const int opps = countLho | countRho;
    if (!opps && countPart == 0) [[unlikely]] {
      if (countOwn == 0) { suit = next(suit); continue; }
      if (!NT && trump != suit) {
        if (lhoTR == 0 && rhoTR == 0) {
          qtricks += countOwn;
          if (qtricks >= cutoff) return qtricks;
        }
        suit = next(suit);
        continue;
      }
      qtricks += countOwn;
      if (qtricks >= cutoff) return qtricks;
      suit = next(suit);
      continue;
    } else {
      if (!opps && !NT && suit == trump) [[unlikely]] {
        int sum = std::max(countOwn, countPart);
        for (int s = 0; s < 4; s++)
          if (sum > 0 && s != trump && countOwn >= countPart && NLEN(hand, s) > 0 && NLEN(P, s) == 0) { sum++; break; }
        if (sum >= cutoff) return sum;
      } else if (!opps) [[unlikely]] {
        const int sum = std::min(countOwn, countPart);
        if (NT) {
          if (sum >= cutoff) return sum;
        } else if (suit != trump && lhoTR == 0 && rhoTR == 0) {
          if (sum >= cutoff) return sum;
        }
      }
      if (commPartner) {
        if (!opps && countOwn == 0) [[unlikely]] {
          if (!NT && trump != suit) {
            if (lhoTR == 0 && rhoTR == 0) {
              qtricks += countPart;
              WR |= commBit;
              if (qtricks >= cutoff) return qtricks;
            }
            suit = next(suit);
            continue;
          }
          qtricks += countPart;
          WR |= commBit;
          if (qtricks >= cutoff) return qtricks;
          suit = next(suit);
          continue;
        } else {
          if (!opps && !NT && suit == trump) [[unlikely]] {
            int sum = std::max(countOwn, countPart);
            for (int s = 0; s < 4; s++)
              if (sum > 0 && s != trump && countOwn <= countPart && NLEN(P, s) > 0 && NLEN(hand, s) == 0) { sum++; break; }
            if (sum >= cutoff) {
              WR |= commBit;
              return sum;
            }
          } else if (!opps) [[unlikely]] {
            const int sum = std::min(countOwn, countPart);
            if (NT) {
              if (sum >= cutoff) return sum;
            } else if (suit != trump && lhoTR == 0 && rhoTR == 0) {
              if (sum >= cutoff) return sum;
            }
          }
        }
      }
    }
    if (b.wb[suit] == 0) [[unlikely]] { suit = next(suit); continue; }
    int res;
    if (b.wh[suit] == hand) {
      if (!NT && trump != suit) qtricks = lead_trump(countLho, countRho, countOwn, countPart, qtricks, res);
      else qtricks = lead_nt(countLho, countRho, countOwn, countPart, qtricks, res);
      if (res == 1) return qtricks;
      if (res == 2) { suit = next(suit); continue; }
    } else if (b.wh[suit] == P && commPartner) {
      if (!NT && trump != suit) qtricks = part_trump(countLho, countRho, countOwn, countPart, qtricks, res);
      else qtricks = part_nt(countLho, countRho, countOwn, countPart, qtricks, res);
      if (res == 1) return qtricks;
      if (res == 2) { suit = next(suit); continue; }
    }
    if (!NT && suit != trump && countOwn > 0 && lowestQ == 0 &&
        (qtricks == 0 || (b.wh[suit] != hand && b.wh[suit] != P && b.wh[T] != hand && b.wh[T] != P))) {
      if (countPart == 0 && NLEN(P, T) > 0) {
        if ((countRho > 0 || NLEN(RH, T) == 0) && (countLho > 0 || NLEN(LH, T) == 0)) {
          lowestQ = 1;
          if (1 >= cutoff) return 1;
          suit = next(suit);
          continue;
        } else if (countRho == 0 && countLho == 0) {
          if ((b.ris(LH, T) | b.ris(RH, T)) < b.ris(P, T)) {
            lowestQ = 1;
            WR |= highest(b.h[P] & LANE(T));
            if (1 >= cutoff) return 1;
          }
          suit = next(suit);
          continue;
        } else if (countLho == 0) {
          if (b.ris(LH, T) < b.ris(P, T)) {
            lowestQ = 1;
            WR |= highest(b.h[P] & LANE(T));
            if (1 >= cutoff) return 1;
          }
          suit = next(suit);
          continue;
        } else if (countRho == 0) {
          if (b.ris(RH, T) < b.ris(P, T)) {
            lowestQ = 1;
            WR |= highest(b.h[P] & LANE(T));
            if (1 >= cutoff) return 1;
          }
          suit = next(suit);
          continue;
        }
      }
    }
    if (qtricks >= cutoff) return qtricks;
    suit = next(suit);
  }
  if (qtricks == 0 && (NT || b.wh[T] == -1)) {
    for (int ss = 0; ss < 4; ss++)
      if (b.wh[ss] != -1 && NLEN(hand, ss) > 0) set_lane(WR, ss, b.wb[ss]);
    cutoff = (hand & 1) ? target : n + 1 - target;
    if (1 >= cutoff) return 0;
  }
  result = false;
  return qtricks;
}

template <bool NT>
__attribute__((always_inline)) inline bool nq_ltmin(const NB& b, int hand, int n, int target, u64& WR) {
  constexpr int T = 3;
  const int d = n - 1;
  if (NT || b.wb[T] == 0) {
    int sum = 0;
    for (int ss = 0; ss < 4; ss++) {
      const int hh = b.wh[ss];
      if (hh != -1 && (hh & 1) == 0) sum += std::max(NLEN(hh, ss), NLEN(hh ^ 2, ss));
    }
    if (sum < target && sum > 0) {
      if (d >= target) return true;
      u64 w = 0;
      for (int ss = 0; ss < 4; ss++) {
        const int hh = b.wh[ss];
        if (hh != -1 && (hh & 1) && ((b.h[hh ^ 2] | b.h[(hh + 1) & 3] | b.h[(hh + 3) & 3]) & LANE(ss))) w |= b.wb[ss];
      }
      WR = w;
      return false;
    }
  } else if (b.wh[T] & 1) {
    if (NLEN(hand, T) == 0 && NLEN(hand ^ 2, T) == 0) {
      if (d + 1 - std::max(NLEN((hand + 1) & 3, T), NLEN((hand + 3) & 3, T)) < target) { WR = 0; return false; }
    } else if (d < target) {
      WR = b.wb[T];
      return false;
    } else if (d == target) {
      const u64 s2 = b.sb(T);
      const int hh = b.sbh(T);
      if (hh == -1) return true;
      if ((hh & 1) && (NLEN(hh, T) > 1 || NLEN(hh ^ 2, T) > 1)) { WR = s2; return false; }
    }
  } else {
    const u64 s2 = b.sb(T);
    const int hh = b.sbh(T);
    if (hh == -1) return true;
    if (!(hh & 1) || NLEN(hh, T) <= 1) return true;
    if (b.wh[T] == ((hh + 3) & 3)) {
      if (d < target) { WR = s2; return false; }
    } else {
      const u64 s3 = b.tb(T);
      const int h = b.tbh(T);
      if (h == -1) return true;
      if ((h & 1) && d < target) { WR = s3; return false; }
    }
  }
  return true;
}

template <bool NT>
__attribute__((always_inline)) inline bool nq_ltmax(const NB& b, int hand, int n, int target, u64& WR) {
  constexpr int T = 3;
  const int d = n - 1;
  if (NT || b.wb[T] == 0) {
    int sum = 0;
    for (int ss = 0; ss < 4; ss++) {
      const int hh = b.wh[ss];
      if (hh != -1 && (hh & 1)) sum += std::max(NLEN(hh, ss), NLEN(hh ^ 2, ss));
    }
    if (d + 1 - sum >= target && sum > 0) {
      if (1 < target) return false;
      u64 w = 0;
      for (int ss = 0; ss < 4; ss++) {
        const int hh = b.wh[ss];
        if (hh != -1 && !(hh & 1) && ((b.h[hh ^ 2] | b.h[(hh + 1) & 3] | b.h[(hh + 3) & 3]) & LANE(ss))) w |= b.wb[ss];
      }
      WR = w;
      return true;
    }
  } else if (!(b.wh[T] & 1)) {
    if (NLEN(hand, T) == 0 && NLEN(hand ^ 2, T) == 0) {
      if (std::max(NLEN((hand + 1) & 3, T), NLEN((hand + 3) & 3, T)) >= target) { WR = 0; return true; }
    } else if (1 >= target) {
      WR = b.wb[T];
      return true;
    } else {
      const u64 s2 = b.sb(T);
      const int hh = b.sbh(T);
      if (hh == -1) return false;
      if (!(hh & 1) && (NLEN(hh, T) > 1 || NLEN(hh ^ 2, T) > 1) && 2 >= target) { WR = s2; return true; }
    }
  } else {
    const u64 s2 = b.sb(T);
    const int hh = b.sbh(T);
    if (hh == -1) return false;
    if ((hh & 1) || NLEN(hh, T) <= 1) return false;
    if (b.wh[T] == ((hh + 3) & 3)) {
      if (1 >= target) { WR = s2; return true; }
    } else {
      const u64 s3 = b.tb(T);
      const int h = b.tbh(T);
      if (h == -1) return false;
      if (!(h & 1) && 1 >= target) { WR = s3; return true; }
    }
  }
  return false;
}
#undef NLEN

// The same through the DDS port (-DPORT_BOUNDS).
template <bool NT>
inline bool port_bounds(int n, int L, int target, bool& filled, u64& R, bool& value, bool try_qt, bool try_lt) {
  const bool max_node = !(L & 1);
  port::Pos& t = S->pos[n];
  fill_holdings(t);
  fill_tops(n, t);
  filled = true;
  t.tricks_max = 0;
  clear_wr(t);
  const int trump = NT ? 4 : 3, depth = 4 * (n - 1);
  if (try_qt) {
    bool res = false;
    const int qt = port::QuickTricks(t, L, depth, target, trump, res);
    if (res) {
      value = max_node ? qt != 0 : qt == 0;
      R = win_ranks(t);
      return true;
    }
  }
  if (!try_lt) return false;
  if (max_node) {
    if (!port::LaterTricksMIN(t, L, depth, target, trump)) {
      value = false;
      R = win_ranks(t);
      return true;
    }
  } else if (port::LaterTricksMAX(t, L, depth, target, trump)) {
    value = true;
    R = win_ranks(t);
    return true;
  }
  return false;
}

// Quick tricks and later tricks at the start of a trick. True if decided.
template <bool NT>
inline bool dds_bounds(int n, int L, int target, u64 all, bool& filled, u64& R, bool& value) {
  const bool max_node = !(L & 1);
  const int needL = max_node ? target : n - target + 1;
  // Cheap filters: the bounds are only asked when they have a chance. Quick tricks: the top
  // cards the leader's side holds must come near the tricks it needs. Later tricks: the other
  // side must need few.
  const u64 side = H[L] | H[L ^ 2];
#ifdef OLD_QT
  int u = 0;
  for (int l = 0; l < 4; l++) {
    u64 ts;
    u += tops(side & LANE(l), all & LANE(l), ts);
  }
#else
  // The same for the four suits at once: per lane, the side's cards above the other side's
  // highest card (lanes are disjoint, so one popcount of their union).
#if DDPORT_AVX512
  const __m256i lanes = _mm256_set_epi64x((long long)LANE(3), (long long)LANE(2), (long long)LANE(1), (long long)LANE(0));
  const __m256i oth = _mm256_and_si256(_mm256_set1_epi64x((long long)(all & ~side)), lanes);
  const __m256i below = _mm256_srlv_epi64(_mm256_set1_epi64x(-1), _mm256_lzcnt_epi64(oth));
  const __m256i tv = _mm256_andnot_si256(below, _mm256_and_si256(_mm256_set1_epi64x((long long)side), lanes));
  const __m128i t2 = _mm_or_si128(_mm256_castsi256_si128(tv), _mm256_extracti128_si256(tv, 1));
  const int u = popc(u64(_mm_cvtsi128_si64(_mm_or_si128(t2, _mm_unpackhi_epi64(t2, t2)))));
#else
  u64 tv = 0;
  {
    const u64 o = all & ~side;
    for (int l = 0; l < 4; l++) {
      const u64 ol = o & LANE(l);
      tv |= side & LANE(l) & (ol ? ~((u64(2) << (63 - __builtin_clzll(ol))) - 1) : ~u64(0));
    }
  }
  const int u = popc(tv);
#endif
#endif
  const bool try_qt = u >= needL;
#ifdef PORT_BOUNDS
  return port_bounds<NT>(n, L, target, filled, R, value, try_qt, true);
#else
  NB b;
  b.init(all);
  u64 WR = 0;
#ifdef VERIFY_BOUNDS
  port::Pos& t = S->pos[n];
  fill_holdings(t);
  fill_tops(n, t);
  filled = true;
  t.tricks_max = 0;
  clear_wr(t);
  auto raw = [&]() { return u64(t.win_ranks[0]) | u64(t.win_ranks[1]) << 16 | u64(t.win_ranks[2]) << 32 | u64(t.win_ranks[3]) << 48; };
  const int trump = NT ? 4 : 3, depth = 4 * (n - 1);
#define VB(cond, what) if (!(cond)) { fprintf(stderr, "VERIFY_BOUNDS %s n=%d L=%d target=%d NT=%d H=%llx %llx %llx %llx WR=%llx port=%llx\n", what, n, L, target, NT, (unsigned long long)H[0], (unsigned long long)H[1], (unsigned long long)H[2], (unsigned long long)H[3], (unsigned long long)WR, (unsigned long long)raw()); abort(); }
#endif
  if (try_qt) {
    bool res = false;
    const int qt = nq_quick<NT>(b, L, n, target, res, WR);
#ifdef VERIFY_BOUNDS
    bool pres = false;
    const int pqt = port::QuickTricks(t, L, depth, target, trump, pres);
    VB(pres == res && pqt == qt && raw() == WR, "qt");
#endif
    if (res) {
      value = max_node ? qt != 0 : qt == 0;
      R = fill_up(WR);
      return true;
    }
  }
  if (max_node) {
    const bool r = nq_ltmin<NT>(b, L, n, target, WR);
#ifdef VERIFY_BOUNDS
    VB(r == port::LaterTricksMIN(t, L, depth, target, trump) && (r || raw() == WR), "ltmin");
#endif
    if (!r) {
      value = false;
      R = fill_up(WR);
      return true;
    }
  } else {
    const bool r = nq_ltmax<NT>(b, L, n, target, WR);
#ifdef VERIFY_BOUNDS
    VB(r == port::LaterTricksMAX(t, L, depth, target, trump) && (!r || raw() == WR), "ltmax");
#endif
    if (r) {
      value = true;
      R = fill_up(WR);
      return true;
    }
  }
  return false;
#endif
}

// ---------------------------------------------------------------------------------------
// More sure-trick bounds at the start of a trick, after DDS's: rules from bridge-solver
// (github.com/macroxue/bridge-solver, MIT or Apache-2.0), solver.cc: Hanhong Xue's
// SlowTrumpTricks, TrumpLengthTricks, TopTrumpTricks and OtherSlowNoTrumpTricks, and Robert
// Salita's TrumpRankTricks, DrawTrumpsThenCash and CannotTakeAllNoTrump (from his fork's
// faster-search branch, github.com/BSalita/bridge-solver, since merged into bridge-solver).
// Each finds tricks one side surely takes and the cards its proof
// relies on: always the top cards of a suit down to some card (R, as DDS's winning ranks),
// together with the suit lengths and the leader, which the table key holds. Where
// bridge-solver records only some of the winners of a suit, all of them are kept here.
#ifndef BSB
#define BSB 1
#endif
namespace bsb {
inline u64 top_n(u64 set, int k) {  // the k highest cards of set (one lane)
  u64 r = 0;
  for (; k > 0 && set; k--) {
    const u64 c = highest(set);
    r |= c, set ^= c;
  }
  return r;
}
inline bool has(u64 set, u64 cards) { return (set & cards) == cards; }
inline bool kx(u64 h, u64 c) { return (h & c) && popc(h) >= 2; }  // c with at least one other card

// Slow trump tricks of the side holding my and pd (my on lead when `leading`): Kx behind the
// ace, Kx against a singleton ace, KQJ or KQ against the ace, Qxx behind or against AK.
struct Res { int tricks; u64 R; };
inline Res slow_trump(u64 allT, u64 my, u64 pd, u64 lho, u64 rho, bool leading, int n) {
  if (popc(allT) < 3) return {0, 0};
  const u64 a = highest(allT), k = highest(allT & ~a);
  if ((kx(pd, k) && (lho & a)) || (kx(my, k) && (rho & a) && (!leading || n >= 3))) return {1, a | k};
  if ((kx(pd, k) || kx(my, k)) && (rho == a || lho == a) && (!leading || n >= 3)) return {1, a | k};
  const u64 q = highest(allT & ~(a | k));
  if (popc(allT) >= 4) {
    const u64 j = highest(allT & ~(a | k | q));
    if (((lho | rho) & a) && has(my | pd, k | q | j) && (popc(my) >= 3 || popc(pd) >= 3)) return {2, a | k | q | j};
  }
  if (((lho | rho) & a) && has(my | pd, k | q) && (popc(my) >= 2 || popc(pd) >= 2)) return {1, a | k | q};
  if (popc(allT) >= 5)
    if (((pd & q) && popc(pd) >= 3 && has(lho, a | k)) || ((my & q) && popc(my) >= 3 && has(rho, a | k) && (!leading || n >= 4)))
      return {1, a | k | q};
  if ((((pd & q) && popc(pd) >= 3) || ((my & q) && popc(my) >= 3)) &&
      (lho == (a | k) || rho == (a | k) || (popc(lho) == 1 && popc(rho) == 1)) && (!leading || n >= 4))
    return {1, a | k | q};
  return {0, 0};
}
// Each trump of the longer hand wins a trick unless an opponent's trump is in it.
inline int trump_length(u64 my, u64 pd, u64 lho, u64 rho) {
  return std::max(popc(my), popc(pd)) - popc(lho) - popc(rho);
}
// The most trumps of `t` that higher trumps of `opp` can beat, each used once.
inline int trump_kills(u64 t, u64 opp) {
  int kills = 0;
  while (t) {
    const u64 low = lowest(t);
    const u64 higher = opp & above(low);
    if (!higher) break;
    kills++;
    t ^= low;
    opp = higher ^ lowest(higher);
  }
  return kills;
}
// Sure trump tricks of one hand by rank, whoever leads; pins as few top trumps as give `wanted`.
inline Res trump_rank(u64 allT, u64 t, u64 opp, int wanted) {
  const int n = popc(t);
  if (wanted <= 0 || n < wanted || n - trump_kills(t, opp) < wanted) return {0, 0};
  u64 rest = t;
  const int num_opp = popc(opp);
  for (int i = 1; i <= n; i++) {
    const u64 card = highest(rest);
    rest ^= card;
    const u64 pinned = allT & ge(card);
    u64 pinned_opp = opp & pinned;
    const int unpinned = n - i, unpinned_opp = num_opp - popc(pinned_opp);
    int kills = std::min(unpinned, unpinned_opp);
    for (int k = unpinned - kills; k > 0 && pinned_opp; k--) pinned_opp ^= lowest(pinned_opp), kills++;
    kills += trump_kills(t & pinned, pinned_opp);
    if (n - kills >= wanted) return {n - kills, pinned};
  }
  return {0, 0};
}
// The other side's top trumps, as many as its longer trump holding.
inline Res top_trump(u64 allT, u64 my, u64 pd) {
  if (my == allT) return {popc(my), 0};
  if (pd == allT) return {popc(pd), 0};
  const u64 both = my | pd;
  const int maxlen = std::max(popc(my), popc(pd));
  u64 R = 0, x = allT;
  int sure = 0;
  while (x && sure < maxlen) {
    const u64 c = highest(x);
    if (!(both & c)) break;
    sure++, R |= c, x ^= c;
  }
  return {sure, R};
}
inline int suit_fast(u64 ms, int mw, int maxrw, bool& entry, u64 ps, int pw) {
  if (ps && mw > 0 && highest(ms) > lowest(ps)) entry = true;
  if (pw == 0) return mw >= maxrw ? popc(ms) : mw;
  if (mw == 0) return ms ? pw : 0;
  if (highest(ms) < lowest(ps)) return pw;
  if (lowest(ms) > highest(ps)) return mw;
  if (pw == popc(ps)) pw--;
  return std::min(popc(ms), mw + pw);
}
// Fast tricks with no trumps in play, my on lead. R: every top card of a suit our side holds.
inline Res fast_nt(u64 my, u64 pd, u64 lho, u64 rho) {
  const u64 allc = my | pd | lho | rho;
  u64 R = 0, pdR = 0;
  int my_tricks = 0, pd_tricks = 0;
  bool my_entry = false, pd_entry = false;
  for (int s = 0; s < 4; s++) {
    const u64 ms = my & LANE(s), ps = pd & LANE(s);
    const int opp_max = std::max(popc(lho & LANE(s)), popc(rho & LANE(s)));
    const int my_max = std::max(popc(ps), opp_max), pd_max = std::max(popc(ms), opp_max);
    int mw = 0, pw = 0;
    u64 x = allc & LANE(s);
    while (x) {
      const u64 c = highest(x);
      x ^= c;
      if (ms & c) mw++, R |= c;
      else if (ps & c) pw++, pdR |= c;
      else break;
    }
    my_tricks += suit_fast(ms, mw, my_max, my_entry, ps, pw);
    pd_tricks += suit_fast(ps, pw, pd_max, pd_entry, ms, mw);
  }
  int fast = my_tricks;
  if (pd_entry) fast = std::max(my_tricks, pd_tricks), R |= pdR;
  return {std::min(fast, popc(my)), R};
}
// Draws the opponents' trumps in `rounds` rounds with the top trumps `run` (all ours), then
// counts the fast tricks of the hand left on lead.
inline Res draw_with_run(const u64* h4, int L, u64 run, int rounds, int n) {
  constexpr u64 T = LANE(3);
  u64 hand[2] = {h4[L], h4[L ^ 2]};
  int disc[2] = {0, 0}, leader = 0;
  for (int r = 0; r < rounds; r++) {
    const int f = 1 - leader;
    const u64 lt = hand[leader] & T, ft = hand[f] & T;
    if (!lt) return {0, 0};
    if (!ft) disc[f]++;
    if (const u64 lr = lt & run) {
      const u64 lc = lowest(lr);
      hand[leader] ^= lc;
      if (ft) {
        const u64 fc = lowest(ft);
        hand[f] ^= fc;
        if (fc > lc) leader = f;
      }
    } else {
      const u64 fr = ft & run;
      if (!fr) return {0, 0};
      hand[leader] ^= lowest(lt);
      hand[f] ^= lowest(fr);
      leader = f;
    }
  }
  for (int x = 0; x < 2; x++)
    for (; disc[x] > 0; disc[x]--) {
      int longest = -1, length = 0;
      for (int s = 0; s < 3; s++)
        if (popc(hand[x] & LANE(s)) > length) length = popc(hand[x] & LANE(s)), longest = s;
      if (longest < 0) return {0, 0};
      hand[x] ^= lowest(hand[x] & LANE(longest));
    }
  const Res f = fast_nt(hand[leader], hand[1 - leader], h4[(L + 1) & 3] & ~T, h4[(L + 3) & 3] & ~T);
  return {std::min(rounds + f.tricks, n), f.R | run};
}
inline Res draw_then_cash(const u64* h4, int L, u64 allT, int n) {
  constexpr u64 T = LANE(3);
  const int rounds = std::max(popc(h4[(L + 1) & 3] & T), popc(h4[(L + 3) & 3] & T));
  if (rounds == 0) return {0, 0};
  const u64 ours = (h4[L] | h4[L ^ 2]) & T;
  const u64 run = top_n(allT, rounds);
  if (run & ~ours) return {0, 0};
  const Res r = draw_with_run(h4, L, run, rounds, n);
  if (r.tricks > 0) return r;
  u64 long_run = 0, x = allT;
  for (int count = 0; x && count < 2 * rounds; count++) {
    const u64 c = highest(x);
    if (!(ours & c)) break;
    long_run |= c, x ^= c;
  }
  if (long_run == run) return {0, 0};
  return draw_with_run(h4, L, long_run, rounds, n);
}
// No trumps in play: the other side holds the top card of every suit the leader holds.
inline Res other_slow_nt(const u64* h4, int L, u64 all) {
  const u64 my = h4[L], ours = my | h4[L ^ 2];
  u64 R = 0;
  for (int s = 0; s < 4; s++) {
    if (!(my & LANE(s))) continue;
    const u64 top = highest(all & LANE(s));
    if (ours & top) return {0, 0};
    R |= top;
  }
  if (has(h4[(L + 1) & 3], R) || has(h4[(L + 3) & 3], R)) return {popc(R), R};
  return {1, R};
}
// No trumps in play: the leader's side cannot take all the tricks (bridge-solver's
// CannotTakeAllNoTrump: too short in the suits it tops, or held up by stoppers).
inline Res cannot_take_all_nt(const u64* h4, int L, u64 all, int n) {
  const int bs[2] = {(L + 1) & 3, (L + 3) & 3};
  const u64 a1 = h4[L], a2 = h4[L ^ 2], ac = a1 | a2;
  int lengths = 0, winners = 0, in_charge[2] = {0, 0};
  u64 topR = 0, stopR = 0;
  for (int s = 0; s < 4; s++) {
    const u64 alls = all & LANE(s);
    if (!alls) continue;
    const u64 top = highest(alls);
    if (!(ac & top)) {
      in_charge[(h4[bs[0]] & top) ? 0 : 1]++;
      topR |= top;
      continue;
    }
    const u64 as = ac & LANE(s);
    const int maxlen = std::max(popc(a1 & LANE(s)), popc(a2 & LANE(s)));
    lengths += maxlen;
    if (const u64 bsuit = alls & ~as) {
      const u64 stopper = highest(bsuit);
      const int holder = (h4[bs[0]] & stopper) ? 0 : 1;
      const u64 before = h4[(bs[holder] + 3) & 3];
      const u64 higher = as & above(stopper);
      if (has(before, higher) && popc(h4[bs[holder]] & LANE(s)) > popc(higher)) {
        winners += popc(higher);
        in_charge[holder]++;
        stopR |= alls & ge(stopper);
        continue;
      }
    }
    winners += maxlen;
  }
  if (lengths < n) return {1, topR};
  const int most = std::max(in_charge[0], in_charge[1]);
  if (most > 0 && winners + most <= n) return {1, topR | stopR};
  return {0, 0};
}
}  // namespace bsb

template <bool NT>
inline bool bs_bounds(int n, int L, int target, u64 all, u64& R, bool& value) {
  using namespace bsb;
  constexpr u64 T = LANE(3);
  const bool max_node = !(L & 1);
  const int needA = max_node ? target : n - target + 1, needB = n - needA + 1;
  const int P = L ^ 2, LH = (L + 1) & 3, RH = (L + 3) & 3;
  const u64 allT = NT ? 0 : all & T;
  const bool trumps = allT != 0;
  const u64 myT = H[L] & allT, pdT = H[P] & allT, lhoT = H[LH] & allT, rhoT = H[RH] & allT;
  auto decide = [&](bool a_wins, u64 r) {
    value = a_wins ? max_node : !max_node;
    R = fill_up(r);
    return true;
  };
  // The leader's side.
  if (trumps) {
    Res a = slow_trump(allT, myT, pdT, lhoT, rhoT, true, n);
    const int len = trump_length(myT, pdT, lhoT, rhoT);
    if (len > 0 && len >= a.tricks) a = {len, 0};
    if (a.tricks >= needA) return decide(true, a.R);
    const int gap = needA - a.tricks;
    if (gap <= 2)
      for (int seat : {L, P}) {
        const Res t = trump_rank(allT, H[seat] & allT, lhoT | rhoT, needA);
        if (t.tricks > 0) return decide(true, t.R);
      }
    if (gap <= 3) {
      const Res d = draw_then_cash(H, L, allT, n);
      if (d.tricks >= needA) return decide(true, d.R);
    }
  }
  // The other side.
  Res b = trumps ? top_trump(allT, lhoT, rhoT) : other_slow_nt(H, L, all);
  if (!trumps && b.tricks == 0 && needB == 1) b = cannot_take_all_nt(H, L, all, n);
  if (b.tricks == 0 && trumps) b = slow_trump(allT, lhoT, rhoT, pdT, myT, false, n);
  if (trumps) {
    const int len = trump_length(lhoT, rhoT, pdT, myT);
    if (len > 0 && len >= b.tricks) b = {len, 0};
    if (needB - b.tricks == 1)
      for (int seat : {LH, RH}) {
        const Res t = trump_rank(allT, H[seat] & allT, myT | pdT, needB);
        if (t.tricks > 0) {
          b = t;
          break;
        }
      }
  }
  if (b.tricks >= needB) return decide(false, b.R);
  return false;
}

// Extra weight for the table's best lead, on top of what DDS's weights give it (fewer nodes: 1.5%).
#ifndef HX
#define HX 50
#endif
// The cards hand (L + K) could play (one per run) with DDS's weights as sort keys.
template <bool NT>
inline int dds_order(int n, int K, int L, u64 hint, u64* cards, uint16_t* key) {
  port::Pos& t = S->pos[n];
  const int trump = NT ? 4 : 3, cur = (L + K) & 3;
  port::MoveType mply[14];
  int lead_suit = 0, move_rank[3] = {0, 0, 0}, move_suit[3] = {0, 0, 0}, high[3] = {0, 0, 0};
  for (int k = 0; k < K; k++) {
    const int b = __builtin_ctzll(S->trk[n][k]);
    const int su = b >> 4, r = (b & 15) + 2;
    if (k == 0) {
      lead_suit = su;
      move_rank[0] = r;
      move_suit[0] = su;
    } else if ((su == move_suit[k - 1] && r > move_rank[k - 1]) || (su != move_suit[k - 1] && su == trump)) {
      move_rank[k] = r;
      move_suit[k] = su;
      high[k] = k;
    } else {
      move_rank[k] = move_rank[k - 1];
      move_suit[k] = move_suit[k - 1];
      high[k] = high[k - 1];
    }
  }
  const port::MoveType none{};
  port::MoveType best = none, best_tt = none;
  if (K == 0) {
    const u64 kc = S->killer[n][0] & H[cur];
    const u64 b1 = hint, b2 = kc;
    if (b1) best = port::MoveType{suit_of(b1), (__builtin_ctzll(b1) & 15) + 2, 0, 0};
    if (b2) best_tt = port::MoveType{suit_of(b2), (__builtin_ctzll(b2) & 15) + 2, 0, 0};
  }
  int num_moves = 0;
  port::HeuristicContext ctx{t, best, best_tt, mply, 0, 0, trump, 0, n - 1, cur, L, lead_suit};
  const int* removed = S->removed[n];
  for (int l = 0; l < 4; l++) ctx.removed_ranks[l] = removed[l];
  if (K >= 1) ctx.lead0_rank = move_rank[0];
  if (K >= 2) ctx.move1_rank = move_rank[1], ctx.move1_suit = move_suit[1], ctx.high1 = high[1];
  if (K >= 3) ctx.move2_rank = move_rank[2], ctx.move2_suit = move_suit[2], ctx.high2 = high[2];
  // One move per run of touching cards (top card first within a suit), as DDS generates them;
  // `sequence` only says whether the run has more cards (all the weights ask).
  u64 allx = H[0] | H[1] | H[2] | H[3];
  for (int k = 0; k < K; k++) allx |= S->trk[n][k];
  u64 seq_all;
  const u64 hd_all = ddport::run_heads(H[cur], allx, &seq_all);
  auto generate = [&](int l) {
    u64 hd = hd_all & LANE(l);
    while (hd) {
      const u64 c = highest(hd);
      hd ^= c;
      mply[num_moves++] = port::MoveType{l, int(__builtin_ctzll(c) & 15) + 2, int((c & seq_all) != 0), 0};
    }
  };
  const int trump_winner = (!NT && t.winner[3].rank != 0) ? 1 : 0;
  if (K > 0 && t.rank_in_suit[cur][lead_suit] != 0) {
    generate(lead_suit);
    ctx.suit = lead_suit;
    ctx.num_moves = num_moves;
    if (num_moves > 1) port::call_heuristic(ctx, static_cast<port::WeightCase>(4 * K + trump_winner));
  } else {
    const port::WeightCase wc = K == 0 ? (trump_winner ? port::WeightCase::Trump0 : port::WeightCase::Nt0)
                                       : static_cast<port::WeightCase>(4 * K + trump_winner + 2);
    for (int l = 0; l < 4; l++) {
      if (!t.rank_in_suit[cur][l]) continue;
      ctx.last_num_moves = num_moves;
      generate(l);
      ctx.suit = l;
      ctx.num_moves = num_moves;
      port::call_heuristic(ctx, wc);
    }
  }
  for (int i = 0; i < num_moves; i++) {
    cards[i] = 1ull << (16 * mply[i].suit + mply[i].rank - 2);
    int w = mply[i].weight + 2000 + ((cards[i] & hint) ? HX : 0);
    w = w < 0 ? 0 : w > 4095 ? 4095 : w;
    key[i] = uint16_t(w * 16 + (15 - i));
  }
  return num_moves;
}

// DDS's weights for the opening card of a trick (weight_alloc_trump0 and weight_alloc_nt0),
// computed on the hand words: the terms that depend only on the suit once per suit, then a
// few per card. Gives the same cards and keys as dds_order with K = 0 (checked with
// -DVERIFY_ORDER), without the port's position.
template <bool NT>
inline int native_order_scalar(int n, int L, u64 hint, u64 all, u64* cards, uint16_t* key) {
  constexpr u64 TRL = LANE(3);
  const int P = L ^ 2, A = (L + 1) & 3, B = (L + 3) & 3;
  const u64 hl = H[L], hp = H[P], ha = H[A], hb = H[B];
  const bool trumps = !NT && (all & TRL);  // DDS uses the no-trump weights once no trump is left
  const u64 kc = S->killer[n][0] & hl;
  u64 seq_all;
  const u64 hd_all = ddport::run_heads(hl, all, &seq_all);
  const u64 tP = hp & TRL, tA = ha & TRL, tB = hb & TRL;
  int nm = 0;
  for (int l = 0; l < 4; l++) {
    const u64 lane = LANE(l);
    u64 hd = hd_all & lane;
    if (!hd) continue;
    const u64 x = all & lane;
    const u64 top1 = highest(x), x2 = x ^ top1, top2 = x2 ? highest(x2) : 0, x3 = x2 ^ top2, top3 = x3 ? highest(x3) : 0;
    const u64 sl = hl & lane, sp = hp & lane, sa = ha & lane, sb = hb & lane;
    const int lenA = popc(sa), lenB = popc(sb), lenP = popc(sp);
    const int cA = (lenA == 0 ? n : lenA) << 2, cB = (lenB == 0 ? n : lenB) << 2;
    const bool w1L = top1 & hl, w1P = top1 & hp, w1A = top1 & ha, w1B = top1 & hb;
    const bool w2L = top2 & hl, w2P = top2 & hp, w2A = top2 & ha, w2B = top2 & hb;
    const bool h3L = top3 & hl, h3P = top3 & hp;
    const bool sing = (lenA == 1 && w1A) || (lenB == 1 && w1B);
    const bool p_over = sp > (sa | sb);
    const bool third13 = (w2L && h3P && lenP > 1) || (w2P && h3L && lenP > 1);
    if (trumps) {
      const bool side = l != 3;
      int bonus = 0;
      if (side && ((lenA == 0 && tA) || (lenB == 0 && tB))) bonus = -12;
      if (side && lenP == 0 && tP && lenB > 0) bonus += 17;
      if (w1B || w2B) {
        if (lenB != 1) bonus -= 12;
      } else if (w1A && w2P) {
        if (lenP != 1) bonus += 27;
      }
      if (side && popc(sl) == 1 && (hl & TRL) && lenP > 1 && w1P) bonus += 19;
      const int swd = bonus - (((cA + cB) << 5) / 13);
      const bool opp_follow = (lenA != 0 || !tA) && (lenB != 0 || !tB);
      // Whether the trick is won: leading the top card, or any other card.
      bool win_top, win_other;
      if (!side) {
        win_top = true;
        win_other = p_over;
      } else {
        if (lenP != 0 || !tP) win_top = opp_follow;
        else win_top = (lenA != 0 || tP > tA) && (lenB != 0 || tP > tB);
        if (p_over) win_other = opp_follow;
        else if (lenP == 0 && tP) {
          const bool ra = lenA == 0 && tA, rb = lenB == 0 && tB;
          win_other = ra && rb ? tP > (tA | tB) : ra ? tP > tA : rb ? tP > tB : true;
        } else win_other = false;
      }
      const int d2 = swd + ((w2P && h3P) ? 20 : third13 ? 13 : 0);
      while (hd) {
        const u64 c = highest(hd);
        hd ^= c;
        const bool is_top = c == top1, seq = c & seq_all;
        const int rr = popc(x & (0 - c)), rank = int(__builtin_ctzll(c) & 15) + 2;
        int w;
        if (is_top ? win_top : win_other) {
          if (sing) w = swd + 35 + rr;
          else if (w1L) w = w2P ? swd + 48 + rr : is_top ? swd + 31 : swd - 3 + rr;
          else if (w1P) w = w2L ? swd + 42 + rr : swd + 28 + rr;
          else if (seq && c == top2) w = swd + 40;
          else if (seq) w = swd + 22 + rr;
          else w = swd + 11 + rr;
          if (c == hint) w += 55;
          else if (c == kc) w += 18;
        } else {
          if (sing) w = d2 + rr + 2;
          else if (w1L) w = w2P ? d2 + 33 + rr : is_top ? d2 + 38 : d2 - 14 + rr;
          else if (w1P) w = d2 + 34 + rr;
          else if (seq && c == top2) w = d2 + 35;
          else w = d2 + 17 - rank;
          if (c == hint) w += 18;
        }
        w += 2000 + (c == hint ? HX : 0);
        w = w < 0 ? 0 : w > 4095 ? 4095 : w;
        cards[nm] = c;
        key[nm] = uint16_t(w * 16 + (15 - nm));
        nm++;
      }
    } else {
      const int swd = -(((cA + cB) << 5) / 19) + (lenP == 0 ? -9 : 0);
      int dw = swd;  // when the trick can be won
      if (w2B) dw += lenB != 1 ? -1 : 0;
      else if (w2A) dw += lenA != 1 ? 22 : 16;
      const int addw = ((!w2A || lenA == 1) && (!w2B || lenB == 1)) ? 45 : 18;
      int dn = swd;  // when it cannot
      if (w1B || w2B) dn += lenB != 1 ? -10 : 0;
      else if (w1A && w2P) dn += lenP != 1 ? 31 : 0;
      dn += (w2P && h3P) ? 35 : third13 ? 25 : 0;
      while (hd) {
        const u64 c = highest(hd);
        hd ^= c;
        const bool seq = c & seq_all;
        const int rr = popc(x & (0 - c));
        int w;
        if (c == top1 || p_over) {
          w = dw + addw + rr;
          if (c == hint) w += 126;
          else if (c == kc) w += 32;
        } else {
          if (sing) w = dn + 28 + rr;
          else if (w1L) w = dn - 17 + rr;
          else if (!seq) w = dn + 12 + rr;
          else if (c == top2) w = dn + 48;
          else w = dn + 29 - rr;
          if (c == hint) w += 47;
          else if (c == kc) w += 19;
        }
        w += 2000 + (c == hint ? HX : 0);
        w = w < 0 ? 0 : w > 4095 ? 4095 : w;
        cards[nm] = c;
        key[nm] = uint16_t(w * 16 + (15 - nm));
        nm++;
      }
    }
  }
  return nm;
}

#if DDPORT_HWY
// native_order with Highway: the per-suit part as in the AVX-512 version below (one 32-bit lane
// per suit in 128-bit vectors), then a branch-free scalar loop over the cards, which looks up
// each card's class in per-suit tables (in place of AVX-512's compress and permutes).
template <bool NT>
inline int native_order_hwy(int n, int L, u64 hint, u64 all, u64* cards, uint16_t* key) {
  namespace hn = hwy::HWY_NAMESPACE;
  const hn::Full128<int32_t> di;
  const hn::Full128<uint32_t> du;
  using V = hn::Vec<decltype(di)>;
  using M = hn::Mask<decltype(di)>;
  constexpr u64 TRL = LANE(3);
  const int P = L ^ 2, A = (L + 1) & 3, B = (L + 3) & 3;
  const u64 hl = H[L], hp = H[P], ha = H[A], hb = H[B];
  const bool trumps = !NT && (all & TRL);
  const u64 kc = S->killer[n][0] & hl;
  u64 seq_all;
  const u64 hd_all = ddport::run_heads(hl, all, &seq_all);
  auto vs = [&](int x) { return hn::Set(di, x); };
  auto ld = [&](u64 h) {
    return hn::BitCast(di, hn::PromoteTo(du, hn::BitCast(hn::Full64<uint16_t>(), hn::Set(hn::Full64<uint64_t>(), h))));
  };
  const auto top = hn::Set(du, 0x80000000u), c31 = hn::Set(du, 31u);
  auto hib = [&](V v) {  // the highest bit of each lane (0 for 0); shift counts stay below 32
    const auto u = hn::BitCast(du, v);
    return hn::BitCast(di, hn::And(u, hn::Shr(top, hn::Min(hn::LeadingZeroCount(u), c31))));
  };
  auto sel = [](M m, V a, V b) { return hn::IfThenElse(m, a, b); };  // m ? a : b
  auto eq = [&](V a, int k) { return hn::Eq(a, hn::Set(di, k)); };
  auto tst = [&](V a, V b) { return hn::Ne(hn::And(a, b), hn::Zero(di)); };
  auto pc = [&](V v) { return hn::BitCast(di, hn::PopulationCount(hn::BitCast(du, v))); };
  auto mk = [&](bool b) { return hn::MaskFromVec(hn::Set(di, b ? -1 : 0)); };
  auto add = [&](V a, int k) { return hn::Add(a, hn::Set(di, k)); };
  auto div = [&](V a, int mul) { return hn::BitCast(di, hn::ShiftRight<18>(hn::BitCast(du, hn::Mul(a, hn::Set(di, mul))))); };
  auto AND = [](M a, M b) { return hn::And(a, b); };
  auto OR = [](M a, M b) { return hn::Or(a, b); };
  auto NOT = [](M a) { return hn::Not(a); };
  auto ANDN = [](M a, M b) { return hn::AndNot(b, a); };  // a & ~b
  const V X = ld(all), VL = ld(hl), VP = ld(hp), VA = ld(ha), VB = ld(hb);
  const V lenA = pc(VA), lenB = pc(VB), lenP = pc(VP);
  const V top1 = hib(X), X2 = hn::Xor(X, top1), top2 = hib(X2), top3 = hib(hn::Xor(X2, top2));
  const M w1L = tst(top1, VL), w1P = tst(top1, VP), w1A = tst(top1, VA), w1B = tst(top1, VB);
  const M w2L = tst(top2, VL), w2P = tst(top2, VP), w2A = tst(top2, VA), w2B = tst(top2, VB);
  const M h3L = tst(top3, VL), h3P = tst(top3, VP);
  const M lA0 = eq(lenA, 0), lB0 = eq(lenB, 0), lP0 = eq(lenP, 0), lA1 = eq(lenA, 1), lB1 = eq(lenB, 1), lP1 = eq(lenP, 1);
  const M lPg1 = hn::Gt(lenP, vs(1));
  const M sing = OR(AND(lA1, w1A), AND(lB1, w1B));
  const M p_over = hn::RebindMask(di, hn::Gt(hn::BitCast(du, VP), hn::BitCast(du, hn::Or(VA, VB))));
  const M third13 = AND(OR(AND(w2L, h3P), AND(w2P, h3L)), lPg1);
  const V cAB = hn::ShiftLeft<5>(hn::Add(hn::ShiftLeft<2>(sel(lA0, vs(n), lenA)), hn::ShiftLeft<2>(sel(lB0, vs(n), lenB))));
  V B0, B1, B2, B3, R0, R1, R2, R3, K2, K3, HW, HN, KW, KN;
  M W0 = mk(true), Wo = mk(false);
  const V zero = vs(0), one = vs(1);
  if (trumps) {
    const M side = hn::FirstN(di, 3);
    const M mtA = mk(ha & TRL), mtB = mk(hb & TRL), mtP = mk(hp & TRL);
    const u64 tP = hp & TRL, tA = ha & TRL, tB = hb & TRL;
    const M mPA = mk(tP > tA), mPB = mk(tP > tB), mPAB = mk(tP > (tA | tB));
    const M ra = AND(lA0, mtA), rb = AND(lB0, mtB);
    V bonus = sel(AND(side, OR(ra, rb)), vs(-12), zero);
    bonus = hn::Add(bonus, sel(ANDN(AND(AND(side, lP0), mtP), lB0), vs(17), zero));
    const M wB = OR(w1B, w2B);
    bonus = hn::Add(bonus, sel(ANDN(wB, lB1), vs(-12), sel(ANDN(AND(ANDN(w1A, wB), w2P), lP1), vs(27), zero)));
    const M lL1 = eq(pc(VL), 1);
    bonus = hn::Add(bonus, sel(AND(AND(AND(side, lL1), mk(hl & TRL)), AND(lPg1, w1P)), vs(19), zero));
    const V swd = hn::Sub(bonus, div(cAB, 20165));  // /13
    const M opp_follow = NOT(OR(ra, rb));
    const M m1 = NOT(AND(lP0, mtP));
    const M wt_side = OR(AND(m1, opp_follow), ANDN(AND(OR(NOT(lA0), mPA), OR(NOT(lB0), mPB)), m1));
    W0 = OR(NOT(side), wt_side);
    const M rcase = OR(OR(AND(AND(ra, rb), mPAB), AND(ANDN(ra, rb), mPA)), OR(AND(ANDN(rb, ra), mPB), opp_follow));
    const M wo_side = OR(AND(p_over, opp_follow), ANDN(AND(AND(lP0, mtP), rcase), p_over));
    Wo = OR(ANDN(p_over, side), AND(side, wo_side));
    const V d2 = hn::Add(swd, sel(AND(w2P, h3P), vs(20), sel(third13, vs(13), zero)));
    B0 = sel(W0, sel(w2P, add(swd, 48), add(swd, 31)), sel(w2P, add(d2, 33), add(d2, 38)));
    R0 = sel(w2P, one, zero);
    const M Sm = OR(OR(sing, w1L), w1P);
    const V BSw = sel(sing, add(swd, 35), sel(w1L, sel(w2P, add(swd, 48), add(swd, -3)), sel(w2L, add(swd, 42), add(swd, 28))));
    const V BSn = sel(sing, add(d2, 2), sel(w1L, sel(w2P, add(d2, 33), add(d2, -14)), add(d2, 34)));
    const V BS = sel(Wo, BSw, BSn);
    B1 = sel(Sm, BS, sel(Wo, add(swd, 40), add(d2, 35)));
    R1 = sel(Sm, one, zero);
    B2 = sel(Sm, BS, sel(Wo, add(swd, 22), add(d2, 17)));
    B3 = sel(Sm, BS, sel(Wo, add(swd, 11), add(d2, 17)));
    R2 = R3 = sel(OR(Sm, Wo), one, zero);
    K2 = K3 = sel(OR(Sm, Wo), zero, vs(-1));
    HW = vs(55 + HX), HN = vs(18 + HX), KW = vs(18), KN = zero;
  } else {
    const V swd = hn::Sub(sel(lP0, vs(-9), zero), div(cAB, 13798));  // /19
    const V dw = hn::Add(swd, sel(w2B, sel(lB1, zero, vs(-1)), sel(w2A, sel(lA1, vs(16), vs(22)), zero)));
    const V addw = sel(AND(OR(NOT(w2A), lA1), OR(NOT(w2B), lB1)), vs(45), vs(18));
    V dn = hn::Add(swd, sel(OR(w1B, w2B), sel(lB1, zero, vs(-10)), sel(AND(w1A, w2P), sel(lP1, zero, vs(31)), zero)));
    dn = hn::Add(dn, sel(AND(w2P, h3P), vs(35), sel(third13, vs(25), zero)));
    const V bw = hn::Add(dw, addw);
    W0 = mk(true), Wo = p_over;
    const M Sm = OR(sing, w1L);
    const V Snt = sel(sing, add(dn, 28), add(dn, -17));
    B0 = bw, R0 = one;
    B1 = sel(p_over, bw, sel(Sm, Snt, add(dn, 48)));
    R1 = sel(OR(p_over, Sm), one, zero);
    B2 = sel(p_over, bw, sel(Sm, Snt, add(dn, 29)));
    R2 = sel(OR(p_over, Sm), one, vs(-1));
    B3 = sel(p_over, bw, sel(Sm, Snt, add(dn, 12)));
    R3 = one;
    K2 = K3 = zero;
    HW = vs(126 + HX), HN = vs(47 + HX), KW = vs(32), KN = vs(19);
  }
  alignas(16) int32_t tb[16], tr[16], tk[16], th[16], tkb[16];
  const V c2000 = vs(2000);
  hn::Store(hn::Add(B0, c2000), di, tb), hn::Store(hn::Add(B1, c2000), di, tb + 4);
  hn::Store(hn::Add(B2, c2000), di, tb + 8), hn::Store(hn::Add(B3, c2000), di, tb + 12);
  hn::Store(R0, di, tr), hn::Store(R1, di, tr + 4), hn::Store(R2, di, tr + 8), hn::Store(R3, di, tr + 12);
  hn::Store(zero, di, tk), hn::Store(zero, di, tk + 4), hn::Store(K2, di, tk + 8), hn::Store(K3, di, tk + 12);
  const V hO = sel(Wo, HW, HN), kO = sel(Wo, KW, KN);
  hn::Store(sel(W0, HW, HN), di, th), hn::Store(hO, di, th + 4), hn::Store(hO, di, th + 8), hn::Store(hO, di, th + 12);
  hn::Store(sel(W0, KW, KN), di, tkb), hn::Store(kO, di, tkb + 4), hn::Store(kO, di, tkb + 8), hn::Store(kO, di, tkb + 12);
  int nm = 0;
  for (int l = 0; l < 4; l++) {
    u64 h = hd_all & LANE(l);
    const u64 x = all & LANE(l);
    while (h) {
      const int b = 63 - __builtin_clzll(h);
      const u64 c = 1ull << b;
      h ^= c;
      const int rr = popc(x >> b);  // c and the cards above it
      const int sq = int(seq_all >> b) & 1;
      const int cls = rr == 1 ? 0 : sq ? (rr == 2 ? 1 : 2) : 3;
      const int idx = 4 * cls + l;
      int w = tb[idx] + tr[idx] * rr + tk[idx] * ((b & 15) + 2);
      w += c == hint ? th[idx] : c == kc ? tkb[idx] : 0;
      w = w < 0 ? 0 : w > 4095 ? 4095 : w;
      cards[nm] = c;
      key[nm] = uint16_t(w * 16 + (15 - nm));
      nm++;
    }
  }
  return nm;
}
#endif

// native_order with the per-suit work done for all four suits at once (one 32-bit vector
// lane per suit): for each suit and card class (top card, second card heading a sequence,
// other sequence head, other card) a base weight, its coefficients on rr and rank, and the
// hint and killer bonuses. The card loop then only classifies each card. Same weights as
// native_order_scalar.
template <bool NT>
inline int native_order(int n, int L, u64 hint, u64 all, u64* cards, uint16_t* key) {
#if defined(OLD_ORDER)
  return native_order_scalar<NT>(n, L, hint, all, cards, key);
#elif DDPORT_HWY
  return native_order_hwy<NT>(n, L, hint, all, cards, key);
#else
  using V = __m128i;
  using M = __mmask8;
  constexpr u64 TRL = LANE(3);
  const int P = L ^ 2, A = (L + 1) & 3, B = (L + 3) & 3;
  const u64 hl = H[L], hp = H[P], ha = H[A], hb = H[B];
  const bool trumps = !NT && (all & TRL);
  const u64 kc = S->killer[n][0] & hl;
  u64 seq_all;
  const u64 hd_all = ddport::run_heads(hl, all, &seq_all);
  auto vs = [](int x) { return _mm_set1_epi32(x); };
  auto ld = [](u64 h) { return _mm_cvtepu16_epi32(_mm_cvtsi64_si128((long long)h)); };
  auto hib = [](V v) { return _mm_srlv_epi32(_mm_set1_epi32(int(0x80000000u)), _mm_lzcnt_epi32(v)); };
  auto sel = [](M m, V a, V b) { return _mm_mask_blend_epi32(m, b, a); };  // m ? a : b
  auto eq = [](V a, int k) { return M(_mm_cmpeq_epi32_mask(a, _mm_set1_epi32(k))); };
  auto tst = [](V a, V b) { return M(_mm_test_epi32_mask(a, b)); };
  const V X = ld(all), VL = ld(hl), VP = ld(hp), VA = ld(ha), VB = ld(hb);
  const V lenA = _mm_popcnt_epi32(VA), lenB = _mm_popcnt_epi32(VB), lenP = _mm_popcnt_epi32(VP);
  const V top1 = hib(X), X2 = _mm_xor_si128(X, top1), top2 = hib(X2), top3 = hib(_mm_xor_si128(X2, top2));
  const M w1L = tst(top1, VL), w1P = tst(top1, VP), w1A = tst(top1, VA), w1B = tst(top1, VB);
  const M w2L = tst(top2, VL), w2P = tst(top2, VP), w2A = tst(top2, VA), w2B = tst(top2, VB);
  const M h3L = tst(top3, VL), h3P = tst(top3, VP);
  const M lA0 = eq(lenA, 0), lB0 = eq(lenB, 0), lP0 = eq(lenP, 0), lA1 = eq(lenA, 1), lB1 = eq(lenB, 1), lP1 = eq(lenP, 1);
  const M lPg1 = M(_mm_cmpgt_epi32_mask(lenP, vs(1)));
  const M sing = (lA1 & w1A) | (lB1 & w1B);
  const M p_over = M(_mm_cmpgt_epu32_mask(VP, _mm_or_si128(VA, VB)));
  const M third13 = ((w2L & h3P) | (w2P & h3L)) & lPg1;
  const V cAB = _mm_slli_epi32(_mm_add_epi32(_mm_slli_epi32(sel(lA0, vs(n), lenA), 2), _mm_slli_epi32(sel(lB0, vs(n), lenB), 2)), 5);
  V B0, B1, B2, B3, R0, R1, R2, R3, K2, K3, HW, HN, KW, KN;
  M W0, Wo;
  const V zero = _mm_setzero_si128(), one = vs(1);
  if (trumps) {
    const M side = 0x7, F = 0xF;
    const M mtA = (ha & TRL) ? F : 0, mtB = (hb & TRL) ? F : 0, mtP = (hp & TRL) ? F : 0;
    const u64 tP = hp & TRL, tA = ha & TRL, tB = hb & TRL;
    const M mPA = tP > tA ? F : 0, mPB = tP > tB ? F : 0, mPAB = tP > (tA | tB) ? F : 0;
    const M ra = lA0 & mtA, rb = lB0 & mtB;
    V bonus = sel(side & (ra | rb), vs(-12), zero);
    bonus = _mm_add_epi32(bonus, sel(side & lP0 & mtP & ~lB0, vs(17), zero));
    const M wB = w1B | w2B;
    bonus = _mm_add_epi32(bonus, sel(wB & ~lB1, vs(-12), sel(~wB & w1A & w2P & ~lP1, vs(27), zero)));
    const M lL1 = eq(_mm_popcnt_epi32(VL), 1);
    bonus = _mm_add_epi32(bonus, sel(side & lL1 & ((hl & TRL) ? F : 0) & lPg1 & w1P, vs(19), zero));
    const V swd = _mm_sub_epi32(bonus, _mm_srli_epi32(_mm_mullo_epi32(cAB, vs(20165)), 18));  // /13
    const M opp_follow = ~ra & ~rb;
    const M m1 = ~lP0 | ~mtP;
    const M wt_side = (m1 & opp_follow) | (~m1 & (~lA0 | mPA) & (~lB0 | mPB));
    W0 = (~side | wt_side) & F;
    const M rcase = (ra & rb & mPAB) | (ra & ~rb & mPA) | (~ra & rb & mPB) | (~ra & ~rb);
    const M wo_side = (p_over & opp_follow) | (~p_over & lP0 & mtP & rcase);
    Wo = ((~side & p_over) | (side & wo_side)) & F;
    const V d2 = _mm_add_epi32(swd, sel(w2P & h3P, vs(20), sel(third13, vs(13), zero)));
    auto add = [](V a, int k) { return _mm_add_epi32(a, _mm_set1_epi32(k)); };
    B0 = sel(W0, sel(w2P, add(swd, 48), add(swd, 31)), sel(w2P, add(d2, 33), add(d2, 38)));
    R0 = sel(w2P, one, zero);
    const M Sm = sing | w1L | w1P;
    const V BSw = sel(sing, add(swd, 35), sel(w1L, sel(w2P, add(swd, 48), add(swd, -3)), sel(w2L, add(swd, 42), add(swd, 28))));
    const V BSn = sel(sing, add(d2, 2), sel(w1L, sel(w2P, add(d2, 33), add(d2, -14)), add(d2, 34)));
    const V BS = sel(Wo, BSw, BSn);
    B1 = sel(Sm, BS, sel(Wo, add(swd, 40), add(d2, 35)));
    R1 = sel(Sm, one, zero);
    B2 = sel(Sm, BS, sel(Wo, add(swd, 22), add(d2, 17)));
    B3 = sel(Sm, BS, sel(Wo, add(swd, 11), add(d2, 17)));
    R2 = R3 = sel(Sm | Wo, one, zero);
    K2 = K3 = sel(Sm | Wo, zero, vs(-1));
    HW = vs(55 + HX), HN = vs(18 + HX), KW = vs(18), KN = zero;
  } else {
    auto add = [](V a, int k) { return _mm_add_epi32(a, _mm_set1_epi32(k)); };
    const V swd = _mm_sub_epi32(sel(lP0, vs(-9), zero), _mm_srli_epi32(_mm_mullo_epi32(cAB, vs(13798)), 18));  // /19
    const V dw = _mm_add_epi32(swd, sel(w2B, sel(lB1, zero, vs(-1)), sel(w2A, sel(lA1, vs(16), vs(22)), zero)));
    const V addw = sel((~w2A | lA1) & (~w2B | lB1), vs(45), vs(18));
    V dn = _mm_add_epi32(swd, sel(w1B | w2B, sel(lB1, zero, vs(-10)), sel(w1A & w2P, sel(lP1, zero, vs(31)), zero)));
    dn = _mm_add_epi32(dn, sel(w2P & h3P, vs(35), sel(third13, vs(25), zero)));
    const V bw = _mm_add_epi32(dw, addw);
    W0 = 0xF, Wo = p_over;
    const M Sm = sing | w1L;
    const V Snt = sel(sing, add(dn, 28), add(dn, -17));
    B0 = bw, R0 = one;
    B1 = sel(p_over, bw, sel(Sm, Snt, add(dn, 48)));
    R1 = sel(p_over | Sm, one, zero);
    B2 = sel(p_over, bw, sel(Sm, Snt, add(dn, 29)));
    R2 = sel(p_over | Sm, one, vs(-1));
    B3 = sel(p_over, bw, sel(Sm, Snt, add(dn, 12)));
    R3 = one;
    K2 = K3 = zero;
    HW = vs(126 + HX), HN = vs(47 + HX), KW = vs(32), KN = vs(19);
  }
  // The cards, one per 32-bit lane, in the order suit up, rank down: bit-reverse each suit,
  // then compress the bit positions.
  using Z = __m512i;
  auto wide4 = [](V a, V b, V c, V d) {
    return _mm512_inserti32x4(_mm512_inserti32x4(_mm512_inserti32x4(_mm512_castsi128_si512(a), b, 1), c, 2), d, 3);
  };
  const V hv = _mm_cvtsi64_si128((long long)hd_all);
  const V rv = _mm_shuffle_epi8(_mm_gf2p8affine_epi64_epi8(hv, _mm_set1_epi64x(0x8040201008040201ll), 0), _mm_set_epi8(15, 14, 13, 12, 11, 10, 9, 8, 6, 7, 4, 5, 2, 3, 0, 1));
  const u64 rmask = u64(_mm_cvtsi128_si64(rv));
  const int nm = popc(hd_all);
  const Z iota8 = _mm512_set_epi8(63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
                                  31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  const Z q = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(_mm512_maskz_compress_epi8(rmask, iota8)));
  const __mmask16 live = __mmask16((1u << nm) - 1);
  const Z f15 = _mm512_set1_epi32(15);
  const Z pos = _mm512_xor_si512(q, f15), su = _mm512_srli_epi32(q, 4), rk = _mm512_and_si512(pos, f15);
  const Z rr = _mm512_popcnt_epi32(_mm512_srlv_epi32(_mm512_permutexvar_epi32(su, _mm512_castsi128_si512(X)), rk));
  const __mmask16 seq = _mm512_test_epi32_mask(_mm512_srlv_epi32(_mm512_permutexvar_epi32(su, _mm512_castsi128_si512(ld(seq_all))), rk), _mm512_set1_epi32(1));
  const __mmask16 r1 = _mm512_cmpeq_epi32_mask(rr, _mm512_set1_epi32(1)), r2 = _mm512_cmpeq_epi32_mask(rr, _mm512_set1_epi32(2));
  // class: 0 top card, 1 second card heading a sequence, 2 other sequence head, 3 other
  Z cls = _mm512_mask_blend_epi32(seq, _mm512_set1_epi32(12), _mm512_mask_blend_epi32(r2, _mm512_set1_epi32(8), _mm512_set1_epi32(4)));
  cls = _mm512_maskz_mov_epi32(__mmask16(~r1), cls);
  const Z idx = _mm512_add_epi32(cls, su);
  const Z c2000 = _mm512_set1_epi32(2000);
  const Z tb = _mm512_add_epi32(wide4(B0, B1, B2, B3), c2000), tr = wide4(R0, R1, R2, R3), tk = wide4(zero, zero, K2, K3);
  const V hO = sel(Wo, HW, HN), kO = sel(Wo, KW, KN);
  const Z th = wide4(sel(W0, HW, HN), hO, hO, hO), tkb = wide4(sel(W0, KW, KN), kO, kO, kO);
  Z w = _mm512_add_epi32(_mm512_permutexvar_epi32(idx, tb), _mm512_mullo_epi32(_mm512_permutexvar_epi32(idx, tr), rr));
  w = _mm512_add_epi32(w, _mm512_mullo_epi32(_mm512_permutexvar_epi32(idx, tk), _mm512_add_epi32(rk, _mm512_set1_epi32(2))));
  const int hp_ = hint ? int(__builtin_ctzll(hint)) : -1, kp_ = kc ? int(__builtin_ctzll(kc)) : -1;
  const __mmask16 mh = _mm512_cmpeq_epi32_mask(pos, _mm512_set1_epi32(hp_)), mk = _mm512_cmpeq_epi32_mask(pos, _mm512_set1_epi32(kp_)) & ~mh;
  Z bonus = _mm512_maskz_permutexvar_epi32(mk, idx, tkb);
  bonus = _mm512_mask_permutexvar_epi32(bonus, mh, idx, th);
  w = _mm512_min_epi32(_mm512_max_epi32(_mm512_add_epi32(w, bonus), _mm512_setzero_si512()), _mm512_set1_epi32(4095));
  const Z kz = _mm512_maskz_or_epi32(live, _mm512_slli_epi32(w, 4), _mm512_sub_epi32(f15, _mm512_set_epi32(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)));
  _mm256_store_si256((__m256i*)key, _mm512_cvtepi32_epi16(kz));
  const Z one64 = _mm512_set1_epi64(1);
  _mm512_store_si512(cards, _mm512_maskz_sllv_epi64(__mmask8(live), one64, _mm512_cvtepu32_epi64(_mm512_castsi512_si256(pos))));
  _mm512_store_si512(cards + 8, _mm512_maskz_sllv_epi64(__mmask8(live >> 8), one64, _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(pos, 1))));
  return nm;
#endif
}

// DDS's QuickTricksSecondHand on the hand words: can the second hand's side win this trick
// (and, at no trumps, further top tricks) and reach what it needs? `s` is the second hand,
// lc the card led (already out of the leader's hand), need the tricks its side needs.
template <bool NT>
inline bool native_second(int s, u64 lc, int need, u64& R) {
  constexpr u64 TRL = LANE(3);
  const int sp = s ^ 2, lh = (s + 1) & 3;
  const u64 lane = lane_of(lc);
  const u64 hs = H[s], hp = H[sp], hl = H[lh];
  const u64 ranks = (hs | hp) & lane, lho_suit = hl & lane;
  const bool side = !NT && lane != TRL;
  u64 wr = 0;
  if (side && ((!(hs & lane) && (hs & TRL)) || (!(hp & lane) && (hp & TRL)))) {
    if (!lho_suit && (hl & TRL)) return false;
  } else if (ranks > (lc | lho_suit)) {
    if (side && (hl & TRL) && !lho_suit) return false;
    wr = highest(ranks);
  } else return false;
  if (need <= 1) {
    R = fill_up(wr);
    return true;
  }
  if (!NT) return false;
  int qt = 1;
  const int hh = (hs & lane) > (hp & lane) ? s : sp;
  const u64 mine = H[hh], others = H[0] | H[1] | H[2] | H[3] | lc;
  {
    const u64 x = others & lane, t1 = highest(x), x2 = x ^ t1;
    if ((t1 & mine) && x2 && (highest(x2) & mine)) {
      qt++;
      wr |= highest(x2);
      if (qt >= need) {
        R = fill_up(wr);
        return true;
      }
    }
  }
  for (int l = 0; l < 4; l++) {
    const u64 ln = LANE(l);
    if (ln == lane || !(mine & ln)) continue;
    const u64 x = others & ln;
    if (x == (mine & ln)) qt += popc(x);
    else if (highest(x) & mine) qt++, wr |= highest(x);
    else continue;
    if (qt >= need) {
      R = fill_up(wr);
      return true;
    }
  }
  return false;
}

// The live card with the highest key (earliest among equals).
inline u64 pick_best(const u64* cards, const uint16_t* key, u64 cand) {
  const unsigned bk = ddport::best_key(cards, key, cand);
  return cards[15 - (bk & 15)];
}

// The endgame tables (two- and three-card tables, the 4-card pattern table): see tables.hpp.
// Interleaved searches (solve_positions on small positions, on small stacks): a probe
// of a table in memory prefetches its entry and hands over to the next search. Outside such
// a batch yield() is one test of a flag.
thread_local bool g_inter = false;
void yield_switch();
#define yield() (g_inter ? yield_switch() : (void)0)

#ifdef STATS
}
extern unsigned long long g_plook[P_LEVELS], g_phit[P_LEVELS];
extern int g_rootL;
extern unsigned long long g_cnS[2][16], g_c1S[2][16], g_wS[2][16], g_allS[2][16], g_allN[2][16];
namespace {
#endif


template <bool NT>
struct Cfg {
  static constexpr u64 TR = NT ? 0 : LANE(3);
};

// Count of the consecutive top cards of `lane` held by `mine` (subset of xs), and those cards.
inline int tops(u64 ml, u64 xs, u64& topset) {
  u64 oth = xs ^ ml;
  topset = ml & ~((1ull << (64 - ddport::clz64(oth))) - 1);  // above the highest other card
  return popc(topset);
}

// The j-th highest card of `set` and above (j >= 1, j <= popcount).
inline u64 top_j(u64 set, int j) {
  return ge(ddport::pdep64(1ull << (popc(set) - j), set));
}

// Sure tricks the leader can cash at once. True if at least `need`; R gets the cards that matter.
template <bool NT>
inline bool quick_tricks(int L, int need, u64 all, u64& R) {
  const int P = L ^ 2, A = (L + 1) & 3, B = (L + 3) & 3;
  const u64 hl = H[L];
  int q = 0;
  u64 r = 0;
  bool side_free = true;  // no opponent can ruff
  int tA = 0, tB = 0;
  if (!NT) {
    tA = len_of(A, 3);
    tB = len_of(B, 3);
    const int oppT = tA > tB ? tA : tB;
    const u64 ml = hl & LANE(3);
    if (ml) {
      u64 ts;
      const int k = tops(ml, all & LANE(3), ts);
      const int lp = len_of(P, 3);
      int tricks, min_top;
      if (k >= oppT && k >= lp) {
        tricks = popc(ml);
        min_top = oppT > lp ? oppT : lp;
      } else {
        tricks = k;
        min_top = k;
        if (k < oppT) side_free = false;
      }
      if (tricks) {
        if (q + tricks >= need && side_free) {
          int use = need - q;
          int t = use < min_top ? use : min_top;
          if (t) r |= top_j(ts, t);
          R = r;
          return true;
        }
        q += tricks;
        if (min_top) r |= top_j(ts, min_top);
      } else if (oppT) {
        side_free = false;
      }
    } else if (oppT) {
      side_free = false;
    }
    if (q >= need) {
      R = r;
      return true;
    }
  }
  for (int l = 0; l < (NT ? 4 : 3); l++) {
    const u64 ml = hl & LANE(l);
    if (!ml) continue;
    u64 ts;
    const int k = tops(ml, all & LANE(l), ts);
    if (!k) continue;
    const int la = len_of(A, l), lb = len_of(B, l);
    int tricks, min_top;
    if (side_free) {
      const int om = la > lb ? la : lb, lp = len_of(P, l);
      if (k >= om && k >= lp) {
        tricks = popc(ml);
        min_top = om > lp ? om : lp;
      } else {
        tricks = k;
        min_top = k;
      }
    } else {
      int rr = 99;
      if (tA && la < rr) rr = la;
      if (tB && lb < rr) rr = lb;
      tricks = k < rr ? k : rr;
      min_top = tricks;
    }
    if (!tricks) continue;
    if (q + tricks >= need) {
      int use = need - q;
      int t = use < min_top ? use : min_top;
      if (t) r |= top_j(ts, t);
      R = r;
      return true;
    }
    q += tricks;
    if (min_top) r |= top_j(ts, min_top);
  }
  return false;
}

// Tricks the side not on lead is sure of. Only "at least one" for now.
template <bool NT>
inline bool later_tricks(int L, int need, u64 all, u64& R) {
  if (need != 1) return false;
  const u64 opp = H[(L + 1) & 3] | H[(L + 3) & 3];
  if (!NT) {
    const u64 xt = all & LANE(3);
    if (xt) {
      u64 top = highest(xt);
      if (top & opp) {
        R = ge(top);
        return true;
      }
      return false;  // the leader's side may ruff
    }
  }
  // No trumps (left): the opponents win the first trick if they hold the top card of every
  // suit the leader has.
  const u64 hl = H[L];
  u64 r = 0;
  for (int l = 0; l < 4; l++) {
    if (!(hl & LANE(l))) continue;
    u64 top = highest(all & LANE(l));
    if (!(top & opp)) return false;
    r |= ge(top);
  }
  R = r;
  return true;
}

// Looks the position up in the per-deal table: 1 or 0 if a pattern decides the question
// (R gets its cards), else -1. hint gets the best card recorded for the key, if any.
#ifndef TTPICK
#define TTPICK 1
#endif
// Transposition-table block matching (8 entries a block). Narrow entries: e[0..7] the masked
// values, e[8..15] the masks with the bounds bits; wide entries: e[0..7] the masks with the
// bounds, e[8..15] and e[16..23] the two masked values.
#if DDPORT_AVX512 && !defined(ABL_SCALAR_TT)
inline unsigned tt_match_narrow(const u64* e, u64 own, u64 dec) {
  const __m512i vb = _mm512_load_si512(e + 8);
  const unsigned k = _mm512_cmpeq_epi64_mask(_mm512_and_si512(vb, _mm512_set1_epi64((long long)own)), _mm512_load_si512(e));
  return k & _mm512_test_epi64_mask(vb, _mm512_set1_epi64((long long)dec));
}
inline unsigned tt_match_wide(const u64* e, u64 ewq, u64 swq, int target) {
  const __m512i vt = _mm512_set1_epi64(target);
  const __m512i v0 = _mm512_load_si512(e);
  const __m512i vm = _mm512_and_si512(v0, _mm512_set1_epi64((long long)W52));
  const unsigned k = _mm512_cmpeq_epi64_mask(_mm512_and_si512(vm, _mm512_set1_epi64((long long)ewq)), _mm512_load_si512(e + 8)) &
                     _mm512_cmpeq_epi64_mask(_mm512_and_si512(vm, _mm512_set1_epi64((long long)swq)), _mm512_load_si512(e + 16));
  const __m512i vlo = _mm512_and_si512(_mm512_srli_epi64(v0, 52), _mm512_set1_epi64(15));
  const __m512i vhi = _mm512_and_si512(_mm512_srli_epi64(v0, 56), _mm512_set1_epi64(15));
  return k & (_mm512_cmpge_epi64_mask(vlo, vt) | _mm512_cmplt_epi64_mask(vhi, vt));
}
// Among the entries in kd (two or more), the one whose mask (v[i] & m) has the fewest bits,
// the earliest among equals: (popcount << 3 | index).
inline u64 tt_min_pc(const u64* v, u64 m, unsigned kd) {
  const __m512i pcv = _mm512_or_si512(_mm512_slli_epi64(_mm512_popcnt_epi64(_mm512_and_si512(_mm512_load_si512(v), _mm512_set1_epi64((long long)m))), 3),
                                      _mm512_set_epi64(7, 6, 5, 4, 3, 2, 1, 0));
  return _mm512_mask_reduce_min_epu64(__mmask8(kd), pcv);
}
#elif DDPORT_AVX512  // ABL_SCALAR_TT: the same, one entry at a time
inline unsigned tt_match_narrow(const u64* e, u64 own, u64 dec) {
  unsigned k = 0;
  for (int i = 0; i < 8; i++) k |= unsigned((e[8 + i] & own) == e[i] && (e[8 + i] & dec)) << i;
  return k;
}
inline unsigned tt_match_wide(const u64* e, u64 ewq, u64 swq, int target) {
  unsigned k = 0;
  for (int i = 0; i < 8; i++) {
    const u64 m = e[i] & W52, lo = (e[i] >> 52) & 15, hi = (e[i] >> 56) & 15;
    k |= unsigned((m & ewq) == e[8 + i] && (m & swq) == e[16 + i] && (lo >= u64(target) || hi < u64(target))) << i;
  }
  return k;
}
inline u64 tt_min_pc(const u64* v, u64 m, unsigned kd) {
  u64 best = ~u64(0);
  while (kd) {
    const unsigned i = unsigned(__builtin_ctz(kd));
    kd &= kd - 1;
    const u64 c = u64(popc(v[i] & m)) << 3 | i;
    best = c < best ? c : best;
  }
  return best;
}
#else  // DDPORT_HWY: Highway
inline unsigned tt_match_narrow(const u64* e, u64 own, u64 dec) {
  namespace hn = hwy::HWY_NAMESPACE;
  const ddport::D64 d;
  const auto vo = hn::Set(d, own), vd = hn::Set(d, dec), z = hn::Zero(d);
  auto m = [&](int i) {
    const auto vb = hn::Load(d, e + 8 + 2 * i);
    return hn::And(hn::Eq(hn::And(vb, vo), hn::Load(d, e + 2 * i)), hn::Ne(hn::And(vb, vd), z));
  };
  return ddport::movemask8x64(m(0), m(1), m(2), m(3));
}
inline unsigned tt_match_wide(const u64* e, u64 ewq, u64 swq, int target) {
  namespace hn = hwy::HWY_NAMESPACE;
  const ddport::D64 d;
  const auto ve = hn::Set(d, ewq), vs = hn::Set(d, swq), vw = hn::Set(d, W52), vt = hn::Set(d, u64(target)), f = hn::Set(d, u64(15));
  auto m = [&](int i) {
    const auto v0 = hn::Load(d, e + 2 * i), vm = hn::And(v0, vw);
    const auto k = hn::And(hn::Eq(hn::And(vm, ve), hn::Load(d, e + 8 + 2 * i)), hn::Eq(hn::And(vm, vs), hn::Load(d, e + 16 + 2 * i)));
    const auto lo = hn::And(hn::ShiftRight<52>(v0), f), hi = hn::And(hn::ShiftRight<56>(v0), f);
    return hn::And(k, hn::Or(hn::Ge(lo, vt), hn::Lt(hi, vt)));
  };
  return ddport::movemask8x64(m(0), m(1), m(2), m(3));
}
inline u64 tt_min_pc(const u64* v, u64 m, unsigned kd) {
  u64 best = ~u64(0);
  while (kd) {
    const unsigned i = unsigned(__builtin_ctz(kd));
    kd &= kd - 1;
    const u64 c = u64(popc(v[i] & m)) << 3 | i;
    best = c < best ? c : best;
  }
  return best;
}
#endif

#ifdef DDF_PAR
// The parallel search: a thread's own table first, then (from PAR_SHMIN cards) the table
// shared by all threads of the deal, under one lock (par_deal.cpp).
[[gnu::always_inline]] inline int tt_lookup_in(TT& tt, int n, int target, u64 all, u64 ewq, u64 swq, u64 key54, u64& hint, u64& R) {
  {
    const bool wide = n >= 8;
#else
[[gnu::always_inline]] inline int tt_lookup(int n, int target, u64 all, u64 ewq, u64 swq, u64 key54, u64& hint, u64& R) {
  {
    const bool wide = n >= 8;
    TT& tt = S->tt;
#endif
    const DirSlot* d0 = tt.find(key54);
    STAT(g_tnoslot[n] += !d0 && !wide);
    if (const DirSlot* d = d0) {
      const unsigned hb = unsigned(d->key >> 54) & 63;
      {
        // Without a branch (it mispredicted): hb >= 52 means no stored lead.
        u64 keep = 0 - u64(hb >= 52);
        asm("" : "+r"(keep));  // or clang turns the select back into a branch
        hint = (ddport::pdep64(1ull << hb, all) & ~keep) | (hint & keep);
      }
#ifdef STATS
      unsigned sb_any = 0; int sb_n = 0;
      struct SBEnd { unsigned& a; int n; bool w; bool done = false; ~SBEnd() { if (!done && !w) { g_tmiss[n]++; g_evict[n] += !a; } } } sb_end{sb_any, n, wide};
#endif
#if TTPICK == 2
      int gbp = 99;
      u64 gbb = 0;
#endif
      if (!wide) {
        const u64 own = ewq | swq << 28, dec = 1ull << (55 + target);
        for (uint32_t blk = d->head; blk != NIL; blk = tt.next[blk]) {
          STAT(g_blk[n]++);
          const u64* e = tt.pool + size_t(blk) * 8;
          const unsigned kd = tt_match_narrow(e, own, dec);
#ifdef STATS
          for (int i_ = 0; i_ < 8; i_++) sb_any |= unsigned((e[8 + i_] & dec) && e[i_] != ~0ull) << i_;
#endif
          STAT(sb_n++);
          if (kd) {
            STAT(g_blkhit[n] += sb_n);
#if TTPICK
            unsigned bi = __builtin_ctz(kd);
            int bp = 0;
            if (TTPICK == 2 || (kd & (kd - 1))) {
              const u64 mn = tt_min_pc(e + 8, 0xFFFFFFF, kd);
              bi = unsigned(mn & 7), bp = int(mn >> 3);
            }
            const u64 bb = e[8 + bi];
#else
            const u64 bb = e[8 + __builtin_ctz(kd)];
#endif
#if TTPICK == 2
            if (bp < gbp) gbp = bp, gbb = bb;
            continue;
#endif
            R = fill_up(ddport::pdep64(bb & 0xFFFFFFF, all));
            STAT(sb_end.done = true);
            return bb >> 63;
          }
        }
#if TTPICK == 2
        if (gbp < 99) {
          R = fill_up(ddport::pdep64(gbb & 0xFFFFFFF, all));
          return gbb >> 63;
        }
#endif
      } else {
        for (uint32_t blk = d->head; blk != NIL; blk = tt.next[blk]) {
          STAT(g_blk[n]++);
          const u64* e = tt.pool + size_t(blk) * 8;
          const unsigned kd = tt_match_wide(e, ewq, swq, target);
          if (kd) {
#if TTPICK
            unsigned bi = __builtin_ctz(kd);
            int bp = 0;
            if (TTPICK == 2 || (kd & (kd - 1))) {
              const u64 mn = tt_min_pc(e, W52, kd);
              bi = unsigned(mn & 7), bp = int(mn >> 3);
            }
            const u64 bb = e[bi];
#else
            const u64 bb = e[__builtin_ctz(kd)];
#endif
#if TTPICK == 2
            if (bp < gbp) gbp = bp, gbb = bb;
            continue;
#endif
            R = fill_up(ddport::pdep64(bb & W52, all));
            return (int(bb >> 52) & 15) >= target;
          }
        }
#if TTPICK == 2
        if (gbp < 99) {
          R = fill_up(ddport::pdep64(gbb & W52, all));
          return (int(gbb >> 52) & 15) >= target;
        }
#endif
      }
    }
  }

  return -1;
}
#ifdef DDF_PAR
#ifndef PAR_SHMIN
#define PAR_SHMIN 6
#endif
struct alignas(64) SpinLock {
  std::atomic<bool> f{false};
  void lock() {
    for (;;) {
      if (!f.exchange(true, std::memory_order_acquire)) return;
      while (f.load(std::memory_order_relaxed)) ddport::cpu_relax();
    }
  }
  void unlock() { f.store(false, std::memory_order_release); }
};
#ifndef PAR_STRIPES
#define PAR_STRIPES 16
#endif
// The deal's shared table (null: none): PAR_STRIPES independent tables, each under its own
// lock, a key's stripe chosen by a hash independent of the directory's.
// Writers take the stripe's lock and make its sequence number odd while they write; readers
// take no lock: they read the sequence number before and after the lookup and count it a miss
// if a writer was active or intervened (the lookup itself only reads; chains always lead to
// lower block numbers, so a torn read ends, and its answer is thrown away).
struct alignas(64) Stripe {
  SpinLock lock;
  alignas(64) std::atomic<uint32_t> seq{0};
};
TT* g_shared = nullptr;
Stripe g_stripes[PAR_STRIPES];
#ifndef PAR_SHCOST
#define PAR_SHCOST 1
#endif
const u64 g_shcost = getenv("PAR_SHCOST") ? atoi(getenv("PAR_SHCOST")) : PAR_SHCOST;  // experiments
inline unsigned stripe_of(u64 key54) { return unsigned((key54 * 0xD6E8FEB86659FD93ull) >> 32) % PAR_STRIPES; }
const int g_shmin = getenv("PAR_SHMIN") ? atoi(getenv("PAR_SHMIN")) : PAR_SHMIN;  // experiments
int tt_lookup(int n, int target, u64 all, u64 ewq, u64 swq, u64 key54, u64& hint, u64& R) {
  int r = tt_lookup_in(S->tt, n, target, all, ewq, swq, key54, hint, R);
  if (r < 0 && n >= g_shmin && g_shared) {
    const unsigned k = stripe_of(key54);
    Stripe& st = g_stripes[k];
    const uint32_t s0 = st.seq.load(std::memory_order_acquire);
    if (!(s0 & 1)) {
      u64 h2 = hint, R2 = 0;
      const int r2 = tt_lookup_in(g_shared[k], n, target, all, ewq, swq, key54, h2, R2);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (st.seq.load(std::memory_order_relaxed) == s0) {
        hint = h2;
        if (r2 >= 0) r = r2, R = R2;
      }
    }
  }
  return r;
}
#endif

// Leads of the root leader's side at 12 cards: cards that cut there earlier in the deal go
// first, the cheapest proof first (round 9, in cycles: nodes -0.2 to -0.3%, cycles about -1%).
// -DNOMEM turns it off.
#if !defined(MEMN) && !defined(NOMEM)
#define MEMN 12
#endif
// A lead's "cost" is the number of lead nodes searched under it.
#ifdef MEMN
#define MEMTICK() (g_mnodes++)
#define MEMNOW() g_mnodes
#else
#define MEMTICK() ((void)0)
#endif
#ifndef RUNOPT
#define RUNOPT 1
#endif
// Adds the run below each relevant card that was tried at a node that failed with every card.
inline u64 add_runs(u64 R, u64 tried, u64 hand, u64 all) {
  u64 rc = tried & R;
  while (rc) {
    const u64 c = lowest(rc);
    rc ^= c;
    R |= ge(run_bottom(c, hand ^ c, all));
  }
  return R;
}
#ifdef SELFTEST
#ifndef SELFTEST_MAX
#define SELFTEST_MAX 13  // check lead nodes of 4 to SELFTEST_MAX cards
#endif
template <bool NT>
bool lead(int n, int L, int target, u64& R);
thread_local SState* g_alt = nullptr;
thread_local u64 g_st_rng = 88172645463325252ull, g_st_cnt = 0, g_st_checks = 0;
thread_local int g_st_depth = 0;
inline u64 st_rand() { g_st_rng ^= g_st_rng << 13; g_st_rng ^= g_st_rng >> 7; g_st_rng ^= g_st_rng << 17; return g_st_rng; }
// Re-deals the cards below the relevant ones (same suit lengths in every hand) and checks
// that a fresh search (own table) gives the same answer.
template <bool NT>
void self_test(int n, int L, int target, u64 R, bool res) {
  if (g_st_depth || n < 4 || n > SELFTEST_MAX || (++g_st_cnt % SELFTEST)) return;
#ifdef DDF_PAR
  // solve_deal_parallel: an aborted work item returns no real value (it stores nothing), and
  // at 13 cards a work item searches only some of hand 2's replies (mask2, ybw).
  if (n == 13 || PAR_STOP()) return;
#endif
  const u64 all = H[0] | H[1] | H[2] | H[3];
  u64 h0[4];
  for (int i = 0; i < 4; i++) h0[i] = H[i];
  const u64 lens0 = LENS;
  for (int l = 0; l < 4; l++) {
    const u64 free = all & LANE(l) & ~R;
    int cnt[4], k = 0;
    u64 cs[13];
    for (int i = 0; i < 4; i++) cnt[i] = popc(H[i] & free), H[i] &= ~free;
    for (u64 f = free; f; f &= f - 1) cs[k++] = lowest(f);
    for (int i = k - 1; i > 0; i--) { const int j = int(st_rand() % u64(i + 1)); const u64 t = cs[i]; cs[i] = cs[j]; cs[j] = t; }
    int q = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < cnt[i]; j++) H[i] |= cs[q++];
  }
  if (!g_alt) { g_alt = new SState(); g_alt->tt.alloc(); }
  SState* keep = S;
  S = g_alt;
  S->tt.reset();
  g_st_depth++;
  u64 R2;
  const bool r2 = lead<NT>(n, L, target, R2);
  g_st_depth--;
  S = keep;
  // The item may have been aborted during the re-search, which then has no real value.
  const bool aborted = PAR_STOP();
  if (!aborted) g_st_checks++;
  if (!aborted && r2 != res) {
    fprintf(stderr, "SELFTEST FAIL n=%d L=%d t=%d nt=%d R=%llx orig %llx %llx %llx %llx\n", n, L, target, int(NT), (unsigned long long)R, (unsigned long long)h0[0], (unsigned long long)h0[1], (unsigned long long)h0[2], (unsigned long long)h0[3]);
    abort();
  }
  for (int i = 0; i < 4; i++) H[i] = h0[i];
  LENS = lens0;
}
struct STPrint { ~STPrint() { fprintf(stderr, "selftest checks %llu\n", (unsigned long long)g_st_checks); } } g_stp;
#endif
// Cut-off cards (bridge-solver's cutoff cache, by Hanhong Xue: github.com/macroxue/bridge-solver,
// solver.cc BuildCutoffIndex): the lead that last cut off, keyed by the
// leader's exact hand, the parity of partner's suit lengths and, in suit contracts, the
// target. Tried first. Only an ordering hint: a collision or a stale entry costs nodes,
// never correctness. Entries carry the deal's generation, so a new deal starts empty.
#ifndef CUTC
#define CUTC 1
#endif
// From 8 cards a hand: below that a search is small, and probing the table (often a cache miss)
// costs more than the better order saves. Full deals, best of 3 (NOTES round 18): 4 cards
// 13.67M cycles a problem and 132.0M a table, 6: 13.57M and 130.3M, 8: 13.13M and 126.9M,
// 10: 13.75M and 131.3M.
#define CUT_FROM (ddfast::detail::g_cutoff_min)  // CUTC_MIN, or 14 with cut-off cards off
#ifndef CUTF
#define CUTF 1
#endif
#if CUTC
struct CutEntry {
  uint32_t tag;
  uint8_t card;
};
#ifndef CUT_BITS
#define CUT_BITS 16
#endif
thread_local CutEntry g_cut[1 << CUT_BITS];
template <bool NT>
inline u64 cut_hash_lead(int L, int target) {
  const int P = L ^ 2;
  const unsigned par = unsigned(len_of(P, 0) & 1) | unsigned(len_of(P, 1) & 1) << 1 | unsigned(len_of(P, 2) & 1) << 2 |
                       unsigned(len_of(P, 3) & 1) << 3;
  const u64 extra = u64(par) | u64(NT ? 0 : target) << 4 | u64(g_gen) << 8;
  return (H[L] * 0x9E3779B97F4A7C15ull) ^ (extra * 0xC2B2AE3D27D4EB4Full);
}
// Followers (bridge-solver's keys): following suit, the led suit's cards at the trick start,
// the card winning so far and the leader; void, the hand, the winning card (at no trumps its
// hand) and in suit contracts the target. Position in the trick K keeps the four apart.
template <bool NT>
inline u64 cut_hash_follow(int K, int L, int s, u64 all, u64 lane, u64 in_suit, u64 best, int bw, int target) {
  u64 a, extra;
  if (in_suit) {
    a = all & lane;
    extra = u64(__builtin_ctzll(best)) | u64(L) << 6 | u64(K) << 8 | 1ull << 10;
  } else {
    a = H[s];
    extra = (NT ? u64(bw) : u64(__builtin_ctzll(best))) | u64(NT ? 0 : target) << 6 | u64(K) << 11;
  }
  return (a * 0x9E3779B97F4A7C15ull) ^ ((extra | u64(g_gen) << 16) * 0xC2B2AE3D27D4EB4Full);
}
#endif

template <bool NT>
bool lead(int n, int L, int target, u64& R) {
  constexpr u64 TR = Cfg<NT>::TR;
  STAT(g_nodes++);
  MEMTICK();
  // Parallel search: an aborted work item unwinds at once; its results are never stored.
  if (PAR_STOP()) [[unlikely]] { R = 0; return false; }
#ifdef DDF_PAR
  const u64 par_m0 = g_mnodes;  // shared-table stores only for subtrees of PAR_SHCOST lead nodes or more
#endif
  STAT(g_leads++);
  STAT(g_n[n][0]++);
  if (n == 1) {
    u64 best = H[L];
    int bw = L;
    bool ranked = false;
    for (int k = 1; k < 4; k++) {
      int s = (L + k) & 3;
      u64 c = H[s];
      if (c & lane_of(best)) {
        ranked = true;
        if (c > best) best = c, bw = s;
      } else if (c & TR) {
        best = c, bw = s, ranked = false;
      }
    }
    R = ranked ? ge(best) : 0;
    return !(bw & 1);
  }
  const u64 all = H[0] | H[1] | H[2] | H[3];
  const bool cut = !(L & 1);
  if (n == 2 && g_use2) return probe2<NT>(H, L, all, target, R);
#if YIELD3
  if (n == 3 && g_use3) return probe3<NT>(H, L, all, target, R, [] { yield(); });
#else
  if (n == 3 && g_use3) return probe3<NT>(H, L, all, target, R, [] {});
#endif
  const int needL = cut ? target : n - target + 1;
  const bool dq = n >= DMIN;
  // Table.
  const bool wide = n >= 8;
  const ddport::BitMask bm_(all);
  const u64 ewq = bm_.ext(H[1] | H[3]), swq = bm_.ext(H[2] | H[3]);
  const u64 key54 = lens48(LENS) | u64(L) << 48 | u64(n) << 50;
  u64 hint = 0;
  STAT(g_ttlook++);
  constexpr bool skiptt = false;
  if (n >= TT_MIN && !skiptt) {
    const int r = tt_lookup(n, target, all, ewq, swq, key54, hint, R);
    if (r >= 0) {
      STAT(g_tthit++);STAT(g_n[n][3]++);
      return r != 0;
    }
  }
#ifdef DDF_PAR
  auto tt_store_in = [&](TT& tt, u64 Racc, bool res, u64 bestc) {
#else
  auto tt_store = [&](u64 Racc, bool res, u64 bestc) {
    if (n < TT_MIN) return;
    TT& tt = S->tt;
#endif
    const uint32_t lines = wide ? 3 : 2;
    if (tt.used + lines > (1u << POOL_BITS)) tt.reset();
    DirSlot* d = tt.obtain(key54);
    unsigned cnt = unsigned(d->key >> 60);
    if (cnt == 8) {
      const uint32_t blk = tt.used;
      tt.used += lines;
      u64* e = tt.pool + size_t(blk) * 8;
      if (wide) {
        memset(e, 0, 64);
        memset(e + 8, 0xFF, 64);
      } else {
        memset(e, 0xFF, 64);
        memset(e + 8, 0, 64);
      }
      tt.next[blk] = d->head;
      d->head = blk;
      cnt = 0;
    }
    STAT(g_store[n]++);
    u64* e = tt.pool + size_t(d->head) * 8;
    const u64 m = ddport::pext64(Racc, all);
    const u64 elo = res ? u64(target) : 0, ehi = res ? u64(n) : u64(target - 1);
    if (wide) {
      e[cnt] = m | elo << 52 | ehi << 56;
      e[8 + cnt] = ewq & m;
      e[16 + cnt] = swq & m;
    } else {
      const u64 mm = m | m << 28;
      e[cnt] = (ewq | swq << 28) & mm;
      // Bit 55 + t: the entry decides target t (1..7); bit 63: the answer.
      const u64 dec = res ? (1ull << target) - 1 : 0x7Full & ~((1ull << (target - 1)) - 1);
      e[8 + cnt] = mm | dec << 56 | u64(res) << 63;
    }
    u64 key = (d->key & ~(15ull << 60)) | u64(cnt + 1) << 60;
    if (bestc) key = (key & ~(63ull << 54)) | u64(popc(all & (bestc - 1))) << 54;
    d->key = key;
  };
#ifdef DDF_PAR
  auto tt_store = [&](u64 Racc, bool res, u64 bestc) {
    if (n < TT_MIN) return;
    if (PAR_STOP()) [[unlikely]] return;
    tt_store_in(S->tt, Racc, res, bestc);
    if (n >= g_shmin && g_shared && g_mnodes - par_m0 >= g_shcost) {
      const unsigned k = stripe_of(key54);
      Stripe& st = g_stripes[k];
      st.lock.lock();
      const uint32_t s0 = st.seq.load(std::memory_order_relaxed);
      st.seq.store(s0 + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      tt_store_in(g_shared[k], Racc, res, bestc);
      st.seq.store(s0 + 2, std::memory_order_release);
      st.lock.unlock();
    }
  };
#endif
  bool filled = false;
  if (dq) {
    bool value;
    if (dds_bounds<NT>(n, L, target, all, filled, R, value)) {
      STAT(g_qt++);STAT(g_n[n][1]++);
#ifndef ABL_NO_BOUNDSTORE  // leave-one-out: bound cutoffs not stored (as in DDS)
      tt_store(R, value, 0);  // finding it again in the table is cheaper than the port
#endif
      return value;
    }
#if BSB
    if (ddfast::detail::g_sure_trick_bounds && bs_bounds<NT>(n, L, target, all, R, value)) {
      STAT(g_bsb++);
      tt_store(R, value, 0);
      return value;
    }
#endif
  } else {
    if (quick_tricks<NT>(L, needL, all, R)) {
      STAT(g_qt++);STAT(g_n[n][1]++);
      return cut;
    }
    if (later_tricks<NT>(L, n + 1 - needL, all, R)) {
      STAT(g_lt++);STAT(g_n[n][2]++);
      return !cut;
    }
  }

  // The 4-card pattern table (read-only here).
  if (n >= 4 && n < 4 + P_LEVELS && g_pt[n - 4].use) {
    STAT(g_plook[n - 4]++);
    PatternQuery pq;
    size_t hit;
#if YIELDP == 2
    const int r = probe_pattern<NT>(H, LENS, n, L, all, target, pq, R, hit, [] { yield(); });
#elif YIELDP == 1
    const int r = probe_pattern<NT>(H, LENS, n, L, all, target, pq, R, hit, [y = true]() mutable { if (y) y = false, yield(); });
#else
    const int r = probe_pattern<NT>(H, LENS, n, L, all, target, pq, R, hit, [] {});
#endif
    if (r >= 0) {
      STAT(g_phit[n - 4]++);
#ifndef ABL_NO_P4STORE  // leave-one-out: 4-card table hits not copied into the per-deal table
      tt_store(R, r, 0);  // a pattern in the per-deal table is cheaper to find again
#endif
      return r;
    }
  }

  // Moves.
  STAT(g_n[n][4]++);
  const u64 hl = H[L];
  u64 cand = heads(hl, all);
  alignas(64) u64 cards[16] = {};
  alignas(32) uint16_t key[16] = {};
  int nm = 0;
  STAT(g_hx[hint != 0][0]++);
  if (n >= DMIN) {
#if defined(PORT_ORDER) || defined(VERIFY_ORDER) || DFOL <= 13
    if (!filled) {  // the port's view of the position, for its weights
      port::Pos& t = S->pos[n];
      fill_holdings(t);
      fill_tops(n, t);
      t.tricks_max = 0;
    }
#endif
#ifdef PORT_ORDER
    nm = dds_order<NT>(n, 0, L, hint, cards, key);
#else
    nm = native_order<NT>(n, L, hint, all, cards, key);
#ifdef VERIFY_ORDER
    {
      alignas(64) u64 c2[16] = {};
      alignas(32) uint16_t k2[16] = {};
      const int nm2 = dds_order<NT>(n, 0, L, hint, c2, k2);
      if (nm2 != nm || memcmp(c2, cards, sizeof c2) || memcmp(k2, key, sizeof k2)) {
        fprintf(stderr, "ORDER MISMATCH n=%d L=%d nt=%d H=%016llx %016llx %016llx %016llx hint=%llx killer=%llx\n", n, L, int(NT), (unsigned long long)H[0], (unsigned long long)H[1], (unsigned long long)H[2], (unsigned long long)H[3], (unsigned long long)hint, (unsigned long long)S->killer[n][0]);
        for (int i = 0; i < 14; i++) fprintf(stderr, "  %d: %llx %d | %llx %d\n", i, (unsigned long long)cards[i], key[i] >> 4, (unsigned long long)c2[i], k2[i] >> 4);
        abort();
      }
    }
#endif
#endif
#ifdef MEMN
    if (n >= MEMN && n <= 12 && !((L ^ S->rootL) & 1)) {
      for (int i = 0; i < nm; i++) {
        const int b = __builtin_ctzll(cards[i]);
        if (S->mres[n][b] == 1) {
          const int lg = 63 - __builtin_clzll(S->mcost[n][b] | 1);
          key[i] = uint16_t((0xE00 + 0x1FF - lg * 8) << 4 | (key[i] & 15));
        }
      }
    }
#endif
  } else {
    const int P = L ^ 2, A = (L + 1) & 3, B = (L + 3) & 3;
    const u64 hp = H[P], ho = H[A] | H[B];
    const bool opp_trumps = !NT && (ho & TR);
    const bool p_trumps = !NT && (hp & TR);
    for (int l = 0; l < 4; l++) {
      u64 hd = cand & LANE(l);
      if (!hd) continue;
      const u64 x = all & LANE(l);
      const u64 top = highest(x);
      const u64 pl = hp & LANE(l);
      const u64 low = lowest(hd);
      const bool is_trump = !NT && l == 3;
      const bool ruffable = opp_trumps && !is_trump && (!(H[A] & LANE(l)) || !(H[B] & LANE(l)));
      while (hd) {
        u64 c = highest(hd);
        hd ^= c;
        int sc;
        if (c & hint) sc = 1000;
        else if (c == top) sc = ruffable ? 15 : (is_trump && opp_trumps ? 70 : 60);
        else if (c == low) {
          if (top & pl) sc = ruffable ? 12 : 50;
          else if (!pl && p_trumps && !is_trump) sc = 55;
          else sc = 20;
        } else sc = 10;
        cards[nm] = c;
        key[nm] = uint16_t(sc * 16 + (15 - nm));
        nm++;
      }
    }
  }
#if CUTC
  const bool usec = n >= CUT_FROM;
  const u64 cuth = usec ? cut_hash_lead<NT>(L, target) : 0;
  CutEntry& cute = g_cut[cuth >> (64 - CUT_BITS)];
  if (usec && cute.tag == uint32_t(cuth)) {
    const u64 cc = 1ull << cute.card;
    if (cc & cand)
      for (int i = 0; i < 16; i++)
        if (cards[i] == cc) key[i] = uint16_t(0xFFF0 | (key[i] & 15));
  }
#endif
  u64 Racc = 0;
  bool res = !cut;
  u64 bestc = 0;
  u64 triedm = 0;
#ifdef STATS
  int tried = 0;
  const u64 nodes0 = g_nodes;
  u64 nodes1 = 0, firstc = 0, wmax = 0;
#endif
  while (cand) {
    // The best remaining card (highest score, then earliest), chosen without branches.
    const unsigned bk = ddport::best_key(cards, key, cand);
    const u64 c = cards[15 - (bk & 15)];
    cand ^= c;
    triedm |= c;
    STAT(wmax = (tried && g_nodes - nodes1 > wmax) ? g_nodes - nodes1 : wmax);
    STAT(nodes1 = g_nodes);
    STAT(firstc = firstc ? firstc : c);
    const int sh = 16 * L + 4 * suit_of(c);
    if constexpr (TRK_NEEDED) S->trk[n][0] = c;
    H[L] ^= c;
    LENS -= 1ull << sh;
    u64 Rc;
#ifdef MEMN
    const bool memo = n >= MEMN && n <= 12 && !((L ^ S->rootL) & 1);
    const u64 mt0 = memo ? MEMNOW() : 0;
#endif
    bool r = follow<NT, 1>(n, L, target, all, lane_of(c), c, L, false, Rc);
#ifdef MEMN
    if (memo) { const int b = __builtin_ctzll(c); S->mcost[n][b] = MEMNOW() - mt0; S->mres[n][b] = r == cut ? 1 : 2; }
#endif
    H[L] ^= c;
    LENS += 1ull << sh;
#ifdef STATS
    tried++;
#endif
    if (r == cut) {
      Racc = Rc;
      res = cut;
      bestc = c;
      S->killer[n][0] = c;
#if CUTC
      if (usec) cute = {uint32_t(cuth), uint8_t(__builtin_ctzll(c))};
#endif
      STAT(g_tried[0]++);
      STAT(g_first[0] += tried == 1);
      STAT(g_hx[hint != 0][1]++);
#ifdef STATS
      g_cn[n]++, g_c1[n] += tried == 1;
      { const int sd = (L & 1) != (g_rootL & 1); g_cnS[sd][n]++; g_c1S[sd][n] += tried == 1; if (tried > 1) g_wS[sd][n] += nodes1 - nodes0; }
      if (tried > 1) g_cs[n] += g_nodes - nodes1, g_wmax[n] += wmax; else g_cs1[n] += g_nodes - nodes1;
      if (tried > 1) {
        g_w[n] += nodes1 - nodes0;
        const u64 pl = H[L ^ 2], opp = H[(L + 1) & 3] | H[(L + 3) & 3];
        auto feat = [&](u64 x) {
          const u64 ln = lane_of(x), suit = all & ln;
          const bool top = x == highest(suit);                    // leads the suit's top card
          const bool ptop = (highest(suit) & pl) != 0;            // partner has the top
          const bool tr = !NT && (ln & TR);
          const bool ruff = !NT && !tr && !(pl & ln) && (pl & TR); // partner can ruff
          const bool low = x == lowest(hl & ln);
          return int(tr) * 8 + (top ? 0 : ptop ? 1 : ruff ? 2 : low ? 3 : 4) + 5 * 0;
        };
        const bool same = lane_of(firstc) == lane_of(c);
        const int f1 = feat(firstc), f2 = feat(c);
        (void)opp;
        g_feat[same][f1 % 8 * 0 + (f1 & 7) + 0]++;  // feature of the failed first lead
        g_wfeat[same][(f2 & 7) + (f2 & 8)]++;       // feature of the cutting lead
        g_feat[same][8 + (f1 >> 3) * 2 + (f2 >> 3)]++;  // trump?(first) x trump?(cutter)
      }
#endif
      break;
    }
    Racc |= Rc;
    if (!(c & Rc)) cand &= ~lane_of(c) | Rc;
  }
#if RUNOPT
  if (res != cut) Racc = add_runs(Racc, triedm, hl, all);
#endif
#ifdef STATS
  if (res != cut) { const int sd = (L & 1) != (g_rootL & 1); g_allS[sd][n]++; g_allN[sd][n] += g_nodes - nodes0; }
#endif
  R = Racc;
#ifdef SELFTEST
  self_test<NT>(n, L, target, Racc, res);
#endif
  // Store.
  tt_store(Racc, res, bestc);
  return res;
}

// The card of `cand` with the lowest rank (any suit).
inline u64 min_rank(u64 cand) {
  u64 best = 0;
  int br = 99;
  for (int l = 0; l < 4; l++) {
    const u64 x = cand & LANE(l);
    const int r = x ? int(ddport::ctz64(x)) & 15 : 99;
    const bool lower = r < br;
    br = lower ? r : br;
    best = lower ? lowest(x) : best;
  }
  return best;
}

#ifndef COV2
#define COV2 1
#endif
#ifndef COVB
#define COVB 1
#endif
#ifndef COVPL
#define COVPL 3  // second hand, partner wins: lowest card above third hand's top when partner (playing last) holds COVPL+ cards and a card above second hand's top
#endif
template <bool NT, int K>
inline u64 pick_follow(u64 cand, bool first, int s, u64 lane, u64 best, int bw) {
  if (!first) return K == 2 ? highest(cand) : lowest(cand);
  const bool pw = bw == (s ^ 2);
  const u64 win = (best & lane) ? cand & above(best) : 0;
  if (K == 3) return (pw || !win) ? lowest(cand) : lowest(win);
  const u64 nl = H[(s + 1) & 3] & lane;  // the next hand's cards in the suit
  if (K == 2) {
    if (pw) {
      if (!nl) return lowest(cand);
      u64 nh = highest(nl);
      if (nh < best) return lowest(cand);
      u64 w = cand & above(nh);
      return w ? lowest(w) : lowest(cand);
    }
    if (!win) return lowest(cand);
    if (!nl) return lowest(win);
    u64 w2 = win & above(highest(nl));
    return w2 ? lowest(w2) : lowest(win);
  }
  if (!win) { STAT(g_why = 0); return lowest(cand); }
#if COV2
  {
    // Second hand: low when its partner, playing last, beats the lead and the third hand.
    const u64 pl = H[(s + 2) & 3] & lane;
    const u64 top3 = nl ? highest(nl) : 0;
    const u64 bar = top3 > best ? top3 : best;
    if (COVPL && pl && highest(pl) > bar && popc(pl) >= COVPL && highest(pl) > highest(cand)) {
      const u64 w3 = nl ? win & above(highest(nl)) : 0;
      if (w3) { STAT(g_why = 1); return lowest(w3); }
    }
    if (pl && highest(pl) > bar) { STAT(g_why = 1); return lowest(cand); }
  }
#endif
  if (!nl) { STAT(g_why = 2); return lowest(win); }
  u64 w2 = win & above(highest(nl));
  STAT(g_why = w2 ? (lowest(w2) == lowest(win) ? 3 : 4) : 5);
#if COVB
  if (!w2 && !(H[(s + 2) & 3] & lane & above(best))) return lowest(win);  // partner cannot beat the lead: cover
#endif
  return w2 ? lowest(w2) : lowest(cand);
}

#ifndef DISC
#define DISC 3
#endif
// The lowest card of the longest of hand s's suits in `side` (first such suit on ties).
inline u64 longest_low(u64 side, int s) {
  const u64 ln = LENS >> (16 * s);
  int bl = -1, bll = 0;
#pragma GCC unroll 4
  for (int l = 0; l < 4; l++) {
    const int sc = (side & LANE(l)) ? int((ln >> (4 * l)) & 15) : -1;
    bll = sc > bl ? l : bll;
    bl = sc > bl ? sc : bl;
  }
  return lowest(side & LANE(bll));
}
template <int K>
inline u64 disc_pick(u64 side, int s) {
  return longest_low(side, s);
}
#ifndef VOIDR
#define VOIDR 1
#endif
template <bool NT, int K>
inline u64 pick_void(u64 cand, bool first, int s, u64 best, int bw) {
  constexpr u64 TR = Cfg<NT>::TR;
  const u64 mt = cand & TR;
  if (first && mt) {
    const bool pw = bw == (s ^ 2);
    const u64 wr = (best & TR) ? mt & above(best) : mt;
#if VOIDR
    if (!NT && K < 3) {
      const u64 nx = H[(s + 1) & 3], ln = lane_of(best);
      const u64 nxl = nx & ln, nxt = nx & TR;
      if (K == 2 && pw) {
        // Partner leads the trick; ruff when the last hand would beat it.
        if (nxl ? highest(nxl) > best : nxt != 0) {
          if (nxl) return lowest(mt);
          const u64 w = mt & above(highest(nxt));
          if (w) return lowest(w);
        }
      } else if (K == 1 && wr) {
        // Second hand: no ruff that the third hand can overruff, if it can discard instead.
        if (!nxl && nxt && highest(nxt) > lowest(wr)) {
          const u64 w = wr & above(highest(nxt));
          if (w) return lowest(w);
          if (cand & ~TR) return min_rank(cand & ~TR);
        }
        return lowest(wr);
      }
    }
#endif
    if (wr && !pw) return lowest(wr);
  }
  const u64 side = cand & ~TR;
  if (!side) return lowest(cand);
#if DISC == 3
  return disc_pick<K>(side, s);
#elif DISC
  {
    const u64 all = H[0] | H[1] | H[2] | H[3], mine = H[s];
    u64 keep = 0;
    int bl = -1;
    u64 bc = 0;
    for (int l = 0; l < 4; l++) {
      const u64 x = side & LANE(l);
      if (!x) continue;
      const u64 ml = mine & LANE(l), al = all & LANE(l);
      if (DISC == 1 && (highest(al) & ml)) keep |= x;  // holds the suit's top card
      if (DISC == 2 || DISC == 4) {
        // stoppers: my top card with fewer outside cards above it than my length below it
        const int above_n = popc(al & ~ml & above(highest(ml)));
        if (above_n < popc(ml)) keep |= x;
      }
      if (DISC == 3 && popc(ml) > bl) bl = popc(ml), bc = lowest(x);
      if (DISC == 5) {
        const int sc = popc(ml) * 16 + 15 - (int(__builtin_ctzll(x)) & 15);
        if (sc > bl) bl = sc, bc = lowest(x);
      }
    }
    if (DISC == 3 || DISC == 5) return bc;
    if (DISC == 4) {
      const u64 pool = (side & ~keep) ? side & ~keep : side;
      for (int l = 0; l < 4; l++) {
        const u64 x = pool & LANE(l);
        if (!x) continue;
        const int sc = popc(mine & LANE(l)) * 16 + 15 - (int(__builtin_ctzll(x)) & 15);
        if (sc > bl) bl = sc, bc = lowest(x);
      }
      return bc;
    }
    if (side & ~keep) return min_rank(side & ~keep);
  }
#endif
  return min_rank(side);
}

// The trick so far: best card, its hand, whether rank decides (as in follow).
template <bool NT>
inline void trick_step(u64 x, int s, u64& best, int& bw, bool& ranked) {
  constexpr u64 TR = Cfg<NT>::TR;
  const bool same = (x & lane_of(best)) != 0, ruff = (x & TR) != 0;
  const bool beats = same ? x > best : ruff;
  if (beats) best = x, bw = s;
  ranked = same | (ranked & !ruff);
}

template <bool NT, int K>
bool follow(int n, int L, int target, u64 all, u64 lane, u64 best, int bw, bool ranked, u64& R) {
  constexpr u64 TR = Cfg<NT>::TR;
  STAT(g_nodes++);
  STAT(g_fn[n]++);
  const int s = (L + K) & 3;
  const bool cut = !(s & 1);
  const u64 mine = H[s];
  const u64 in_suit = mine & lane;
  u64 cand = heads(in_suit ? in_suit : mine, all);
#ifdef DDF_PAR
  if constexpr (K == 1) if (n == 13) cand &= g_hot.mask2;  // a work item: some of hand 2's replies
#endif
  u64 Racc = 0;
  u64 tried = 0;
  bool first = true;
#ifdef STATS
  const u64 fn0 = g_nodes;
  u64 fn1 = 0, firstc = 0;
  int why0 = -1;
  if (K == 1 && in_suit && (cand & (cand - 1))) g_why = -1;
#endif
  // With the thresholds above 13 this is false at compile time and the move arrays vanish.
  const bool dq = DFOL <= 13 && n >= DFOL;
  alignas(64) u64 cards[16];
  alignas(32) uint16_t key[16];
  if (K == 1 && n >= DMIN) {
    const u64 lc = best;  // follow 1: the lead card
    const bool hit = native_second<NT>(s, lc, cut ? target : n + 1 - target, R);
#ifdef VERIFY_ORDER
    {
      port::Pos& t = S->pos[n];
      fill_holdings(t);
      // As in DDS, rank_in_suit has the led card out, but aggr, winner and second_best
      // still describe the trick start: count the led card back in for those.
      t.aggr[suit_of(lc)] |= uint16_t(lc >> (16 * suit_of(lc)));
      H[(s + 3) & 3] ^= lc;
      fill_tops(n, t);
      H[(s + 3) & 3] ^= lc;
      t.tricks_max = 0;
      t.lead = port::MoveType{suit_of(lc), (__builtin_ctzll(lc) & 15) + 2, 0, 0};
      clear_wr(t);
      const bool h2 = port::QuickTricksSecondHand(t, s, 4 * (n - 1) - 1, target, NT ? 4 : 3);
      if (h2 != hit || (hit && R != win_ranks(t))) {
        fprintf(stderr, "SECOND MISMATCH n=%d s=%d nt=%d target=%d %d/%d R %llx/%llx\n", n, s, int(NT), target, int(hit), int(h2), (unsigned long long)R, (unsigned long long)win_ranks(t));
        abort();
      }
    }
#endif
    if (hit) return cut;
  }
  if (dq) {
    port::Pos& t = S->pos[n];
    fill_holdings(t);
    (void)t;
    if (dq && (cand & (cand - 1))) {
      memset(cards, 0, sizeof cards), memset(key, 0, sizeof key);
#ifndef NDEBUG
      const int nm =
#endif
      dds_order<NT>(n, K, L, 0, cards, key);
#ifndef NDEBUG
      u64 cc = 0;
      for (int i = 0; i < nm; i++) cc |= cards[i];
      if (cc != cand) fprintf(stderr, "move mismatch\n"), abort();
#endif
    }
  }
#if CUTC && CUTF
  const bool usec = n >= CUT_FROM && (cand & (cand - 1));
  const u64 cuth = usec ? cut_hash_follow<NT>(K, L, s, all, lane, in_suit, best, bw, target) : 0;
  CutEntry& cute = g_cut[cuth >> (64 - CUT_BITS)];
  u64 forced = (usec && cute.tag == uint32_t(cuth)) ? (1ull << cute.card) & cand : 0;
#endif
  while (cand) {
#if CUTC && CUTF
    const u64 c = forced ? forced : (dq && (cand & (cand - 1))) ? pick_best(cards, key, cand)
                  : in_suit ? pick_follow<NT, K>(cand, first, s, lane, best, bw) : pick_void<NT, K>(cand, first, s, best, bw);
    forced = 0;
#else
    const u64 c = (dq && (cand & (cand - 1))) ? pick_best(cards, key, cand)
                  : in_suit ? pick_follow<NT, K>(cand, first, s, lane, best, bw) : pick_void<NT, K>(cand, first, s, best, bw);
#endif
    cand ^= c;
    STAT(firstc = first ? c : firstc);
#ifdef STATS
    const int why = (K == 1 && first && in_suit && !dq) ? g_why : (first ? -1 : why0);
    [[maybe_unused]] const int why0_ = why;
    if (first) why0 = why;
#endif
    tried |= c;
    if constexpr (TRK_NEEDED) S->trk[n][K] = c;
    const bool same = (c & lane_of(best)) != 0, ruff = (c & TR) != 0;
    const bool beats = same ? c > best : ruff;
    const u64 nb = beats ? c : best;
    const int nbw = beats ? s : bw;
    const bool nr = same | (ranked & !ruff);
    const int sh = 16 * s + 4 * suit_of(c);
    H[s] ^= c;
    LENS -= 1ull << sh;
    STAT(fn1 = g_nodes);
    u64 Rc;
    bool r;
    if (K < 3) {
      r = follow<NT, K < 3 ? K + 1 : 3>(n, L, target, all, lane, nb, nbw, nr, Rc);
    } else {
      const u64 Rt = (RUNOPT ? ge(nb) : ge(run_bottom(nb, H[nbw], all))) & (0 - u64(nr));
      const int nt = target - !(nbw & 1);
      if (nt <= 0) r = true, Rc = Rt;
      else if (nt > n - 1) r = false, Rc = Rt;
      else {
        r = lead<NT>(n - 1, nbw, nt, Rc);
        Rc |= Rt;
      }
    }
    H[s] ^= c;
    LENS += 1ull << sh;
    if (r == cut) {
      R = Rc;
#if CUTC && CUTF
      if (usec) cute = {uint32_t(cuth), uint8_t(__builtin_ctzll(c))};
#endif
      STAT(g_tried[K]++);
#ifdef STATS
      if (K == 1 && why0 >= 0 && n >= 9) g_whyn[why0]++;
#endif
      STAT(g_first[K] += first);
#ifdef STATS
      if (!first) {
        g_fwn[K][n]++, g_fw[K][n] += fn1 - fn0;
        auto cat = [&](u64 x) {
          if (!in_suit) return (x & TR) ? 4 : 5;
          const u64 win = (best & lane) ? in_suit & above(best) : 0;
          if (x == lowest(in_suit)) return 0;
          if (win && x == lowest(win)) return 1;
          if (x & win) return 2;
          return 3;
        };
        const int a = cat(firstc), b = cat(c);
        g_cat[K][a][b]++, g_catw[K][a][b] += fn1 - fn0;
        if (K == 1 && why0 >= 0 && n >= 9) g_whyc[why0][b]++, g_whyw[why0][b] += fn1 - fn0;
      }
#endif
      return r;
    }
    Racc |= Rc;
    if (!(c & Rc)) cand &= ~lane_of(c) | Rc;
    first = false;
#ifdef DDF_PAR
    // A work item that splits after hand 2's first reply: the rest go to other threads.
    if constexpr (K == 1) if (n == 13 && g_hot.ybw && cand) { g_hot.rest = cand; R = 0; return !cut; }
#endif
  }
#if RUNOPT
  Racc = add_runs(Racc, tried, mine, all);
#endif
  R = Racc;
  STAT(g_ff[K][n] += g_nodes - fn0);
  return !cut;
}

#include "guess_tables.inc"

// The first target to ask about. For 4 to 7 cards: an additive model fitted on random
// endings (tools/guess/), one table entry per suit keyed on the leader-relative suit lengths
// (capped at 3) and which side holds each of the suit's top four cards, plus a term for the
// difference in trump length. It estimates the leader's side's tricks e; the first target
// is floor(e) + 1, so that the two cheapest outcomes (the value is that target or one
// less) straddle the estimate. The guess only chooses which questions are asked.
// Full deals: a linear model of the leader's side's tricks, fitted on 6,000 random deals
// (dev/fit_guess.py). Features are differences between the leader's side and the other side:
// counts of the A K Q J T 9 in side suits [0-5] and in trumps [6-11], trump length [12],
// side-suit voids, singletons, doubletons [13-15], cards beyond the fourth in a suit [16],
// combined length beyond seven per suit [17] and in trumps [18]; [19] is the constant.
// Weights are in 1/256 trick.
static const int kFullSuit[20] = {208, 125, 63, 28, 11, 7, 232, 174, 114, 68, 36, 13, 173, 186, 88, 10, 14, -34, -175, 1715};
static const int kFullNT[20] = {282, 192, 105, 52, 23, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 23, 67, 0, 1808};

#include "guess_full.inc"
// Full deals (second model, dev/fit_guess2.py): features of the leader's side A (leader and
// partner) against B, per suit, then summed over the side suits (all four suits in no trumps)
// with the trump suit kept apart: which side holds each of the A K Q J T 9, the run of top
// cards held by one side (capped by its longest hand), length beyond the other side's longest
// hand (and that length when the other side has no stopper: A, Kx, Qxx or Jxxx), stoppers,
// pooled-holding suit tricks for each side, combined length beyond seven, cards beyond the
// fourth; in no trumps also the best suit of each side, whether a side stops every suit and
// the leader's longest suit; in suit contracts trump lengths, ruffing values per hand and
// the trump fit. A small MLP (one tanh hidden layer, softmax output) gives a distribution of the leader's side's tricks and
// the first target minimises the expected cost of the questions (see NOTES.md).
template <bool NT>
inline int guess_full(int L) {
  constexpr int F = NT ? kGF_FNT : kGF_FSUIT;
  int f[F] = {};
  int ln[4][4], trl[4] = {};
  int bestA = 0, bestB = 0, maxlA = 0, maxlB = 0, nstA = 0, nstB = 0, ll0 = 0, mtrA = 0, mtrB = 0;
  for (int l = 0; l < 4; l++) {
    unsigned x[4];
    for (int r = 0; r < 4; r++) x[r] = unsigned(H[(L + r) & 3] >> (16 * l)) & 0x1FFF, ln[r][l] = __builtin_popcount(x[r]);
    const bool tr = !NT && l == 3;
    const unsigned A = x[0] | x[2], B = x[1] | x[3];
    const int mA = std::max(ln[0][l], ln[2][l]), mB = std::max(ln[1][l], ln[3][l]);
    const int lenA = ln[0][l] + ln[2][l], lenB = ln[1][l] + ln[3][l];
    auto bit = [](unsigned y, int j) { return int(y >> (12 - j) & 1); };
    const int own = bit(A, 0);
    int run = 0;
    while (run < 13 && bit(A, run) == own) run++;
    const int runA = own ? std::min(run, mA) : 0, runB = own ? 0 : std::min(run, mB);
    auto stop = [&](int r) { return bit(x[r], 0) | (bit(x[r], 1) & (ln[r][l] >= 2)) | (bit(x[r], 2) & (ln[r][l] >= 3)) | (bit(x[r], 3) & (ln[r][l] >= 4)); };
    const int stA = stop(0) | stop(2), stB = stop(1) | stop(3);
    const int longA = std::max(0, mA - mB), longB = std::max(0, mB - mA);
    int ja[14], jb[14], na = 0, nb = 0;
    for (int j = 0; j < 13; j++) {
      if (bit(A, j)) ja[++na] = j;
      if (bit(B, j)) jb[++nb] = j;
    }
    int trA = 0, trB = 0;
    for (int k = 1; k <= 13; k++) {
      const int a = k <= na ? ja[k] : 99, b = k <= nb ? jb[k] : 99;
      trA += (k <= mA) & ((k > mB) | (a < b));
      trB += (k <= mB) & ((k > mA) | (b < a));
    }
    const int l7 = std::max(0, lenA - 7) - std::max(0, lenB - 7);
    int ln4 = 0;
    for (int r = 0; r < 4; r++) ln4 += (r & 1 ? -1 : 1) * std::max(0, ln[r][l] - 4);
    // column layout: NT: one column per feature; suits: side-suit sum then trump column
    const int vals[] = {runA - runB, longA * (1 - stB), longB * (1 - stA), longA - longB, stA - stB, trA, trB};
    if (NT) {
      for (int j = 0; j < 6; j++) f[j] += 2 * bit(A, j) - 1;
      for (int i = 0; i < 7; i++) f[6 + i] += vals[i];
      f[15] += l7, f[16] += ln4;
      bestA = std::max(bestA, runA + longA * (1 - stB)), bestB = std::max(bestB, runB + longB * (1 - stA));
      maxlA = std::max(maxlA, longA), maxlB = std::max(maxlB, longB);
      mtrA = std::max(mtrA, trA), mtrB = std::max(mtrB, trB);
      nstA += stA, nstB += stB, ll0 = std::max(ll0, ln[0][l]);
    } else {
      const int o = tr ? 6 : 0;
      for (int j = 0; j < 6; j++) f[o + j] += 2 * bit(A, j) - 1;
      for (int i = 0; i < 7; i++) f[12 + 2 * i + tr] += vals[i];
      f[26 + tr] += l7, f[28 + tr] += ln4;
      if (tr)
        for (int r = 0; r < 4; r++) trl[r] = ln[r][l];
    }
  }
  if (NT) {
    f[13] = mtrA, f[14] = mtrB, f[17] = bestA, f[18] = bestB, f[19] = maxlA, f[20] = maxlB;
    f[21] = nstB == 4, f[22] = nstA == 4, f[23] = ll0;
  } else {
    f[30] = trl[0] - trl[1] + trl[2] - trl[3];
    for (int r = 0; r < 4; r++) {
      int sh = 0;
      for (int l = 0; l < 3; l++) sh += std::max(0, 3 - ln[r][l]);
      f[31 + r] = std::min(sh, trl[r]);
    }
    f[35] = std::max(trl[0] + trl[2] - 8, 0) - std::max(trl[1] + trl[3] - 8, 0);
    f[36] = std::abs(trl[0] - trl[2]), f[37] = std::abs(trl[1] - trl[3]);
  }
  f[F - 1] = 1;
  const float* W0 = NT ? &kGF_W0NT[0][0] : &kGF_W0SUIT[0][0];  // [F][H]
  const float* W1 = NT ? &kGF_W1NT[0][0] : &kGF_W1SUIT[0][0];  // [H + 1][14]: logits per leader's-side value
  float hid[kGF_H] = {};
  for (int i = 0; i < F; i++)
    if (f[i])
      for (int k = 0; k < kGF_H; k++) hid[k] += float(f[i]) * W0[kGF_H * i + k];
  float sc[14];
  for (int v = 0; v < 14; v++) sc[v] = W1[14 * kGF_H + v];
  for (int k = 0; k < kGF_H; k++) {
    const float a = std::tanh(hid[k]);
    for (int v = 0; v < 14; v++) sc[v] += a * W1[14 * k + v];
  }
  float mx = sc[0];
  for (int v = 1; v < 14; v++) mx = std::max(mx, sc[v]);
  float p[14];
  for (int v = 0; v < 14; v++) p[v] = __builtin_expf(sc[v] - mx);
  int ta = 1;
  float best = 1e30f;
  for (int t = 1; t <= 13; t++) {  // expected extra cost of first target t (leader's side)
    float c = 0;
    for (int v = 0; v < 14; v++) c += p[v] * kGF_COST[t - v + 13];
    if (c < best) best = c, ta = t;
  }
  const int t = (L & 1) ? 14 - ta : ta;  // North-South's target
  return t < 1 ? 1 : t > 13 ? 13 : t;
}

template <bool NT>
inline int guess(int n, int L) {
  if (n == 13) return guess_full<NT>(L);
  const u64 all = H[0] | H[1] | H[2] | H[3];
  if (n >= 4 && n <= 7) {
    const int16_t* w = kGuessW[n - 4][NT];
    const u64 rot = std::rotr(LENS, 16 * L);  // nibble 4 * i + lane: the length in the i-th hand from the leader
    const u64 side = H[L] | H[L ^ 2];
    int e = 0;
    for (int l = 0; l < 4; l++) {
      const unsigned x = unsigned(all >> (16 * l)) & 0x1FFF;
      const unsigned pm = ddport::pext32(unsigned(side >> (16 * l)) & 0x1FFF, x);
      const unsigned pat = ((pm << (16 - popc(x))) >> 12) & 15;  // the top four cards, highest in bit 3
      const unsigned c0 = unsigned(rot >> (4 * l)) & 15, c1 = unsigned(rot >> (16 + 4 * l)) & 15;
      const unsigned c2 = unsigned(rot >> (32 + 4 * l)) & 15, c3 = unsigned(rot >> (48 + 4 * l)) & 15;
      const unsigned key = pat + 16 * ((c0 < 3 ? c0 : 3) + 4 * ((c1 < 3 ? c1 : 3) + 4 * ((c2 < 3 ? c2 : 3) + 4 * (c3 < 3 ? c3 : 3))));
      const bool trump = !NT && l == 3;
      e += w[(trump ? 4096 : 0) + key];
      if (trump) e += w[8192 + 14 + int(c0 + c2) - int(c1 + c3)];
    }
    if (NT) e += w[8192 + 14];
    if (L & 1) e = 64 * n - e;  // North-South's tricks, in 1/64
    const int t = (e + 64) >> 6;
    return t < 1 ? 1 : t > n ? n : t;
  }
  // Other sizes: a linear fit on top-card counts (made for six cards, scaled).
  const u64 ns = H[0] | H[2];
  int c[3] = {0, 0, 0}, t[3] = {0, 0, 0};
  for (int l = 0; l < 4; l++) {
    u64 x = all & LANE(l);
    int* d = (!NT && l == 3) ? t : c;
    for (int j = 0; j < 3 && x; j++) {
      u64 top = highest(x);
      x ^= top;
      for (int i = j; i < 3; i++) d[i] += (top & ns) != 0;
    }
  }
  int e;  // hundredths of a trick
  if (NT) e = -22 - 143 * (L & 1) + 119 * c[0] + 21 * c[1] + 12 * c[2];
  else e = 52 - 55 * (L & 1) + 76 * c[0] + 11 * c[1] + 6 * c[2] + 87 * t[0] + 30 * t[1] + 20 * t[2] +
           45 * (popc(ns & LANE(3)) - popc((all ^ ns) & LANE(3)));
  e = (e * n + 300) / 600;
  return e < 1 ? 1 : e > n ? n : e;
}

template <bool NT>
int solve_one(int n, int L, int first) {
  ensure_state();
  int lo = 0, hi = n;
#if defined(ABL_GUESS_DDS)  // DDS's first target: 7 tricks for the leader's side, 6 when E-W lead
  int t = first > 0 ? first : (L & 1) ? n + 1 - (n + 1) / 2 + 1 : (n + 1) / 2;
#else
  int t = first > 0 ? first : guess<NT>(n, L);
#endif
  if (first < 0) {  // a prior value -first - 1 (the same side's result from another leader): v or v + 1 by the model
    const int v = -first - 1;
    t = t > v ? v + 1 : v;
    t = t < 1 ? 1 : t > n ? n : t;
  }
  if (n == 13 && first <= 0) t = t < 1 ? 1 : t > 13 ? 13 : t;
#ifdef STATS
  const int t0 = t;
  int nq = 0;
  char qb[256]; int qp = 0; qb[0] = 0;
#endif
#ifdef STATS
  if (n == 13) g_rootL = L;
#endif
  S->rootL = L;
#ifdef MEMN
  if (n >= MEMN) memset(S->mres, 0, sizeof S->mres);  // read only for leads of MEMN cards
#endif
  while (lo < hi) {
    STAT(nq++);
    u64 R;
    STAT(g_queries++);
#ifdef STATS
    const u64 qn0 = g_nodes;
    const bool qr = lead<NT>(n, L, t, R);
    if (n == 13 && nq <= 4) qp += snprintf(qb + qp, sizeof qb - qp, " q%d %d %d %llu", nq, t, int(qr), (unsigned long long)(g_nodes - qn0));
    if (qr) {
#else
    if (lead<NT>(n, L, t, R)) {
#endif
      lo = t;
      t = lo + 1;
    } else {
      hi = t - 1;
      t = hi;
    }
  }
#ifdef STATS
  if (n == 13) fprintf(stderr, "GUESS nt %d L %d first %d value %d queries %d%s\n", int(NT), L, t0, lo, nq, qb);
#endif
  return lo;
}

// ---------------------------------------------------------------------------------------
// One question: do North-South take at least t of the n tricks (1 <= t <= n)? As solve_one,
// for a single target.
template <bool NT>
bool ask_one(int n, int L, int t) {
  ensure_state();
  S->rootL = L;
#ifdef MEMN
  if (n >= MEMN) memset(S->mres, 0, sizeof S->mres);
#endif
  u64 R;
  STAT(g_queries++);
  return lead<NT>(n, L, t, R);
}

// ---------------------------------------------------------------------------------------
// Coroutines: several positions are searched in turn on one thread. A search that needs a
// three-card table entry prefetches it and hands over to the next search, so the memory
// access is over by the time it resumes.
#ifdef DDF_PAR
#define ddfast_ctx_switch ddfast_par_ctx_switch
DDPORT_DEFINE_CTX_SWITCH(ddfast_par_ctx_switch)
#else
DDPORT_DEFINE_CTX_SWITCH(ddfast_ctx_switch)
#endif

constexpr int MAX_CTX = 64;
constexpr size_t STACK_BYTES = 256 << 10;
struct Ctx {
  void* sp;
  u64 h[4];
  u64 lens;
  uint32_t gen;
  u64 mnodes;  // lead memory's node count (MEMCOST), per search
  int next, prev;
  SState* ss;
};
thread_local SState* g_sstates;
thread_local Ctx g_ctx[MAX_CTX];
thread_local int g_cur = -1;  // the running context, or -1 outside a batch
thread_local void* g_main_sp;
thread_local char* g_stacks;
thread_local const ddcore::Position* g_pos;
thread_local size_t g_count, g_next;
thread_local uint8_t* g_out;
thread_local uint32_t g_gen_counter = 1;
thread_local void (*g_job)(size_t) = nullptr;  // a batch of other jobs than positions (tables)
thread_local const uint16_t* const* g_thand;   // tables: 16 suit masks per deal
thread_local uint8_t* const* g_tout;           // tables: 20 results per deal

inline void save_state(Ctx& c) {
  c.h[0] = H[0], c.h[1] = H[1], c.h[2] = H[2], c.h[3] = H[3];
  c.lens = LENS;
  c.gen = g_gen;
  c.mnodes = g_mnodes;
}
inline void load_state(const Ctx& c) {
  H[0] = c.h[0], H[1] = c.h[1], H[2] = c.h[2], H[3] = c.h[3];
  LENS = c.lens;
  g_gen = c.gen;
  g_mnodes = c.mnodes;
  S = c.ss;
}

__attribute__((noinline)) void yield_switch() {
  if (g_cur < 0) return;
  Ctx& a = g_ctx[g_cur];
  if (a.next == g_cur) return;
  save_state(a);
  g_cur = a.next;
  Ctx& b = g_ctx[g_cur];
  load_state(b);
  ddfast_ctx_switch(&a.sp, b.sp);
}

// keep: the previous call was the same deal and strain, so its table still holds (its entries
// are valid for any leader); a full table solves the four declarers of a strain this way.
// ask > 0: answer only "do North-South take at least ask tricks?" (1 or 0; 1 <= ask <= n).
inline int solve_position(const ddcore::Position& p, int first = 0, bool keep = false, int ask = 0) {
  ensure_state();
  const int tr = p.trump;
  u64 lens = 0;
  int n = 0;
  for (int q = 0; q < 4; q++) {
    u64 hq = 0;
    for (int s = 0; s < 4; s++) {
      int lane = s;
      if (tr < 4) lane = s == tr ? 3 : (s == 3 ? tr : s);
      hq |= u64(p.hand[q][s]) << (16 * lane);
      lens |= u64(__builtin_popcount(p.hand[q][s])) << (16 * q + 4 * lane);
    }
    H[q] = hq;
    if (q == 0) n = popc(hq);
  }
  LENS = lens;
  if (!keep) {
    g_gen = ++g_gen_counter;
    S->tt.reset();
  }
  if (ask > 0) return tr == 4 ? int(ask_one<true>(n, p.leader, ask)) : int(ask_one<false>(n, p.leader, ask));
  return tr == 4 ? solve_one<true>(n, p.leader, first) : solve_one<false>(n, p.leader, first);
}

void ctx_main() {
  for (;;) {
    const size_t i = g_next;
    if (i >= g_count) break;
    g_next = i + 1;
    if (g_job) {
      g_job(i);
      continue;
    }
    g_out[i] = uint8_t(solve_position(g_pos[i], ddfast::detail::g_first_target ? ddfast::detail::g_first_target[i] : 0));
  }
  Ctx& a = g_ctx[g_cur];
  if (a.next == g_cur) {
    g_cur = -1;
    ddfast_ctx_switch(&a.sp, g_main_sp);
  } else {
    g_ctx[a.prev].next = a.next;
    g_ctx[a.next].prev = a.prev;
    g_cur = a.next;
    Ctx& b = g_ctx[g_cur];
    load_state(b);
    ddfast_ctx_switch(&a.sp, b.sp);
  }
  abort();
}

void run_batch(const ddcore::Position* positions, size_t count, uint8_t* tricks, int nctx) {
  g_pos = positions, g_count = count, g_next = 0, g_out = tricks;
  if (size_t(nctx) > count) nctx = int(count);
  if (nctx < 1) return;
  if (!g_stacks) g_stacks = static_cast<char*>(aligned_alloc(4096, MAX_CTX * STACK_BYTES));
  if (!g_sstates) g_sstates = new SState[MAX_CTX]();
  for (int i = 0; i < nctx; i++) {
    Ctx& c = g_ctx[i];
    c.next = (i + 1) % nctx;
    c.prev = (i + nctx - 1) % nctx;
    c.sp = ddport::init_stack(g_stacks + (i + 1) * STACK_BYTES, &ctx_main);
    c.ss = &g_sstates[i];
    if (!c.ss->tt.dir) c.ss->tt.alloc();
    save_state(c);
  }
  SState* const s0 = S;
  const u64 m0 = g_mnodes;
  g_inter = nctx > 1;
  g_cur = 0;
  S = g_ctx[0].ss;
  ddfast_ctx_switch(&g_main_sp, g_ctx[0].sp);
  g_inter = false;
  S = s0, g_mnodes = m0;
}

}  // namespace

#ifndef TPGUESS
#define TPGUESS 1
#endif
#ifndef DDF_PAR
namespace ddfast::detail {


void init(const char* dir, bool map_tables) {
  if (const char* m = ddport::cpu_check()) {
    fprintf(stderr, "%s\n", m);
    abort();
  }
  ensure_state();
  // The endgame tables: the two-card table (built in memory), the three-card table
  // and the four-card pattern table, loaded from the table directory or built there.
  const char* lv = getenv("DDFAST_TABLE_LEVELS");
  ddcore::tables::init_tables(dir, lv && *lv ? atoi(lv) : 1, map_tables);
  ddcore::tables::attach();  // this file's copy of the table state (see tables.hpp)
  if (getenv("LEAN_NOP4")) g_pt[0].use = false;
  if (getenv("LEAN_NOT3")) g_use3 = false;
  if (getenv("LEAN_NOT2")) g_use2 = false;  // leave-one-out / DDS-search study: search 2-card endings
#ifdef STATS
  g_nodes = g_leads = g_queries = g_tthit = g_qt = g_lt = g_ttlook = 0;
  memset(g_plook, 0, sizeof g_plook), memset(g_phit, 0, sizeof g_phit);
  memset(g_n, 0, sizeof g_n), memset(g_fn, 0, sizeof g_fn), memset(g_first, 0, sizeof g_first), memset(g_tried, 0, sizeof g_tried);
#endif
}


void solve(const Position* positions, size_t count, uint8_t* tricks) {
  run_batch(positions, count, tricks, NCTX);
}

// North-South's tricks for each position, nctx searches interleaved on this thread.
void solve_positions(const Position* positions, size_t count, uint8_t* tricks, int nctx) {
  ensure_state();
  run_batch(positions, count, tricks, nctx);
}

// Full tables, nctx interleaved on this thread: hands[i] holds deal i's 16 suit masks, out[i]
// gets its 20 results as solve_table writes them.
void solve_tables_interleaved(const uint16_t* const* hands, uint8_t* const* out, size_t count, int nctx) {
  ensure_state();
  g_thand = hands, g_tout = out;
  g_job = [](size_t i) {
    uint16_t h[4][4];
    uint8_t t[5][4];
    memcpy(h, g_thand[i], sizeof h);
    solve_table(h, t);
    memcpy(g_tout[i], t, sizeof t);
  };
  run_batch(nullptr, count, nullptr, nctx);
  g_job = nullptr;
}

int solve_position(const Position& p, bool keep) {
  ensure_state();
  if (!S->tt.dir) S->tt.alloc();  // as in solve_table
  return ::solve_position(p, 0, keep);
}

bool query_position(const Position& p, int target, bool keep) {
  ensure_state();
  if (!S->tt.dir) S->tt.alloc();
  return ::solve_position(p, 0, keep, target) != 0;
}

void solve_table(const uint16_t hand[4][4], uint8_t tricks[5][4]) {
  Position p;
  memcpy(p.hand, hand, sizeof p.hand);
  ensure_state();
  if (!S->tt.dir) S->tt.alloc();  // the default state's table is allocated on first use
  for (int strain = 0; strain < 5; strain++) {
    p.trump = uint8_t(strain);
    int v[4] = {-1, -1, -1, -1};  // North-South's tricks, by declarer
    for (int di = 0; di < 4; di++) {
#if TPGUESS
      // East, West, South, then North lead: South's first target then comes from both
      // East-West results.
      const int d = (0x3120 >> (4 * di)) & 15;
#else
      const int d = di;
#endif
      p.leader = uint8_t((d + 1) & 3);
      // First targets from the results already found (they only choose the questions, so
      // the answers stay exact). The same side's other declarer (the other defender leads)
      // usually gives the same tricks (93%): that value and one more or one less, by the
      // model. North-South on lead almost always take as many tricks as when either East or
      // West leads, or one more (96%).
      int first = 0;
#if TPGUESS
      if (v[d ^ 2] >= 0) first = -1 - v[d ^ 2];
      else if (d & 1) {
        const int o = std::max(v[0], v[2]);
        if (o >= 0) first = std::min(o + 1, 13);
      } else {
        const int o = std::max(v[1], v[3]);
        if (o >= 0) first = std::max(o, 1);
      }
#endif
      const int ns = ::solve_position(p, first, di > 0);
      v[d] = ns;
      tricks[strain][d] = uint8_t((d & 1) ? popc(H[0]) - ns : ns);
    }
  }
}

unsigned long long nodes() {
#ifdef STATS
  return g_nodes;
#else
  return 0;
#endif
}

unsigned long long lead_nodes() { return g_mnodes; }

}  // namespace ddfast::detail
#endif  // DDF_PAR
