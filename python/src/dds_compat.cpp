// DDS semantics on ddfast: see dds_compat.hpp.
#include "dds_compat.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "ddfast.hpp"

#include <api/dll.h>  // DDS's Par
#include <pbn.hpp>    // DDS's convert_from_pbn

namespace ddfast_compat {
namespace {

using ddfast::Card;
using ddfast::PlayPosition;

std::once_flag g_init_once;

int popc(unsigned x) { return __builtin_popcount(x); }

// The seat that wins a complete trick (cards in play order from the leader).
int trick_winner(const Card t[4], int leader, int trump) {
  int best = 0;
  for (int k = 1; k < 4; k++) {
    const bool ruff = trump < 4 && t[k].suit == trump && t[best].suit != trump;
    const bool beats = t[k].suit == t[best].suit && t[k].rank > t[best].rank;
    if (ruff || beats) best = k;
  }
  return (leader + best) & 3;
}

// The position after the seat to play plays c. If c completes the trick, `start` is the next
// trick start (empty hands when the deal is over) and won is 1 when North-South won the trick;
// else start.played > 0 and won = 0.
void after(const PlayPosition& p, Card c, PlayPosition& start, int& won) {
  const int seat = (p.leader + p.played) & 3;
  start = p;
  start.hand[seat][c.suit] = uint16_t(start.hand[seat][c.suit] & ~(1u << (c.rank - 2)));
  won = 0;
  if (p.played < 3) {
    start.trick[p.played] = c;
    start.played = uint8_t(p.played + 1);
    return;
  }
  Card t[4] = {p.trick[0], p.trick[1], p.trick[2], c};
  const int w = trick_winner(t, p.leader, p.trump);
  won = (w & 1) == 0;
  start.leader = uint8_t(w);
  start.played = 0;
}

// Tricks still to play from q, the current one included: the cards of the seat to play (it has
// not played to the current trick).
int cards_left(const PlayPosition& q) {
  const int seat = (q.leader + q.played) & 3;
  int n = 0;
  for (int s = 0; s < 4; s++) n += popc(q.hand[seat][s]);
  return n;
}

// A follower in the current trick who did not follow suit but still holds the suit led. DDS
// accepts such a position (it only plays on from it); ddfast's solvers reject it.
bool revoked(const PlayPosition& p) {
  for (int k = 1; k < p.played; k++)
    if (p.trick[k].suit != p.trick[0].suit && p.hand[(p.leader + k) & 3][p.trick[0].suit]) return true;
  return false;
}

// Tricks North-South take from q on (trick start or mid-trick); keep: reuse this thread's table.
// A position with a revoke in the current trick is played out card by card to the end of the
// trick (at most 3 cards, each completed trick solved from its start, which is always valid).
int ns_value(const PlayPosition& q, bool keep) {
  if (q.played == 0 || !revoked(q)) return ddfast::solve_play(q, keep);
  const int seat = (q.leader + q.played) & 3;
  const bool ns = (seat & 1) == 0;
  const bool follow = q.hand[seat][q.trick[0].suit] != 0;
  int best = ns ? -1 : 99;
  for (int s = 0; s < 4; s++) {
    if (follow && s != q.trick[0].suit) continue;
    for (unsigned h = q.hand[seat][s]; h; h &= h - 1) {
      const Card c{uint8_t(s), uint8_t(__builtin_ctz(h) + 2)};
      PlayPosition r;
      int won;
      after(q, c, r, won);
      const int v = won + (r.played == 0 ? (cards_left(r) ? ddfast::solve_play(r, keep) : 0) : ns_value(r, keep));
      keep = true;
      best = ns ? std::max(best, v) : std::min(best, v);
    }
  }
  return best;
}

// North-South's tricks from p on when the seat to play plays c (exact).
int ns_after(const PlayPosition& p, Card c, bool keep) {
  PlayPosition q;
  int won;
  after(p, c, q, won);
  if (q.played == 0 && cards_left(q) == 0) return won;
  return won + ns_value(q, keep);
}

// Does North-South take at least t tricks from p on when the seat to play plays c?
bool ns_at_least_after(const PlayPosition& p, Card c, int t, bool keep) {
  PlayPosition q;
  int won;
  after(p, c, q, won);
  t -= won;
  if (t <= 0) return true;
  if (t > cards_left(q)) return false;
  if (q.played > 0 && revoked(q)) return ns_value(q, keep) >= t;
  return ddfast::at_least(q, t, keep) == 1;
}

// One of DDS's moves: a card of the seat to play and the lower cards equivalent to it.
struct Group {
  Card top;
  int equals;  // DDS's equals: bit r for each lower equivalent rank r
  int score = 0;
};

// The seat to play's moves, grouped as DDS's move generator groups them: in each suit, a card
// joins the group of the next higher card it holds when no card between them is held by
// another hand or has been played to the current trick (DDS's relative ranks describe the trick
// start, so the trick's cards still separate). Suits in order, high cards first.
std::vector<Group> moves(const PlayPosition& p) {
  const int seat = (p.leader + p.played) & 3;
  unsigned aggr[4];
  for (int s = 0; s < 4; s++) aggr[s] = p.hand[0][s] | p.hand[1][s] | p.hand[2][s] | p.hand[3][s];
  for (int k = 0; k < p.played; k++) aggr[p.trick[k].suit] |= 1u << (p.trick[k].rank - 2);
  int lo = 0, hi = 3;
  if (p.played > 0 && p.hand[seat][p.trick[0].suit]) lo = hi = p.trick[0].suit;
  std::vector<Group> g;
  for (int s = lo; s <= hi; s++) {
    const unsigned mine = p.hand[seat][s], others = aggr[s] & ~mine;
    int prev = -1;  // rank bit of the previous card of mine (higher)
    for (int b = 15; b >= 0; b--) {
      if (!(mine >> b & 1)) continue;
      const bool joins = prev >= 0 && !(others & (((1u << prev) - 1) & ~((1u << (b + 1)) - 1)));
      if (joins) g.back().equals |= 1 << (b + 2);
      else g.push_back(Group{Card{uint8_t(s), uint8_t(b + 2)}, 0});
      prev = b;
    }
  }
  return g;
}

// The per-thread memory of the last position solved, for DDS's table reuse rule: reuse when the
// strain is the same and the cards are the same deal (mode 0 and 1), or always in the same
// strain (mode 2). ddfast's table entries are patterns of relative ranks, valid for any
// position in the same strain, so reuse is always safe; it only pays for related positions.
struct LastSolve {
  bool valid = false;
  int trump = -1;
  uint16_t all[4][4] = {};
};
thread_local LastSolve t_last;

bool keep_for(const PlayPosition& p, int mode) {
  uint16_t all[4][4];
  std::memcpy(all, p.hand, sizeof all);
  for (int k = 0; k < p.played; k++) all[(p.leader + k) & 3][p.trick[k].suit] |= uint16_t(1u << (p.trick[k].rank - 2));
  const bool keep = t_last.valid && t_last.trump == p.trump && (mode == 2 || !std::memcmp(all, t_last.all, sizeof all));
  t_last.valid = true;
  t_last.trump = p.trump;
  std::memcpy(t_last.all, all, sizeof all);
  return keep;
}

void put(Future& f, int i, const Group& g, int score) {
  f.suit[i] = g.top.suit;
  f.rank[i] = g.top.rank;
  f.equals[i] = g.equals;
  f.score[i] = score;
}

int range_checks(const unsigned remain[4][4], int trump, int first, const int ts[3], const int tr[3],
                 int target, int solutions, int mode) {
  if (target < -1) return RETURN_TARGET_WRONG_LO;
  if (target > 13) return RETURN_TARGET_WRONG_HI;
  if (solutions < 1) return RETURN_SOLNS_WRONG_LO;
  if (solutions > 3) return RETURN_SOLNS_WRONG_HI;
  if (mode < 0) return RETURN_MODE_WRONG_LO;
  if (mode > 2) return RETURN_MODE_WRONG_HI;
  if (trump < 0 || trump > 4) return RETURN_TRUMP_WRONG;
  if (first < 0 || first > 3) return RETURN_FIRST_WRONG;
  int seen[3] = {0, 0, 0};
  for (int k = 0; k < 3; k++) {
    if (tr[k] == 0) continue;
    seen[k] = 1;
    if (tr[k] < 2 || tr[k] > 14) return RETURN_SUIT_OR_RANK;
    if (ts[k] < 0 || ts[k] > 3) return RETURN_SUIT_OR_RANK;
  }
  if ((seen[2] && (!seen[1] || !seen[0])) || (seen[1] && !seen[0])) return RETURN_SUIT_OR_RANK;
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++)
      if (remain[h][s] != 0 && (remain[h][s] < 0x0004 || remain[h][s] >= 0x8000)) return RETURN_SUIT_OR_RANK;
  return RETURN_NO_FAULT;
}

}  // namespace

