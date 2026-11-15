// Shared by the tests and benchmarks: random deals, test-set files, DDS reference answers,
// CPU lists and hardware counters.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "ddfast.hpp"
#include "dll.h"

namespace testing {

inline double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Random problems, as the competition referee's make_ref deals them: for each, a shuffled
// deck (std::mt19937 seeded with `seed`), then a strain and a declarer.
inline std::vector<ddfast::Problem> random_problems(size_t n, unsigned seed) {
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

// Random deals for tables (std::mt19937_64 seeded with `seed`).
inline std::vector<ddfast::Deal> random_deals(size_t n, unsigned long long seed) {
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

// A test set: uint32 n; n Problems; n answers (declarer's tricks, uint8); n DDS node counts
// (uint32). The format of the competition's referee.
struct TestSet {
  std::vector<ddfast::Problem> problems;
  std::vector<uint8_t> answers;
  std::vector<uint32_t> nodes;
};

inline bool read_set(const char* path, TestSet& s) {
  FILE* f = fopen(path, "rb");
  uint32_t n = 0;
  if (!f) return false;
  bool ok = fread(&n, 4, 1, f) == 1;
  if (ok) {
    s.problems.resize(n), s.answers.resize(n), s.nodes.resize(n);
    ok = fread(s.problems.data(), sizeof(ddfast::Problem), n, f) == n && fread(s.answers.data(), 1, n, f) == n &&
         fread(s.nodes.data(), 4, n, f) == n;
  }
  fclose(f);
  return ok;
}

inline bool write_set(const char* path, const TestSet& s) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const uint32_t n = uint32_t(s.problems.size());
  bool ok = fwrite(&n, 4, 1, f) == 1 && fwrite(s.problems.data(), sizeof(ddfast::Problem), n, f) == n &&
            fwrite(s.answers.data(), 1, n, f) == n && fwrite(s.nodes.data(), 4, n, f) == n;
  return fclose(f) == 0 && ok;
}

// DDS's answer (declarer's tricks) with SolveBoard(target -1); nodes gets DDS's node count.
inline int dds_solve(const ddfast::Problem& p, uint32_t* nodes = nullptr) {
  Deal dl{};
  dl.trump = p.trump;
  dl.first = (p.declarer + 1) & 3;
  for (int q = 0; q < 4; q++)
    for (int s = 0; s < 4; s++) dl.remainCards[q][s] = unsigned(p.hand[q][s]) << 2;
  FutureTricks fut{};
  if (SolveBoard(dl, -1, 1, 1, &fut, 0) != 1) {
    fprintf(stderr, "DDS SolveBoard failed\n");
    exit(1);
  }
  if (nodes) *nodes = uint32_t(fut.nodes);
  return 13 - fut.score[0];  // DDS scores the side on lead
}

// DDS's full table (CalcDDtable on one thread).
inline void dds_table(const ddfast::Deal& d, ddfast::Table& t) {
  DdTableDeal dd{};
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) dd.cards[h][s] = unsigned(d.hand[h][s]) << 2;
  DdTableResults r{};
  // CalcDDtable may spread the 20 results over threads despite SetMaxThreads(1); the
  // explicit cap keeps the reference on one thread.
  if (CalcDDtableN(dd, &r, 1) != 1) {
    fprintf(stderr, "DDS CalcDDtable failed\n");
    exit(1);
  }
  for (int st = 0; st < 5; st++)
    for (int h = 0; h < 4; h++) t.tricks[st][h] = uint8_t(r.res_table[st][h]);
}

// "0-3,12-15" -> {0, 1, 2, 3, 12, 13, 14, 15}; "auto" -> ddfast::available_cpus().
inline std::vector<int> parse_cpus(const std::string& t) {
  if (t == "auto") return ddfast::available_cpus();
  std::vector<int> v;
  size_t i = 0;
  while (i < t.size()) {
    size_t j = t.find(',', i);
    if (j == std::string::npos) j = t.size();
    const std::string part = t.substr(i, j - i);
    const size_t d = part.find('-');
    if (d == std::string::npos) v.push_back(atoi(part.c_str()));
    else
      for (int c = atoi(part.substr(0, d).c_str()); c <= atoi(part.substr(d + 1).c_str()); c++) v.push_back(c);
    i = j + 1;
  }
  return v;
}

// A counter of the calling thread's user-mode events, or -1 (no permission or no counters).
inline int perf_open(unsigned long long config) {
  perf_event_attr a{};
  a.type = PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = config;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
}
inline long long perf_read(int fd) {
  long long v = 0;
  return fd >= 0 && read(fd, &v, 8) == 8 ? v : -1;
}

// The table directory for init(): $DDCLAIM_DIR if set (init() reads it), else the default.
inline void init_or_exit() {
  const double t0 = now();
  ddfast::init();
  fprintf(stderr, "init %.2f s\n", now() - t0);
}

}  // namespace testing
