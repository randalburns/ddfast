// The internal position type and thread pinning, shared by the search and the endgame tables
// (tables.hpp).
//
// A position is the start of a trick with the same number of cards in every hand.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ddcore {

struct Position {
  uint16_t hand[4][4];  // [seat][suit]; seats N E S W are 0..3, suits 0..3; bit r is rank 2 + r (bit 12 the ace)
  uint8_t trump;        // 0..3, or 4 for no trumps
  uint8_t leader;       // the seat to lead
};

// Pins the calling thread to one CPU (as numbered by the operating system); false if that
// fails. Used by ddfast's thread pools.
bool pin_thread(int cpu);
// Places a pool worker for CPU `cpu` of the pool's list (by default within its NUMA node and
// core class; DDFAST_AFFINITY=cpu pins it to `cpu`, =none leaves it to the scheduler).
bool place_thread(int cpu, const int* cpus, int ncpu);

}  // namespace ddcore
