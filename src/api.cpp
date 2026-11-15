// The public API (ddfast.hpp): conversions around the search in solver.cpp, and the pool of
// pinned worker threads for the parallel calls. The read-only tables are shared; every worker
// has its own search state (thread_local in solver.cpp).
#include "ddfast.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <sched.h>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <thread>

#include "ddclaim.hpp"
#include "internal.hpp"

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#else
#include <sys/sysctl.h>
#endif
#include <unistd.h>

namespace ddfast {
namespace {

inline ddclaim::Position to_position(const Problem& p) {
  ddclaim::Position q;
  for (int s = 0; s < 4; s++)
    for (int h = 0; h < 4; h++) q.hand[s][h] = p.hand[s][h];
  q.trump = p.trump;
  q.leader = static_cast<uint8_t>((p.declarer + 1) & 3);
  return q;
}

// A counter of the calling thread's user-mode events, or -1 (always -1 off Linux: the
// WorkerStats counters then stay 0).
#if !defined(__linux__)
enum { PERF_TYPE_HARDWARE, PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES,
       PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_CACHE_DTLB = 0, PERF_COUNT_HW_CACHE_OP_READ = 0, PERF_COUNT_HW_CACHE_RESULT_MISS = 0 };
int perf_open(uint64_t, uint32_t = 0) { return -1; }
#else
int perf_open(uint64_t config, uint32_t type = PERF_TYPE_HARDWARE) {
  perf_event_attr a{};
  a.type = type;
  a.size = sizeof a;
  a.config = config;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
}
#endif
double perf_read(int fd) {
  uint64_t v = 0;
  return fd >= 0 && read(fd, &v, sizeof v) == sizeof v ? double(v) : 0.0;
}

struct Pool {
  std::vector<int> cpus;
  std::vector<std::thread> threads;
  std::vector<WorkerStats> stats;
  std::mutex m;
  std::condition_variable start_cv, done_cv;
  uint64_t round = 0;
  int running = 0;
  bool quit = false;
  // The current batch.
  const Problem* problems = nullptr;
  const Deal* deals = nullptr;  // a table batch instead (problems is then null)
  Table* tables = nullptr;
  const uint32_t* order = nullptr;
  uint8_t* tricks = nullptr;
  size_t n = 0;
  alignas(64) std::atomic<size_t> next{0};

  void worker(int w) {
    if (cpus[w] >= 0) ddclaim::pin_thread(cpus[w]);
    const int fc = perf_open(PERF_COUNT_HW_CPU_CYCLES), fi = perf_open(PERF_COUNT_HW_INSTRUCTIONS);
    const int fb = perf_open(PERF_COUNT_HW_BRANCH_MISSES), fm = perf_open(PERF_COUNT_HW_CACHE_MISSES);
    const int ft = perf_open(PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16), PERF_TYPE_HW_CACHE);
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m);
        start_cv.wait(lk, [&] { return quit || round != seen; });
        if (quit) {
          if (fc >= 0) close(fc);
          if (fi >= 0) close(fi);
          for (int fd : {fb, fm, ft})
            if (fd >= 0) close(fd);
          return;
        }
        seen = round;
      }
      WorkerStats& st = stats[w];
      const auto t0 = std::chrono::steady_clock::now();
      const double c0 = perf_read(fc), i0 = perf_read(fi), b0 = perf_read(fb), m0 = perf_read(fm), t0c = perf_read(ft);
      size_t done = 0;
      for (;;) {
        const size_t k = next.fetch_add(1, std::memory_order_relaxed);
        if (k >= n) break;
        const size_t i = order ? order[k] : k;
        if (deals) detail::solve_table(deals[i].hand, tables[i].tricks);
        else solve(&problems[i], 1, &tricks[i]);
        done++;
      }
      st.cycles += perf_read(fc) - c0, st.instructions += perf_read(fi) - i0;
      st.branch_misses += perf_read(fb) - b0, st.cache_misses += perf_read(fm) - m0, st.dtlb_misses += perf_read(ft) - t0c;
      st.deals += done;
      st.busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::lock_guard<std::mutex> lk(m);
      if (--running == 0) done_cv.notify_one();
    }
  }
  void start(const int* c, int nc) {
    cpus.assign(c, c + nc);
    stats.assign(nc, WorkerStats{});
    for (int w = 0; w < nc; w++) stats[w].cpu = cpus[w];
    quit = false;
    for (int w = 0; w < nc; w++) threads.emplace_back(&Pool::worker, this, w);
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m);
      quit = true;
    }
    start_cv.notify_all();
    for (auto& t : threads) t.join();
    threads.clear();
    cpus.clear();
  }
  ~Pool() { stop(); }
};

Pool& pool() {
  static Pool p;
  return p;
}

