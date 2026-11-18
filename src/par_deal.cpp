// The parallel search of one deal (ddfast.hpp: solve_deal_parallel). The search of solver.cpp
// is compiled here a second time with DDF_PAR, which gives it its own thread-local state and
// two hooks: an abort flag checked at every lead node (and before every table store), and a
// mask on hand 2's replies at trick 1. The single-threaded path in solver.cpp is unchanged.
//
// The calling thread runs the question sequence of solve_one (a fitted first guess, then one
// trick up or down). Each question ("does North-South take t tricks?") is split into work
// items over the root leads: the first lead in DDS's order is split by hand 2's replies (one
// item per reply: when that lead cuts, every reply must be refuted, so the items are all
// needed), the other leads are one item each. The calling thread and ncpu-1 pinned helpers take
// items in that order from a shared counter. A lead cuts when all its items cut; it is refuted
// when one item is refuted, which aborts the lead's other items; the question is decided when a
// lead cuts (abort everything) or every lead is refuted. Every thread keeps its own per-deal
// table across the questions of a deal; an aborted item stores nothing (a node stores only if
// the flag was still clear when it finished, and the flag is never cleared during a question),
// so every stored entry is exact.
#define DDF_PAR 1
#include "solver.cpp"

#include <chrono>
#include <condition_variable>
#include <mutex>


namespace {

void begin_deal(const ddclaim::Position& p) {
  ensure_state();
  if (!S->tt.dir) S->tt.alloc();
  const int tr = p.trump;
  u64 lens = 0;
  for (int q = 0; q < 4; q++) {
    u64 hq = 0;
    for (int s = 0; s < 4; s++) {
      int lane = s;
      if (tr < 4) lane = s == tr ? 3 : (s == 3 ? tr : s);
      hq |= u64(p.hand[q][s]) << (16 * lane);
      lens |= u64(__builtin_popcount(p.hand[q][s])) << (16 * q + 4 * lane);
    }
    H[q] = hq;
  }
  LENS = lens;
  g_gen = ++g_gen_counter;
  S->tt.reset();
  memset(S->htried, 0, sizeof S->htried), memset(S->hcut, 0, sizeof S->hcut);
  memset(S->mres, 0, sizeof S->mres);
  S->rootL = p.leader;
  g_expfalse = false;
}

// The root leads in the order lead() would try them (hint: the last cutting root lead).
template <bool NT>
int root_order(int L, u64 hint, u64* out) {
  const u64 all = H[0] | H[1] | H[2] | H[3];
  alignas(64) u64 cards[16] = {};
  alignas(32) uint16_t key[16] = {};
  S->killer[13][0] = hint;
  native_order<NT>(13, L, hint, all, cards, key);
  u64 cand = heads(H[L], all);
  int k = 0;
  while (cand) {
    const unsigned bk = ddport::best_key(cards, key, cand);
    const u64 c = cards[15 - (bk & 15)];
    cand ^= c;
    out[k++] = c;
  }
  return k;
}

// Hand 2's distinct replies to lead c.
u64 hand2_options(int L, u64 c) {
  const u64 all = H[0] | H[1] | H[2] | H[3];
  const u64 mine = H[(L + 1) & 3], in_suit = mine & lane_of(c);
  return heads(in_suit ? in_suit : mine, all);
}

// 1: lead c cuts against hand 2's replies in mask2; 0: refuted; -1: aborted. With ybw, the
// search stops after hand 2's first reply if that fails; rest = the replies still to search.
template <bool NT>
int run_item(int L, int target, u64 c, u64 mask2, bool ybw, u64& rest) {
  const u64 all = H[0] | H[1] | H[2] | H[3];
  const bool cut = !(L & 1);
  const int sh = 16 * L + 4 * suit_of(c);
  S->trk[13][0] = c;
  H[L] ^= c;
  LENS -= 1ull << sh;
  g_hot.mask2 = mask2;
  g_hot.ybw = ybw;
  g_hot.rest = 0;
  u64 Rc;
  const bool r = follow<NT, 1>(13, L, target, all, lane_of(c), c, L, false, Rc);
  H[L] ^= c;
  LENS += 1ull << sh;
  g_hot.mask2 = ~0ull;
  g_hot.ybw = false;
  rest = g_hot.rest;
  if (PAR_STOP()) return -1;
  return r == cut;
}

const std::atomic<uint8_t> g_never_stop{0};

struct alignas(64) LeadState {
  std::atomic<int> pending{0};  // items of this lead not yet cut
  std::atomic<uint8_t> stop{0};  // refuted, or the question is decided
};
struct Item {
  u64 c, mask;
  int lead, prio;  // taken lowest prio first
  bool ybw, taken;
};

#ifndef PAR_SPLIT
// 0: one item per root lead; 1: the first lead split by hand 2's replies; 2: every lead split;
// 3 (default): every lead searched first against hand 2's first reply, the other replies then
// one item each, ahead of the leads not yet started (young brothers wait, at trick 1).
#define PAR_SPLIT 3
#endif
#ifndef PAR_SPIN_US
#define PAR_SPIN_US 3000  // helpers spin this long for the next question before they sleep
#endif
const int g_split = getenv("PAR_SPLIT") ? atoi(getenv("PAR_SPLIT")) : PAR_SPLIT;  // experiments

struct DealPool {
  std::vector<int> cpus;
  std::vector<std::thread> threads;
  // The deal and the question.
  ddclaim::Position pos{};
  uint64_t deal_id = 0;
  int target = 0, L = 0, nlead = 0;
  LeadState ls[16];
  SpinLock qlock;  // guards the item list
  Item items[256];
  int nitems = 0;
  alignas(64) std::atomic<int> refuted{0};
  std::atomic<int> result{-1};
  alignas(64) std::atomic<int> active{0};
  std::atomic<unsigned long long> nodes{0};
  std::atomic<unsigned long long> waste{0}, useful{0};  // lead nodes in aborted and in finished items
  std::atomic<unsigned long long> busy{0}, busy_waste{0}, wall{0};  // TSC ticks: in items, aborted items, questions x threads
  alignas(64) std::atomic<uint64_t> round{0};
  std::atomic<int> sleepers{0};
  std::mutex m;
  std::condition_variable cv;
  bool quit = false;

