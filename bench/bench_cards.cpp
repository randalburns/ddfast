// Times ddfast::card_results and ddfast::analyse_play.
//
// usage: bench_cards [ROUNDS=3] [SCALE=1] [SEED=1] [DEALS=200]
//
// card_results: positions reached by random legal play from random deals (a random strain and
// leader), for 13 down to 5 cards a hand at the trick start and 0 to 3 cards played to the
// trick; SCALE times 300 (13 and 12 cards), 500, 1000, 2000, 3000 and 5000 (7 and fewer)
// positions a cell. analyse_play: DEALS whole deals played out, each card one of the best with
// chance 0, 90 and 100%, else any legal card. After one untimed pass, ROUNDS timed passes; the
// lowest cycle count is printed with the spread (highest over lowest). Also per cell: the
// heads a call (a head is the first of a run of equal cards) and how far the heads fall below
// the best card, and at the end a hash of every answer, to compare two builds. One thread
// (pin it: taskset -c 0 ...).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "ddfast.hpp"
#include "positions.hpp"

using ddfast::Card;
using ddfast::CardResult;
using ddfast::PlayPosition;

namespace {

std::mt19937 rng;

int to_play(const PlayPosition& p) { return (p.leader + p.played) & 3; }

Card random_card(const PlayPosition& p) {
  const uint16_t* h = p.hand[to_play(p)];
  Card c[13];
  int n = 0;
  const bool follow = p.played > 0 && h[p.trick[0].suit];
  for (int s = 0; s < 4; s++)
    if (!follow || s == p.trick[0].suit)
      for (int r = 0; r < 13; r++)
        if (h[s] >> r & 1) c[n++] = Card{uint8_t(s), uint8_t(r + 2)};
  return c[rng() % n];
}

void apply(PlayPosition& p, Card c) {
  p.hand[to_play(p)][c.suit] &= uint16_t(~(1u << (c.rank - 2)));
  if (p.played < 3) {
    p.trick[p.played++] = c;
    return;
  }
  const Card t[4] = {p.trick[0], p.trick[1], p.trick[2], c};
  int w = 0;
  for (int i = 1; i < 4; i++)
    if (t[i].suit == t[w].suit ? t[i].rank > t[w].rank : t[i].suit == p.trump) w = i;
  p.leader = uint8_t((p.leader + w) & 3), p.played = 0;
}

PlayPosition deal() {
  PlayPosition p{};
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  std::shuffle(deck, deck + 52, rng);
  for (int k = 0; k < 52; k++) p.hand[k % 4][deck[k] / 13] |= uint16_t(1u << (deck[k] % 13));
  p.trump = uint8_t(rng() % 5), p.leader = uint8_t(rng() % 4);
  return p;
}

struct Out {
  int n;
  CardResult c[13];
};

// Is c[i] the first of a run of equal cards? (No card of another hand or of the trick lies
// between it and the mover's next higher card of the suit.)
bool head(const PlayPosition& p, const Out& o, int i) {
  if (i == 0 || o.c[i - 1].card.suit != o.c[i].card.suit) return true;
  const int x = o.c[i].card.suit;
  unsigned others = 0;
  for (int q = 0; q < 4; q++)
    if (q != to_play(p)) others |= p.hand[q][x];
  for (int k = 0; k < p.played; k++)
    if (p.trick[k].suit == x) others |= 1u << (p.trick[k].rank - 2);
  for (int r = o.c[i].card.rank + 1; r < o.c[i - 1].card.rank; r++)
    if (others >> (r - 2) & 1) return true;
  return false;
}

struct Game {
  ddfast::Position start;
  Card seq[52];
};

uint64_t g_hash = 1469598103934665603ull;
void mix(const void* p, size_t n) {
  for (size_t i = 0; i < n; i++) g_hash = (g_hash ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
}

}  // namespace

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? atoi(argv[1]) : 3;
  const double scale = argc > 2 ? atof(argv[2]) : 1;
  rng.seed(argc > 3 ? unsigned(atol(argv[3])) : 1);
  const int deals = argc > 4 ? atoi(argv[4]) : 200;
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // a performance core (macOS cannot pin)
#endif
  ddfast::init();
  const int fc = positions::cycles_open();
  static const int count[14] = {0, 0, 0, 0, 0, 5000, 5000, 5000, 3000, 2000, 1000, 500, 300, 300};
  long calls = 0;
  printf("card_results: cycles per call (spread); heads a call; heads 0, 1, 2, 3+ tricks below the best\n");
  for (int size = 13; size >= 5 && scale > 0; size--)
    for (int played = 0; played < 4; played++) {
      const int n = std::max(10, int(count[size] * scale));
      std::vector<PlayPosition> ps(n);
      for (auto& p : ps) {
        p = deal();
        for (int i = 0; i < (13 - size) * 4 + played; i++) apply(p, random_card(p));
      }
      std::vector<Out> out(n);
      for (int i = 0; i < n; i++) ddfast::card_results(ps[i], out[i].c);  // untimed: the first pass over a set is slower
      long long lo = -1, hi = 0;
      for (int r = 0; r < rounds; r++) {
        for (auto& x : out) memset(&x, 0, sizeof x);
        const long long c0 = positions::cycles_read(fc);
        for (int i = 0; i < n; i++) out[i].n = ddfast::card_results(ps[i], out[i].c);
        const long long c = positions::cycles_read(fc) - c0;
        if (lo < 0 || c < lo) lo = c;
        hi = std::max(hi, c);
      }
      calls += n;
      long heads = 0, below[4] = {};
      for (int i = 0; i < n; i++) {
        const Out& o = out[i];
        mix(&o, sizeof o);
        const bool ns = (to_play(ps[i]) & 1) == 0;
        int best = o.c[0].ns_tricks;
        for (int j = 1; j < o.n; j++) best = ns ? std::max<int>(best, o.c[j].ns_tricks) : std::min<int>(best, o.c[j].ns_tricks);
        for (int j = 0; j < o.n; j++)
          if (head(ps[i], o, j)) heads++, below[std::min(3, abs(best - o.c[j].ns_tricks))]++;
      }
      printf("%2d cards, %d played, %4d positions: %9.0f (%4.1f%%); %5.2f heads; %4.1f %4.1f %4.1f %4.1f%%\n", size, played, n,
             double(lo) / n, 100.0 * (double(hi) / double(lo) - 1), double(heads) / n, 100.0 * below[0] / heads,
             100.0 * below[1] / heads, 100.0 * below[2] / heads, 100.0 * below[3] / heads);
      fflush(stdout);
    }

