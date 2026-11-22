// bench_play_history: one thread, the work of a bot playing a deal, so that one program
// measures every commit (bench/history.sh with PLAY=1; docs/HISTORY.md). Three parts, each
// user-mode cycles (macOS: the process's), the best of `reps` passes after one untimed pass:
//
// 1. Positions: trick starts of 12 down to 6 cards a hand in batches of 32 (two hands fixed,
//    the other two re-dealt, as a bot samples the hidden hands), ddfast::solve_position one at
//    a time. Cycles per position.
// 2. Cards: ddfast::card_results (every legal card's result) at leads of 11 to 8 cards a hand,
//    the same kind of batches. Cycles per call.
// 3. Boards, where the commit has the DDS-compatible layer (python/src/dds_compat.cpp, compiled
//    with this file): its SolveBoard on batches of 32 at 11 to 8 cards a hand, at a lead and
//    with two cards played to the trick, `solutions` 1 and 3 (target -1, mode 1), as BEN calls
//    it. Cycles per board.
//
// Positions are reached by random legal play from random deals with a random strain. A hash
// of every answer (for boards the scores, not the cards: with `solutions` 1 the card may be
// another equally good one) on each line shows that two commits agree.
//   bench_play_history [reps=3] [seed=1] [scale=1]
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "ddfast.hpp"
#include "positions.hpp"
#if __has_include("dds_compat.hpp")
#include "dds_compat.hpp"
#define HAVE_COMPAT 1
#else
#define HAVE_COMPAT 0
#endif

