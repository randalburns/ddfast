// Another solver behind DDS's C API (a libdds.so, such as bridge-solver's: see
// bench/get_bsolver.sh) against ddfast, and DDS itself, on the same deals. Every result is
// compared.
//
// usage: bench_libdds LIB tables  [deals=100] [seed=1]        one thread: CalcDDtable (DDS, LIB)
//                                                              against ddfast's solve_table
//        bench_libdds LIB problems [set file=tests/data/public.set]
//                                                              one thread: SolveBoard (LIB; target
//                                                              -1, solutions 1, opening lead) against
//                                                              ddfast's solve, checked against the
//                                                              set's DDS answers
//        bench_libdds LIB batch   [deals=500] [seed=1] [cpus]  CalcAllTables (LIB, SetMaxThreads =
//                                                              the CPU count, the process confined
//                                                              to the CPUs) against ddfast's
//                                                              solve_tables_parallel on the CPUs
//        bench_libdds LIB boards  [samples=200] [threads=8] [cpus] [seed=11] [positions=10]
//                                                              BEN's workload: `positions` positions
//                                                              at each of four stages, part-way
//                                                              through play, the two hidden hands
//                                                              re-dealt per sample, one batch per
//                                                              position and `solutions` (1 and 3,
//                                                              target -1, mode 1): LIB's
//                                                              SolveAllBoardsBin on its threads
//                                                              (as BEN's bsolver.py calls it),
//                                                              ddfast's DDS-compatible SolveBoard
//                                                              (python/src/dds_compat, behind
//                                                              solve_boards_pbn) on as many
//                                                              threads, and DDS's SolveAllBoardsBin;
//                                                              the process confined to the CPUs
// Run the one-thread modes pinned (taskset -c 0). NO_DDS=1 leaves DDS out of `tables` and
// `boards`.
//
// LIB is loaded with its own symbol scope (RTLD_LOCAL | RTLD_DEEPBIND), so its DDS functions do
// not collide with the DDS linked in here. Its structs are DDS 2.9's (as ddss and bridge-solver
// declare them), declared below. Times are wall clock; cycles are user-mode cycles summed over
// every thread of the process (counters opened on each thread after a warm-up call has started
// the library's thread pool), so work on another library's pool threads is counted, as is
// ddfast's on the calling thread or its workers.
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>

#include <sys/resource.h>

#include "common.hpp"
#include "dds_compat.hpp"

using namespace testing;

namespace lib {
// DDS 2.9's C API structs (ddss's dll.h, with its MAXNOOFTABLES of 1000).
constexpr int LIB_MAX_TABLES = 1000;
struct deal {
  int trump, first, currentTrickSuit[3], currentTrickRank[3];
  unsigned remainCards[4][4];
};
struct futureTricks {
  int nodes, cards, suit[13], rank[13], equals[13], score[13];
};
struct ddTableDeal {
  unsigned cards[4][4];
};
struct ddTableDeals {
  int noOfTables;
  ddTableDeal deals[LIB_MAX_TABLES * 5];
};
struct ddTableResults {
  int resTable[5][4];
};
struct ddTablesRes {
  int noOfBoards;
  ddTableResults results[LIB_MAX_TABLES * 5];
};
constexpr int LIB_MAX_BOARDS = LIB_MAX_TABLES * 5;
struct boards {
  int noOfBoards;
  deal deals[LIB_MAX_BOARDS];
  int target[LIB_MAX_BOARDS], solutions[LIB_MAX_BOARDS], mode[LIB_MAX_BOARDS];
};
struct solvedBoards {
  int noOfBoards;
  futureTricks solvedBoard[LIB_MAX_BOARDS];
};
typedef int (*SolveAllBoardsBinF)(boards*, solvedBoards*);
typedef void (*SetMaxThreadsF)(int);
typedef int (*SolveBoardF)(deal, int, int, int, futureTricks*, int);
typedef int (*CalcDDtableF)(ddTableDeal, ddTableResults*);
typedef int (*CalcAllTablesF)(ddTableDeals*, int, int*, ddTablesRes*, void*);
typedef void (*ErrorMessageF)(int, char*);
SetMaxThreadsF SetMaxThreads;
SolveAllBoardsBinF SolveAllBoardsBin;
SolveBoardF SolveBoard;
CalcDDtableF CalcDDtable;
CalcAllTablesF CalcAllTables;
ErrorMessageF ErrorMessage;

void load(const char* path) {
  void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h) {
    fprintf(stderr, "bench_libdds: %s\n", dlerror());
    exit(2);
  }
  auto sym = [&](const char* name) {
    void* p = dlsym(h, name);
    if (!p) {
      fprintf(stderr, "bench_libdds: %s has no %s\n", path, name);
      exit(2);
    }
    return p;
  };
  SetMaxThreads = reinterpret_cast<SetMaxThreadsF>(sym("SetMaxThreads"));
  SolveBoard = reinterpret_cast<SolveBoardF>(sym("SolveBoard"));
  CalcDDtable = reinterpret_cast<CalcDDtableF>(sym("CalcDDtable"));
  CalcAllTables = reinterpret_cast<CalcAllTablesF>(sym("CalcAllTables"));
  ErrorMessage = reinterpret_cast<ErrorMessageF>(sym("ErrorMessage"));
  SolveAllBoardsBin = reinterpret_cast<SolveAllBoardsBinF>(sym("SolveAllBoardsBin"));
}
void check(int ret, const char* what) {
  if (ret == 1) return;
  char line[80] = {};
  ErrorMessage(ret, line);
  fprintf(stderr, "bench_libdds: %s: %d %s\n", what, ret, line);
  exit(1);
}
ddTableDeal table_deal(const ddfast::Deal& d) {
  ddTableDeal t{};
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) t.cards[h][s] = unsigned(d.hand[h][s]) << 2;
  return t;
}
void to_table(const ddTableResults& r, ddfast::Table& t) {
  for (int s = 0; s < 5; s++)
    for (int h = 0; h < 4; h++) t.tricks[s][h] = uint8_t(r.resTable[s][h]);
}
}  // namespace lib

