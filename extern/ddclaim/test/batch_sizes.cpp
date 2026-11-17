// How much batching matters: the same random positions solved in batches of several sizes.
//
// usage: batch_sizes <cards> <positions>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include "ddclaim.hpp"

int main(int argc, char** argv) {
  const int cards = argc > 1 ? atoi(argv[1]) : 6, n = argc > 2 ? atoi(argv[2]) : 100000;
  ddclaim::init();
  for (size_t batch : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{64}, size_t{1000}, static_cast<size_t>(n)}) {
    // Fresh positions for every batch size, so nothing is remembered from an earlier pass.
    std::mt19937 rng(static_cast<unsigned>(1000 + batch));
    std::vector<ddclaim::Position> p(static_cast<size_t>(n));
    for (ddclaim::Position& q : p) {
      int deck[52];
      std::iota(deck, deck + 52, 0);
      std::shuffle(deck, deck + 52, rng);
      std::memset(&q, 0, sizeof q);
      for (int k = 0; k < 4 * cards; k++) q.hand[k % 4][deck[k] / 13] |= static_cast<uint16_t>(1u << (deck[k] % 13));
      q.trump = static_cast<uint8_t>(rng() % 5);
      q.leader = static_cast<uint8_t>(rng() % 4);
    }
    std::vector<uint8_t> out(p.size());
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < p.size(); i += batch) ddclaim::solve(&p[i], std::min(batch, p.size() - i), &out[i]);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("batches of %7zu: %9.0f positions/s  %7.1f ns each\n", batch, p.size() / s, 1e9 * s / p.size());
  }
}
