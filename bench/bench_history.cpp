// bench_history: one thread, ddfast's public API only (init, solve, solve_table), so the same
// program measures every commit: user-mode cycles per problem (random deals, a strain and a
// declarer, as tests/common.hpp's random_problems) and per full table (random_deals), best
// of `reps` passes, the instructions of the last pass (exact, unlike cycles), and a hash of
// every answer.
//   bench_history [n=100] [seed=1] [reps=3]   (docs/HISTORY.md: 300 7 3)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#include <linux/perf_event.h>
#include <sys/syscall.h>
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

int main(int argc, char** argv) {
  const size_t n = argc > 1 ? size_t(atol(argv[1])) : 100;
  const unsigned seed = argc > 2 ? unsigned(atol(argv[2])) : 1;
  const int reps = argc > 3 ? atoi(argv[3]) : 3;
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
  double best_p = 1e30, best_t = 1e30;
  for (int r = 0; r < reps; r++) {
    const long long c0 = rd(fd), i0 = rd(fi);
    for (size_t i = 0; i < n; i++) ddfast::solve(&ps[i], 1, &tricks[i]);
    best_p = std::min(best_p, double(rd(fd) - c0) / double(n));
    ins_p = double(rd(fi) - i0) / double(n);
  }
  for (int r = 0; r < reps; r++) {
    const long long c0 = rd(fd), i0 = rd(fi);
    for (size_t i = 0; i < n; i++) ddfast::solve_table(ds[i], tabs[i]);
    best_t = std::min(best_t, double(rd(fd) - c0) / double(n));
    ins_t = double(rd(fi) - i0) / double(n);
  }
  mix(tricks.data(), n);
  for (auto& t : tabs) mix(t.tricks, sizeof t.tricks);
  printf("problems %.2fM cycles %.2fM instr  tables %.1fM cycles %.1fM instr  hash %016llx\n", best_p / 1e6, ins_p / 1e6, best_t / 1e6, ins_t / 1e6, (unsigned long long)h);
}