void ensure_init(const char* table_dir, bool map_tables) {
  std::call_once(g_init_once, [&] { ddfast::init(table_dir, map_tables); });
}

const char* error_text(int code) {
  switch (code) {
    case RETURN_NO_FAULT: return TEXT_NO_FAULT;
    case RETURN_UNKNOWN_FAULT: return TEXT_UNKNOWN_FAULT;
    case RETURN_ZERO_CARDS: return TEXT_ZERO_CARDS;
    case RETURN_TARGET_TOO_HIGH: return TEXT_TARGET_TOO_HIGH;
    case RETURN_DUPLICATE_CARDS: return TEXT_DUPLICATE_CARDS;
    case RETURN_TARGET_WRONG_LO: return TEXT_TARGET_WRONG_LO;
    case RETURN_TARGET_WRONG_HI: return TEXT_TARGET_WRONG_HI;
    case RETURN_SOLNS_WRONG_LO: return TEXT_SOLNS_WRONG_LO;
    case RETURN_SOLNS_WRONG_HI: return TEXT_SOLNS_WRONG_HI;
    case RETURN_TOO_MANY_CARDS: return TEXT_TOO_MANY_CARDS;
    case RETURN_SUIT_OR_RANK: return TEXT_SUIT_OR_RANK;
    case RETURN_PLAYED_CARD: return TEXT_PLAYED_CARD;
    case RETURN_CARD_COUNT: return TEXT_CARD_COUNT;
    case RETURN_THREAD_INDEX: return TEXT_THREAD_INDEX;
    case RETURN_MODE_WRONG_LO: return TEXT_MODE_WRONG_LO;
    case RETURN_MODE_WRONG_HI: return TEXT_MODE_WRONG_HI;
    case RETURN_TRUMP_WRONG: return TEXT_TRUMP_WRONG;
    case RETURN_FIRST_WRONG: return TEXT_FIRST_WRONG;
    case RETURN_PLAY_FAULT: return TEXT_PLAY_FAULT;
    case RETURN_PBN_FAULT: return TEXT_PBN_FAULT;
    case RETURN_TOO_MANY_BOARDS: return TEXT_TOO_MANY_BOARDS;
    case RETURN_THREAD_CREATE: return TEXT_THREAD_CREATE;
    case RETURN_THREAD_WAIT: return TEXT_THREAD_WAIT;
    case RETURN_THREAD_MISSING: return TEXT_THREAD_MISSING;
    case RETURN_NO_SUIT: return TEXT_NO_SUIT;
    case RETURN_TOO_MANY_TABLES: return TEXT_TOO_MANY_TABLES;
    case RETURN_CHUNK_SIZE: return TEXT_CHUNK_SIZE;
    case RETURN_PAR_TABLE_FAULT: return TEXT_PAR_TABLE_FAULT;
    default: return "Not a DDS error code";
  }
}

