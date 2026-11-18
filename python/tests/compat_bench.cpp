// Per-board speed of the DDS-compatible layer against DDS's SolveBoard, one thread, by the
// number of tricks left and by `solutions` (target -1, mode 1, as BEN calls it):
//   compat_bench [positions per row] [seed]
// Positions are random deals played out at random (the current trick 0..3 cards in). Every
// answer's score[0] is compared.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <vector>

#include "dds_compat.hpp"
#include <dll.h>

namespace {
std::mt19937_64 rng;
int R(int n) { return int(rng() % uint64_t(n)); }
struct P {
  unsigned remain[4][4];
  int trump, first, ts[3], tr[3];
};
P random_position(int cards) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  std::shuffle(deck, deck + 52, rng);
  P p{};
  for (int i = 0; i < 52; i++) p.remain[i / 13][deck[i] / 13] |= 1u << (deck[i] % 13 + 2);
  p.trump = R(5);
  int leader = R(4), played = 0, ts[4], tr[4];
  for (int c = 0; c < cards; c++) {
    const int seat = (leader + played) & 3;
    int s;
    if (played > 0 && p.remain[seat][ts[0]]) s = ts[0];
    else do s = R(4); while (!p.remain[seat][s]);
    unsigned h = p.remain[seat][s];
    for (int k = R(__builtin_popcount(h)); k--;) h &= h - 1;
    const int r = __builtin_ctz(h);
    p.remain[seat][s] &= ~(1u << r);
    ts[played] = s, tr[played] = r;
    if (++played == 4) {
      int best = 0;
      for (int j = 1; j < 4; j++) {
        const bool ruff = p.trump < 4 && ts[j] == p.trump && ts[best] != p.trump;
        if (ruff || (ts[j] == ts[best] && tr[j] > tr[best])) best = j;
      }
      leader = (leader + best) & 3;
      played = 0;
    }
  }
  p.first = leader;
  for (int k = 0; k < 3; k++) p.ts[k] = k < played ? ts[k] : 0, p.tr[k] = k < played ? tr[k] : 0;
  return p;
}
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? atoi(argv[1]) : 200;
  rng.seed(argc > 2 ? strtoull(argv[2], nullptr, 10) : 7);
  SetMaxThreads(1);
  ddfast_compat::ensure_init();
  printf("%-28s %10s %12s %12s %8s\n", "position", "solutions", "DDS us", "ddfast us", "ratio");
  int bad = 0;
  for (int tricks_left : {13, 11, 9, 7, 5}) {
    for (int extra : {0, 2}) {  // cards already in the current trick
      std::vector<P> ps;
      for (int i = 0; i < n; i++) ps.push_back(random_position(4 * (13 - tricks_left) + extra));
      for (int sol : {1, 3}) {
        std::vector<int> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
        double t0 = now();
        for (int i = 0; i < n; i++) {
          Deal d{};
          d.trump = ps[size_t(i)].trump, d.first = ps[size_t(i)].first;
          for (int k = 0; k < 3; k++) d.currentTrickSuit[k] = ps[size_t(i)].ts[k], d.currentTrickRank[k] = ps[size_t(i)].tr[k];
          std::memcpy(d.remainCards, ps[size_t(i)].remain, sizeof d.remainCards);
          FutureTricks f{};
          SolveBoard(d, -1, sol, 1, &f, 0);
          a[size_t(i)] = f.score[0];
        }
        const double td = (now() - t0) / n * 1e6;
        t0 = now();
        for (int i = 0; i < n; i++) {
          ddfast_compat::Future f;
          ddfast_compat::solve_board(ps[size_t(i)].remain, ps[size_t(i)].trump, ps[size_t(i)].first, ps[size_t(i)].ts,
                                     ps[size_t(i)].tr, -1, sol, 1, f);
          b[size_t(i)] = f.score[0];
        }
        const double tf = (now() - t0) / n * 1e6;
        if (a != b) bad++;
        char what[64];
        snprintf(what, sizeof what, "%d tricks left, %d in trick", tricks_left, extra);
        printf("%-28s %10d %12.1f %12.1f %7.2fx\n", what, sol, td, tf, td / tf);
      }
    }
  }
  printf("%s\n", bad ? "SCORES DIFFER" : "all score[0] agree");
  return bad != 0;
}