namespace {
// User-mode cycles summed over every thread of the process that exists when open() is called.
struct ProcessCycles {
  std::vector<int> fds;
  void open() {
    close();
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
#if defined(__linux__)
      perf_event_attr a{};
      a.type = PERF_TYPE_HARDWARE;
      a.size = sizeof a;
      a.config = PERF_COUNT_HW_CPU_CYCLES;
      a.exclude_kernel = 1;
      a.exclude_hv = 1;
      const int fd = int(syscall(SYS_perf_event_open, &a, atoi(e->d_name), -1, -1, 0));
      if (fd >= 0) fds.push_back(fd);
#endif
    }
    closedir(d);
  }
  long long read() const {
    long long sum = 0;
    for (int fd : fds) {
      long long v = perf_read(fd);
      if (v < 0) return -1;
      sum += v;
    }
    return fds.empty() ? -1 : sum;
  }
  void close() {
    for (int fd : fds) ::close(fd);
    fds.clear();
  }
};

// Times f() (wall seconds) and counts the process's cycles over it.
template <class F>
void timed(F&& f, double& secs, double& cycles) {
  ProcessCycles pc;
  pc.open();
  const long long c0 = pc.read();
  const double t0 = now();
  f();
  secs = now() - t0;
  const long long c1 = pc.read();
  cycles = c0 >= 0 && c1 >= 0 ? double(c1 - c0) : -1;
}

void report(const char* what, size_t n, const char* unit, double secs, double cycles, int bad, double base) {
  printf("%-34s %8.3f ms/%s  %8.1f %s/s", what, 1e3 * secs / double(n), unit, double(n) / secs, unit);
  if (cycles > 0) printf("  %7.2fM cycles/%s", cycles / 1e6 / double(n), unit);
  if (base > 0 && cycles > 0) printf("  %5.2fx", cycles / base);
  printf("  %s\n", bad < 0 ? "" : bad ? "DIFFER" : "same");
}

