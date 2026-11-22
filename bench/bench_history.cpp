// bench_history: one thread, ddfast's public API only (init, solve, solve_table), so the same
// program measures every commit: user-mode cycles per problem (random deals, a strain and a
// declarer, as tests/common.hpp's random_problems) and per full table (random_deals), best
// of `reps` passes, the instructions of the last pass (exact, unlike cycles), and a hash of
// every answer.
//   bench_history [n=100] [seed=1] [reps=3] [threads=1]   (docs/HISTORY.md: 300 7 3)
// With threads > 1 (macOS only: there the counters cover every thread of the process) the same
// work goes through solve_parallel and solve_tables_parallel on that many threads, and the
// line also gives problems and tables per second of wall time; cycles are summed over the
// threads.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#if defined(__APPLE__)
#include <libproc.h>
#include <pthread.h>
#include <sys/resource.h>
#else
#include <linux/perf_event.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include "ddfast.hpp"

static std::vector<ddfast::Problem> random_problems(size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<ddfast::Problem> p(n);
  for (auto& q : p) {
    int deck[52];
    std::iota(deck, deck + 52, 0);
    std::shuffle(deck, deck + 52, rng);
    std::memset(&q, 0, sizeof q);
    for (int k = 0; k < 52; k++) q.hand[k % 4][deck[k] / 13] |= uint16_t(1u << (deck[k] % 13));
    q.trump = uint8_t(rng() % 5);
    q.declarer = uint8_t(rng() % 4);
  }
  return p;
}
static std::vector<ddfast::Deal> random_deals(size_t n, unsigned long long seed) {
  std::mt19937_64 rng(seed);
  std::vector<ddfast::Deal> d(n);
  for (auto& x : d) {
    int c[52];
    for (int i = 0; i < 52; i++) c[i] = i;
    for (int i = 51; i > 0; i--) std::swap(c[i], c[rng() % (i + 1)]);
    std::memset(&x, 0, sizeof x);
    for (int i = 0; i < 52; i++) x.hand[i / 13][c[i] / 13] |= uint16_t(1u << (c[i] % 13));
  }
  return d;
}
#if defined(__APPLE__)
// macOS: the process's cycles and instructions (proc_pid_rusage: user and system, all threads;
// this program has one). fd 0 reads cycles, 1 instructions.
static int cycles_fd() { return 0; }
static int instr_fd(int) { return 1; }
static long long rd(int fd) {
  rusage_info_v4 ri{};
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0) return -1;
  return (long long)(fd ? ri.ri_instructions : ri.ri_cycles);
}
#else
static int cycles_fd() {
  perf_event_attr a{};
  a.type = PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = PERF_COUNT_HW_CPU_CYCLES;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
}
static int instr_fd(int group) {
  perf_event_attr a{};
  a.type = PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = PERF_COUNT_HW_INSTRUCTIONS;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(SYS_perf_event_open, &a, 0, -1, group, 0));
}
static long long rd(int fd) { long long v = 0; return read(fd, &v, 8) == 8 ? v : -1; }

#endif

int main(int argc, char** argv) {
  const size_t n = argc > 1 ? size_t(atol(argv[1])) : 100;
  const unsigned seed = argc > 2 ? unsigned(atol(argv[2])) : 1;
  const int reps = argc > 3 ? atoi(argv[3]) : 3;
  const int threads = argc > 4 ? atoi(argv[4]) : 1;
  std::vector<int> cpus(size_t(threads > 1 ? threads : 1));
  std::iota(cpus.begin(), cpus.end(), 0);
  auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  double wall_p = 1e30, wall_t = 1e30;
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // a performance core (macOS cannot pin)
#endif
  ddfast::init(nullptr);
  const int fd = cycles_fd();
  const int fi = instr_fd(-1);
  double ins_p = 0, ins_t = 0;
  const auto ps = random_problems(n, seed);
  const auto ds = random_deals(n, seed);
  std::vector<uint8_t> tricks(n);
  std::vector<ddfast::Table> tabs(n);
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t k) { for (size_t i = 0; i < k; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull; };
  ddfast::solve(ps.data(), 1, tricks.data());  // warm-up
  if (threads > 1) {  // starts the pool and warms every thread
    ddfast::solve_parallel(ps.data(), std::min<size_t>(n, 64), tricks.data(), cpus.data(), threads);
    ddfast::solve_tables_parallel(ds.data(), std::min<size_t>(n, 8), tabs.data(), cpus.data(), threads);
  }
  double best_p = 1e30, best_t = 1e30;
  for (int r = 0; r < reps; r++) {
    const long long c0 = rd(fd), i0 = rd(fi);
    const double t0 = now();
    if (threads > 1) ddfast::solve_parallel(ps.data(), n, tricks.data(), cpus.data(), threads);
    else for (size_t i = 0; i < n; i++) ddfast::solve(&ps[i], 1, &tricks[i]);
    wall_p = std::min(wall_p, now() - t0);
    best_p = std::min(best_p, double(rd(fd) - c0) / double(n));
    ins_p = double(rd(fi) - i0) / double(n);
  }
  for (int r = 0; r < reps; r++) {
    const long long c0 = rd(fd), i0 = rd(fi);
    const double t0 = now();
    if (threads > 1) ddfast::solve_tables_parallel(ds.data(), n, tabs.data(), cpus.data(), threads);
    else for (size_t i = 0; i < n; i++) ddfast::solve_table(ds[i], tabs[i]);
    wall_t = std::min(wall_t, now() - t0);
    best_t = std::min(best_t, double(rd(fd) - c0) / double(n));
    ins_t = double(rd(fi) - i0) / double(n);
  }
  mix(tricks.data(), n);
  for (auto& t : tabs) mix(t.tricks, sizeof t.tricks);
  if (threads > 1) printf("%d thr %.0f problems/s %.1f tables/s  ", threads, double(n) / wall_p, double(n) / wall_t);
  printf("problems %.2fM cycles %.2fM instr  tables %.1fM cycles %.1fM instr  hash %016llx\n", best_p / 1e6, ins_p / 1e6, best_t / 1e6, ins_t / 1e6, (unsigned long long)h);
}
