// ddfast: an exact double-dummy solver for full bridge deals.
//
// Given four 13-card hands, a strain and a declarer, ddfast returns the number of tricks
// declarer's side takes with best play by both sides, the same answer as DDS's SolveBoard
// (target -1) or CalcDDtable, several times faster on one core. It finishes endings of 2, 3
// and 4 cards a hand in endgame tables, which init() loads.
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

// Loads the endgame tables from `table_dir`, or if it is null or empty from $DDFAST_TABLES, or
// else from build/tables under the current directory. Missing tables are built there first
// (the three-card table and the four-card pattern table: about 380 MB and 550 MB
// of files; a few minutes on many threads, once). The tables use about 1 GB of memory.
//
// map_tables: by default every process reads the two files into its own memory (on huge pages
// where the system has them, which is the fastest to search). With map_tables, or with the
// environment variable DDFAST_TABLE_MAP set to anything but 0, the files are mapped read-only
// instead: all processes on the machine that use the same files then share one copy of the
// tables, and a process starts without copying it. For many solver processes on one machine.
void init(const char* table_dir = nullptr, bool map_tables = false);

// problems[0..n) -> tricks[i], declarer's tricks. On the calling thread, one at a time.
void solve(const Problem* problems, size_t n, uint8_t* tricks);

// North-South's tricks from position p with best play by both sides, or -1 if the hands do not
// hold the same number of cards (1..13). On the calling thread. With keep_table the search
// table of the previous call on this thread is kept, which pays when positions are related
// (e.g. reached by play from one deal): its entries are patterns of relative ranks, so they
// hold for any position in the same strain. Pass false when the strain changes.
int solve_position(const Position& p, bool keep_table = false);

// North-South's tricks for each of positions[0..n), each solved from an empty table, or 0xFF
// for a position solve_position() would reject. On the calling thread. Positions of up to
// kInterleaveMax cards a hand are searched kInterleaveCtx at a time, interleaved (a search
// that waits for an endgame table hands over to the next), which is much faster per position
// than solve_position() one at a time; larger positions are solved one at a time.
constexpr int kInterleaveMax = 10, kInterleaveCtx = 4;
void solve_positions(const Position* positions, size_t n, uint8_t* tricks);

// ---------------------------------------------------------------------------------------
// Play: positions in the middle of a trick, card-by-card results, play analysis, targets.
// Kept simple rather than fast: the cards still to come in the current trick are searched
// card by card, and each completed trick is solved with the trick-start solver (its table
// is kept for the whole call).

// A card: suit 0..3 (as hand[seat][suit]), rank 2..14 (14 the ace).
struct Card {
  uint8_t suit, rank;
};

// A position anywhere in the play. hand holds the cards not yet played: the cards already
// played to the current trick are in trick[0..played) and not in hand. The seats that have
// played to the trick hold one card fewer than the others. played == 0 is a trick start (a
// Position). A follower who did not follow suit must hold no card of the suit led.
struct PlayPosition {
  uint16_t hand[4][4];
  uint8_t trump;   // strain: 0..3 the suits, 4 no trumps
  uint8_t leader;  // the seat that led (or leads) the current trick
  uint8_t played;  // cards already played to the current trick, 0..3
  Card trick[3];
};

// North-South's tricks from p on, the current trick included, with best play; -1 if p is not a
// valid position. keep_table as for solve_position().
int solve_play(const PlayPosition& p, bool keep_table = false);

// 1 if North-South take at least `target` tricks from p on (the current trick included), 0 if
// not, -1 if p is not a valid position. One question, so cheaper than solve_play() (DDS's
// SolveBoard with a target): a claim of k tricks by North-South holds when at_least(p, k) is 1.
int at_least(const PlayPosition& p, int target, bool keep_table = false);

// Every legal card of the seat to play, with North-South's tricks (from p on, the current trick
// included) when that card is played and play is best afterwards (DDS's SolveBoard with
// solutions 3). out needs room for 13. Returns the number of cards (in suit order, high cards
// first), or -1 if p is not a valid position.
struct CardResult {
  Card card;
  uint8_t ns_tricks;
};
int card_results(const PlayPosition& p, CardResult* out, bool keep_table = false);

// Play analysis (DDS's AnalysePlayBin): from the trick-start position `start`, the cards
// cards[0..n) are played in order. ns_tricks[i] gets North-South's total tricks after the
// first i cards: those already won in the sequence plus best play from there (so
// ns_tricks[0] is the value of start, and a drop marks a mistake). Returns the number of cards
// played, which is less than n if cards[result] is not a legal play.
int analyse_play(const Position& start, const Card* cards, int n, uint8_t* ns_tricks);