int tables(size_t n, unsigned long long seed) {
  const auto deals = random_deals(n, seed);
  const bool with_dds = !getenv("NO_DDS") || !*getenv("NO_DDS") || !strcmp(getenv("NO_DDS"), "0");
  std::vector<ddfast::Table> ref(n), fast(n), other(n), dds(n);
  double secs, cycles, fast_cycles;
  init_or_exit();
  ddfast::solve_table(deals[0], fast[0]);  // warm-up
  timed([&] { for (size_t k = 0; k < n; k++) ddfast::solve_table(deals[k], fast[k]); }, secs, fast_cycles);
  report("ddfast solve_table, 1 thread", n, "table", secs, fast_cycles, -1, 0);
  lib::SetMaxThreads(1);
  {
    lib::ddTableResults r{};
    lib::check(lib::CalcDDtable(lib::table_deal(deals[0]), &r), "CalcDDtable");  // warm-up: starts the pool
  }
  timed(
      [&] {
        for (size_t k = 0; k < n; k++) {
          lib::ddTableResults r{};
          lib::check(lib::CalcDDtable(lib::table_deal(deals[k]), &r), "CalcDDtable");
          lib::to_table(r, other[k]);
        }
      },
      secs, cycles);
  auto differ = [&](const std::vector<ddfast::Table>& a) {
    int bad = 0;
    for (size_t k = 0; k < n; k++) bad += memcmp(a[k].tricks, fast[k].tricks, sizeof a[k].tricks) != 0;
    return bad;
  };
  const int bad_other = differ(other);
  report("LIB CalcDDtable, 1 thread", n, "table", secs, cycles, bad_other, fast_cycles);
  int bad_dds = 0;
  if (with_dds) {
    SetMaxThreads(1);
    timed([&] { for (size_t k = 0; k < n; k++) dds_table(deals[k], dds[k]); }, secs, cycles);
    bad_dds = differ(dds);
    report("DDS CalcDDtable, 1 thread", n, "table", secs, cycles, bad_dds, fast_cycles);
  }
  printf("(x: cycles against ddfast's; \"same\": every table equals ddfast's)\n");
  return bad_other || bad_dds ? 1 : 0;
}

int problems(const char* path) {
  TestSet set;
  if (!read_set(path, set)) {
    fprintf(stderr, "bench_libdds: cannot read %s\n", path);
    return 2;
  }
  const size_t n = set.problems.size();
  std::vector<uint8_t> fast(n), other(n);
  double secs, cycles, fast_cycles;
  init_or_exit();
  ddfast::solve(set.problems.data(), 1, fast.data());  // warm-up
  timed([&] { ddfast::solve(set.problems.data(), n, fast.data()); }, secs, fast_cycles);
  int bad = 0;
  for (size_t i = 0; i < n; i++) bad += fast[i] != set.answers[i];
  report("ddfast solve, 1 thread", n, "deal", secs, fast_cycles, bad, 0);
  lib::SetMaxThreads(1);
  auto solve_one = [&](size_t i) {
    const auto& p = set.problems[i];
    lib::deal d{};
    d.trump = p.trump;
    d.first = (p.declarer + 1) & 3;
    for (int h = 0; h < 4; h++)
      for (int s = 0; s < 4; s++) d.remainCards[h][s] = unsigned(p.hand[h][s]) << 2;
    lib::futureTricks f{};
    lib::check(lib::SolveBoard(d, -1, 1, 1, &f, 0), "SolveBoard");
    other[i] = uint8_t(13 - f.score[0]);  // the score is the side on lead's
  };
  solve_one(0);  // warm-up
  timed([&] { for (size_t i = 0; i < n; i++) solve_one(i); }, secs, cycles);
  int bad_other = 0;
  for (size_t i = 0; i < n; i++) bad_other += other[i] != set.answers[i];
  report("LIB SolveBoard, 1 thread", n, "deal", secs, cycles, bad_other, fast_cycles);
  printf("(x: cycles against ddfast's; \"same\": every answer equals DDS's in %s)\n", path);
  return bad || bad_other ? 1 : 0;
}

