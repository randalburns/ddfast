// ddclaim: an exact double-dummy solver for small endings (a lean scalar zero-window search
// over tables of solved endings). See README.md for use and NOTES.md for the design.
#include "ddclaim/port.hpp"

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
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ddclaim.hpp"
#include "ddclaim/tables.hpp"

namespace ddport {
bool pin_to_cpu(int cpu) {
#if defined(__linux__)
  if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#elif defined(__APPLE__)
  (void)cpu;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  return false;
#else
  (void)cpu;
  return false;
#endif
}
}  // namespace ddport

namespace {

// The hand-word helpers and the tables of solved endings, with their probes, are in
// ddclaim/tables.hpp (shared with programs that use the tables).
using namespace ddclaim::bits;
using namespace ddclaim::tables;

#ifdef DBG
}
void dbg_hit(const uint64_t* h, int L, bool nt, int target, bool res, uint64_t mask, uint64_t own, uint64_t all, int n);
namespace {
int g_sw = getenv("SW") ? atoi(getenv("SW")) : 15;
#define SW(b) (g_sw & (b))
#else
#define SW(b) 1
#endif
#ifdef STATS
}
unsigned long long g_plook[P_LEVELS], g_phit[P_LEVELS];
namespace {
u64 g_n[8][5], g_fn[8], g_nodes, g_leads, g_queries, g_tthit, g_qt, g_lt, g_ttlook, g_first[4], g_tried[4];
struct StatPrinter {
  ~StatPrinter() {
    fprintf(stderr, "queries %llu nodes %llu (%.1f/query) leads %llu qt %llu lt %llu ttlook %llu tthit %llu\n",
            (unsigned long long)g_queries, (unsigned long long)g_nodes, double(g_nodes) / double(g_queries),
            (unsigned long long)g_leads, (unsigned long long)g_qt, (unsigned long long)g_lt,
            (unsigned long long)g_ttlook, (unsigned long long)g_tthit);
    for (int i = 0; i < P_LEVELS; i++) fprintf(stderr, "p%d: look %llu hit %llu\n", i + 4, g_plook[i], g_phit[i]);
    for (int n = 1; n < 8; n++)
      fprintf(stderr, "  n=%d: leads %llu qt %llu lt %llu tthit %llu expanded %llu follow-nodes %llu\n", n, (unsigned long long)g_n[n][0], (unsigned long long)g_n[n][1], (unsigned long long)g_n[n][2], (unsigned long long)g_n[n][3], (unsigned long long)g_n[n][4], (unsigned long long)g_fn[n]);
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
#ifndef TT_MIN_N
#define TT_MIN_N 5
#endif
constexpr int TT_MIN = TT_MIN_N;
thread_local Bucket g_tt[1 << TT_BITS];

// The position being searched. Thread-local so that table building can use several threads.
thread_local u64 H[4];
thread_local u64 LENS;  // nibble 4*seat+suit: the length of that holding
thread_local uint32_t g_gen = 1;

inline Bucket& bucket_for(u64 tag) { return g_tt[((tag ^ g_gen) * 0x9E3779B97F4A7C15ull) >> (64 - TT_BITS)]; }

inline int len_of(int seat, int suit) { return int(LENS >> (16 * seat + 4 * suit)) & 15; }

template <bool NT, int K>
bool follow(int n, int L, int target, u64 all, u64 lane, u64 best, int bw, bool ranked, u64& R);

void yield();

thread_local std::vector<PRec>* g_plog;  // new patterns while filling (one vector per level)
#ifdef STATS
}
extern unsigned long long g_plook[P_LEVELS], g_phit[P_LEVELS];
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

template <bool NT>
bool lead(int n, int L, int target, u64& R) {
  constexpr u64 TR = Cfg<NT>::TR;
  STAT(g_nodes++);
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
  if (n == 3 && g_use3) return probe3<NT>(H, L, all, target, R, [] { yield(); });
  const int needL = cut ? target : n - target + 1;
  if (SW(1) && quick_tricks<NT>(L, needL, all, R)) {
    STAT(g_qt++);STAT(g_n[n][1]++);
    return cut;
  }
  if (SW(2) && later_tricks<NT>(L, n + 1 - needL, all, R)) {
    STAT(g_lt++);STAT(g_n[n][2]++);
    return !cut;
  }

  // Table.
  const u64 tag = LENS | u64(L & 1) << 3 | u64(L >> 1) << 7;
  Bucket& b = bucket_for(tag);
  const u64 own = ddport::pext64(H[1] | H[3], all) | ddport::pext64(H[2] | H[3], all) << 28;
  u64 hint = 0;
  STAT(g_ttlook++);
  if (n >= TT_MIN && SW(4) && b.tag == tag && b.gen == g_gen) {
    unsigned k = ddport::match8(b.mask, b.val, own) &
                 ((1u << b.cnt) - 1);
    while (k) {
      int i = __builtin_ctz(k);
      k &= k - 1;
#ifdef DBG
      if (b.lo[i] >= target || b.hi[i] < target) dbg_hit(H, L, NT, target, b.lo[i] >= target, b.mask[i], own, all, n);
#endif
      if (b.lo[i] >= target) {
        R = fill_up(ddport::pdep64(b.mask[i] & 0xFFFFFFF, all));
        STAT(g_tthit++);STAT(g_n[n][3]++);
        return true;
      }
      if (b.hi[i] < target) {
        R = fill_up(ddport::pdep64(b.mask[i] & 0xFFFFFFF, all));
        STAT(g_tthit++);STAT(g_n[n][3]++);
        return false;
      }
    }
    if (b.hint < 64) hint = ddport::pdep64(1ull << b.hint, all);
  }

  // Persistent pattern table.
  PatternQuery pq;
  if (n >= 4 && n < 4 + P_LEVELS && g_pt[n - 4].use) {
    STAT(g_plook[n - 4]++);
    size_t hit;
    const int r = probe_pattern<NT>(H, LENS, n, L, all, target, pq, R, hit, [] { yield(); });
    if (r >= 0) {
      STAT(g_phit[n - 4]++);
      if (g_plog) {
        uint16_t& c = g_pt[n - 4].cnt[hit];
        if (c != 0xFFFF) c++;
      }
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
  {
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
  u64 Racc = 0;
  bool res = !cut;
  u64 bestc = 0;
#ifdef STATS
  int tried = 0;
#endif
  while (cand) {
    // The best remaining card (highest score, then earliest), chosen without branches.
    const unsigned bk = ddport::best_key(cards, key, cand);
    const u64 c = cards[15 - (bk & 15)];
    cand ^= c;
    const int sh = 16 * L + 4 * suit_of(c);
    H[L] ^= c;
    LENS -= 1ull << sh;
    u64 Rc;
    bool r = follow<NT, 1>(n, L, target, all, lane_of(c), c, L, false, Rc);
    H[L] ^= c;
    LENS += 1ull << sh;
#ifdef STATS
    tried++;
#endif
    if (r == cut) {
      Racc = Rc;
      res = cut;
      bestc = c;
      STAT(g_tried[0]++);
      STAT(g_first[0] += tried == 1);
      break;
    }
    Racc |= Rc;
    if (SW(8) && !(c & Rc)) cand &= ~lane_of(c) | Rc;
  }
  R = Racc;
  // Store.
  if (pq.key && g_plog) {
    const u64 rs = shuffle_lanes(Racc, g_shuf[pq.pid]);
    const u64 m = ddport::pext64(rs, pq.alls);
    const int tl = cut ? target : n + 1 - target;
    const u64 bounds = res == cut ? u64(tl) << 56 | 7ull << 59 : u64(tl - 1) << 59;
    const u64 mm = m | m << (4 * n);
    g_plog[n - 4].push_back({pq.key, pq.own & mm, mm | bounds | 1ull << 63});
  }
  if (n >= TT_MIN) {
    if (b.tag != tag || b.gen != g_gen) {
      b.tag = tag;
      b.gen = g_gen;
      b.cnt = 0;
      b.nxt = 0;
      b.hint = 255;
    }
    if (bestc) b.hint = uint8_t(popc(all & (bestc - 1)));
    const u64 m28 = ddport::pext64(Racc, all);
    const u64 mask = m28 | m28 << 28;
    const u64 val = own & mask;
    int i;
    for (i = 0; i < b.cnt; i++)
      if (b.mask[i] == mask && b.val[i] == val) break;
    if (i == b.cnt) {
      if (b.cnt < 8) b.cnt++;
      else i = b.nxt++ & 7;
      b.mask[i] = mask;
      b.val[i] = val;
      b.lo[i] = 0;
      b.hi[i] = uint8_t(n);
    }
    if (res) {
      if (b.lo[i] < target) b.lo[i] = uint8_t(target);
    } else {
      if (b.hi[i] > target - 1) b.hi[i] = uint8_t(target - 1);
    }
  }
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
  if (!win) return lowest(cand);
  if (!nl) return lowest(win);
  u64 w2 = win & above(highest(nl));
  return w2 ? lowest(w2) : lowest(cand);
}

template <bool NT, int K>
inline u64 pick_void(u64 cand, bool first, int s, u64 best, int bw) {
  constexpr u64 TR = Cfg<NT>::TR;
  const u64 mt = cand & TR;
  if (first && mt) {
    const bool pw = bw == (s ^ 2);
    const u64 wr = (best & TR) ? mt & above(best) : mt;
    if (wr && !pw) return lowest(wr);
  }
  const u64 side = cand & ~TR;
  if (!side) return lowest(cand);
  return min_rank(side);
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
  u64 Racc = 0;
  bool first = true;
  while (cand) {
    const u64 c = in_suit ? pick_follow<NT, K>(cand, first, s, lane, best, bw) : pick_void<NT, K>(cand, first, s, best, bw);
    cand ^= c;
    const bool same = (c & lane_of(best)) != 0, ruff = (c & TR) != 0;
    const bool beats = same ? c > best : ruff;
    const u64 nb = beats ? c : best;
    const int nbw = beats ? s : bw;
    const bool nr = same | (ranked & !ruff);
    const int sh = 16 * s + 4 * suit_of(c);
    H[s] ^= c;
    LENS -= 1ull << sh;
    u64 Rc;
    bool r;
    if (K < 3) {
      r = follow<NT, K < 3 ? K + 1 : 3>(n, L, target, all, lane, nb, nbw, nr, Rc);
    } else {
      const u64 Rt = ge(run_bottom(nb, H[nbw], all)) & (0 - u64(nr));
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
      STAT(g_tried[K]++);
      STAT(g_first[K] += first);
      return r;
    }
    Racc |= Rc;
    if (SW(8) && !(c & Rc)) cand &= ~lane_of(c) | Rc;
    first = false;
  }
  R = Racc;
  return !cut;
}

#include "guess_tables.inc"

// The first target to ask about. For 4 to 7 cards: an additive model fitted on random
// endings (tools/guess/), one table entry per suit keyed on the leader-relative suit lengths
// (capped at 3) and which side holds each of the suit's top four cards, plus a term for the
// difference in trump length. It estimates the leader's side's tricks e; the first target
// is floor(e) + 1, so that the two cheapest outcomes (the value is that target or one
// less) straddle the estimate. The guess only chooses which questions are asked.
template <bool NT>
inline int guess(int n, int L) {
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
int solve_one(int n, int L) {
  int lo = 0, hi = n;
  int t = guess<NT>(n, L);
  while (lo < hi) {
    u64 R;
    STAT(g_queries++);
    if (lead<NT>(n, L, t, R)) {
      lo = t;
      t = lo + 1;
    } else {
      hi = t - 1;
      t = hi;
    }
  }
  return lo;
}

// For one (composition, strain, target): replace each entry's relevant cards by a smallest
// set of top cards that is exact, i.e. every two-card ending with the same suit lengths in
// every hand and the same owners of those cards has the same result.
void minimize_t2(uint16_t* tab, const u64* lens, const uint16_t* own, const int* len) {
  struct Slot { u64 key; uint32_t stamp; uint32_t flags; };
  static thread_local Slot slots[8192];
  static thread_local uint32_t stamp = 0;
  int off[4];
  for (int s = 0, o = 0; s < 4; o += len[s], s++) off[s] = o;
  uint8_t best[2520], bestpop[2520];
  u64 keys[2520];
  memset(bestpop, 9, sizeof bestpop);
  for (int k0 = 0; k0 <= len[0]; k0++)
    for (int k1 = 0; k1 <= len[1]; k1++)
      for (int k2 = 0; k2 <= len[2]; k2++)
        for (int k3 = 0; k3 <= len[3]; k3++) {
          const int k[4] = {k0, k1, k2, k3};
          unsigned m = 0;
          for (int s = 0; s < 4; s++) m |= (((1u << k[s]) - 1) << (len[s] - k[s])) << off[s];
          const int pc = popc(m);
          stamp++;
          for (int r = 0; r < 2520; r++) {
            const u64 key = (lens[r] * 0x9E3779B97F4A7C15ull) ^ ((own[r] & (m | m << 8)) * 0xC2B2AE3D27D4EB4Full);
            keys[r] = key;
            unsigned i = unsigned(key >> 40) & 8191;
            while (slots[i].stamp == stamp && slots[i].key != key) i = (i + 1) & 8191;
            if (slots[i].stamp != stamp) slots[i] = {key, stamp, 0};
            slots[i].flags |= 1u << (tab[2 * r] >> 8);
          }
          for (int r = 0; r < 2520; r++) {
            if (pc >= bestpop[r]) continue;
            unsigned i = unsigned(keys[r] >> 40) & 8191;
            while (slots[i].key != keys[r] || slots[i].stamp != stamp) i = (i + 1) & 8191;
            if (slots[i].flags != 3) bestpop[r] = uint8_t(pc), best[r] = uint8_t(m);
          }
        }
  for (int r = 0; r < 2520; r++) tab[2 * r] = uint16_t((tab[2 * r] & 0x100) | best[r]);
}

void build_t2() {
  int c70 = 0, c6 = 0;
  for (int m = 0; m < 256; m++) g_r70[m] = popc(m) == 4 ? c70++ : 255;
  for (int m = 0; m < 16; m++) g_r6[m] = popc(m) == 2 ? c6++ : 255;
  g_t2 = static_cast<uint16_t*>(aligned_alloc(4096, 165 * 2 * 2520 * 2 * sizeof(uint16_t) + 4096 - (165 * 2 * 2520 * 2 * sizeof(uint16_t)) % 4096));
  static u64 lens_of[165][2520];
  static uint16_t own_of[165][2520];
  static int len_of_comp[165][4];
  int nc = 0;
  for (int l2 = 0; l2 <= 8; l2++)
    for (int l1 = 0; l1 + l2 <= 8; l1++)
      for (int l0 = 0; l0 + l1 + l2 <= 8; l0++) {
        const int comp = nc++;
        g_comp[l0 + 9 * l1 + 81 * l2] = uint8_t(comp);
        const int len[4] = {l0, l1, l2, 8 - l0 - l1 - l2};
        memcpy(len_of_comp[comp], len, sizeof len);
        u64 card[8];
        int suit[8];
        for (int s = 0, i = 0; s < 4; s++)
          for (int r = 0; r < len[s]; r++, i++) card[i] = 1ull << (16 * s + r), suit[i] = s;
        for (unsigned lo = 0; lo < 256; lo++) {
          if (popc(lo) != 4) continue;
          for (unsigned a = 0; a < 16; a++) {
            if (popc(a) != 2) continue;
            for (unsigned b = 0; b < 16; b++) {
              if (popc(b) != 2) continue;
              const unsigned hi = ddport::pdep32(a, lo) | ddport::pdep32(b, lo ^ 0xFF);
              const unsigned rank = g_r70[lo] * 36 + g_r6[a] * 6 + g_r6[b];
              u64 h[4] = {0, 0, 0, 0}, lens = 0;
              for (int i = 0; i < 8; i++) {
                int seat = (lo >> i & 1) + 2 * (hi >> i & 1);
                h[seat] |= card[i];
                lens += 1ull << (16 * seat + 4 * suit[i]);
              }
              lens_of[comp][rank] = lens;
              own_of[comp][rank] = uint16_t(lo | hi << 8);
              const u64 all = h[0] | h[1] | h[2] | h[3];
              for (int nt = 0; nt < 2; nt++)
                for (int t = 1; t <= 2; t++) {
                  memcpy(H, h, sizeof h);
                  LENS = lens;
                  g_gen++;
                  u64 R;
                  bool res = nt ? lead<true>(2, 0, t, R) : lead<false>(2, 0, t, R);
                  g_t2[((comp * 2 + nt) * 2520 + rank) * 2 + (t - 1)] = uint16_t(ddport::pext64(R, all) | (res ? 256 : 0));
                }
            }
          }
        }
      }
  std::atomic<int> next{0};
  auto work = [&] {
    for (;;) {
      int j = next.fetch_add(1);
      if (j >= 165 * 4) break;
      int comp = j >> 2, nt = j >> 1 & 1, t = j & 1;
      minimize_t2(g_t2 + ((comp * 2 + nt) * 2520) * 2 + t, lens_of[comp], own_of[comp], len_of_comp[comp]);
    }
  };
  std::vector<std::thread> th;
  for (int i = 0; i < 8; i++) th.emplace_back(work);
  for (auto& x : th) x.join();
  g_use2 = true;
}


// ---------------------------------------------------------------------------------------
// The three-card table.
// Where the table files live: $DDCLAIM_DIR, or build/ddclaim under the current directory.
void make_table_dir() {
  const std::string d = table_dir();
  for (size_t i = 1; i <= d.size(); i++)
    if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0777);
}
// Computed when the table is loaded, not at start-up: the caller may set the directory first.
std::string t3_path() { return table_dir() + "/t3_v2.bin"; }

void build_t3_tables() {
  int c = 0;
  for (int m = 0; m < 4096; m++) g_r924[m] = popc(m) == 6 ? uint16_t(c++) : 0xFFFF;
  c = 0;
  for (int m = 0; m < 64; m++) g_r20[m] = popc(m) == 3 ? uint8_t(c++) : 0xFF;
  for (int pid = 0; pid < 8; pid++) {
    const int c01 = pid & 1, c02 = pid >> 1 & 1, c12 = pid >> 2 & 1;
    int rank[4] = {c01 + c02, !c01 + c12, !c02 + !c12, 3};
    memset(g_shuf[pid], 0x80, 16);
    memset(g_unshuf[pid], 0x80, 16);
    bool ok = rank[0] != rank[1] && rank[0] != rank[2] && rank[1] != rank[2];
    for (int i = 0; i < 4; i++) {
      int r = ok ? rank[i] : i;  // output lane r <- input lane i
      g_shuf[pid][2 * r] = uint8_t(2 * i), g_shuf[pid][2 * r + 1] = uint8_t(2 * i + 1);
      g_unshuf[pid][2 * i] = uint8_t(2 * r), g_unshuf[pid][2 * i + 1] = uint8_t(2 * r + 1);
    }
  }
}

// The sorted suit lengths (a >= b >= c, then the trump lane) of every config.
int g_cfg3_len[T3_CFG][4];
void enum_cfg3() {
  int id = 0;
  for (int t = 0; t <= 12; t++)
    for (int a = 12 - t; a >= 0; a--)
      for (int b = a; b >= 0; b--) {
        int c = 12 - t - a - b;
        if (c < 0 || c > b) continue;
        g_cfg3_len[id][0] = a, g_cfg3_len[id][1] = b, g_cfg3_len[id][2] = c, g_cfg3_len[id][3] = t;
        int p[3] = {a, b, c};
        for (int x = 0; x < 3; x++)
          for (int y = 0; y < 3; y++)
            for (int z = 0; z < 3; z++)
              if (x != y && x != z && y != z) g_cfg3[p[x] + 13 * p[y] + 169 * p[z]] = uint8_t(id);
        id++;
      }
  if (id != int(T3_CFG)) abort();
}

void gen_t3_part(int cfg, unsigned lo) {
  const int* len = g_cfg3_len[cfg];
  u64 card[12];
  int suit[12];
  for (int s = 0, i = 0; s < 4; s++)
    for (int r = 0; r < len[s]; r++, i++) card[i] = 1ull << (16 * s + r), suit[i] = s;
  for (unsigned a = 0; a < 64; a++) {
    if (popc(a) != 3) continue;
    for (unsigned b = 0; b < 64; b++) {
      if (popc(b) != 3) continue;
      const unsigned hi = ddport::pdep32(a, lo) | ddport::pdep32(b, lo ^ 0xFFF);
      const size_t rank = size_t(g_r924[lo]) * 400 + g_r20[a] * 20 + g_r20[b];
      u64 h[4] = {0, 0, 0, 0}, lens = 0;
      for (int i = 0; i < 12; i++) {
        int seat = (lo >> i & 1) + 2 * (hi >> i & 1);
        h[seat] |= card[i];
        lens += 1ull << (16 * seat + 4 * suit[i]);
      }
      const u64 all = h[0] | h[1] | h[2] | h[3];
      for (int nt = 0; nt < 2; nt++) {
        u64 e = 0;
        int v = 0;
        for (int t = 1; t <= 3; t++) {
          H[0] = h[0], H[1] = h[1], H[2] = h[2], H[3] = h[3];
          LENS = lens;
          u64 R;
          bool res = nt ? lead<true>(3, 0, t, R) : lead<false>(3, 0, t, R);
          v += res;
          e |= ddport::pext64(R, all) << (12 * t - 10);
        }
        e |= u64(v);
        memcpy(g_t3 + ((size_t(cfg) * 2 + nt) * T3_ARR + rank) * 5, &e, 5);
      }
    }
  }
}

// Shrinks the relevant cards of every entry of one config to a minimal set that is still
// exact: all three-card endings with the same suit lengths in every hand and the same owners
// of those cards have the same answer. A set of top cards is described by how many top cards
// of each suit it has (four nibbles); the endings that agree on it are found by sorting the
// owners of all other cards, which gives a canonical ending whose rank identifies the group.
void minimize_t3(int cfg) {
  const int* len = g_cfg3_len[cfg];
  const size_t N = T3_ARR;
  int off[4];
  for (int s = 0, o = 0; s < 4; o += len[s], s++) off[s] = o;
  std::vector<uint16_t> lo(N), hi(N);
  for (unsigned l = 0; l < 4096; l++) {
    if (popc(l) != 6) continue;
    for (unsigned a = 0; a < 64; a++) {
      if (popc(a) != 3) continue;
      for (unsigned b = 0; b < 64; b++) {
        if (popc(b) != 3) continue;
        const size_t rank = size_t(g_r924[l]) * 400 + g_r20[a] * 20 + g_r20[b];
        lo[rank] = uint16_t(l);
        hi[rank] = uint16_t(ddport::pdep32(a, l) | ddport::pdep32(b, l ^ 0xFFF));
      }
    }
  }
  // Variant v = 3 * nt + (target - 1).
  std::vector<uint8_t> res(N);
  std::vector<uint16_t> cur[6];
  std::vector<uint32_t> cnt[6];
  for (int v = 0; v < 6; v++) cur[v].resize(N), cnt[v].assign(65536, 0);
  for (int nt = 0; nt < 2; nt++) {
    const uint8_t* base = g_t3 + (size_t(cfg) * 2 + nt) * T3_ARR * 5;
    for (size_t r = 0; r < N; r++) {
      u64 e = 0;
      memcpy(&e, base + r * 5, 5);
      const int val = int(e & 3);
      for (int t = 1; t <= 3; t++) {
        const int v = 3 * nt + t - 1;
        if (val >= t) res[r] |= uint8_t(1 << v);
        const unsigned m = unsigned(e >> (12 * t - 10)) & 0xFFF;
        unsigned k = 0;
        for (int s = 0; s < 4; s++) k |= unsigned(popc((m >> off[s]) & ((1u << len[s]) - 1))) << (4 * s);
        cur[v][r] = uint16_t(k);
        cnt[v][k]++;
      }
    }
  }
  // Sets of top cards, larger ones first.
  std::vector<unsigned> ks;
  for (int k0 = 0; k0 <= len[0]; k0++)
    for (int k1 = 0; k1 <= len[1]; k1++)
      for (int k2 = 0; k2 <= len[2]; k2++)
        for (int k3 = 0; k3 <= len[3]; k3++) ks.push_back(unsigned(k0 | k1 << 4 | k2 << 8 | k3 << 12));
  auto total = [](unsigned k) { return (k & 15) + (k >> 4 & 15) + (k >> 8 & 15) + (k >> 12); };
  std::stable_sort(ks.begin(), ks.end(), [&](unsigned a, unsigned b) { return total(a) > total(b); });
  std::vector<uint32_t> gid(N);
  std::vector<uint8_t> f0(N), f1(N);
  for (unsigned k : ks) {
    unsigned want = 0;  // variants with an entry one card above k
    for (int s = 0; s < 4; s++) {
      if (int(k >> (4 * s) & 15) >= len[s]) continue;
      for (int v = 0; v < 6; v++)
        if (cnt[v][k + (1u << (4 * s))]) want |= 1u << v;
    }
    if (!want) continue;
    unsigned seg[4];  // the cards of each suit below the top k
    for (int s = 0; s < 4; s++) seg[s] = ((1u << (len[s] - int(k >> (4 * s) & 15))) - 1) << off[s];
    const unsigned segs = seg[0] | seg[1] | seg[2] | seg[3];
    memset(f0.data(), 0, N);
    memset(f1.data(), 0, N);
    for (size_t r = 0; r < N; r++) {
      const unsigned l = lo[r], h = hi[r];
      unsigned nl = l & ~segs, nh = h & ~segs;
      for (int s = 0; s < 4; s++) {
        const unsigned m = seg[s];
        if (!m) continue;
        const int c1 = popc(l & ~h & m), c2 = popc(~l & h & m), c3 = popc(l & h & m);
        const int c0 = popc(m) - c1 - c2 - c3;
        const int o = __builtin_ctz(m);
        nl |= (((1u << c1) - 1) << c0 | ((1u << c3) - 1) << (c0 + c1 + c2)) << o;
        nh |= (((1u << (c2 + c3)) - 1) << (c0 + c1)) << o;
      }
      const uint32_t g = uint32_t(g_r924[nl]) * 400 + g_r20[ddport::pext32(nh, nl)] * 20 + g_r20[ddport::pext32(nh, nl ^ 0xFFF)];
      gid[r] = g;
      f1[g] |= res[r];
      f0[g] |= uint8_t(~res[r]);
    }
    for (size_t r = 0; r < N; r++) {
      const unsigned ok = want & ~unsigned(f0[gid[r]] & f1[gid[r]]);
      if (!ok) continue;
      for (int v = 0; v < 6; v++) {
        if (!(ok >> v & 1)) continue;
        const unsigned d = unsigned(cur[v][r]) - k;
        if (d == 1 || d == 16 || d == 256 || d == 4096) {
          cnt[v][cur[v][r]]--;
          cur[v][r] = uint16_t(k);
          cnt[v][k]++;
        }
      }
    }
  }
  for (int nt = 0; nt < 2; nt++) {
    uint8_t* base = g_t3 + (size_t(cfg) * 2 + nt) * T3_ARR * 5;
    for (size_t r = 0; r < N; r++) {
      u64 e = 0;
      memcpy(&e, base + r * 5, 5);
      e &= 3;
      for (int t = 1; t <= 3; t++) {
        const unsigned k = cur[3 * nt + t - 1][r];
        u64 m = 0;
        for (int s = 0; s < 4; s++) {
          const int ksuit = int(k >> (4 * s) & 15);
          m |= u64(((1u << ksuit) - 1) << (len[s] - ksuit)) << off[s];
        }
        e |= m << (12 * t - 10);
      }
      memcpy(base + r * 5, &e, 5);
    }
  }
}

void build_t3() {
  build_t3_tables();
  enum_cfg3();
  const size_t bytes = (T3_BYTES + 16 + (2u << 20) - 1) & ~size_t((2u << 20) - 1);
  g_t3 = static_cast<uint8_t*>(aligned_alloc(2u << 20, bytes));
#ifdef MADV_HUGEPAGE
  madvise(g_t3, bytes, MADV_HUGEPAGE);
#endif
  bool loaded = false;
  if (FILE* f = fopen(t3_path().c_str(), "rb")) {
    loaded = fread(g_t3, 1, T3_BYTES, f) == T3_BYTES && fgetc(f) == EOF;
    fclose(f);
  }
  if (!loaded) {
    memset(g_t3, 0, bytes);
    std::vector<unsigned> los;
    for (unsigned m = 0; m < 4096; m++)
      if (popc(m) == 6) los.push_back(m);
    std::atomic<size_t> next{0};
    const size_t total = T3_CFG * los.size();
    auto work = [&] {
      for (;;) {
        size_t j = next.fetch_add(1);
        if (j >= total) break;
        gen_t3_part(int(j / los.size()), los[j % los.size()]);
      }
    };
    unsigned nthreads = std::thread::hardware_concurrency();
    nthreads = nthreads > 16 ? 16 : nthreads < 1 ? 1 : nthreads;
    std::vector<std::thread> th;
    for (unsigned i = 0; i < nthreads; i++) th.emplace_back(work);
    for (auto& x : th) x.join();
    th.clear();
    std::atomic<int> next_cfg{0};
    for (unsigned i = 0; i < nthreads; i++)
      th.emplace_back([&] {
        for (int c; (c = next_cfg.fetch_add(1)) < int(T3_CFG);) minimize_t3(c);
      });
    for (auto& x : th) x.join();
    mkdir("build", 0777);
    make_table_dir();
    std::string tmp = t3_path() + ".tmp" + std::to_string(getpid());
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
      bool ok = fwrite(g_t3, 1, T3_BYTES, f) == T3_BYTES;
      ok = fclose(f) == 0 && ok;
      if (ok) rename(tmp.c_str(), t3_path().c_str());
      else remove(tmp.c_str());
    }
  }
  g_use3 = true;
}

// ---------------------------------------------------------------------------------------
// Coroutines: several positions are searched in turn on one thread. A search that needs a
// three-card table entry prefetches it and hands over to the next search, so the memory
// access is over by the time it resumes.
DDPORT_DEFINE_CTX_SWITCH(ddclaim_ctx_switch)

constexpr int MAX_CTX = 64;
constexpr size_t STACK_BYTES = 64 << 10;
struct Ctx {
  void* sp;
  u64 h[4];
  u64 lens;
  uint32_t gen;
  int next, prev;
};
thread_local Ctx g_ctx[MAX_CTX];
thread_local int g_cur = -1;  // the running context, or -1 outside a batch
thread_local void* g_main_sp;
thread_local char* g_stacks;
thread_local const ddclaim::Position* g_pos;
thread_local size_t g_count, g_next;
thread_local uint8_t* g_out;
thread_local uint32_t g_gen_counter = 1;

inline void save_state(Ctx& c) {
  c.h[0] = H[0], c.h[1] = H[1], c.h[2] = H[2], c.h[3] = H[3];
  c.lens = LENS;
  c.gen = g_gen;
}
inline void load_state(const Ctx& c) {
  H[0] = c.h[0], H[1] = c.h[1], H[2] = c.h[2], H[3] = c.h[3];
  LENS = c.lens;
  g_gen = c.gen;
}

__attribute__((noinline)) void yield() {
  if (g_cur < 0) return;
  Ctx& a = g_ctx[g_cur];
  if (a.next == g_cur) return;
  save_state(a);
  g_cur = a.next;
  Ctx& b = g_ctx[g_cur];
  load_state(b);
  ddclaim_ctx_switch(&a.sp, b.sp);
}

inline int solve_position(const ddclaim::Position& p) {
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
  g_gen = ++g_gen_counter;
  return tr == 4 ? solve_one<true>(n, p.leader) : solve_one<false>(n, p.leader);
}

void ctx_main() {
  for (;;) {
    const size_t i = g_next;
    if (i >= g_count) break;
    g_next = i + 1;
    g_out[i] = uint8_t(solve_position(g_pos[i]));
  }
  Ctx& a = g_ctx[g_cur];
  if (a.next == g_cur) {
    g_cur = -1;
    ddclaim_ctx_switch(&a.sp, g_main_sp);
  } else {
    g_ctx[a.prev].next = a.next;
    g_ctx[a.next].prev = a.prev;
    g_cur = a.next;
    Ctx& b = g_ctx[g_cur];
    load_state(b);
    ddclaim_ctx_switch(&a.sp, b.sp);
  }
  abort();
}

void run_batch(const ddclaim::Position* positions, size_t count, uint8_t* tricks, int nctx) {
  g_pos = positions, g_count = count, g_next = 0, g_out = tricks;
  if (size_t(nctx) > count) nctx = int(count);
  if (nctx < 1) return;
  if (!g_stacks) g_stacks = static_cast<char*>(aligned_alloc(4096, MAX_CTX * STACK_BYTES));
  for (int i = 0; i < nctx; i++) {
    Ctx& c = g_ctx[i];
    c.next = (i + 1) % nctx;
    c.prev = (i + nctx - 1) % nctx;
    c.sp = ddport::init_stack(g_stacks + (i + 1) * STACK_BYTES, &ctx_main);
    save_state(c);
  }
  g_cur = 0;
  ddclaim_ctx_switch(&g_main_sp, g_ctx[0].sp);
}

// ---------------------------------------------------------------------------------------
// The four-card pattern table is filled by solving random endings.
double now_seconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

struct Rng {
  u64 s;
  u64 next() {
    u64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

void random_position(Rng& rng, int cards, ddclaim::Position& p) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  memset(&p, 0, sizeof p);
  for (int k = 0; k < 4 * cards; k++) {
    int j = k + int(rng.next() % u64(52 - k));
    int t = deck[k];
    deck[k] = deck[j], deck[j] = t;
    p.hand[k % 4][deck[k] / 13] |= uint16_t(1u << (deck[k] % 13));
  }
  p.trump = uint8_t(rng.next() % 5);
  p.leader = uint8_t(rng.next() % 4);
}

// The pattern tables live in files under the table directory: : p<cards>_v5.ent holds the runs (in
// key order), .idx the list of runs (key, offset, size), and while filling .cnt holds a hit
// count per entry. A merge streams the old file and the new patterns into a new file, so no
// table is ever assembled in memory; the search uses the file memory-mapped.
struct PClass {
  u64 key;
  uint32_t off, groups;
};
std::vector<PClass> g_pclasses[P_LEVELS];  // sorted by key

void* alloc_huge(size_t bytes) {
  bytes = (bytes + (2u << 20)) & ~size_t((2u << 20) - 1);
  void* p = aligned_alloc(2u << 20, bytes);
#ifdef MADV_HUGEPAGE
  madvise(p, bytes, MADV_HUGEPAGE);
#endif
  return p;
}

std::string p_path(int lv, const char* ext) { return table_dir() + "/p" + std::to_string(lv + 4) + "_v5." + ext; }

void* map_file(const std::string& path, size_t bytes, bool writable) {
  if (!bytes) return nullptr;
  const int fd = open(path.c_str(), writable ? O_RDWR : O_RDONLY);
  if (fd < 0) return nullptr;
  // Map at a 2 MB boundary so that the kernel can back a read-only mapping with huge pages.
  const size_t huge = size_t(2) << 20, span = ((bytes + huge - 1) & ~(huge - 1)) + huge;
  char* base = static_cast<char*>(mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (base == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  char* at = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(base) + huge - 1) & ~(huge - 1));
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#define DDCLAIM_NO_POPULATE 1
#endif
  void* p = mmap(at, bytes, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED | MAP_FIXED | (writable ? 0 : MAP_POPULATE), fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    munmap(base, span);
    return nullptr;
  }
  if (at > base) munmap(base, size_t(at - base));
  char* end = at + ((bytes + 4095) & ~size_t(4095));
  if (base + span > end) munmap(end, size_t(base + span - end));
#ifdef DDCLAIM_NO_POPULATE  // macOS: read the file in now, as MAP_POPULATE does
  if (!writable) {
    madvise(p, bytes, MADV_WILLNEED);
    volatile char sink = 0;
    for (size_t i = 0; i < bytes; i += 16384) sink = sink + static_cast<const char*>(p)[i];
  }
#endif
#ifdef MADV_COLLAPSE
  if (!writable && !getenv("DDCLAIM_NOCOLLAPSE")) madvise(p, bytes & ~(huge - 1), MADV_COLLAPSE);  // best effort
#endif
  return p;
}

void p_unmap(int lv) {
  PTable& pt = g_pt[lv];
  if (pt.ent) munmap(const_cast<u64*>(pt.ent), pt.lines * 64);
  if (pt.cnt) munmap(pt.cnt, pt.lines * 8);
  pt.ent = nullptr, pt.cnt = nullptr;
}

// Builds the directory from the list of runs.
void p_make_dir(int lv, const std::vector<PClass>& classes) {
  PTable& pt = g_pt[lv];
  int bits = 16;
  while ((size_t(1) << bits) < classes.size() * 2 + 16) bits++;
  if (bits != pt.dir_bits) {
    free(pt.dir);
    pt.dir = static_cast<PDir*>(alloc_huge(sizeof(PDir) << bits));
  }
  PDir* dir = pt.dir;
  memset(dir, 0, sizeof(PDir) << bits);
  for (const PClass& c : classes) {
    size_t h = (c.key * 0x9E3779B97F4A7C15ull) >> (64 - bits);
    while (dir[h].key) h = (h + 1) & ((size_t(1) << bits) - 1);
    dir[h] = {c.key, c.off, c.groups};
  }
  pt.dir_bits = bits;
}

// Merges new patterns into a table file: within a run the entries hit most often come first,
// and only the first `cap` are kept. `grow` false: runs get no new entries beyond what they
// have (used when the file budget is reached), only bounds are merged.
bool p_merge(int lv, std::vector<PRec>& recs, size_t cap, bool keep_counts) {
  PTable& pt = g_pt[lv];
  const std::vector<PClass>& oldc = g_pclasses[lv];
  auto pat_less = [](u64 a1, u64 b1, u64 a2, u64 b2) { return a1 != a2 ? a1 < a2 : (b1 & 0xFFFFFFFFFFFFull) < (b2 & 0xFFFFFFFFFFFFull); };
  std::sort(recs.begin(), recs.end(), [&](const PRec& x, const PRec& y) { return x.key != y.key ? x.key < y.key : pat_less(x.a, x.b, y.a, y.b); });
  struct Item {
    u64 a, b;
    uint32_t count;
  };
  const std::string fe_tmp = p_path(lv, "ent.tmp"), fc_tmp = p_path(lv, "cnt.tmp");
  FILE* fe = fopen(fe_tmp.c_str(), "wb");
  FILE* fc = keep_counts ? fopen(fc_tmp.c_str(), "wb") : nullptr;
  if (!fe || (keep_counts && !fc)) return false;
  setvbuf(fe, nullptr, _IOFBF, 4 << 20);
  if (fc) setvbuf(fc, nullptr, _IOFBF, 1 << 20);
  std::vector<PClass> classes;
  classes.reserve(oldc.size() + oldc.size() / 4 + 1024);
  std::vector<Item> items, merged;
  std::vector<u64> out;
  std::vector<uint16_t> outc;
  bool ok = true;
  size_t io = 0, in = 0, line = 0;
  while (io < oldc.size() || in < recs.size()) {
    u64 key = ~0ull;
    if (io < oldc.size()) key = oldc[io].key;
    if (in < recs.size() && recs[in].key < key) key = recs[in].key;
    items.clear();
    if (io < oldc.size() && oldc[io].key == key) {
      const PClass& c = oldc[io++];
      const u64* ea = pt.ent + size_t(c.off) * 8;
      const u64* eb = ea + 8 * size_t(c.groups);
      for (size_t i = 0; i < size_t(c.groups) * 8; i++)
        if (eb[i]) items.push_back({ea[i], eb[i], pt.cnt[size_t(c.off) * 4 + i]});
    }
    const size_t old = items.size();
    size_t in2 = in;
    while (in2 < recs.size() && recs[in2].key == key) in2++;
    if (in2 > in) {
      // Entries with the same pattern are merged (their bounds combined).
      std::sort(items.begin(), items.end(), [&](const Item& x, const Item& y) { return pat_less(x.a, x.b, y.a, y.b); });
      merged.clear();
      size_t i = 0, j = in;
      auto add = [&](u64 a, u64 b, uint32_t c) {
        if (!merged.empty() && merged.back().a == a && ((merged.back().b ^ b) & 0xFFFFFFFFFFFFull) == 0) {
          u64& m = merged.back().b;
          const u64 lo = std::max(m >> 56 & 7, b >> 56 & 7), hi = std::min(m >> 59 & 7, b >> 59 & 7);
          m = (m & ~(63ull << 56)) | lo << 56 | hi << 59;
          merged.back().count += c;
        } else {
          merged.push_back({a, b, c});
        }
      };
      while (i < old || j < in2) {
        if (j >= in2 || (i < old && !pat_less(recs[j].a, recs[j].b, items[i].a, items[i].b))) add(items[i].a, items[i].b, items[i].count), i++;
        else add(recs[j].a, recs[j].b, 0), j++;
      }
      items.swap(merged);
      in = in2;
      std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.count > y.count; });
    }
    const size_t keep = items.size() < cap ? items.size() : cap;
    const size_t ng = (keep + 7) / 8;
    if (!ng) continue;
    out.assign(ng * 16, 0);
    outc.assign(ng * 8, 0);
    for (size_t i = 0; i < ng * 8; i++) {
      out[i] = i < keep ? items[i].a : ~0ull;
      out[ng * 8 + i] = i < keep ? items[i].b : 0;
      outc[i] = i < keep ? uint16_t(items[i].count > 0xFFFF ? 0xFFFF : items[i].count) : 0;
    }
    ok = ok && fwrite(out.data(), 8, out.size(), fe) == out.size();
    if (fc) ok = ok && fwrite(outc.data(), 2, outc.size(), fc) == outc.size();
    classes.push_back({key, uint32_t(line), uint32_t(ng)});
    line += ng * 2;
  }
  ok = fclose(fe) == 0 && ok;
  if (fc) ok = fclose(fc) == 0 && ok;
  if (!ok) return false;
  p_unmap(lv);
  rename(fe_tmp.c_str(), p_path(lv, "ent").c_str());
  if (keep_counts) rename(fc_tmp.c_str(), p_path(lv, "cnt").c_str());
  else remove(p_path(lv, "cnt").c_str());
  pt.lines = line;
  pt.ent = static_cast<const u64*>(map_file(p_path(lv, "ent"), line * 64, false));
  if (keep_counts) pt.cnt = static_cast<uint16_t*>(map_file(p_path(lv, "cnt"), line * 8, true));
  if (line && (!pt.ent || (keep_counts && !pt.cnt))) return false;
  p_make_dir(lv, classes);
  g_pclasses[lv].swap(classes);
  return true;
}

constexpr u64 P_MAGIC = 0x3576415034504F31ull;

bool p_load(int lv) {
  FILE* f = fopen(p_path(lv, "idx").c_str(), "rb");
  if (!f) return false;
  PTable& pt = g_pt[lv];
  u64 head[4];
  std::vector<PClass> classes;
  bool ok = fread(head, 8, 4, f) == 4 && head[0] == P_MAGIC && head[1] < (1ull << 28);
  if (ok) {
    classes.resize(head[1]);
    ok = fread(classes.data(), sizeof(PClass), classes.size(), f) == classes.size() && fgetc(f) == EOF;
  }
  fclose(f);
  struct stat st;
  ok = ok && stat(p_path(lv, "ent").c_str(), &st) == 0 && size_t(st.st_size) == head[2] * 64;
  if (!ok) return false;
  pt.lines = head[2];
  // By default the file is read into huge pages, which is about 9% faster to search than
  // mapping it (this kernel gives file mappings 4 KB pages). DDCLAIM_MAP=1 maps it instead.
  if (!getenv("DDCLAIM_MAP")) {
    u64* m = static_cast<u64*>(alloc_huge(pt.lines * 64 + 64));
    FILE* g = fopen(p_path(lv, "ent").c_str(), "rb");
    ok = g && fread(m, 64, pt.lines, g) == pt.lines;
    if (g) fclose(g);
    pt.ent = m;
  } else {
    pt.ent = static_cast<const u64*>(map_file(p_path(lv, "ent"), pt.lines * 64, false));
    ok = pt.ent || !pt.lines;
  }
  if (ok) p_make_dir(lv, classes);
  return ok;
}

void p_save_index(int lv) {
  const std::string path = p_path(lv, "idx"), tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  const std::vector<PClass>& c = g_pclasses[lv];
  const u64 head[4] = {P_MAGIC, c.size(), g_pt[lv].lines, 0};
  bool ok = fwrite(head, 8, 4, f) == 4 && fwrite(c.data(), sizeof(PClass), c.size(), f) == c.size();
  ok = fclose(f) == 0 && ok;
  if (ok) rename(tmp.c_str(), path.c_str());
  else remove(tmp.c_str());
}

long env_long(const char* name, long dflt) { return getenv(name) ? atol(getenv(name)) : dflt; }

void build_p(int levels) {
  if (levels < 0) levels = 0;
  if (levels > P_LEVELS) levels = P_LEVELS;
  if (!getenv("DDCLAIM_WARM")) {
    bool ok = true;
    for (int lv = 0; lv < levels; lv++) ok = ok && p_load(lv);
    if (ok) {
      for (int lv = 0; lv < levels; lv++) g_pt[lv].use = true;
      return;
    }
  }
  // Fill: rounds of random six-card endings solved against the table as it stands (the
  // search above 4 cards, with its own transposition table, only drives the fill); the
  // patterns they prove are merged in after each round. Stops at the ending count, the
  // file budget or the time budget.
  const long warm = env_long("DDCLAIM_WARM", 200000000);
  const long round_max = env_long("DDCLAIM_ROUND", 3000000);
  const double max_seconds = double(env_long("DDCLAIM_SECONDS", 170));
  const size_t max_bytes = size_t(env_long("DDCLAIM_MB", 3400)) << 20;
  const size_t caps[P_LEVELS] = {size_t(env_long("DDCLAIM_CAP4", 256))};
  const double t_start = now_seconds();
  mkdir("build", 0777);
  make_table_dir();
  for (int lv = 0; lv < levels; lv++) {
    remove(p_path(lv, "idx").c_str());
    p_unmap(lv);
    g_pt[lv].lines = 0;
    g_pclasses[lv].clear();
    p_make_dir(lv, g_pclasses[lv]);
    g_pt[lv].use = true;
  }
  unsigned nthreads = std::thread::hardware_concurrency();
  nthreads = nthreads > 22 ? 22 : nthreads < 1 ? 1 : nthreads;
  long done = 0;
  int round = 0;
  bool ok = true;
  while (ok) {
    long n = done < 100000 ? 100000 : done > round_max ? round_max : done;
    if (n > warm - done) n = warm - done;
    std::vector<std::vector<PRec>> logs(nthreads * P_LEVELS);
    std::vector<std::thread> th;
    for (unsigned i = 0; i < nthreads; i++)
      th.emplace_back([&, i] {
        g_plog = &logs[i * P_LEVELS];
        Rng rng{0x1234567ull * (i + 1) + 0x9876543ull * u64(round)};
        std::vector<ddclaim::Position> ps(1024);
        std::vector<uint8_t> out(1024);
        for (long k = 0; k < n / long(nthreads); k += 1024) {
          for (auto& p : ps) random_position(rng, 6, p);
          run_batch(ps.data(), ps.size(), out.data(), 8);
        }
        g_plog = nullptr;
      });
    for (auto& x : th) x.join();
    done += n;
    round++;
    size_t bytes = 0;
    for (int lv = 0; lv < levels; lv++) bytes += g_pt[lv].lines * 64;
    // The next round would add about as much as this one did; stop before the budget.
    static size_t prev_bytes = 0;
    const size_t growth = bytes - prev_bytes;
    prev_bytes = bytes;
    const bool last = done >= warm || now_seconds() - t_start > max_seconds || bytes + 2 * growth > max_bytes;
    // One merge per table, side by side.
    std::vector<std::vector<PRec>> recs(levels);
    std::vector<char> merged_ok(levels, 1);
    std::vector<std::thread> mt;
    for (int lv = 0; lv < levels; lv++) {
      for (unsigned i = 0; i < nthreads; i++) {
        auto& l = logs[i * P_LEVELS + lv];
        recs[lv].insert(recs[lv].end(), l.begin(), l.end());
        std::vector<PRec>().swap(l);
      }
      mt.emplace_back([&, lv] { merged_ok[lv] = p_merge(lv, recs[lv], caps[lv], !last); });
    }
    for (auto& x : mt) x.join();
    for (int lv = 0; lv < levels; lv++) {
      ok = ok && merged_ok[lv];
      if (getenv("DDCLAIM_VERBOSE"))
        fprintf(stderr, "p%d round %d: %ld endings, %zu new, %zu runs, %.0f MB, %.1f s\n", lv + 4, round, done, recs[lv].size(),
                g_pclasses[lv].size(), double(g_pt[lv].lines) * 64 / 1e6, now_seconds() - t_start);
    }
    if (last) break;
  }
  for (int lv = 0; lv < levels; lv++) {
    if (ok) p_save_index(lv);
    else g_pt[lv].use = false;
    std::vector<PClass>().swap(g_pclasses[lv]);
  }
}

}  // namespace

namespace ddclaim {
namespace tables {

void snapshot(Snapshot& out) {
  out.use2 = g_use2;
  memcpy(out.r70, g_r70, sizeof g_r70), memcpy(out.r6, g_r6, sizeof g_r6), memcpy(out.comp, g_comp, sizeof g_comp);
  out.t2 = g_t2;
  out.use3 = g_use3;
  memcpy(out.r924, g_r924, sizeof g_r924), memcpy(out.r20, g_r20, sizeof g_r20), memcpy(out.cfg3, g_cfg3, sizeof g_cfg3);
  out.t3 = g_t3;
  memcpy(out.shuf, g_shuf, sizeof g_shuf), memcpy(out.unshuf, g_unshuf, sizeof g_unshuf);
  for (int i = 0; i < P_LEVELS; i++) out.pt[i] = g_pt[i];
}

namespace {
std::string g_dir;  // set by init_tables(); empty: $DDCLAIM_DIR or the default
}

std::string table_dir() {
  if (!g_dir.empty()) return g_dir;
  const char* d = getenv("DDCLAIM_DIR");
  return d && *d ? d : "build/ddclaim";
}

void init_tables(const char* dir, int pattern_levels) {
  if (dir && *dir) g_dir = dir;
  build_t2();
  build_t3();
  build_p(pattern_levels);
}

}  // namespace tables

bool pin_thread(int cpu) { return ddport::pin_to_cpu(cpu); }

}  // namespace ddclaim
