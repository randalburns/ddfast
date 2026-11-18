// The DDS-compatible layer (python/src/dds_compat.cpp) against DDS itself, on the same inputs:
//   compat_test [positions] [seed] [tables]
// Positions: random deals played out at random to every point of the play (any trick, 0..3
// cards into the current trick, down to the last trick), each with a random strain, solutions
// 1..3, target -1, 0 or 1..tricks left (+1 now and then), mode 0..2. Compared: the return code,
// `cards`, and every (card, equals, score) entry; with solutions 1 only the score must match
// exactly (the card may be another equally good one, which must reach the same score in DDS's
// solutions-2 answer). Then error inputs, then full tables and par text, and tables of hands
// with fewer than 13 cards.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

#include "dds_compat.hpp"
#include "ddfast.hpp"
#include <dll.h>

using namespace ddfast_compat;

namespace {
std::mt19937_64 rng;
int R(int n) { return int(rng() % uint64_t(n)); }

struct TPos {
  unsigned remain[4][4];  // DDS layout: rank r at bit r
  int trump, first, ts[3], tr[3];
  int played;
};

// A random deal played out at random for `cards` cards. revoke: followers in the current (last,
// unfinished) trick may play off suit while holding the suit led (DDS accepts such positions).
TPos random_position(int cards, bool revoke = false) {
  int deck[52];
  for (int i = 0; i < 52; i++) deck[i] = i;
  std::shuffle(deck, deck + 52, rng);
  TPos p{};
  for (int i = 0; i < 52; i++) p.remain[i / 13][deck[i] / 13] |= 1u << (deck[i] % 13 + 2);
  p.trump = R(5);
  int leader = R(4), played = 0, ts[4], tr[4];
  for (int c = 0; c < cards; c++) {
    const int seat = (leader + played) & 3;
    int s;
    const bool last_trick = c >= cards - cards % 4;
    if (played > 0 && p.remain[seat][ts[0]] && !(revoke && last_trick && R(2))) s = ts[0];
    else do s = R(4); while (!p.remain[seat][s]);
    unsigned h = p.remain[seat][s];
    int k = R(__builtin_popcount(h));
    while (k--) h &= h - 1;
    const int r = __builtin_ctz(h);
    p.remain[seat][s] &= ~(1u << r);
    ts[played] = s, tr[played] = r;
    if (++played == 4) {  // the trick is over: its winner leads
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
  p.played = played;
  for (int k = 0; k < 3; k++) p.ts[k] = k < played ? ts[k] : 0, p.tr[k] = k < played ? tr[k] : 0;
  return p;
}

Deal to_dds(const TPos& p) {
  Deal d{};
  d.trump = p.trump;
  d.first = p.first;
  for (int k = 0; k < 3; k++) d.currentTrickSuit[k] = p.ts[k], d.currentTrickRank[k] = p.tr[k];
  std::memcpy(d.remainCards, p.remain, sizeof d.remainCards);
  return d;
}

using Entry = std::tuple<int, int, int, int>;  // score, suit, rank, equals
std::vector<Entry> entries(const int* suit, const int* rank, const int* eq, const int* score, int n) {
  std::vector<Entry> v;
  for (int i = 0; i < n; i++) v.emplace_back(score[i], suit[i], rank[i], eq[i]);
  std::sort(v.begin(), v.end());
  return v;
}

int fails = 0;
void fail(const char* what, const TPos& p, int target, int sol, int mode, const FutureTricks& a, const Future& b, int rc) {
  if (++fails > 10) return;
  fprintf(stderr, "MISMATCH %s rc %d: trump %d first %d played %d trick (%d,%d)(%d,%d)(%d,%d) target %d sol %d mode %d\n",
          what, rc, p.trump, p.first, p.played, p.ts[0], p.tr[0], p.ts[1], p.tr[1], p.ts[2], p.tr[2], target, sol, mode);
  for (int h = 0; h < 4; h++) fprintf(stderr, "  hand %d: %04x %04x %04x %04x\n", h, p.remain[h][0], p.remain[h][1], p.remain[h][2], p.remain[h][3]);
  fprintf(stderr, "  DDS    cards %d:", a.cards);
  for (int i = 0; i < 13; i++) fprintf(stderr, " %d%c/%x=%d", a.suit[i], "xx23456789TJQKA"[a.rank[i]], a.equals[i], a.score[i]);
  fprintf(stderr, "\n  ddfast cards %d:", b.cards);
  for (int i = 0; i < 13; i++) fprintf(stderr, " %d%c/%x=%d", b.suit[i], "xx23456789TJQKA"[b.rank[i]], b.equals[i], b.score[i]);
  fprintf(stderr, "\n");
}

int same_card_1 = 0, total_1 = 0;

void compare(const TPos& p, int target, int sol, int mode) {
  const Deal d = to_dds(p);
  FutureTricks a{};
  const int ra = SolveBoard(d, target, sol, mode, &a, 0);
  Future b;
  const int rb = solve_board(p.remain, p.trump, p.first, p.ts, p.tr, target, sol, mode, b);
  if (ra != rb) return fail("return code", p, target, sol, mode, a, b, rb);
  if (ra != RETURN_NO_FAULT) return;
  if (a.cards != b.cards) return fail("cards", p, target, sol, mode, a, b, rb);
  // Target 0, or every move scoring 0: DDS lists every move with score 0 (beyond `cards` too),
  // in its move generator's order: compare as a set.
  const bool zero_fill = (target == 0 && sol < 3) || (target == -1 && sol < 3 && a.score[0] == 0);
  if (sol == 1 && a.cards == 1 && !zero_fill) {
    total_1++;
    if (a.score[0] != b.score[0]) return fail("solutions 1 score", p, target, sol, mode, a, b, rb);
    if (a.suit[0] == b.suit[0] && a.rank[0] == b.rank[0]) {
      same_card_1++;
      if (a.equals[0] != b.equals[0]) return fail("solutions 1 equals", p, target, sol, mode, a, b, rb);
      return;
    }
    if (b.score[0] < 0) return fail("solutions 1 card (special case)", p, target, sol, mode, a, b, rb);
    // Another card: DDS's solutions 2 must list it with the same score and equals.
    FutureTricks a2{};
    SolveBoard(d, target == 0 ? -1 : target, 2, mode == 0 ? 1 : mode, &a2, 0);
    for (int i = 0; i < a2.cards; i++)
      if (a2.suit[i] == b.suit[0] && a2.rank[i] == b.rank[0] && a2.equals[i] == b.equals[0] &&
          (target == 0 || a2.score[i] == b.score[0]))
        return;
    return fail("solutions 1 card not among DDS's best", p, target, sol, mode, a, b, rb);
  }
  // Every filled entry, as a set (equal scores may come in another order).
  const int n = std::max(a.cards, 0);
  int fill = n;
  if (n == 0) fill = 1;  // score[0] carries the "not reached" value
  if (zero_fill) fill = 13;
  if (entries(a.suit, a.rank, a.equals, a.score, fill) != entries(b.suit, b.rank, b.equals, b.score, fill))
    return fail("entries", p, target, sol, mode, a, b, rb);
  for (int i = 1; i < n && sol == 3; i++)
    if (b.score[i] > b.score[i - 1]) return fail("solutions 3 order", p, target, sol, mode, a, b, rb);
}

void error_cases() {
  TPos p = random_position(0);
  auto both = [&](const char* what, TPos q, int target, int sol, int mode) {
    const Deal d = to_dds(q);
    FutureTricks a{};
    Future b;
    const int ra = SolveBoard(d, target, sol, mode, &a, 0);
    const int rb = solve_board(q.remain, q.trump, q.first, q.ts, q.tr, target, sol, mode, b);
    if (ra != rb) {
      fails++;
      fprintf(stderr, "MISMATCH error case %s: DDS %d, ddfast %d\n", what, ra, rb);
    }
  };
  both("target -2", p, -2, 1, 0);
  both("target 14", p, 14, 1, 0);
  both("solutions 0", p, -1, 0, 0);
  both("solutions 4", p, -1, 4, 0);
  both("mode 3", p, -1, 1, 3);
  TPos q = p; q.trump = 5; both("trump 5", q, -1, 1, 0);
  q = p; q.first = 4; both("first 4", q, -1, 1, 0);
  q = p; q.tr[1] = 5; q.ts[1] = 0; both("trick hole", q, -1, 1, 0);
  q = p; q.tr[0] = 15; both("trick rank 15", q, -1, 1, 0);
  q = p; { unsigned c = q.remain[1][0] & -q.remain[1][0]; q.remain[0][0] |= c; q.remain[1][0] &= ~c; } both("card count", q, -1, 1, 0);
  q = p; { unsigned c = q.remain[1][0] & -q.remain[1][0]; q.remain[0][0] |= c; } both("duplicate", q, -1, 1, 0);
  q = p; std::memset(q.remain, 0, sizeof q.remain); both("zero cards", q, -1, 1, 0);
  TPos m = random_position(17);  // 35 cards: 1 card into the trick
  both("target too high", m, 13, 1, 0);
  q = m; { const int h = (q.first + 1) & 3; unsigned c = q.remain[h][0] ? q.remain[h][0] & -q.remain[h][0] : 0;
    if (c) { q.ts[0] = 0; q.tr[0] = __builtin_ctz(c); both("played card still held", q, -1, 1, 0); } }
  Future b;
  if (solve_board_pbn("N:N:AKQJ.AKQ.AKQ.AKQ", 4, 0, p.ts, p.tr, -1, 1, 0, b) != RETURN_PBN_FAULT) {
    fails++;
    fprintf(stderr, "MISMATCH: 'N:N:...' should be a PBN fault\n");
  }
}

void tables(int ntab) {
  struct Hands { uint16_t h[4][4]; };
  std::vector<Hands> hands(static_cast<size_t>(ntab) + 20);
  DdTableDeals dd{};
  int filter[5] = {0, 0, 0, 0, 0};
  for (int base = 0; base < ntab; base += 40) {
    const int m = std::min(40, ntab - base);
    dd.no_of_tables = m;
    for (int k = 0; k < m; k++) {
      TPos p = random_position(0);
      for (int h = 0; h < 4; h++)
        for (int s = 0; s < 4; s++) dd.deals[k].cards[h][s] = p.remain[h][s], hands[size_t(base + k)].h[h][s] = uint16_t(p.remain[h][s] >> 2);
    }
    const int vul = R(4);
    DdTablesRes ra{};
    AllParResults pa{};
    if (CalcAllTables(&dd, vul, filter, &ra, &pa) != RETURN_NO_FAULT) { fails++; fprintf(stderr, "DDS CalcAllTables failed\n"); return; }
    std::vector<DdTableResults> rb(static_cast<size_t>(m));
    calc_tables(&hands[size_t(base)].h, size_t(m), filter, 0, rb.data());
    for (int k = 0; k < m; k++) {
      if (std::memcmp(ra.results[k].res_table, rb[size_t(k)].res_table, sizeof ra.results[k].res_table)) {
        fails++;
        fprintf(stderr, "MISMATCH table %d\n", base + k);
      }
      ParResults pb{};
      par(rb[size_t(k)], vul, pb);
      if (std::memcmp(pa.par_results[k].par_score, pb.par_score, sizeof pb.par_score) ||
          std::memcmp(pa.par_results[k].par_contracts_string, pb.par_contracts_string, sizeof pb.par_contracts_string)) {
        fails++;
        fprintf(stderr, "MISMATCH par %d: DDS '%s' '%s' / '%s' '%s'\n", base + k, pa.par_results[k].par_score[0],
                pa.par_results[k].par_contracts_string[0], pb.par_score[0], pb.par_contracts_string[0]);
      }
    }
  }
  // Hands of fewer than 13 cards (whole tricks played out).
  for (int k = 0; k < 20; k++) {
    TPos p = random_position(4 * (1 + R(11)));
    DdTableDeal one{};
    uint16_t h[4][4];
    for (int q = 0; q < 4; q++)
      for (int s = 0; s < 4; s++) one.cards[q][s] = p.remain[q][s], h[q][s] = uint16_t(p.remain[q][s] >> 2);
    DdTableResults ra{}, rb{};
    if (CalcDDtable(one, &ra) != RETURN_NO_FAULT) { fails++; fprintf(stderr, "DDS CalcDDtable failed\n"); continue; }
    calc_tables(&h, 1, filter, 1, &rb);
    if (std::memcmp(ra.res_table, rb.res_table, sizeof ra.res_table)) {
      fails++;
      fprintf(stderr, "MISMATCH small table %d\n", k);
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  const int npos = argc > 1 ? atoi(argv[1]) : 2000;
  rng.seed(argc > 2 ? strtoull(argv[2], nullptr, 10) : 1);
  const int ntab = argc > 3 ? atoi(argv[3]) : 80;
  SetMaxThreads(1);
  ensure_init();
  size_t checks = 0;
  for (int i = 0; i < npos; i++) {
    const int cards = R(52);  // cards already played
    TPos p = random_position(cards, R(4) == 0);
    const int left = 13 - cards / 4;
    for (int sol = 1; sol <= 3; sol++) {
      const int mode = R(3);
      const int tg = R(4);
      const int target = tg == 0 ? -1 : tg == 1 ? 0 : (R(10) == 0 ? left + 1 : 1 + R(left));
      compare(p, target, sol, mode);
      compare(p, -1, sol, mode);
      checks += 2;
    }
  }
  error_cases();
  tables(ntab);
  printf("%zu solve_board checks on %d positions, %d tables (+20 small): %s\n", checks, npos, ntab,
         fails ? "MISMATCHES" : "all agree with DDS");
  printf("solutions 1: same card as DDS in %d of %d (the others are equally good cards)\n", same_card_1, total_1);
  return fails != 0;
}