int batch(size_t n, unsigned long long seed, std::vector<int> cpus) {
  if (cpus.empty()) cpus = ddfast::available_cpus();
  const auto deals = random_deals(n, seed);
  std::vector<ddfast::Table> fast(n), other(n);
  double secs, cycles, fast_cycles;
  // The library's pool threads inherit the process's CPUs: confine it to the list first.
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : cpus) CPU_SET(c, &set);
  sched_setaffinity(0, sizeof set, &set);
  init_or_exit();
  ddfast::solve_tables_parallel(deals.data(), n < 16 ? n : 16, fast.data(), cpus.data(), int(cpus.size()));  // warm-up
  timed([&] { ddfast::solve_tables_parallel(deals.data(), n, fast.data(), cpus.data(), int(cpus.size())); }, secs,
        fast_cycles);
  char what[80];
  snprintf(what, sizeof what, "ddfast solve_tables_parallel, %zu thr", cpus.size());
  report(what, n, "table", secs, fast_cycles, -1, 0);
  ddfast::stop_pool();
  lib::SetMaxThreads(int(cpus.size()));
  auto* in = new lib::ddTableDeals;
  auto* out = new lib::ddTablesRes;
  int filter[5] = {0, 0, 0, 0, 0};
  auto run = [&](size_t first, size_t count) {
    in->noOfTables = int(count);
    for (size_t k = 0; k < count; k++) in->deals[k] = lib::table_deal(deals[first + k]);
    lib::check(lib::CalcAllTables(in, -1, filter, out, nullptr), "CalcAllTables");
    for (size_t k = 0; k < count; k++) lib::to_table(out->results[k], other[first + k]);
  };
  run(0, n < 16 ? n : 16);  // warm-up: starts the pool
  timed(
      [&] {
        for (size_t first = 0; first < n; first += lib::LIB_MAX_TABLES)
          run(first, std::min(n - first, size_t(lib::LIB_MAX_TABLES)));
      },
      secs, cycles);
  int bad = 0;
  for (size_t k = 0; k < n; k++) bad += memcmp(other[k].tricks, fast[k].tricks, sizeof other[k].tricks) != 0;
  snprintf(what, sizeof what, "LIB CalcAllTables, %zu threads", cpus.size());
  report(what, n, "table", secs, cycles, bad, fast_cycles);
  printf("(cycles summed over all threads; x: against ddfast's; \"same\": every table equals ddfast's)\n");
  return bad ? 1 : 0;
}
// ---- boards: BEN's workload ----------------------------------------------------------------

// A position part-way through play, in DDS's binary layout (rank r at bit r).
struct BPos {
  unsigned remain[4][4];
  int trump, first, ts[3], tr[3];
};

// A random deal played out at random (following suit) for `cards` cards.
BPos played_out(std::mt19937_64& rng, int cards) {
  auto R = [&](int n) { return int(rng() % uint64_t(n)); };
  int deck[52];
  std::iota(deck, deck + 52, 0);
  std::shuffle(deck, deck + 52, rng);
  BPos p{};
  for (int i = 0; i < 52; i++) p.remain[i / 13][deck[i] / 13] |= 1u << (deck[i] % 13 + 2);
  p.trump = R(5);
  int leader = R(4), played = 0, ts[4], tr[4];
  for (int c = 0; c < cards; c++) {
    const int seat = (leader + played) & 3;
    int s;
    if (played > 0 && p.remain[seat][ts[0]]) s = ts[0];
    else do s = R(4); while (!p.remain[seat][s]);
    unsigned h = p.remain[seat][s];
    for (int k = R(__builtin_popcount(h)); k--;) h &= h - 1;
    const int r = __builtin_ctz(h);
    p.remain[seat][s] &= ~(1u << r);
    ts[played] = s, tr[played] = r;
    if (++played == 4) {
      int best = 0;
      for (int j = 1; j < 4; j++) {
        const bool ruff = p.trump < 4 && ts[j] == p.trump && ts[best] != p.trump;
        if (ruff || (ts[j] == ts[best] && tr[j] > tr[best])) best = j;
      }
      leader = (leader + best) & 3;
      played = 0;
    }
  }
  p.first = leader;
  for (int k = 0; k < 3; k++) p.ts[k] = k < played ? ts[k] : 0, p.tr[k] = k < played ? tr[k] : 0;
  return p;
}

// `n` samples of p: the cards of the two seats other than the one to play and its partner
// re-dealt between them, each keeping its number of cards (as BEN samples the hidden hands).
std::vector<BPos> samples(std::mt19937_64& rng, const BPos& p, int to_play, size_t n) {
  const int h0 = (to_play + 1) & 3, h1 = (to_play + 3) & 3;
  std::vector<int> pool;
  int k0 = 0;
  for (int s = 0; s < 4; s++)
    for (int r = 2; r <= 14; r++) {
      if (p.remain[h0][s] >> r & 1) pool.push_back(16 * s + r), k0++;
      if (p.remain[h1][s] >> r & 1) pool.push_back(16 * s + r);
    }
  std::vector<BPos> out(n, p);
  for (auto& q : out) {
    std::shuffle(pool.begin(), pool.end(), rng);
    for (int s = 0; s < 4; s++) q.remain[h0][s] = q.remain[h1][s] = 0;
    for (size_t i = 0; i < pool.size(); i++) q.remain[int(i) < k0 ? h0 : h1][pool[i] / 16] |= 1u << (pool[i] % 16);
  }
  return out;
}

