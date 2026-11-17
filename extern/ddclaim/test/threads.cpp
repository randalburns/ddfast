// Checks that solve() can be called from several threads at once (after one init()): each
// thread solves its own batches, and the answers must equal those of a single thread.
//
// usage: threads <cards> <positions per thread> <threads> [cpus]
//
// With a comma-separated list of CPU numbers, thread t is pinned to the t-th of them (the
// single-thread pass runs on the first), which makes the speeds comparable.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>


#include "ddclaim.hpp"

int main(int argc, char** argv) {
  const int cards = argc > 1 ? atoi(argv[1]) : 6, n = argc > 2 ? atoi(argv[2]) : 50000, threads = argc > 3 ? atoi(argv[3]) : 8;
  std::vector<int> cpus;
  if (argc > 4)
    for (const char* c = argv[4]; *c;) {
      cpus.push_back(atoi(c));
      while (*c && *c != ',') c++;
      if (*c) c++;
    }
  auto pin = [&](int t) {
    if (!cpus.empty()) ddclaim::pin_thread(cpus[static_cast<size_t>(t) % cpus.size()]);
  };
  ddclaim::init();
  pin(0);
  std::vector<std::vector<ddclaim::Position>> batch(static_cast<size_t>(threads));
  for (int t = 0; t < threads; t++) {
    std::mt19937 rng(static_cast<unsigned>(4242 + t));
    batch[t].resize(static_cast<size_t>(n));
    for (ddclaim::Position& q : batch[t]) {
      int deck[52];
      std::iota(deck, deck + 52, 0);
      std::shuffle(deck, deck + 52, rng);
      std::memset(&q, 0, sizeof q);
      for (int k = 0; k < 4 * cards; k++) q.hand[k % 4][deck[k] / 13] |= static_cast<uint16_t>(1u << (deck[k] % 13));
      q.trump = static_cast<uint8_t>(rng() % 5);
      q.leader = static_cast<uint8_t>(rng() % 4);
    }
  }
  // One thread first, for the expected answers and the single-thread speed.
  std::vector<std::vector<uint8_t>> want(batch.size()), got(batch.size());
  auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < threads; t++) {
    want[t].resize(batch[t].size());
    ddclaim::solve(batch[t].data(), batch[t].size(), want[t].data());
  }
  const double one = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  // Then all threads at once, several rounds.
  size_t differ = 0;
  double many = 0;
  for (int round = 0; round < 3; round++) {
    std::vector<std::thread> pool;
    t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; t++)
      pool.emplace_back([&, t] {
        pin(t);
        got[t].assign(batch[t].size(), 0xFF);
        ddclaim::solve(batch[t].data(), batch[t].size(), got[t].data());
      });
    for (std::thread& th : pool) th.join();
    many = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (int t = 0; t < threads; t++)
      for (size_t i = 0; i < got[t].size(); i++) differ += got[t][i] != want[t][i];
  }
  const double total = static_cast<double>(threads) * n;
  printf("%d cards, %d threads x %d positions: one thread %.0f positions/s, %d threads together %.0f positions/s (%.1fx); "
         "%zu answers differ\n", cards, threads, n, total / one, threads, total / many, one / many, differ);
  return differ ? 1 : 0;
}
