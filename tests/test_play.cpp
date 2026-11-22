// Checks ddfast's play interface (solve_play, at_least, card_results, analyse_play) against
// DDS on positions reached by random legal play from random deals: every stage of the deal
// and every point within a trick.
//
// usage: test_play [deals] [seed] [--no-bounds] [--no-cutoff]   (the flags: ddfast::set_options)
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "ddfast.hpp"
#include "dll.h"

using ddfast::analyse_play;
using ddfast::at_least;
using ddfast::card_results;
using ddfast::CardResult;
using ddfast::PlayPosition;
using ddfast::Position;
using ddfast::solve_play;
using DCard = ddfast::Card;  // DDS has its own Card and Deal

namespace {

std::mt19937 rng;

// DDS on a play position: tricks for the side to play (score) per card, or the best (sol 1).
FutureTricks dds(const PlayPosition& p, int sol) {
  Deal dl{};
  dl.trump = p.trump;
  dl.first = p.leader;
  for (int i = 0; i < p.played; i++) dl.currentTrickSuit[i] = p.trick[i].suit, dl.currentTrickRank[i] = p.trick[i].rank;
  for (int q = 0; q < 4; q++)
    for (int s = 0; s < 4; s++) dl.remainCards[q][s] = unsigned(p.hand[q][s]) << 2;
  FutureTricks f{};
  if (SolveBoard(dl, -1, sol, 1, &f, 0) != 1) {  // mode 1: search even with one legal card
    fprintf(stderr, "DDS SolveBoard failed\n");
    exit(1);
  }
  return f;
}

int to_play(const PlayPosition& p) { return (p.leader + p.played) & 3; }
int left(const PlayPosition& p) {
  int k = 0;
  for (int s = 0; s < 4; s++) k += __builtin_popcount(p.hand[to_play(p)][s]);
  return k;
}
// DDS's score (side to play) as North-South's tricks.
int ns_of(const PlayPosition& p, int score) { return (to_play(p) & 1) ? left(p) - score : score; }

// A random legal card of the seat to play.
DCard random_card(const PlayPosition& p) {
  const uint16_t* h = p.hand[to_play(p)];
  std::vector<DCard> c;
  const bool follow = p.played > 0 && h[p.trick[0].suit];
  for (int s = 0; s < 4; s++)
    if (!follow || s == p.trick[0].suit)
      for (int r = 0; r < 13; r++)
        if (h[s] >> r & 1) c.push_back(DCard{uint8_t(s), uint8_t(r + 2)});
  return c[rng() % c.size()];
}

// Plays c; returns the trick's winner when it completes it, else -1.
int apply(PlayPosition& p, DCard c) {
  p.hand[to_play(p)][c.suit] &= uint16_t(~(1u << (c.rank - 2)));
  if (p.played < 3) {
    p.trick[p.played++] = c;
    return -1;
  }
  DCard t[4] = {p.trick[0], p.trick[1], p.trick[2], c};
  int w = 0;
  for (int i = 1; i < 4; i++)
    if (t[i].suit == t[w].suit ? t[i].rank > t[w].rank : t[i].suit == p.trump) w = i;
  p.leader = uint8_t((p.leader + w) & 3), p.played = 0;
  return p.leader;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<const char*> args;
  ddfast::Options opts;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-bounds")) opts.sure_trick_bounds = false;
    else if (!strcmp(argv[i], "--no-cutoff")) opts.cutoff_cards = false;
    else args.push_back(argv[i]);
  }
  const int deals = args.size() > 0 ? atoi(args[0]) : 200;
  rng.seed(args.size() > 1 ? unsigned(atol(args[1])) : 1);
  ddfast::set_options(opts);
  ddfast::init();
  SetMaxThreads(1);
  long checked = 0, cards = 0, wrong = 0, pars = 0;
  for (int d = 0; d < deals; d++) {
    PlayPosition p{};
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    std::shuffle(deck, deck + 52, rng);
    for (int k = 0; k < 52; k++) p.hand[k % 4][deck[k] / 13] |= uint16_t(1u << (deck[k] % 13));
    p.trump = uint8_t(rng() % 5), p.leader = uint8_t(rng() % 4);
    const Position start{{{p.hand[0][0], p.hand[0][1], p.hand[0][2], p.hand[0][3]},
                          {p.hand[1][0], p.hand[1][1], p.hand[1][2], p.hand[1][3]},
                          {p.hand[2][0], p.hand[2][1], p.hand[2][2], p.hand[2][3]},
                          {p.hand[3][0], p.hand[3][1], p.hand[3][2], p.hand[3][3]}},
                         p.trump, p.leader};
    // Random play to a random point: 0..12 complete tricks, then 0..3 cards.
    const int stop = int(rng() % 52);
    std::vector<DCard> seq;
    std::vector<int> want(1, 0);  // North-South's total after each card, from DDS
    int won = 0;
    want[0] = ns_of(p, dds(p, 1).score[0]);
    for (int i = 0; i < 52; i++) {
      const DCard c = random_card(p);
      seq.push_back(c);
      const int w = apply(p, c);
      if (w >= 0 && (w & 1) == 0) won++;
      want.push_back(left(p) > 0 ? won + ns_of(p, dds(p, 1).score[0]) : won);
      if (i < stop) continue;
      if (left(p) == 0) break;
      // The position after `stop` cards: value, target questions and every card.
      const int v = ns_of(p, dds(p, 1).score[0]);
      const int got = solve_play(p);
      if (got != v) wrong++, fprintf(stderr, "deal %d card %d: solve_play %d, DDS %d\n", d, i, got, v);
      if (at_least(p, v) != 1 || at_least(p, v + 1) != 0) wrong++, fprintf(stderr, "deal %d card %d: at_least wrong around %d\n", d, i, v);
      const FutureTricks f = dds(p, 3);
      int want_card[4][13];
      memset(want_card, -1, sizeof want_card);
      for (int j = 0; j < f.cards; j++) {
        want_card[f.suit[j]][f.rank[j] - 2] = ns_of(p, f.score[j]);
        for (int r = 0; r < 13; r++)
          if ((f.equals[j] >> 2) >> r & 1) want_card[f.suit[j]][r] = ns_of(p, f.score[j]);
      }
      CardResult cr[13];
      const int nc = card_results(p, cr);
      int dds_cards = 0;
      for (int s = 0; s < 4; s++)
        for (int r = 0; r < 13; r++) dds_cards += want_card[s][r] >= 0;
      if (nc != dds_cards) wrong++, fprintf(stderr, "deal %d card %d: %d cards, DDS %d\n", d, i, nc, dds_cards);
      for (int j = 0; j < nc; j++) {
        cards++;
        if (want_card[cr[j].card.suit][cr[j].card.rank - 2] != cr[j].ns_tricks)
          wrong++, fprintf(stderr, "deal %d card %d: card %d/%d gives %d, DDS %d\n", d, i, cr[j].card.suit, cr[j].card.rank,
                           cr[j].ns_tricks, want_card[cr[j].card.suit][cr[j].card.rank - 2]);
      }
      checked++;
      break;
    }
    // Par from the deal's table, against DDS's DealerPar, for every dealer and vulnerability.
    {
      ddfast::Deal dd;
      memcpy(dd.hand, start.hand, sizeof dd.hand);
      ddfast::Table t;
      ddfast::solve_table(dd, t);
      DdTableResults tr{};
      for (int x = 0; x < 5; x++)
        for (int q = 0; q < 4; q++) tr.res_table[x][q] = t.tricks[x][q];
      for (int dealer = 0; dealer < 4; dealer++)
        for (int vul = 0; vul < 4; vul++) {
          ParResultsDealer pr{};
          if (DealerPar(&tr, &pr, dealer, vul) != 1) { fprintf(stderr, "DDS DealerPar failed\n"); exit(1); }
          ddfast::ParResult mine;
          ddfast::par(t, dealer, vul, mine);
          const int want_ns = pr.score;  // DDS gives it from North-South's side for every dealer
          pars++;
          if (mine.score != want_ns) {
            wrong++;
            fprintf(stderr, "deal %d dealer %d vul %d: par %d, DDS %d (%s)\n", d, dealer, vul, mine.score, want_ns, pr.number ? pr.contracts[0] : "");
          }
        }
    }
    // Play analysis of the whole random sequence so far.
    std::vector<uint8_t> got(seq.size() + 1);
    const int np = analyse_play(start, seq.data(), int(seq.size()), got.data());
    if (np != int(seq.size())) wrong++, fprintf(stderr, "deal %d: analyse_play stopped at %d of %zu\n", d, np, seq.size());
    for (size_t i = 0; i <= seq.size() && i < want.size(); i++)
      if (got[i] != want[i]) {
        wrong++, fprintf(stderr, "deal %d: analyse_play after %zu cards %d, DDS %d\n", d, i, got[i], want[i]);
        break;
      }
    if (wrong > 20) break;
  }
  printf("%ld positions, %ld card results, %d deals analysed, %ld par scores: %s\n", checked, cards, deals, pars, wrong ? "WRONG" : "all agree with DDS");
  return wrong ? 1 : 0;
}
