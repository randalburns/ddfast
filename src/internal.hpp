// The interface between the search (solver.cpp) and the public API (api.cpp). Not installed.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ddclaim.hpp"

namespace ddfast::detail {

// Loads ddclaim's tables (building any that are missing); dir may be null.
void init(const char* dir);
// tricks[i] = North-South's tricks in positions[i] (ddclaim::Position: hands, trump, leader;
// 13 cards a hand here). On the calling thread.
void solve(const ddclaim::Position* positions, size_t n, uint8_t* tricks);
// tricks[strain][declarer]: declarer's side's tricks (DDS's res_table layout).
void solve_table(const uint16_t hand[4][4], uint8_t tricks[5][4]);

unsigned long long nodes();              // nodes searched, lead and follow (STATS builds; otherwise 0)
extern const uint8_t* g_first_target;    // development: per position, the first target (0: the usual guess)

}  // namespace ddfast::detail
