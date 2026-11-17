// ddclaim: exact double-dummy values for small bridge endings.
//
// A position is the start of a trick with the same number of cards, 1 to 7, in every hand.
// solve() returns, for each position of a batch, the number of tricks North-South take with
// best play by both sides. Seven cards a hand is a hard limit: see README.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ddclaim {

struct Position {
  uint16_t hand[4][4];  // [seat][suit]; seats N E S W are 0..3, suits 0..3; bit r is rank 2 + r (bit 12 the ace)
  uint8_t trump;        // 0..3, or 4 for no trumps
  uint8_t leader;       // the seat to lead
};

constexpr int kMaxCards = 7;

// The number of cards in each hand, or -1 if the position is not one solve() accepts: the
// hands differ in size, hold more than kMaxCards cards or none, share a card, use a bit above
// the ace, or the trump or leader is out of range.
inline int cards_per_hand(const Position& p) {
  if (p.trump > 4 || p.leader > 3) return -1;
  int n[4] = {0, 0, 0, 0};
  for (int s = 0; s < 4; s++) {
    unsigned seen = 0;
    for (int q = 0; q < 4; q++) {
      const unsigned h = p.hand[q][s];
      if ((h & ~0x1FFFu) || (h & seen)) return -1;
      seen |= h;
      n[q] += __builtin_popcount(h);
    }
  }
  if (n[0] < 1 || n[0] > kMaxCards || n[1] != n[0] || n[2] != n[0] || n[3] != n[0]) return -1;
  return n[0];
}

// Call once, from one thread, before any solving. Loads the tables of solved endings from
// `dir`, or if it is null or empty from $DDCLAIM_DIR (default build/ddclaim under the current
// directory), building them first if they are missing: about 3 minutes on many threads and
// 2.6 GB of files, once. $DDCLAIM_LEVELS (default 2) sets how many pattern-table levels are
// used (0 to 3: the 4-, 5- and 6-card tables). The tables themselves, with inline probes, are
// exported in ddclaim/tables.hpp.
void init();
void init(const char* dir);

// Solves positions[0..n): tricks[i] gets the tricks North-South take in positions[i]. Every
// position must be valid (cards_per_hand() >= 1); the positions of a batch may differ in size.
// Runs on the calling thread. After init() it may be called from several threads at once,
// each with its own batch: the tables are only read, and everything a search writes is kept
// per thread. Much faster per position on a batch than one position at a time: see README.md.
void solve(const Position* positions, size_t n, uint8_t* tricks);

// Pins the calling thread to one CPU (as numbered by the operating system); false if that
// fails. Optional: when several threads call solve(), giving each its own core keeps them from
// sharing one.
bool pin_thread(int cpu);

}  // namespace ddclaim