// Log of the expected solving time, up to a constant: a linear fit on 4,600 random deals
// (NOTES.md, round B). Rank correlation with the measured time about 0.43 on held-out deals.
float predict_cost(const Problem& p) {
  static const int kHcp[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  int len[4][4], hcp[4] = {0, 0, 0, 0}, voids = 0, singles = 0;
  float shape = 0;
  for (int q = 0; q < 4; q++)
    for (int s = 0; s < 4; s++) {
      const unsigned h = p.hand[q][s];
      len[q][s] = __builtin_popcount(h);
      for (int r = 9; r < 13; r++)
        if (h >> r & 1) hcp[q] += kHcp[r];
      shape += len[q][s] > 3 ? len[q][s] - 3.25f : 3.25f - len[q][s];
      voids += len[q][s] == 0, singles += len[q][s] == 1;
    }
  const bool nt = p.trump == 4;
  const int d = p.declarer & 1, ds = hcp[d] + hcp[d + 2];
  const int dfit = nt ? 0 : len[d][p.trump] + len[d + 2][p.trump];
  int fit = 0;
  for (int s = 0; s < 4; s++) fit = std::max({fit, len[0][s] + len[2][s], len[1][s] + len[3][s]});
  const int side = std::max(std::abs(hcp[0] - hcp[2]), std::abs(hcp[1] - hcp[3]));
  const float x[12] = {float(nt), float(ds), float(std::abs(ds - 20)), float((ds - 20) * (ds - 20)), float(dfit), float(dfit * dfit),
                       shape, float(voids), float(singles), float(fit), float(side), float(std::abs(hcp[d] - hcp[d + 2]))};
  static const float c[12] = {3.5248f, -0.0007f, -0.0041f, -0.0093f, 1.1111f, -0.0796f, -0.0819f, 0.9707f, 0.4071f, -0.4401f, -0.0209f, -0.0127f};
  float e = 0;
  for (int k = 0; k < 12; k++) e += c[k] * x[k];
  return e;
}

// Runs the batch already set in P (problems or deals) on its workers and waits for it.
void run_round(Pool& P, size_t n, const uint32_t* order, int ncpu, std::vector<WorkerStats>* stats) {
  for (auto& s : P.stats) s.deals = 0, s.busy_s = 0, s.cycles = 0, s.instructions = 0, s.branch_misses = 0, s.cache_misses = 0, s.dtlb_misses = 0;
  {
    std::lock_guard<std::mutex> lk(P.m);
    P.n = n;
    P.order = order;
    P.next.store(0);
    P.running = ncpu;
    P.round++;
  }
  P.start_cv.notify_all();
  {
    std::unique_lock<std::mutex> lk(P.m);
    P.done_cv.wait(lk, [&] { return P.running == 0; });
  }
  if (stats) *stats = P.stats;
}

}  // namespace

void init(const char* table_dir) { detail::init(table_dir); }

void solve(const Problem* problems, size_t n, uint8_t* tricks) {
  std::vector<ddclaim::Position> ps(n);
  for (size_t i = 0; i < n; i++) ps[i] = to_position(problems[i]);
  detail::solve(ps.data(), n, tricks);  // North-South's tricks
  for (size_t i = 0; i < n; i++)
    if (problems[i].declarer & 1) tricks[i] = uint8_t(13 - tricks[i]);
}

void solve_table(const Deal& deal, Table& table) { detail::solve_table(deal.hand, table.tricks); }

bool pin_thread(int cpu) { return ddclaim::pin_thread(cpu); }

unsigned long long stat_nodes() { return detail::nodes(); }

void solve_parallel(const Problem* problems, size_t n, uint8_t* tricks, const int* cpus, int ncpu, BatchOrder order,
                    std::vector<WorkerStats>* stats) {
  Pool& P = pool();
  if (P.cpus.size() != size_t(ncpu) || !std::equal(P.cpus.begin(), P.cpus.end(), cpus)) {
    P.stop();
    P.start(cpus, ncpu);
  }
  std::vector<uint32_t> ord;
  if (order == BatchOrder::Predicted) {
    std::vector<float> cost(n);
    for (size_t i = 0; i < n; i++) cost[i] = predict_cost(problems[i]);
    ord.resize(n);
    std::iota(ord.begin(), ord.end(), 0u);
    std::stable_sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) { return cost[a] > cost[b]; });
  }
  P.problems = problems, P.tricks = tricks, P.deals = nullptr, P.tables = nullptr;
  run_round(P, n, ord.empty() ? nullptr : ord.data(), ncpu, stats);
}

void solve_tables(const Deal* deals, size_t n, Table* tables) {
  for (size_t i = 0; i < n; i++) detail::solve_table(deals[i].hand, tables[i].tricks);
}

void solve_tables_parallel(const Deal* deals, size_t n, Table* tables, const int* cpus, int ncpu,
                           std::vector<WorkerStats>* stats) {
  Pool& P = pool();
  if (P.cpus.size() != size_t(ncpu) || !std::equal(P.cpus.begin(), P.cpus.end(), cpus)) {
    P.stop();
    P.start(cpus, ncpu);
  }
  P.problems = nullptr, P.tricks = nullptr, P.deals = deals, P.tables = tables;
  run_round(P, n, nullptr, ncpu, stats);
}

std::vector<int> available_cpus(bool smt) {
#if defined(__APPLE__)
  // No pinning on macOS (workers ask for the performance cores by QoS instead), so these are
  // just worker slots: the performance cores first, then the efficiency cores.
  (void)smt;
  int n = 0;
  size_t len = sizeof n;
  if (sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0) != 0 || n < 1) n = int(std::thread::hardware_concurrency());
  std::vector<int> v(size_t(n > 0 ? n : 1));
  std::iota(v.begin(), v.end(), 0);
  return v;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return {};
  struct C { int cpu, core; long mhz; bool sibling; };
  std::vector<C> cs;
  std::vector<int> seen_core;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (!CPU_ISSET(c, &set)) continue;
    long mhz = 0;
    int core = c;
    char path[128];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
    if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%ld", &mhz) != 1) mhz = 0; fclose(f); }
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", c);
    if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%d", &core) != 1) core = c; fclose(f); }
    const bool sib = std::find(seen_core.begin(), seen_core.end(), core) != seen_core.end();
    if (!sib) seen_core.push_back(core);
    if (sib && !smt) continue;
    cs.push_back({c, core, mhz, sib});
  }
  std::stable_sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.sibling != b.sibling ? !a.sibling : a.mhz > b.mhz; });
  std::vector<int> v;
  for (auto& c : cs) v.push_back(c.cpu);
  return v;
#endif
}

void stop_pool() { pool().stop(); }

}  // namespace ddfast
