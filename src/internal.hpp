// The interface between the search (solver.cpp) and the public API (api.cpp). Not installed.
#pragma once

#include <cstddef>
#include <cstdint>

#include "position.hpp"

namespace ddfast::detail {

// Loads the endgame tables (building any that are missing); dir may be null. map_tables: see
// ddfast::init().
void init(const char* dir, bool map_tables);
// tricks[i] = North-South's tricks in positions[i] (ddcore::Position: hands, trump, leader;
// 13 cards a hand here). On the calling thread.
void solve(const ddcore::Position* positions, size_t n, uint8_t* tricks);
// North-South's tricks in one position at the start of a trick (any equal hand size 1..13);
// keep: keep the search table of the previous call (same strain).
int solve_position(const ddcore::Position& p, bool keep);
// Whether North-South take at least target tricks from p (1 <= target <= cards per hand).
bool query_position(const ddcore::Position& p, int target, bool keep);
// North-South's tricks for each position, nctx searches interleaved on this thread.
void solve_positions(const ddcore::Position* positions, size_t n, uint8_t* tricks, int nctx);
void solve_tables_interleaved(const uint16_t* const* hands, uint8_t* const* out, size_t n, int nctx);
// tricks[strain][declarer]: declarer's side's tricks (DDS's res_table layout).
void solve_table(const uint16_t hand[4][4], uint8_t tricks[5][4]);
// Rows s0 .. s1 - 1 (strains) of the same table only.
void solve_table_strains(const uint16_t hand[4][4], int s0, int s1, uint8_t tricks[5][4]);

// One deal (13 cards a hand) searched on the calling thread and helpers on cpus[1..ncpu)
// (par_deal.cpp); North-South's tricks. lead_nodes: lead nodes searched by all threads.
int solve_deal_parallel(const ddcore::Position& p, const int* cpus, int ncpu, unsigned long long* lead_nodes);
void stop_deal_pool();
unsigned long long lead_nodes();         // lead nodes searched on the calling thread (all builds)
unsigned long long nodes();              // nodes searched, lead and follow (STATS builds; otherwise 0)
#ifndef CUTC_MIN
#define CUTC_MIN 8  // cut-off cards from 8 cards a hand (solver.cpp, NOTES round 18)
#endif
// Options (set_options), read in the search: hidden, so a PIC build reads them directly, not
// through the GOT.
extern bool g_sure_trick_bounds __attribute__((visibility("hidden")));  // Options::sure_trick_bounds
extern int g_cutoff_min __attribute__((visibility("hidden")));  // cut-off cards from this many cards a hand; 14: never
extern const uint8_t* g_first_target;    // development: per position, the first target (0: the usual guess)

}  // namespace ddfast::detail