// What the answers of the three solvers are compared on: the number of cards and their scores
// in order (the order of equal cards, and with solutions 1 which card, may differ).
struct Answer {
  int cards, score[13];
  bool operator==(const Answer& o) const {
    if (cards != o.cards) return false;
    for (int i = 0; i < cards; i++)
      if (score[i] != o.score[i]) return false;
    return true;
  }
};
template <class F>
Answer answer_of(const F& f) {
  Answer a{};
  a.cards = f.cards;
  for (int i = 0; i < f.cards && i < 13; i++) a.score[i] = f.score[i];
  return a;
}

double cpu_seconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + 1e-6 * double(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}

struct FastJob {
  const std::vector<BPos>* ps;
  int sol;
  std::vector<Answer>* out;
};
void fast_one(size_t i, void* arg) {
  auto& j = *static_cast<FastJob*>(arg);
  const BPos& p = (*j.ps)[i];
  ddfast_compat::Future f;
  if (ddfast_compat::solve_board(p.remain, p.trump, p.first, p.ts, p.tr, -1, j.sol, 1, f) != 1) {
    fprintf(stderr, "bench_libdds: ddfast solve_board failed\n");
    exit(1);
  }
  (*j.out)[i] = answer_of(f);
}

int boards(size_t n, int threads, std::vector<int> cpus, unsigned long long seed, int positions) {
  if (cpus.empty()) cpus = ddfast::available_cpus();
  if (threads <= 0) threads = int(cpus.size());
  const bool with_dds = !getenv("NO_DDS") || !*getenv("NO_DDS") || !strcmp(getenv("NO_DDS"), "0");
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : cpus) CPU_SET(c, &set);
  sched_setaffinity(0, sizeof set, &set);
  ddfast_compat::ensure_init();
  lib::SetMaxThreads(threads);
  SetMaxThreads(threads);
  auto* lb = new lib::boards;
  auto* ls = new lib::solvedBoards;
  static Boards db;
  static SolvedBoards ds;
  std::mt19937_64 rng(seed);
  printf("%d positions a row, each a batch of %zu samples; %d threads on %zu CPUs; ms per batch (CPU ms per\n"
         "batch in brackets), best of 3\n", positions, n, threads, cpus.size());
  printf("%-22s %3s %18s %18s %18s %8s %8s\n", "position", "sol", "ddfast", "LIB", with_dds ? "DDS" : "", "LIB/ddf",
         with_dds ? "DDS/ddf" : "");
  int bad = 0;
  double sum_fast = 0, sum_lib = 0, sum_dds = 0;
  for (auto [tricks, extra] : {std::pair{0, 1}, {3, 0}, {6, 2}, {9, 1}}) {
    // `positions` positions at this stage, each a batch of n samples.
    std::vector<std::vector<BPos>> sets;
    for (int k = 0; k < positions; k++) {
      const BPos p = played_out(rng, 4 * tricks + extra);
      sets.push_back(samples(rng, p, (p.first + extra) & 3, n));
    }
    const size_t total = n * sets.size();
    for (int sol : {1, 3}) {
      std::vector<Answer> fast(total), other(total), dds(total);
      auto run_fast = [&] {
        for (size_t k = 0; k < sets.size(); k++) {
          std::vector<Answer> out(n);
          FastJob j{&sets[k], sol, &out};
          ddfast_compat::parallel_for(n, threads, fast_one, &j);
          std::copy(out.begin(), out.end(), fast.begin() + long(k * n));
        }
      };
      auto run_lib = [&] {
        for (size_t k = 0; k < sets.size(); k++) {
          const auto& ps = sets[k];
          for (size_t i0 = 0; i0 < n; i0 += lib::LIB_MAX_BOARDS) {
            const int m = int(std::min<size_t>(lib::LIB_MAX_BOARDS, n - i0));
            lb->noOfBoards = m;
            for (int b = 0; b < m; b++) {
              const BPos& q = ps[i0 + size_t(b)];
              lib::deal& d = lb->deals[b];
              d.trump = q.trump, d.first = q.first;
              for (int t = 0; t < 3; t++) d.currentTrickSuit[t] = q.ts[t], d.currentTrickRank[t] = q.tr[t];
              memcpy(d.remainCards, q.remain, sizeof d.remainCards);
              lb->target[b] = -1, lb->solutions[b] = sol, lb->mode[b] = 1;
            }
            lib::check(lib::SolveAllBoardsBin(lb, ls), "SolveAllBoardsBin");
            for (int b = 0; b < m; b++) other[k * n + i0 + size_t(b)] = answer_of(ls->solvedBoard[b]);
          }
        }
      };
      auto run_dds = [&] {
        for (size_t k = 0; k < sets.size(); k++) {
          const auto& ps = sets[k];
          for (size_t i0 = 0; i0 < n; i0 += MAXNOOFBOARDS) {
            const int m = int(std::min<size_t>(MAXNOOFBOARDS, n - i0));
            db.no_of_boards = m;
            for (int b = 0; b < m; b++) {
              const BPos& q = ps[i0 + size_t(b)];
              Deal& d = db.deals[b];
              d = Deal{};
              d.trump = q.trump, d.first = q.first;
              for (int t = 0; t < 3; t++) d.currentTrickSuit[t] = q.ts[t], d.currentTrickRank[t] = q.tr[t];
              memcpy(d.remainCards, q.remain, sizeof d.remainCards);
              db.target[b] = -1, db.solutions[b] = sol, db.mode[b] = 1;
            }
            if (SolveAllBoardsBin(&db, &ds) != 1) {
              fprintf(stderr, "bench_libdds: DDS SolveAllBoardsBin failed\n");
              exit(1);
            }
            for (int b = 0; b < m; b++) dds[k * n + i0 + size_t(b)] = answer_of(ds.solved_board[b]);
          }
        }
      };
      // Best of 3, the solvers alternating.
      double tf = 1e30, tl = 1e30, td = 1e30, cf = 0, cl = 0, cd = 0;
      for (int rep = 0; rep < 3; rep++) {
        auto best = [](auto&& f, double& t, double& c) {
          const double c0 = cpu_seconds(), t0 = now();
          f();
          const double dt = now() - t0, dc = cpu_seconds() - c0;
          if (dt < t) t = dt, c = dc;
        };
        best(run_fast, tf, cf);
        best(run_lib, tl, cl);
        if (with_dds) best(run_dds, td, cd);
      }
      int b = 0;
      for (size_t i = 0; i < total; i++) b += !(other[i] == fast[i]) + (with_dds && !(dds[i] == fast[i]));
      bad += b;
      sum_fast += tf, sum_lib += tl, sum_dds += td;
      const double per = 1e3 / double(sets.size());  // ms per batch
      char what[40], f1[32], f2[32], f3[32];
      snprintf(what, sizeof what, "trick %d, %d in trick", tricks + 1, extra);
      snprintf(f1, sizeof f1, "%.2f (%.1f)", per * tf, per * cf);
      snprintf(f2, sizeof f2, "%.2f (%.1f)", per * tl, per * cl);
      snprintf(f3, sizeof f3, "%.2f (%.1f)", per * td, per * cd);
      printf("%-22s %3d %18s %18s %18s %7.2fx", what, sol, f1, f2, with_dds ? f3 : "", tl / tf);
      if (with_dds) printf(" %7.2fx", td / tf);
      printf("  %s\n", b ? "DIFFER" : "same");
    }
  }
  printf("all batches: LIB/ddfast %.2fx", sum_lib / sum_fast);
  if (with_dds) printf(", DDS/ddfast %.2fx", sum_dds / sum_fast);
  printf(" (wall time); \"same\": every board's scores agree\n");
  return bad ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: bench_libdds LIB tables [deals] [seed] | problems [set file] | batch [deals] [seed] [cpus] | boards [samples] [threads] [cpus] [seed] [positions]\n");
    return 2;
  }
  lib::load(argv[1]);
  const std::string mode = argv[2];
  auto arg = [&](int i, const char* def) { return argc > i ? argv[i] : def; };
  if (mode == "tables") return tables(size_t(atol(arg(3, "100"))), strtoull(arg(4, "1"), nullptr, 10));
  if (mode == "problems") return problems(arg(3, "tests/data/public.set"));
  if (mode == "batch")
    return batch(size_t(atol(arg(3, "500"))), strtoull(arg(4, "1"), nullptr, 10),
                 argc > 5 ? parse_cpus(argv[5]) : std::vector<int>{});
  if (mode == "boards")
    return boards(size_t(atol(arg(3, "200"))), atoi(arg(4, "8")), argc > 5 ? parse_cpus(argv[5]) : std::vector<int>{},
                  strtoull(arg(6, "11"), nullptr, 10), atoi(arg(7, "10")));
  fprintf(stderr, "bench_libdds: unknown mode %s\n", mode.c_str());
  return 2;
}
