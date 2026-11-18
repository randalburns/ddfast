// Times ddfast::solve_position on random positions of one size (ddclaim's generator, see
// positions.hpp), one position at a time on one thread (pin it: taskset -c 0 ...).
//
// usage: bench_positions CARDS N SEED [ANSWERS_FILE] [dds]
//
// Prints cycles per position, one at a time and as one solve_positions() batch. ANSWERS_FILE gets North-South's tricks, one byte per position
// (to compare with ddclaim_positions). With "dds", DDS (SolveBoard, one thread) is also timed
// on the same positions and every answer is checked against it.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ddfast.hpp"
#include "dll.h"
#include "positions.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: bench_positions CARDS N SEED [ANSWERS_FILE] [dds]\n");
    return 2;
  }
  const int cards = atoi(argv[1]), n = atoi(argv[2]);
  const unsigned seed = unsigned(atol(argv[3]));
  const bool dds = argc > 5 && std::string(argv[5]) == "dds";
  const auto ps = positions::make<ddfast::Position>(cards, n, seed);
  ddfast::init();
  ddfast::solve_position(ps[0]);  // warm-up
  std::vector<uint8_t> got(n);
  const int fc = positions::cycles_open();
  const long long c0 = positions::cycles_read(fc);
  for (int i = 0; i < n; i++) got[i] = uint8_t(ddfast::solve_position(ps[i]));
  const long long c = positions::cycles_read(fc) - c0;
  std::vector<uint8_t> bat(n);
  const long long b0 = positions::cycles_read(fc);
  ddfast::solve_positions(ps.data(), size_t(n), bat.data());
  const long long b = positions::cycles_read(fc) - b0;
  int diff = 0;
  for (int i = 0; i < n; i++) diff += bat[i] != got[i];
  positions::save(argc > 4 ? argv[4] : nullptr, got);
  printf("ddfast %d cards: %d positions, one at a time %.0f cycles/position, solve_positions %.0f%s\n", cards, n,
         double(c) / n, double(b) / n, diff ? "  (DIFFER!)" : "");
  if (diff) return 1;
  if (!dds) return 0;
  SetMaxThreads(1);
  std::vector<uint8_t> ref(n);
  auto dds_one = [&](const ddfast::Position& p) {
    Deal dl{};
    dl.trump = p.trump;
    dl.first = p.leader;
    for (int q = 0; q < 4; q++)
      for (int s = 0; s < 4; s++) dl.remainCards[q][s] = unsigned(p.hand[q][s]) << 2;
    FutureTricks fut{};
    if (SolveBoard(dl, -1, 1, 1, &fut, 0) != 1) {
      fprintf(stderr, "DDS SolveBoard failed\n");
      exit(1);
    }
    return uint8_t((p.leader & 1) ? cards - fut.score[0] : fut.score[0]);  // DDS scores the side on lead
  };
  dds_one(ps[0]);
  const long long d0 = positions::cycles_read(fc);
  for (int i = 0; i < n; i++) ref[i] = dds_one(ps[i]);
  const long long d = positions::cycles_read(fc) - d0;
  int wrong = 0;
  for (int i = 0; i < n; i++) wrong += got[i] != ref[i];
  printf("DDS    %d cards: %d positions, %.0f cycles/position  ddfast %s\n", cards, n, double(d) / n,
         wrong ? "WRONG" : "all correct");
  return wrong ? 1 : 0;
}