int solve_board(const unsigned remain[4][4], int trump, int first, const int ts[3], const int tr[3],
                int target, int solutions, int mode, Future& f) {
  f = Future{};
  int rc = range_checks(remain, trump, first, ts, tr, target, solutions, mode);
  if (rc != RETURN_NO_FAULT) return rc;

  // board_value_checks
  int card_count = 0;
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) card_count += popc(remain[h][s] >> 2);
  if (card_count <= 0) return RETURN_ZERO_CARDS;
  if (card_count > 52) return RETURN_TOO_MANY_CARDS;
  const int total_tricks = (card_count % 4) ? ((card_count - 4) >> 2) + 2 : ((card_count - 4) >> 2) + 1;
  if (total_tricks < target) return RETURN_TARGET_TOO_HIGH;
  const int played = (52 - card_count) % 4;  // DDS: (48 - ini_depth) % 4
  int per_hand[4] = {0, 0, 0, 0};
  for (int k = 0; k < played; k++) per_hand[(first + k) & 3] = 1;
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) per_hand[h] += popc(remain[h][s] >> 2);
  for (int h = 1; h < 4; h++)
    if (per_hand[h] != per_hand[0]) return RETURN_CARD_COUNT;
  for (int k = 0; k < played; k++) {
    if (ts[k] < 0 || ts[k] > 3) return RETURN_SUIT_OR_RANK;
    unsigned aggr = 0;
    for (int h = 0; h < 4; h++) aggr |= remain[h][ts[k]] >> 2;
    if (tr[k] >= 2 && (aggr >> (tr[k] - 2) & 1)) return RETURN_PLAYED_CARD;
  }
  for (int s = 0; s < 4; s++) {
    unsigned seen = 0;
    for (int h = 0; h < 4; h++) {
      const unsigned c = remain[h][s] >> 2;
      if (seen & c) return RETURN_DUPLICATE_CARDS;
      seen |= c;
    }
  }

  PlayPosition p{};
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) p.hand[h][s] = uint16_t(remain[h][s] >> 2);
  p.trump = uint8_t(trump);
  p.leader = uint8_t(first);
  p.played = uint8_t(played);
  for (int k = 0; k < played; k++) p.trick[k] = Card{uint8_t(ts[k]), uint8_t(tr[k])};
  const int seat = (first + played) & 3;
  const bool side_ns = (seat & 1) == 0;

  // The last trick: no search (DDS's last_trick_winner).
  if (card_count <= 4) {
    Card t[4];
    for (int k = 0; k < played; k++) t[k] = p.trick[k];
    for (int k = played; k < 4; k++) {
      const int h = (first + k) & 3;
      for (int s = 0; s < 4; s++)
        if (p.hand[h][s]) {
          t[k] = Card{uint8_t(s), uint8_t(31 - __builtin_clz(p.hand[h][s]) + 2)};
          break;
        }
    }
    const int w = trick_winner(t, first, trump);
    const int wins = ((w & 1) == (seat & 1)) ? 1 : 0;
    f.cards = 1;
    f.suit[0] = t[played].suit;
    f.rank[0] = t[played].rank;
    f.equals[0] = 0;
    f.score[0] = (target == 0 && solutions < 3) ? 0 : wins;
    return RETURN_NO_FAULT;
  }

  std::vector<Group> g = moves(p);
  const int nm = int(g.size());
  const int left = per_hand[seat];  // tricks still to play, the current one included

  if (mode == 0 && nm == 1 && solutions != 3) {  // one move: no search
    f.cards = 1;
    put(f, 0, g[0], -2);
    return RETURN_NO_FAULT;
  }

  bool keep = keep_for(p, mode);
  auto side_score = [&](int ns) { return side_ns ? ns : left - ns; };

  if (solutions == 3) {
    // At a lead: the position's value first. No card scores more, and two in three score
    // exactly that, so each card is then asked "at least that?" and one less until yes: about
    // 1.5 questions a card in place of a full solve from a fresh guess. In the middle of a
    // trick the extra solve does not pay (docs/NOTES.md, round 19): a full solve per card.
    // DDFAST_OLD3 (any value): a full solve per card everywhere, to compare.
    static const bool old3 = getenv("DDFAST_OLD3") != nullptr;
    if (old3 || played > 0) {
      for (Group& x : g) {
        x.score = side_score(ns_after(p, x.top, keep));
        keep = true;
      }
    } else {
      const int best = side_score(ns_value(p, keep));
      keep = true;
      auto reaches = [&](const Group& x, int sc) {
        return side_ns ? ns_at_least_after(p, x.top, sc, keep) : !ns_at_least_after(p, x.top, left - sc + 1, keep);
      };
      for (Group& x : g) {
        int sc = best;
        while (sc > 0 && !reaches(x, sc)) sc--;
        x.score = sc;
      }
    }
    std::stable_sort(g.begin(), g.end(), [](const Group& a, const Group& b) { return a.score > b.score; });
    f.cards = nm;
    for (int i = 0; i < nm; i++) put(f, i, g[i], g[i].score);
    return RETURN_NO_FAULT;
  }
  if (target == 0) {
    f.cards = solutions == 1 ? 1 : nm;
    for (int i = 0; i < nm; i++) put(f, i, g[i], 0);
    return RETURN_NO_FAULT;
  }

  // The tricks the side to play must reach (target -1: the best it can do), and a test of
  // whether a move reaches them: one null-window question per move.
  int need;
  if (target == -1) {
    need = side_score(ns_value(p, keep));
    keep = true;
    if (need == 0) {
      f.cards = solutions == 1 ? 1 : nm;
      for (int i = 0; i < nm; i++) put(f, i, g[i], 0);
      return RETURN_NO_FAULT;
    }
  } else {
    need = target;
  }
  auto reaches = [&](const Group& x) {
    const bool r = side_ns ? ns_at_least_after(p, x.top, need, keep) : !ns_at_least_after(p, x.top, left - need + 1, keep);
    keep = true;
    return r;
  };
  int n = 0;
  for (const Group& x : g) {
    if (!reaches(x)) continue;
    put(f, n++, x, need);
    if (solutions == 1) break;
  }
  if (n == 0) {  // only with a target: no move reaches it
    f.cards = 0;
    f.score[0] = target > 1 ? -1 : 0;
    return RETURN_NO_FAULT;
  }
  f.cards = n;
  return RETURN_NO_FAULT;
}