  long bad = 0;
  for (int pc = 0; pc <= 100 && deals > 0; pc += pc ? 10 : 90) {
    std::vector<Game> g(deals);
    for (auto& x : g) {
      PlayPosition p = deal();
      memcpy(x.start.hand, p.hand, sizeof p.hand);
      x.start.trump = p.trump, x.start.leader = p.leader;
      for (int i = 0; i < 52; i++) {
        Card c = random_card(p);
        if (int(rng() % 100) < pc) {
          CardResult cr[13];
          const int k = ddfast::card_results(p, cr);
          const bool ns = (to_play(p) & 1) == 0;
          int b = cr[0].ns_tricks, nb = 0;
          for (int j = 1; j < k; j++) b = ns ? std::max<int>(b, cr[j].ns_tricks) : std::min<int>(b, cr[j].ns_tricks);
          for (int j = 0; j < k; j++)
            if (cr[j].ns_tricks == b) cr[nb++] = cr[j];
          c = cr[rng() % nb].card;
        }
        x.seq[i] = c;
        apply(p, c);
      }
    }
    std::vector<uint8_t> out(53 * deals);
    for (int i = 0; i < deals; i++) ddfast::analyse_play(g[i].start, g[i].seq, 52, &out[53 * i]);
    long long lo = -1, hi = 0;
    for (int r = 0; r < rounds; r++) {
      std::fill(out.begin(), out.end(), uint8_t(255));
      const long long c0 = positions::cycles_read(fc);
      for (int i = 0; i < deals; i++)
        if (ddfast::analyse_play(g[i].start, g[i].seq, 52, &out[53 * i]) != 52) bad++;
      const long long c = positions::cycles_read(fc) - c0;
      if (lo < 0 || c < lo) lo = c;
      hi = std::max(hi, c);
    }
    mix(out.data(), out.size());
    long moves = 0;
    for (int i = 0; i < deals; i++)
      for (int j = 0; j < 52; j++) moves += out[53 * i + j] != out[53 * i + j + 1];
    printf("analyse_play, %d deals of 52 cards, %3d%% best cards (the total changes %.1f times a deal): %.0f cycles a deal (spread %.1f%%)\n",
           deals, pc, double(moves) / deals, double(lo) / deals, 100.0 * (double(hi) / double(lo) - 1));
    fflush(stdout);
  }
  printf("%ld card_results positions; hash of every answer %016llx%s\n", calls, (unsigned long long)g_hash, bad ? "  (analyse_play FAILED)" : "");
  return bad != 0;
}
