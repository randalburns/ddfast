// ddclaim: the position type and thread pinning shared with ddfast. The endgame tables and
// their probes are in ddclaim/tables.hpp; ddclaim's own search is used only to build them.
//
// A position is the start of a trick with the same number of cards in every hand.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ddclaim {

struct Position {
  uint16_t hand[4][4];  // [seat][suit]; seats N E S W are 0..3, suits 0..3; bit r is rank 2 + r (bit 12 the ace)
  uint8_t trump;        // 0..3, or 4 for no trumps
  uint8_t leader;       // the seat to lead
};

// Pins the calling thread to one CPU (as numbered by the operating system); false if that
// fails. Used by ddfast's thread pools.
bool pin_thread(int cpu);

}  // namespace ddclaim