int solve_board_pbn(const char* pbn, int trump, int first, const int ts[3], const int tr[3], int target,
                    int solutions, int mode, Future& f) {
  unsigned remain[4][4];
  if (convert_from_pbn(pbn, remain) != RETURN_NO_FAULT) {
    f = Future{};
    return RETURN_PBN_FAULT;
  }
  return solve_board(remain, trump, first, ts, tr, target, solutions, mode, f);
}

int parse_table_deal(const char* pbn, uint16_t hand[4][4]) {
  unsigned remain[4][4];
  if (convert_from_pbn(pbn, remain) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  // DDS's table_deal_checks
  int count[4] = {0, 0, 0, 0};
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) {
      if (remain[h][s] & ~0x7FFCu) return RETURN_SUIT_OR_RANK;
      count[h] += popc(remain[h][s]);
    }
  for (int s = 0; s < 4; s++) {
    unsigned seen = 0;
    for (int h = 0; h < 4; h++) {
      if (seen & remain[h][s]) return RETURN_DUPLICATE_CARDS;
      seen |= remain[h][s];
    }
  }
  for (int h = 1; h < 4; h++)
    if (count[h] != count[0]) return RETURN_CARD_COUNT;
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) hand[h][s] = uint16_t(remain[h][s] >> 2);
  return RETURN_NO_FAULT;
}

