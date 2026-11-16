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
// North-South's tricks in one position at the start of a trick (any equal hand size 1..13);
// keep: keep the search table of the previous call (same strain).
int solve_position(const ddclaim::Position& p, bool keep);
// tricks[strain][declarer]: declarer's side's tricks (DDS's res_table layout).
void solve_table(const uint16_t hand[4][4], uint8_t tricks[5][4]);

// One deal (13 cards a hand) searched on the calling thread and helpers on cpus[1..ncpu)
// (par_deal.cpp); North-South's tricks. lead_nodes: lead nodes searched by all threads.
int solve_deal_parallel(const ddclaim::Position& p, const int* cpus, int ncpu, unsigned long long* lead_nodes);
void stop_deal_pool();
unsigned long long lead_nodes();         // lead nodes searched on the calling thread (all builds)
unsigned long long nodes();              // nodes searched, lead and follow (STATS builds; otherwise 0)
extern const uint8_t* g_first_target;    // development: per position, the first target (0: the usual guess)

}  // namespace ddfast::detail