  void finish(int v) {
    int e = -1;
    if (result.compare_exchange_strong(e, v))
      for (int i = 0; i < nlead; i++) ls[i].stop.store(1, std::memory_order_relaxed);
  }
  // The next item to search (false when none is left now).
  bool take(Item& out) {
    qlock.lock();
    int b = -1;
    for (int k = 0; k < nitems; k++)
      if (!items[k].taken && (b < 0 || items[k].prio < items[b].prio)) b = k;
    if (b >= 0) items[b].taken = true, out = items[b];
    qlock.unlock();
    return b >= 0;
  }
  void add(const Item& it) {
    qlock.lock();
    items[nitems++] = it;
    qlock.unlock();
  }
  // One participant's share of the current question: items until the question is decided.
  void work() {
    thread_local uint64_t my_deal = 0;
    if (my_deal != deal_id) begin_deal(pos), my_deal = deal_id;
    const u64 n0 = g_mnodes;
    while (result.load(std::memory_order_acquire) < 0) {
      Item it;
      if (!take(it)) {
        ddport::cpu_relax();
        continue;
      }
      LeadState& l = ls[it.lead];
      if (l.stop.load(std::memory_order_relaxed)) continue;
      g_hot.stop = &l.stop;
      const u64 i0 = g_mnodes, c0 = ddport::ticks();
      u64 rest = 0;
      const int r = pos.trump == 4 ? run_item<true>(L, target, it.c, it.mask, it.ybw, rest)
                                   : run_item<false>(L, target, it.c, it.mask, it.ybw, rest);
      g_hot.stop = &g_never_stop;
      (r < 0 ? waste : useful).fetch_add(g_mnodes - i0, std::memory_order_relaxed);
      (r < 0 ? busy_waste : busy).fetch_add(ddport::ticks() - c0, std::memory_order_relaxed);
      if (r < 0) continue;
      if (r == 1) {
        if (rest) {  // the lead's other replies, one item each, ahead of the leads not started
          l.pending.fetch_add(__builtin_popcountll(rest));
          for (u64 x = rest; x; x &= x - 1) add(Item{it.c, x & (0 - x), it.lead, it.lead, false, false});
        }
        if (l.pending.fetch_sub(1) == 1) finish(!(L & 1));
      } else {
        uint8_t z = 0;
        if (l.stop.compare_exchange_strong(z, 1) && refuted.fetch_add(1) + 1 == nlead) finish(L & 1);
      }
    }
    nodes.fetch_add(g_mnodes - n0, std::memory_order_relaxed);
  }
  void helper(int w, uint64_t seen) {
    if (cpus[w] >= 0) ddclaim::pin_thread(cpus[w]);
    ensure_state();
    g_hot.stop = &g_never_stop;
    for (;;) {
      // Spin for the next question, then sleep.
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; round.load(std::memory_order_acquire) == seen; i++) {
        ddport::cpu_relax();
        if ((i & 1023) == 1023 && std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(PAR_SPIN_US)) {
          std::unique_lock<std::mutex> lk(m);
          sleepers.fetch_add(1);
          cv.wait(lk, [&] { return quit || round.load() != seen; });
          sleepers.fetch_sub(1);
          break;
        }
      }
      if (quit) return;
      seen = round.load(std::memory_order_acquire);
      work();
      active.fetch_sub(1, std::memory_order_release);
    }
  }
  void start(const int* c, int nc) {
    cpus.assign(c, c + nc);
    quit = false;
    const uint64_t r0 = round.load();  // helpers wait for the round after this one
    for (int w = 1; w < nc; w++) threads.emplace_back(&DealPool::helper, this, w, r0);
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m);
      quit = true;
      round.fetch_add(1);
    }
    cv.notify_all();
    for (auto& t : threads) t.join();
    threads.clear();
    cpus.clear();
  }
  ~DealPool() {
    stop();
    if (getenv("PAR_STATS"))
      printf("par: lead nodes in finished items %llu, in aborted items %llu; thread time in items %.1f%% (aborted items %.1f%%)\n",
             useful.load(), waste.load(), 100.0 * double(busy + busy_waste) / double(wall + 1), 100.0 * double(busy_waste) / double(wall + 1));
  }

  // Runs one question on all participants; true if North-South take `t` tricks.
  bool question(int t, u64& hint) {
    target = t;
    u64 leads[16];
    nlead = pos.trump == 4 ? root_order<true>(L, hint, leads) : root_order<false>(L, hint, leads);
    nitems = 0;
    for (int i = 0; i < nlead; i++) {
      const u64 o = hand2_options(L, leads[i]);
      int cnt = 0;
      if ((g_split == 2 || (g_split == 1 && i == 0)) && (o & (o - 1))) {
        for (u64 x = o; x; x &= x - 1) items[nitems++] = Item{leads[i], x & (0 - x), i, 1000 + i, false, false}, cnt++;
      } else {
        items[nitems++] = Item{leads[i], ~0ull, i, 1000 + i, g_split == 3, false}, cnt++;
      }
      ls[i].pending.store(cnt, std::memory_order_relaxed);
      ls[i].stop.store(0, std::memory_order_relaxed);
    }
    refuted.store(0, std::memory_order_relaxed);
    result.store(-1, std::memory_order_relaxed);
    active.store(int(cpus.size()) - 1, std::memory_order_relaxed);
    const u64 w0 = ddport::ticks();
    round.fetch_add(1);  // seq_cst: publishes the question (and orders the sleepers check)
    if (sleepers.load() > 0) {
      std::lock_guard<std::mutex> lk(m);
      cv.notify_all();
    }
    work();
    while (active.load(std::memory_order_acquire) > 0) ddport::cpu_relax();
    wall.fetch_add((ddport::ticks() - w0) * cpus.size(), std::memory_order_relaxed);
    const int r = result.load();
    if (r == !(L & 1)) {  // the root leader's side reached t: remember the cutting lead
      for (int i = 0; i < nlead; i++)
        if (ls[i].pending.load() == 0) { hint = leads[i]; break; }
    }
    return r != 0;
  }
};

