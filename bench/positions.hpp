// For bench_positions: random positions of one size, and a cycle counter.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#else
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace positions {

// 4 * cards cards dealt from a shuffled deck, a random strain (four suits and no trumps
// equally likely) and a random leader. P is any struct with hand[4][4], trump and leader.
template <class P>
std::vector<P> make(int cards, int n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<P> out(n);
  for (P& p : out) {
    int deck[52];
    std::iota(deck, deck + 52, 0);
    std::shuffle(deck, deck + 52, rng);
    std::memset(&p, 0, sizeof p);
    for (int k = 0; k < 4 * cards; k++) p.hand[k % 4][deck[k] / 13] |= static_cast<uint16_t>(1u << (deck[k] % 13));
    p.trump = static_cast<uint8_t>(rng() % 5);
    p.leader = static_cast<uint8_t>(rng() % 4);
  }
  return out;
}

// User-mode cycles of the calling thread (-1 without permission). macOS: the process's
// cycles (from proc_pid_rusage; the benchmarks are single-threaded).
#if defined(__APPLE__)
inline int cycles_open() { return 0; }
inline long long cycles_read(int) {
  rusage_info_v4 ri{};
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0) return -1;
  return (long long)ri.ri_cycles;
}
#else
inline int cycles_open() {
  perf_event_attr a{};
  a.type = PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = PERF_COUNT_HW_CPU_CYCLES;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
}
inline long long cycles_read(int fd) {
  long long v = 0;
  return fd >= 0 && read(fd, &v, sizeof v) == sizeof v ? v : -1;
}
#endif

// Writes the answers (one byte each) to path, if given.
inline void save(const char* path, const std::vector<uint8_t>& a) {
  if (!path || !*path) return;
  if (FILE* f = fopen(path, "wb")) {
    fwrite(a.data(), 1, a.size(), f);
    fclose(f);
  }
}

}  // namespace positions