namespace {

constexpr int kBatch = 32;
std::mt19937_64 rng;
int R(int n) { return int(rng() % uint64_t(n)); }

// A position in the play: hand[seat][suit] (bit r is rank 2 + r), the strain, the leader of
// the current trick and the cards played to it.
struct Pos {
  uint16_t hand[4][4];
  int trump, leader, played, ts[3], tr[3];  // tr: rank 2..14
};

// A random deal played out at random (following suit) for `cards` cards.
Pos played_out(int cards) {
  int deck[52];
  std::iota(deck, deck + 52, 0);
  std::shuffle(deck, deck + 52, rng);
  Pos p{};
  for (int i = 0; i < 52; i++) p.hand[i / 13][deck[i] / 13] |= uint16_t(1u << (deck[i] % 13));
  p.trump = R(5);
  int leader = R(4), played = 0, ts[4], tr[4];
  for (int c = 0; c < cards; c++) {
    const int seat = (leader + played) & 3;
    int s;
    if (played > 0 && p.hand[seat][ts[0]]) s = ts[0];
    else do s = R(4); while (!p.hand[seat][s]);
    unsigned h = p.hand[seat][s];
    for (int k = R(__builtin_popcount(h)); k--;) h &= h - 1;
    const int r = __builtin_ctz(h);
    p.hand[seat][s] &= uint16_t(~(1u << r));
    ts[played] = s, tr[played] = r + 2;
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
  p.leader = leader, p.played = played;
  for (int k = 0; k < 3; k++) p.ts[k] = k < played ? ts[k] : 0, p.tr[k] = k < played ? tr[k] : 0;
  return p;
}

// kBatch samples of p: the hands of the two seats other than the one to play and its partner
// re-dealt (each keeps its number of cards).
std::vector<Pos> samples(const Pos& p) {
  const int to_play = (p.leader + p.played) & 3, h0 = (to_play + 1) & 3, h1 = (to_play + 3) & 3;
  std::vector<int> pool;
  int k0 = 0;
  for (int s = 0; s < 4; s++)
    for (int r = 0; r < 13; r++) {
      if (p.hand[h0][s] >> r & 1) pool.push_back(16 * s + r), k0++;
      if (p.hand[h1][s] >> r & 1) pool.push_back(16 * s + r);
    }
  std::vector<Pos> out(kBatch, p);
  for (auto& q : out) {
    std::shuffle(pool.begin(), pool.end(), rng);
    for (int s = 0; s < 4; s++) q.hand[h0][s] = q.hand[h1][s] = 0;
    for (size_t i = 0; i < pool.size(); i++) q.hand[int(i) < k0 ? h0 : h1][pool[i] / 16] |= uint16_t(1u << (pool[i] % 16));
  }
  return out;
}

// `sets` batches at `tricks` tricks played and `extra` cards into the next.
std::vector<Pos> batches(int tricks, int extra, int sets) {
  std::vector<Pos> all;
  for (int k = 0; k < sets; k++) {
    const auto b = samples(played_out(4 * tricks + extra));
    all.insert(all.end(), b.begin(), b.end());
  }
  return all;
}

uint64_t g_hash;
void mix(uint64_t v) { g_hash = (g_hash ^ v) * 1099511628211ull; }
void hash_start() { g_hash = 1469598103934665603ull; }
void hash_print() { printf("  hash %016llx\n", (unsigned long long)g_hash); }

int g_fc, g_reps;
// The fewest cycles of g_reps passes of f() after one untimed pass.
template <class F>
double best(F&& f) {
  f();
  long long lo = -1;
  for (int r = 0; r < g_reps; r++) {
    const long long c0 = positions::cycles_read(g_fc);
    f();
    const long long c = positions::cycles_read(g_fc) - c0;
    if (lo < 0 || c < lo) lo = c;
  }
  return double(lo);
}

}  // namespace

int main(int argc, char** argv) {
  g_reps = argc > 1 ? atoi(argv[1]) : 3;
  rng.seed(argc > 2 ? uint64_t(atoll(argv[2])) : 1);
  const double scale = argc > 3 ? atof(argv[3]) : 1;
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // a performance core (macOS cannot pin)
#endif
  ddfast::init(nullptr);
  g_fc = positions::cycles_open();
  auto sets = [&](int base) { return std::max(2, int(base * scale)); };

  // 1. Positions.
  hash_start();
  printf("positions (k cycles):");
  for (int cards = 12; cards >= 6; cards--) {
    const auto ps = batches(13 - cards, 0, sets(cards >= 12 ? 6 : cards == 11 ? 12 : cards == 10 ? 25 : 60));
    std::vector<ddfast::Position> q(ps.size());
    for (size_t i = 0; i < ps.size(); i++) {
      memcpy(q[i].hand, ps[i].hand, sizeof q[i].hand);
      q[i].trump = uint8_t(ps[i].trump), q[i].leader = uint8_t(ps[i].leader);
    }
    std::vector<int> v(q.size());
    const double c = best([&] { for (size_t i = 0; i < q.size(); i++) v[i] = ddfast::solve_position(q[i]); });
    for (int x : v) mix(uint64_t(x));
    printf(" %d:%.0f", cards, c / double(q.size()) / 1e3);
  }
  hash_print();

  // 2. Cards.
  hash_start();
  printf("card_results at a lead (k cycles):");
  for (int cards = 11; cards >= 8; cards--) {
    const auto ps = batches(13 - cards, 0, sets(cards == 11 ? 4 : cards == 10 ? 8 : 20));
    std::vector<ddfast::PlayPosition> q(ps.size());
    for (size_t i = 0; i < ps.size(); i++) {
      q[i] = ddfast::PlayPosition{};
      memcpy(q[i].hand, ps[i].hand, sizeof q[i].hand);
      q[i].trump = uint8_t(ps[i].trump), q[i].leader = uint8_t(ps[i].leader), q[i].played = 0;
    }
    std::vector<ddfast::CardResult> out(13 * q.size());
    std::vector<int> n(q.size());
    const double c = best([&] { for (size_t i = 0; i < q.size(); i++) n[i] = ddfast::card_results(q[i], &out[13 * i]); });
    for (size_t i = 0; i < q.size(); i++)
      for (int j = 0; j < n[i]; j++) mix(uint64_t(out[13 * i + size_t(j)].ns_tricks) | uint64_t(out[13 * i + size_t(j)].card.rank) << 8);
    printf(" %d:%.0f", cards, c / double(q.size()) / 1e3);
  }
  hash_print();

  // 3. Boards.
#if HAVE_COMPAT
  ddfast_compat::ensure_init();
  for (int extra : {0, 2})
    for (int sol : {1, 3}) {
      hash_start();
      printf("boards, %s, solutions %d (k cycles):", extra ? "2 cards played" : "at a lead", sol);
      for (int cards = 11; cards >= 8; cards--) {
        rng.seed(1000u * unsigned(cards) + unsigned(extra));  // the same boards for both `solutions`
        const auto ps = batches(13 - cards, extra, sets(cards == 11 ? 4 : cards == 10 ? 8 : 20));
        std::vector<ddfast_compat::Future> f(ps.size());
        const double c = best([&] {
          for (size_t i = 0; i < ps.size(); i++) {
            unsigned remain[4][4];
            for (int h = 0; h < 4; h++)
              for (int s = 0; s < 4; s++) remain[h][s] = unsigned(ps[i].hand[h][s]) << 2;
            if (ddfast_compat::solve_board(remain, ps[i].trump, ps[i].leader, ps[i].ts, ps[i].tr, -1, sol, 1, f[i]) != 1) {
              fprintf(stderr, "bench_play_history: solve_board failed\n");
              exit(1);
            }
          }
        });
        for (const auto& x : f) {
          mix(uint64_t(sol == 1 ? 1 : x.cards));
          for (int j = 0; j < (sol == 1 ? 1 : x.cards); j++) mix(uint64_t(x.score[j]));
        }
        printf(" %d:%.0f", cards, c / double(ps.size()) / 1e3);
      }
      hash_print();
    }
#else
  printf("boards: no DDS-compatible layer at this commit\n");
#endif
}