namespace {
// The helper threads of parallel_for: kept for the life of the process (never joined; they
// wait on a condition variable between calls), so their ddfast state survives between calls.
struct Helpers {
  std::mutex call;  // one parallel_for at a time
  std::mutex m;
  std::condition_variable start, done;
  size_t threads = 0;  // helper threads started so far
  size_t use = 0;      // helpers taking part in the current call (the first `use` of them)
  uint64_t round = 0;
  int active = 0;
  size_t n = 0;
  void (*f)(size_t, void*) = nullptr;
  void* arg = nullptr;
  std::atomic<size_t> next{0};

  void run() {
    for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) f(i, arg);
  }
  void helper(size_t idx) {
    uint64_t seen = 0;
    for (;;) {
      bool part;
      {
        std::unique_lock<std::mutex> lk(m);
        start.wait(lk, [&] { return round != seen && idx < use; });
        seen = round;
        part = true;
      }
      if (part) run();
      std::lock_guard<std::mutex> lk(m);
      if (--active == 0) done.notify_one();
    }
  }
};
Helpers& helpers() {
  static Helpers* h = new Helpers;  // never destroyed: its threads outlive static destructors
  return *h;
}

std::mutex g_pool_mutex;  // ddfast's thread pool is not reentrant

// A table of hands with fewer than 13 cards: declarer's tricks, position by position.
void small_table(const uint16_t hand[4][4], const int filter[5], DdTableResults& r) {
  int n = 0;
  for (int s = 0; s < 4; s++) n += popc(hand[0][s]);
  for (int st = 0; st < 5; st++) {
    if (filter[st]) continue;
    for (int d = 0; d < 4; d++) {
      if (n == 0) { r.res_table[st][d] = 0; continue; }
      ddfast::Position pos{};
      std::memcpy(pos.hand, hand, sizeof pos.hand);
      pos.trump = uint8_t(st);
      pos.leader = uint8_t((d + 1) & 3);
      const int ns = ddfast::solve_position(pos, d > 0);
      r.res_table[st][d] = (d & 1) ? n - ns : ns;
    }
  }
}
}  // namespace