DealPool& deal_pool() {
  static DealPool p;
  return p;
}

}  // namespace

namespace ddfast::detail {

int solve_deal_parallel(const ddclaim::Position& p, const int* cpus, int ncpu, unsigned long long* lead_nodes) {
  static std::once_flag once;
  std::call_once(once, [] { ddclaim::tables::attach(); });
  DealPool& P = deal_pool();
  if (ncpu < 1) ncpu = 1;
  if (P.cpus.size() != size_t(ncpu) || !std::equal(P.cpus.begin(), P.cpus.end(), cpus)) {
    P.stop();
    P.start(cpus, ncpu);
  }
  ensure_state();
  g_hot.stop = &g_never_stop;
  P.pos = p;
  P.deal_id++;
  P.L = p.leader;
  P.nodes.store(0);
  begin_deal(p);  // the calling thread's state (for the guess and the root order)
  {
  static TT shared[PAR_STRIPES];
  g_shared = nullptr;
  if (ncpu > 1 && g_shmin <= 13) {
    for (TT& t : shared) {
      if (!t.dir) t.alloc();
      t.reset();
    }
    g_shared = shared;
  }
  }
  const int n = 13, L = p.leader;
  // North-South's tricks by solve_one's sequence of questions ("do North-South take t?").
  int lo = 0, hi = n;
  int t = p.trump == 4 ? guess<true>(n, L) : guess<false>(n, L);  // as solve_one (GOFF 0)
  u64 hint = 0;
  while (lo < hi) {
    if (P.question(t, hint)) lo = t, t = lo + 1;
    else hi = t - 1, t = hi;
  }
  if (lead_nodes) *lead_nodes = P.nodes.load();
  return lo;
}

void stop_deal_pool() { deal_pool().stop(); }

}  // namespace ddfast::detail
