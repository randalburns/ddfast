// Writes random endings and their exact values (found with ddclaim itself) to build/guess/data_<cards>.bin,
// the training data for the first-guess tables. usage: dump <cards> <count>. Records: 16 u16 hands, trump, leader, value, pad.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ddclaim.hpp"
struct Rng { uint64_t s; uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); } };
int main(int argc, char** argv) {
  int cards = atoi(argv[1]); size_t n = atol(argv[2]);
  Rng rng{0xABCDEF123ull + cards};
  std::vector<ddclaim::Position> ps(n);
  for (auto& p : ps) {
    int deck[52]; for (int i = 0; i < 52; i++) deck[i] = i;
    memset(&p, 0, sizeof p);
    for (int k = 0; k < 4 * cards; k++) { int j = k + int(rng.next() % uint64_t(52 - k)); int t = deck[k]; deck[k] = deck[j]; deck[j] = t; p.hand[k % 4][deck[k] / 13] |= uint16_t(1u << (deck[k] % 13)); }
    p.trump = uint8_t(rng.next() % 5); p.leader = uint8_t(rng.next() % 4);
  }
  ddclaim::init();
  std::vector<uint8_t> out(n);
  ddclaim::solve(ps.data(), n, out.data());
  char path[100]; snprintf(path, sizeof path, "build/guess/data_%d.bin", cards);
  FILE* f = fopen(path, "wb");
  for (size_t i = 0; i < n; i++) { uint8_t rec[36]; memcpy(rec, ps[i].hand, 32); rec[32] = ps[i].trump; rec[33] = ps[i].leader; rec[34] = out[i]; rec[35] = 0; fwrite(rec, 1, 36, f); }
  fclose(f);
}
