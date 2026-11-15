// ddfast: an exact double-dummy solver for full bridge deals.
//
// Given four 13-card hands, a strain and a declarer, ddfast returns the number of tricks
// declarer's side takes with best play by both sides, the same answer as DDS's SolveBoard
// (target -1) or CalcDDtable, several times faster on one core. It finishes endings of 2, 3
// and 4 cards a hand in the tables of ddclaim (extern/ddclaim), which init() loads.
//
// Conventions: seats N E S W are 0..3; suits S H D C are 0..3 (as in DDS); a holding is a
// 13-bit mask, bit r for rank 2 + r (bit 12 the ace); strains 0..3 are the suits, 4 no trumps.
// The opening leader is declarer's left-hand opponent.
//
// Thread safety: call init() once, from one thread, before anything else. After that,
// solve(), solve_table() and solve_tables() may be called from any number of threads at once:
// the tables are read-only and every search keeps its state per thread. solve_parallel() and
// solve_tables_parallel() share one worker pool and must not be called from two threads at
// once (nor overlap each other).
//
// Requires an x86-64 CPU with AVX-512 and BMI2, and Linux.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ddfast {

// A full deal: hand[seat][suit], 13 cards in every hand, no card twice.
struct Deal {
  uint16_t hand[4][4];
};

// One problem: a deal, a strain and a declarer. (The layout of the test-set files in tests/.)
struct Problem {
  uint16_t hand[4][4];
  uint8_t trump;     // strain: 0..3 the suits, 4 no trumps
  uint8_t declarer;  // 0..3
};

// A full table: tricks[strain][declarer], the tricks declarer's side takes (DDS's res_table).
struct Table {
  uint8_t tricks[5][4];
};

// A position at the start of a trick: hand[seat][suit], every hand with the same number of
// cards (1..13), no card twice; the strain and the seat on lead.
struct Position {
  uint16_t hand[4][4];
  uint8_t trump;   // strain: 0..3 the suits, 4 no trumps
  uint8_t leader;  // 0..3
};

// Loads the endgame tables from `table_dir`, or if it is null or empty from $DDCLAIM_DIR, or
// else from build/ddclaim under the current directory. Missing tables are built there first
// by ddclaim (the three-card table and the four-card pattern table: about 380 MB and 760 MB
// of files; a few minutes on many threads, once). The tables use about 1.2 GB of memory.
void init(const char* table_dir = nullptr);

// problems[0..n) -> tricks[i], declarer's tricks. On the calling thread, one at a time.
void solve(const Problem* problems, size_t n, uint8_t* tricks);

// North-South's tricks from position p with best play by both sides, or -1 if the hands do not
// hold the same number of cards (1..13). On the calling thread. With keep_table the search
// table of the previous call on this thread is kept, which pays when positions are related
// (e.g. reached by play from one deal): its entries are patterns of relative ranks, so they
// hold for any position in the same strain. Pass false when the strain changes.
int solve_position(const Position& p, bool keep_table = false);

// The full table of one deal, or of many. On the calling thread. The search table is kept
// across the four declarers of each strain, so a table costs about half of 20 solve()s.
void solve_table(const Deal& deal, Table& table);
void solve_tables(const Deal* deals, size_t n, Table* tables);

// Counters per worker of the last parallel call (cycles and the like are 0 when the kernel
// does not allow hardware counters).
struct WorkerStats {
  int cpu = -1;
  size_t deals = 0;   // problems or tables solved
  double busy_s = 0;  // time spent solving (excludes waiting for work)
  double cycles = 0;  // user-mode core cycles spent solving
  double instructions = 0;
  double branch_misses = 0, cache_misses = 0, dtlb_misses = 0;
};

// Given: in the order given. Predicted: the problems expected to be costliest first, so that
// the end of the batch does not leave workers idle.
enum class BatchOrder { Given, Predicted };

// The CPUs this process may run on, fastest cores first (by maximum frequency), then their
// SMT siblings (if smt): a reasonable cpu list for the parallel calls.
std::vector<int> available_cpus(bool smt = true);

// Like solve(), on a pool of worker threads, one per entry of cpus[0..ncpu), each pinned to
// that CPU (-1: not pinned). Work is handed out one problem at a time. The pool is kept between
// calls while the CPU list stays the same. Not reentrant (see above).
void solve_parallel(const Problem* problems, size_t n, uint8_t* tricks, const int* cpus, int ncpu,
                    BatchOrder order = BatchOrder::Predicted, std::vector<WorkerStats>* stats = nullptr);

// Like solve_tables(), on the same worker pool, one table at a time per worker.
void solve_tables_parallel(const Deal* deals, size_t n, Table* tables, const int* cpus, int ncpu,
                           std::vector<WorkerStats>* stats = nullptr);

// Stops the pool's threads (optional; also done at exit).
void stop_pool();

// Pins the calling thread to one CPU; false if that fails.
bool pin_thread(int cpu);

// Nodes searched so far (lead and follow nodes, all threads) in a build with -DSTATS, which
// also prints search counters at exit: a fingerprint of the search's behaviour (the counter is
// not atomic, so count on one thread). 0 in other builds.
unsigned long long stat_nodes();

}  // namespace ddfast
