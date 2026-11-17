// Times ddclaim on random positions and checks every answer against the reference solver
// (altsolve, an independent search with no tables).
//
// usage: bench <cards> <positions> [seed] [batches]
//
// Positions are uniformly random: 4 * cards cards dealt from a full deck, a random strain
// (the four suits and no trumps equally likely) and a random leader. ddclaim is timed on
// `batches` different batches of <positions> positions (seeds seed, seed + 1, ...), each
// solved once, so that nothing is answered from a repeated batch; the speed is
// total positions over total time. The reference answers are computed with altsolve once
// per (cards, positions, seed) and cached in build/.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "altsolve.hpp"
#include "ddclaim.hpp"

using ddclaim::Position;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<Position> make_positions(int cards, int n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<Position> out(n);
  for (Position& p : out) {
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

std::vector<uint8_t> reference(const std::vector<Position>& positions, int cards, unsigned seed) {
  const std::string path =
      "build/ref_" + std::to_string(cards) + "_" + std::to_string(positions.size()) + "_" + std::to_string(seed) + ".bin";
  std::vector<uint8_t> ref(positions.size());
  {
    std::ifstream in(path, std::ios::binary);
    if (in && in.read(reinterpret_cast<char*>(ref.data()), static_cast<std::streamsize>(ref.size()))) return ref;
  }
  altsolve::Options o;
  o.table_bits = 18;
  std::vector<std::unique_ptr<altsolve::Solver>> solver;
  for (int t = 0; t < 5; t++) {
    solver.push_back(std::make_unique<altsolve::Solver>(o));
    solver[t]->set_trump(t);
  }
  for (size_t i = 0; i < positions.size(); i++) {
    const Position& p = positions[i];
    altsolve::Holding h{};
    for (int q = 0; q < 4; q++)
      for (int s = 0; s < 4; s++) h[q][s] = static_cast<uint16_t>(p.hand[q][s] << 2);
    ref[i] = static_cast<uint8_t>(solver[p.trump]->ns_value(h, p.leader, {}));
  }
  std::ofstream outf(path, std::ios::binary);
  outf.write(reinterpret_cast<const char*>(ref.data()), static_cast<std::streamsize>(ref.size()));
  return ref;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: bench <cards> <positions> [seed] [reps]\n");
    return 2;
  }
  const int cards = atoi(argv[1]), n = atoi(argv[2]);
  const unsigned seed = argc > 3 ? static_cast<unsigned>(atoi(argv[3])) : 1;
  const int reps = argc > 4 ? atoi(argv[4]) : 3;
  if (cards < 1 || cards > 13 || n < 1) return 2;

  std::vector<std::vector<Position>> batch;
  std::vector<std::vector<uint8_t>> ref;
  for (int r = 0; r < reps; r++) {
    batch.push_back(make_positions(cards, n, seed + static_cast<unsigned>(r)));
    ref.push_back(reference(batch[r], cards, seed + static_cast<unsigned>(r)));
  }

  auto t0 = std::chrono::steady_clock::now();
  ddclaim::init();
  const double init_secs = seconds_since(t0);

  std::vector<uint8_t> got(static_cast<size_t>(n));
  double total = 0;
  size_t wrong = 0;
  for (int r = 0; r < reps; r++) {
    std::fill(got.begin(), got.end(), 0xFF);
    t0 = std::chrono::steady_clock::now();
    ddclaim::solve(batch[r].data(), batch[r].size(), got.data());
    total += seconds_since(t0);
    for (size_t i = 0; i < got.size(); i++) wrong += got[i] != ref[r][i];
  }
  const double best = total / reps;  // seconds per batch
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  printf("%-24s %d cards  %9d positions  %12.0f positions/s  %8.1f ns each  init %.2f s  peak memory %ld MB  %s\n",
         "ddclaim", cards, n, n / best, 1e9 * best / n, init_secs, ru.ru_maxrss / 1024,
         wrong ? "WRONG ANSWERS" : "all correct");
  if (wrong) printf("  %zu of %d answers differ from the reference\n", wrong, n * reps);
  return wrong ? 1 : 0;
}
