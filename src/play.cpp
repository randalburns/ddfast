// Play: positions in the middle of a trick, card-by-card results, play analysis and target
// questions (see ddfast.hpp). Deliberately simple: the cards still to come in the current
// trick are searched one by one (equal cards once), and every completed trick is handed to
// the trick-start solver, whose table is kept across the whole call (the positions are all
// from one deal and strain).
#include "ddfast.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstring>

namespace ddfast {
namespace {

struct Play {
  uint16_t h[4][4];  // cards not yet played
  int trump, leader, played;
  Card tr[4];
  bool keep;         // the solver's table holds for this deal and strain
};

inline uint16_t bit(const Card& c) { return uint16_t(1u << (c.rank - 2)); }
inline int count(const uint16_t h[4]) {
  return __builtin_popcount(h[0]) + __builtin_popcount(h[1]) + __builtin_popcount(h[2]) + __builtin_popcount(h[3]);
}
inline int to_play(const Play& s) { return (s.leader + s.played) & 3; }
// Tricks still to be played, the current one included.
inline int tricks_left(const Play& s) { return count(s.h[to_play(s)]); }

// The seat that wins a complete trick.
int winner(const Play& s) {
  int w = 0;
  for (int i = 1; i < 4; i++) {
    const Card &c = s.tr[i], &b = s.tr[w];
    if (c.suit == b.suit ? c.rank > b.rank : c.suit == s.trump) w = i;
  }
  return (s.leader + w) & 3;
}

// The cards the seat to play may play, as suit masks.
void legal(const Play& s, uint16_t out[4]) {
  const uint16_t* h = s.h[to_play(s)];
  memcpy(out, h, 4 * sizeof(uint16_t));
  if (s.played > 0 && h[s.tr[0].suit]) {
    for (int x = 0; x < 4; x++) out[x] = 0;
    out[s.tr[0].suit] = h[s.tr[0].suit];
  }
}

// One card per run of equal cards in mask (cards of the seat to play in suit x): two cards are
// equal when no card still in play (in a hand or on the table) lies between them.
uint16_t heads(const Play& s, int x, uint16_t mask) {
  uint16_t present = uint16_t(s.h[0][x] | s.h[1][x] | s.h[2][x] | s.h[3][x]);
  for (int i = 0; i < s.played; i++)
    if (s.tr[i].suit == x) present = uint16_t(present | bit(s.tr[i]));
  uint16_t out = 0;
  bool prev_mine = false;
  for (int r = 12; r >= 0; r--) {
    if (!(present >> r & 1)) continue;
    const bool mine = mask >> r & 1;
    if (mine && !prev_mine) out = uint16_t(out | 1u << r);
    prev_mine = mine;
  }
  return out;
}

ddcore::Position trick_start(const Play& s) {
  ddcore::Position q;
  memcpy(q.hand, s.h, sizeof q.hand);
  q.trump = uint8_t(s.trump);
  q.leader = uint8_t(s.leader);
  return q;
}

// North-South's tricks from a trick start (all hands the same size, maybe empty).
int value_start(Play& s) {
  if (count(s.h[0]) == 0) return 0;
  const int v = detail::solve_position(trick_start(s), s.keep);
  s.keep = true;
  return v;
}

bool at_least_start(Play& s, int t) {
  const int n = count(s.h[0]);
  if (t <= 0) return true;
  if (t > n) return false;
  const bool r = detail::query_position(trick_start(s), t, s.keep);
  s.keep = true;
  return r;
}

// Plays card c (of the seat to play). When it completes the trick, the next trick starts and
// the return value is 1 if North-South won the trick, else 0; -1 if the trick goes on.
int play(Play& s, Card c) {
  s.h[to_play(s)][c.suit] = uint16_t(s.h[to_play(s)][c.suit] & ~bit(c));
  s.tr[s.played++] = c;
  if (s.played < 4) return -1;
  const int w = winner(s);
  s.leader = w, s.played = 0;
  return (w & 1) == 0;
}

// Do North-South take at least t tricks from s on (the current trick included)?
bool at_least_play(Play& s, int t) {
  if (t <= 0) return true;
  if (t > tricks_left(s)) return false;
  if (s.played == 0) return at_least_start(s, t);
  const bool ns = (to_play(s) & 1) == 0;
  uint16_t lg[4];
  legal(s, lg);
  for (int x = 0; x < 4; x++)
    for (uint16_t m = heads(s, x, lg[x]); m; m = uint16_t(m & (m - 1))) {
      Play c = s;
      const int won = play(c, Card{uint8_t(x), uint8_t(__builtin_ctz(m) + 2)});
      const bool r = won < 0 ? at_least_play(c, t) : at_least_start(c, t - won);
      s.keep = c.keep;
      if (r == ns) return ns;  // North-South found a way, or East-West did
    }
  return !ns;
}

// North-South's tricks from s on, the current trick included.
int value_play(Play& s) {
  if (s.played == 0) return value_start(s);
  int lo = 0, hi = tricks_left(s);
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (at_least_play(s, mid)) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

// A kept table from another deal pays only below this many cards a hand. Worlds that share one
// or two hands (a player's samples of the hidden hands) rarely meet the same patterns again at
// 12 and 13 cards, and a fuller table then costs more than it saves: with the table kept at
// every size, the single-dummy-bridge harness's batches of 32 worlds took 16.2 s at 13 cards and
// 8.4 s at 12; not kept, 15.0 and 8.1 s. From 11 cards down keeping wins (3.12 against 3.34 s
// at 11). Thresholds 9 to 13 tried there; 12 was best (docs/NOTES.md).
#ifndef KEEP_OTHER_DEAL_BELOW
#define KEEP_OTHER_DEAL_BELOW 12
#endif

// The cards each seat held at the previous call on this thread (its hand and its card on the
// table): a position is of the same deal exactly when every seat holds a subset of them.
thread_local uint16_t g_prev[4][4];
thread_local bool g_prev_set = false;

// Checks p and fills s; false if p is not a valid position.
bool load(const PlayPosition& p, bool keep, Play& s) {
  if (p.trump > 4 || p.leader > 3 || p.played > 3) return false;
  uint16_t seen[4] = {0, 0, 0, 0};
  for (int q = 0; q < 4; q++)
    for (int x = 0; x < 4; x++) {
      if ((p.hand[q][x] & ~0x1FFFu) || (p.hand[q][x] & seen[x])) return false;
      seen[x] = uint16_t(seen[x] | p.hand[q][x]);
    }
  for (int i = 0; i < p.played; i++) {
    const Card& c = p.trick[i];
    if (c.suit > 3 || c.rank < 2 || c.rank > 14 || (seen[c.suit] & bit(c))) return false;
    seen[c.suit] = uint16_t(seen[c.suit] | bit(c));
    // A follower who did not follow suit holds none of the suit led.
    if (i > 0 && c.suit != p.trick[0].suit && p.hand[(p.leader + i) & 3][p.trick[0].suit]) return false;
  }
  const int k = count(p.hand[(p.leader + p.played) & 3]);  // tricks left, this one included
  if (k < 1 || k > 13) return false;
  for (int i = 0; i < 4; i++)
    if (count(p.hand[(p.leader + i) & 3]) != (i < p.played ? k - 1 : k)) return false;
  memcpy(s.h, p.hand, sizeof s.h);
  s.trump = p.trump, s.leader = p.leader, s.played = p.played;
  for (int i = 0; i < p.played; i++) s.tr[i] = p.trick[i];
  uint16_t held[4][4];
  memcpy(held, p.hand, sizeof held);
  for (int i = 0; i < p.played; i++) {
    uint16_t& x = held[(p.leader + i) & 3][p.trick[i].suit];
    x = uint16_t(x | bit(p.trick[i]));
  }
  bool same_deal = g_prev_set;
  for (int q = 0; q < 4 && same_deal; q++)
    for (int x = 0; x < 4; x++)
      if (held[q][x] & ~g_prev[q][x]) { same_deal = false; break; }
  memcpy(g_prev, held, sizeof held);
  g_prev_set = true;
  s.keep = keep && (same_deal || k < KEEP_OTHER_DEAL_BELOW);
  return true;
}

}  // namespace

int solve_play(const PlayPosition& p, bool keep_table) {
  Play s;
  if (!load(p, keep_table, s)) return -1;
  return value_play(s);
}

int at_least(const PlayPosition& p, int target, bool keep_table) {
  Play s;
  if (!load(p, keep_table, s)) return -1;
  return at_least_play(s, target) ? 1 : 0;
}

// Every card's result. A full solve per card starts each from a fresh guess (a trick start) or
// halves the range (in the middle of a trick). The cards' results lie close together, so
// (docs/NOTES.md, round 20):
// - at a lead, the position's value first. No card scores more for the side to play, and two
//   in three score exactly that, so each card is asked "at least that?" and one less until yes.
// - with one or two cards played, the first card is solved and each later card is asked about
//   the best score so far: one more until no, or one less until yes. (The position's value
//   would cost as much as the cards' questions here.)
// - with three cards played, a full solve per card: each is a trick start, which the solver
//   guesses well by itself.
int card_results(const PlayPosition& p, CardResult* out, bool keep_table) {
  Play s;
  if (!load(p, keep_table, s)) return -1;
  const bool ns = (to_play(s) & 1) == 0;
  const int left = tricks_left(s);
  // Scores below are the side to play's tricks from s on; v is North-South's.
  auto side = [&](int v) { return ns ? v : left - v; };
  // North-South's tricks when cd is played: a full solve.
  auto solve = [&](Card cd) {
    Play c = s;
    const int won = play(c, cd);
    const int v = won < 0 ? value_play(c) : won + value_start(c);
    s.keep = c.keep;
    return v;
  };
  // Does the side to play take at least sc tricks when it plays cd? For East-West that is
  // North-South not taking left - sc + 1.
  auto reaches = [&](Card cd, int sc) {
    Play c = s;
    const int won = play(c, cd);
    const int t = ns ? sc : left - sc + 1;
    const bool r = won < 0 ? at_least_play(c, t) : at_least_start(c, t - won);
    s.keep = c.keep;
    return r == ns;
  };
  const bool each = s.played == 3;
  const bool lead = s.played == 0;
  int best = -1;  // at a lead the best any card scores; otherwise the best so far (-1: none yet)
  if (lead) best = side(value_start(s));
  uint16_t lg[4];
  legal(s, lg);
  int n = 0;
  for (int x = 0; x < 4; x++) {
    const uint16_t hd = heads(s, x, lg[x]);
    int v = 0;
    for (int r = 12; r >= 0; r--) {
      if (!(lg[x] >> r & 1)) continue;
      const Card cd{uint8_t(x), uint8_t(r + 2)};
      if (hd >> r & 1) {  // the head of a run of equal cards: solve; the rest of the run shares it
        if (each || best < 0) {
          v = solve(cd);
          if (!each) best = side(v);
        } else {
          int sc = best;
          if (!reaches(cd, sc)) {
            sc--;
            while (sc > 0 && !reaches(cd, sc)) sc--;
          } else if (!lead) {
            while (sc < left && reaches(cd, sc + 1)) sc++;
            best = sc;
          }
          v = side(sc);
        }
      }
      out[n++] = CardResult{cd, uint8_t(v)};
    }
  }
  return n;
}

// After each card the total can only move against the side that played it, and mostly does not
// move, so each step asks about the previous total, and one less (North-South played) or one
// more (East-West played) until the answer changes: mostly one question in place of a full
// solve.
int analyse_play(const Position& start, const Card* cards, int n, uint8_t* ns_tricks) {
  PlayPosition p{};
  memcpy(p.hand, start.hand, sizeof p.hand);
  p.trump = start.trump, p.leader = start.leader, p.played = 0;
  Play s;
  if (!load(p, false, s) || count(s.h[0]) != count(s.h[1])) return 0;
  int won = 0;
  ns_tricks[0] = uint8_t(value_play(s));
  for (int i = 0; i < n; i++) {
    const Card c = cards[i];
    uint16_t lg[4];
    legal(s, lg);
    if (c.suit > 3 || c.rank < 2 || c.rank > 14 || !(lg[c.suit] & bit(c))) return i;
    const bool ns = (to_play(s) & 1) == 0;
    const int w = play(s, c);
    if (w > 0) won++;
    // at_least_play answers yes at 0 or fewer and no beyond the tricks left, so both walks end.
    int t = ns_tricks[i];
    if (ns) {
      while (!at_least_play(s, t - won)) t--;
    } else {
      while (at_least_play(s, t + 1 - won)) t++;
    }
    ns_tricks[i + 1] = uint8_t(t);
  }
  return n;
}

// ---------------------------------------------------------------------------------------
// Par: the auction as a game between the two sides (see ddfast.hpp).
namespace {

// The declaring side's score for a contract of `level` in `strain` taking `tricks`.
// Contracts that make are undoubled; contracts that go down are doubled.
int contract_score(int level, int strain, int tricks, bool vul) {
  const int need = level + 6;
  if (tricks < need) {
    const int down = need - tricks;
    if (vul) return -(200 + 300 * (down - 1));
    return -(down == 1 ? 100 : down == 2 ? 300 : 500 + 300 * (down - 3));
  }
  const int per = strain == 4 ? 30 : (strain == 2 || strain == 3) ? 20 : 30;  // 2, 3: diamonds, clubs
  const int points = per * level + (strain == 4 ? 10 : 0);
  int score = points + (points >= 100 ? (vul ? 500 : 300) : 50);
  if (level == 6) score += vul ? 750 : 500;
  if (level == 7) score += vul ? 1500 : 1000;
  return score + per * (tricks - need);
}

struct Auction {
  int tricks[2][5];  // per side and strain, the better declarer's tricks
  bool vul[2];
  int memo[36][2][2][2];  // [rank][side holding it][side to act][last call was a pass]
  bool seen[36][2][2][2];
  bool visited[36][2][2][2];  // finals() has been here
  // Bid rank r = 1..35 -> level and strain (clubs, diamonds, hearts, spades, no trumps).
  static int level_of(int r) { return (r - 1) / 5 + 1; }
  static int strain_of(int r) { static const int s[5] = {3, 2, 1, 0, 4}; return s[(r - 1) % 5]; }
  int ns_value(int r, int side) const {
    if (r == 0) return 0;
    const int v = contract_score(level_of(r), strain_of(r), tricks[side][strain_of(r)], vul[side]);
    return side == 0 ? v : -v;
  }
  // North-South's result with best bidding from here.
  int value(int r, int side, int turn, bool passed) {
    int& m = memo[r][side][turn][passed];
    if (seen[r][side][turn][passed]) return m;
    int best = passed ? ns_value(r, side) : value(r, side, turn ^ 1, true);  // pass
    for (int r2 = r + 1; r2 <= 35; r2++) {
      const int v = value(r2, turn, turn ^ 1, false);
      best = turn == 0 ? std::max(best, v) : std::min(best, v);
    }
    seen[r][side][turn][passed] = true;
    return m = best;
  }
  // The contracts the auction can end in with best bidding from here.
  void finals(int r, int side, int turn, bool passed, bool out[36][2]) {
    if (visited[r][side][turn][passed]) return;
    visited[r][side][turn][passed] = true;
    const int best = value(r, side, turn, passed);
    if (passed && ns_value(r, side) == best) out[r][side] = true;
    else if (!passed && value(r, side, turn ^ 1, true) == best) finals(r, side, turn ^ 1, true, out);
    for (int r2 = r + 1; r2 <= 35; r2++)
      if (value(r2, turn, turn ^ 1, false) == best) finals(r2, turn, turn ^ 1, false, out);
  }
};

}  // namespace

void par(const Table& table, int dealer, int vulnerable, ParResult& result) {
  Auction a{};
  a.vul[0] = vulnerable == 1 || vulnerable == 2;
  a.vul[1] = vulnerable == 1 || vulnerable == 3;
  for (int side = 0; side < 2; side++)
    for (int x = 0; x < 5; x++) a.tricks[side][x] = std::max(table.tricks[x][side], table.tricks[x][side + 2]);
  const int first = dealer & 1;
  result.score = a.value(0, 0, first, false);
  bool out[36][2] = {};
  a.finals(0, 0, first, false, out);
  result.n = 0;
  bool done[5][2] = {};
  for (int r = 1; r <= 35; r++)  // the cheapest contract of each strain and side
    for (int side = 0; side < 2; side++) {
      const int x = Auction::strain_of(r);
      if (!out[r][side] || done[x][side] || result.n == 10) continue;
      done[x][side] = true;
      ParContract& c = result.c[result.n++];
      c.level = uint8_t(Auction::level_of(r)), c.strain = uint8_t(x), c.side = uint8_t(side);
      c.result = int8_t(a.tricks[side][x] - (c.level + 6));
      c.declarers = 0;
      for (int q = side; q < 4; q += 2)
        if (table.tricks[x][q] == a.tricks[side][x]) c.declarers = uint8_t(c.declarers | 1u << q);
    }
}

}  // namespace ddfast