// Par (DDS's DealerPar) from a table of solve_table(): the result of the auction when both
// sides know the table, starting with the dealer's side. Each side in turn bids any higher
// contract (declared by whichever partner takes more tricks in its strain) or passes;
// contracts that go down are doubled, and two passes in a row end the auction. dealer 0..3
// (N E S W); vulnerable 0 none, 1 both, 2 North-South, 3 East-West (DDS's codes).
struct ParContract {
  uint8_t level;      // 1..7
  uint8_t strain;     // 0..3 the suits (as Table), 4 no trumps
  uint8_t side;       // declaring side: 0 North-South, 1 East-West
  uint8_t declarers;  // bit q: seat q (of that side) can declare it for the par score
  int8_t result;      // overtricks (>= 0, undoubled) or undertricks (< 0, doubled)
};
struct ParResult {
  int score;            // North-South's par score (positive: North-South gain)
  int n;                // par contracts in c, cheapest first within each strain and side
  ParContract c[10];
};
void par(const Table& table, int dealer, int vulnerable, ParResult& result);

// The full table of one deal, or of many. On the calling thread. The search table is kept
// across the four declarers of each strain, so a table costs about half of 20 solve()s.
void solve_table(const Deal& deal, Table& table);
void solve_tables(const Deal* deals, size_t n, Table* tables);

// Counters per worker of the last parallel call (cycles and the like are 0 when the kernel
// does not allow hardware counters).
struct WorkerStats {
  int cpu = -1;
  size_t deals = 0;   // problems solved; in a table batch on two or more threads, strains (five a table)
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

// Like solve(), on a pool of worker threads, one per entry of cpus[0..ncpu) (-1: not placed).
// Each worker may run on any CPU of the list in the same NUMA node and core class (performance
// or efficiency) as its own entry, so the scheduler can move it away from a CPU shared with
// other work; DDFAST_AFFINITY=cpu pins each worker to its own CPU instead (the old behaviour),
// =none leaves placement to the scheduler. Work is handed out one problem at a time. The pool
// is kept between calls while the CPU list stays the same. Not reentrant (see above).
void solve_parallel(const Problem* problems, size_t n, uint8_t* tricks, const int* cpus, int ncpu,
                    BatchOrder order = BatchOrder::Predicted, std::vector<WorkerStats>* stats = nullptr);

// Like solve_tables(), on the same worker pool. A worker takes one strain of one deal at a
// time, the strains expected to be costliest first, so that the end of a batch does not leave
// workers idle.
void solve_tables_parallel(const Deal* deals, size_t n, Table* tables, const int* cpus, int ncpu,
                           std::vector<WorkerStats>* stats = nullptr);

// One problem searched on several cores at once, for lower latency on a single deal: the
// calling thread and one helper thread for each of cpus[1..ncpu) (placed as for
// solve_parallel) share the search of each question by root leads (and by second hand's
// replies to the first lead). Pin the calling
// thread to cpus[0] (pin_thread) for the best results. Returns declarer's tricks, exact like
// solve(). Helpers sleep between questions. Several of these calls (or other work) sharing the
// same CPUs slow each other down far more than solve() would: prefer solve_parallel() for
// batches, and solve_deal_parallel() only on otherwise idle cores (issue #4). Not reentrant,
// and not to be overlapped with solve_parallel() on the same CPUs. lead_nodes, if given: the
// lead nodes searched by all threads (compare stat_lead_nodes() for solve()).
int solve_deal_parallel(const Problem& p, const int* cpus, int ncpu, unsigned long long* lead_nodes = nullptr);

// Lead nodes (nodes at the start of a trick) searched so far by solve(), solve_position() and
// solve_table() on the calling thread, in any build.
unsigned long long stat_lead_nodes();

// Stops the pool's threads, both pools (optional; also done at exit).
void stop_pool();

// Pins the calling thread to one CPU; false if that fails.
bool pin_thread(int cpu);

// Two techniques from bridge-solver (Hanhong Xue; README), both on by default. They help single
// solves and full tables (24% fewer nodes, 13% fewer cycles), but cost card_results() with a kept
// table about 6% more cycles and 8% more instructions (the harness's all-moves batches), all of
// it from the cut-off cards. Process-wide: set before solving, not while another thread solves. Answers are the same
// either way; only the work changes.
struct Options {
  bool sure_trick_bounds = true;  // bs_bounds() after DDS's quick and later tricks
  bool cutoff_cards = true;       // the card that last cut off, tried first (from 8 cards a hand)
};
void set_options(const Options& options);
Options options();

// Nodes searched so far (lead and follow nodes, all threads) in a build with -DSTATS, which
// also prints search counters at exit: a fingerprint of the search's behaviour (the counter is
// not atomic, so count on one thread). 0 in other builds.
unsigned long long stat_nodes();

}  // namespace ddfast