int calc_tables(const uint16_t (*hands)[4][4], size_t n, const int filter[5], int max_threads,
                DdTableResults* res) {
  for (size_t i = 0; i < n; i++) std::memset(&res[i], 0, sizeof res[i]);
  std::vector<size_t> full, part;
  for (size_t i = 0; i < n; i++) {
    int c = 0;
    for (int s = 0; s < 4; s++) c += popc(hands[i][0][s]);
    (c == 13 ? full : part).push_back(i);
  }
  std::vector<int> cpus = ddfast::available_cpus(true);
  if (max_threads > 0 && size_t(max_threads) < cpus.size()) cpus.resize(size_t(max_threads));
  if (cpus.empty()) cpus.push_back(-1);

  if (!full.empty()) {
    std::vector<ddfast::Deal> deals(full.size());
    std::vector<ddfast::Table> tabs(full.size());
    for (size_t k = 0; k < full.size(); k++) std::memcpy(deals[k].hand, hands[full[k]], sizeof deals[k].hand);
    if (cpus.size() == 1 || full.size() == 1) {
      ddfast::solve_tables(deals.data(), deals.size(), tabs.data());
    } else {
      std::lock_guard<std::mutex> lk(g_pool_mutex);
      ddfast::solve_tables_parallel(deals.data(), deals.size(), tabs.data(), cpus.data(), int(cpus.size()));
    }
    for (size_t k = 0; k < full.size(); k++)
      for (int st = 0; st < 5; st++)
        for (int d = 0; d < 4; d++) res[full[k]].res_table[st][d] = filter[st] ? 0 : tabs[k].tricks[st][d];
  }
  if (!part.empty()) {
    struct Job {
      const uint16_t (*hands)[4][4];
      const std::vector<size_t>* part;
      const int* filter;
      DdTableResults* res;
    } job{hands, &part, filter, res};
    parallel_for(part.size(), int(cpus.size()), [](size_t k, void* p) {
      const Job& j = *static_cast<const Job*>(p);
      const size_t i = (*j.part)[k];
      small_table(j.hands[i], j.filter, j.res[i]);
    }, &job);
  }
  return RETURN_NO_FAULT;
}

int par(const DdTableResults& table, int vulnerable, ParResults& out) {
  return Par(&table, &out, vulnerable);
}

void parallel_for(size_t n, int max_threads, void (*f)(size_t, void*), void* arg) {
  if (n == 0) return;
  size_t want = max_threads > 0 ? size_t(max_threads) : size_t(std::max(1u, std::thread::hardware_concurrency()));
  want = std::min(want, n);
  if (want <= 1) {
    for (size_t i = 0; i < n; i++) f(i, arg);
    return;
  }
  Helpers& h = helpers();
  std::lock_guard<std::mutex> call(h.call);
  {
    std::lock_guard<std::mutex> lk(h.m);
    h.f = f, h.arg = arg, h.n = n;
    h.next.store(0);
    h.use = want - 1;
    h.active = int(h.use);
    h.round++;
  }
  while (h.threads < want - 1) {  // new helpers see this round (round != 0 = their `seen`)
    std::thread(&Helpers::helper, &h, h.threads).detach();
    h.threads++;
  }
  h.start.notify_all();
  h.run();
  std::unique_lock<std::mutex> lk(h.m);
  h.done.wait(lk, [&] { return h.active == 0; });
}

}  // namespace ddfast_compat

#ifdef DDFAST_COMPAT_CALCDDTABLE
// DDS's CalcDDtable, on ddfast: DDS's par.cpp (compiled into the Python module for its par
// text) calls it from CalcPar. One table on the calling thread, checked as DDS checks it. Not
// defined when DDS itself is linked (the C++ tests).
auto STDCALL CalcDDtable(DdTableDeal tableDeal, DdTableResults* tablep) -> int {
  if (tablep == nullptr) return RETURN_UNKNOWN_FAULT;
  unsigned remain[4][4];
  std::memcpy(remain, tableDeal.cards, sizeof remain);
  int count[4] = {0, 0, 0, 0};
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) {
      if (remain[h][s] & ~0x7FFCu) return RETURN_SUIT_OR_RANK;
      count[h] += __builtin_popcount(remain[h][s]);
    }
  for (int s = 0; s < 4; s++) {
    unsigned seen = 0;
    for (int h = 0; h < 4; h++) {
      if (seen & remain[h][s]) return RETURN_DUPLICATE_CARDS;
      seen |= remain[h][s];
    }
  }
  for (int h = 1; h < 4; h++)
    if (count[h] != count[0]) return RETURN_CARD_COUNT;
  uint16_t hand[4][4];
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) hand[h][s] = uint16_t(remain[h][s] >> 2);
  const int filter[5] = {0, 0, 0, 0, 0};
  ddfast_compat::ensure_init();
  return ddfast_compat::calc_tables(&hand, 1, filter, 1, tablep);
}
#endif
