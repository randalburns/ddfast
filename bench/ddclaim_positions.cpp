// Times ddclaim's standalone solver (the separate repository's main, which has
// ddclaim::init/solve for 1-7 cards) on the same random positions as bench_positions.
// Built by bench/positions_vs_ddclaim.sh against a checkout of that repository.
//
// usage: ddclaim_positions CARDS N SEED [ANSWERS_FILE]
//
// Prints cycles per position solved as one batch (ddclaim's normal use: 8 searches
// interleaved on the thread) and one position at a time.
#include <cstdio>
#include <cstdlib>

#include "ddclaim.hpp"
#include "positions.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: ddclaim_positions CARDS N SEED [ANSWERS_FILE]\n");
    return 2;
  }
  const int cards = atoi(argv[1]), n = atoi(argv[2]);
  const unsigned seed = unsigned(atol(argv[3]));
  const auto ps = positions::make<ddclaim::Position>(cards, n, seed);
  ddclaim::init();
  std::vector<uint8_t> got(n), one(n);
  ddclaim::solve(ps.data(), 1, got.data());  // warm-up
  const int fc = positions::cycles_open();
  long long c0 = positions::cycles_read(fc);
  ddclaim::solve(ps.data(), size_t(n), got.data());
  const long long cb = positions::cycles_read(fc) - c0;
  c0 = positions::cycles_read(fc);
  for (int i = 0; i < n; i++) ddclaim::solve(&ps[i], 1, &one[i]);
  const long long cs = positions::cycles_read(fc) - c0;
  int diff = 0;
  for (int i = 0; i < n; i++) diff += got[i] != one[i];
  positions::save(argc > 4 ? argv[4] : nullptr, got);
  printf("ddclaim %d cards: %d positions, batch %.0f cycles/position, one at a time %.0f%s\n", cards, n, double(cb) / n,
         double(cs) / n, diff ? "  (batch and single differ!)" : "");
  return diff ? 1 : 0;
}
