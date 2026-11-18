// DDS's SolveBoard / CalcAllTables / Par semantics on top of ddfast, with no Python in it, so
// the same code serves the Python module and the C++ test that compares it with DDS itself.
//
// Faithful to DDS 3.x (extern/dds, library/src/solver_if.cpp and calc_tables.cpp): the same
// input checks in the same order with the same return codes, the same result fields, the same
// special cases (the last trick, a single legal move with mode 0, targets, "all cards score
// 0"). PBN parsing and the par text are DDS's own code (pbn.cpp, par.cpp, compiled in).
// Differences, by design: `nodes` is 0; when several cards (or groups of equivalent cards) are
// equally good, which one is reported first, and the order of equal scores, may differ from
// DDS. Positions where a follower in the current trick did not follow suit but still holds the
// suit led (which DDS accepts and ddfast's solvers reject) are solved by playing out the rest of
// the trick card by card.
#pragma once

#include <cstddef>
#include <cstdint>

#include <api/dds_constants.hpp>
#include <api/dds_data_types.hpp>

namespace ddfast_compat {

// DDS's FutureTricks.
struct Future {
  int nodes = 0;
  int cards = 0;
  int suit[13] = {};
  int rank[13] = {};
  int equals[13] = {};
  int score[13] = {};
};

// Loads the endgame tables once (thread-safe). table_dir as for ddfast::init().
void ensure_init(const char* table_dir = nullptr);

// DDS's SolveBoardPBN: remain_cards in PBN, the cards already played to the current trick in
// trick_suit/trick_rank (DDS reads as many entries as the card count implies). Returns a DDS
// return code (RETURN_NO_FAULT on success). Thread-safe; ddfast's search state is per thread.
int solve_board_pbn(const char* remain_cards, int trump, int first, const int trick_suit[3],
                    const int trick_rank[3], int target, int solutions, int mode, Future& out);
// The same from DDS's binary deal (remain[hand][suit], rank r at bit r).
int solve_board(const unsigned remain[4][4], int trump, int first, const int trick_suit[3],
                const int trick_rank[3], int target, int solutions, int mode, Future& out);

// A table deal from PBN, checked as DDS's CalcAllTables checks it (equal hands of any size).
// hand[seat][suit] in ddfast's layout (rank r at bit r - 2). Returns a DDS return code.
int parse_table_deal(const char* pbn, uint16_t hand[4][4]);

// DDS's CalcAllTables on parsed deals: res[i].res_table[strain][hand], declarer's tricks.
// Strains with trump_filter[s] != 0 are left 0. max_threads 0: all CPUs ddfast finds.
// Full deals use ddfast's table solver on its thread pool; smaller hands are solved position
// by position. Returns a DDS return code.
int calc_tables(const uint16_t (*hands)[4][4], size_t n, const int trump_filter[5], int max_threads,
                DdTableResults* res);

// DDS's Par (text results) for one table; vulnerable 0 none, 1 both, 2 NS, 3 EW.
int par(const DdTableResults& table, int vulnerable, ParResults& out);

// DDS's ErrorMessage text for a return code.
const char* error_text(int code);

// f(0) .. f(n-1) on up to max_threads threads (0: all CPUs), the calling thread included. The
// helper threads are kept between calls, so each keeps ddfast's per-thread search state (set up
// once per thread, which is costly next to a small position). Calls are serialized.
void parallel_for(size_t n, int max_threads, void (*f)(size_t, void*), void* arg);

}  // namespace ddfast_compat
