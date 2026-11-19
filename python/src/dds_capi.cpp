// DDS's legacy C interface (dll.h) on ddfast, built as a libdds replacement (libdds.so /
// libdds.dylib) for programs that load DDS by name, such as BGADLL (BEN's PIMC engine).
//
// Each call goes to ddfast_compat, which holds DDS's SolveBoard semantics (input checks and
// their order, return codes, special cases, the grouping of equivalent cards into `equals`).
// This file only converts the structs. It is linked against libddfast.so, which holds the
// solver, the endgame tables and the thread pool, so a process that also uses ddfast's Python
// module has one copy of the tables.
//
// Thread model: DDS 2.x takes a thread index (0 .. the count given to SetMaxThreads) and keeps
// a search table per index. ddfast keeps its search state per OS thread, so the index is not
// needed; any index is accepted and calls from any number of threads run in parallel.
// SetMaxThreads, SetResources and FreeMemory have nothing to set and do nothing.
//
// Covered: SetMaxThreads, SetResources, FreeMemory, SolveBoard, SolveBoardPBN, ErrorMessage.
// The tables are loaded on the first solve from $DDFAST_TABLES, else build/tables under the
// current directory (built there first if missing, as ddfast::init does).
//
// DDFAST_CAPI_STATS=<file> appends a summary of the solves to <file> when the process exits:
// calls, time in the solver (in all and by tricks left), the OS threads that called and the
// time of each thread's first call against later ones (ddfast sets up its search state per
// thread on the first call), and how many solves overlapped (busy wall time, mean and peak
// concurrency). Unset, the cost is one branch per call.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <api/dll.h>

#include "dds_compat.hpp"

namespace {

void to_future(const ddfast_compat::Future& f, FutureTricks* out) {
  out->nodes = f.nodes;
  out->cards = f.cards;
  for (int i = 0; i < 13; i++) {
    out->suit[i] = f.suit[i];
    out->rank[i] = f.rank[i];
    out->equals[i] = f.equals[i];
    out->score[i] = f.score[i];
  }
}

// ----- DDFAST_CAPI_STATS -----

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Stats {
  bool on = false;
  std::string path;
  std::mutex m;
  uint64_t calls = 0, errors = 0, threads = 0;
  double solve_ns = 0, first_ns = 0;      // all calls; each thread's first call
  uint64_t by_tricks_calls[14] = {};
  double by_tricks_ns[14] = {};
  int active = 0, max_active = 0;
  int64_t first_t = -1, last_t = 0, mark = 0;
  double busy_ns = 0, active_ns = 0;      // wall time with >= 1 solve running; integral of the count

  Stats() {
    const char* p = std::getenv("DDFAST_CAPI_STATS");
    if (p != nullptr && *p != '\0') {
      on = true;
      path = p;
    }
  }
  ~Stats() {
    if (on) report();
  }
  void advance(int64_t t) {  // with m held
    if (active > 0) {
      busy_ns += double(t - mark);
      active_ns += double(active) * double(t - mark);
    }
    mark = t;
  }
  void enter(int64_t t) {
    std::lock_guard<std::mutex> lk(m);
    if (first_t < 0) first_t = t;
    advance(t);
    if (++active > max_active) max_active = active;
  }
  void leave(int64_t t, int64_t d, bool first, int tricks, int rc) {
    std::lock_guard<std::mutex> lk(m);
    advance(t);
    active--;
    last_t = t;
    calls++;
    if (rc != RETURN_NO_FAULT) errors++;
    solve_ns += double(d);
    if (first) {
      threads++;
      first_ns += double(d);
    }
    if (tricks >= 0 && tricks <= 13) {
      by_tricks_calls[tricks]++;
      by_tricks_ns[tricks] += double(d);
    }
  }
  void report() {
    std::lock_guard<std::mutex> lk(m);
    FILE* f = std::fopen(path.c_str(), "a");
    if (f == nullptr) return;
    const double ms = 1e-6, us = 1e-3;
    const uint64_t later = calls - threads;
    std::fprintf(f, "ddfast libdds.so stats (pid %ld)\n", long(getpid_()));
    std::fprintf(f, "  calls            %llu (%llu errors)\n", (unsigned long long)calls, (unsigned long long)errors);
    std::fprintf(f, "  solve time       %.1f ms in all, %.1f us per call\n", solve_ns * ms,
                 calls ? solve_ns * us / double(calls) : 0.0);
    std::fprintf(f, "  threads          %llu distinct OS threads\n", (unsigned long long)threads);
    std::fprintf(f, "  first call       %.1f us per thread (%.1f ms in all)\n",
                 threads ? first_ns * us / double(threads) : 0.0, first_ns * ms);
    std::fprintf(f, "  later calls      %.1f us per call\n", later ? (solve_ns - first_ns) * us / double(later) : 0.0);
    std::fprintf(f, "  span             %.1f ms from first call to last\n",
                 first_t >= 0 ? double(last_t - first_t) * ms : 0.0);
    std::fprintf(f, "  busy             %.1f ms with at least one solve running\n", busy_ns * ms);
    std::fprintf(f, "  concurrency      %.2f mean while busy, %d peak\n", busy_ns > 0 ? active_ns / busy_ns : 0.0,
                 max_active);
    std::fprintf(f, "  tricks left      calls      us/call   ms in all\n");
    for (int t = 13; t >= 1; t--)
      if (by_tricks_calls[t])
        std::fprintf(f, "  %11d %9llu %12.1f %11.1f\n", t, (unsigned long long)by_tricks_calls[t],
                     by_tricks_ns[t] * us / double(by_tricks_calls[t]), by_tricks_ns[t] * ms);
    std::fclose(f);
  }
  static long getpid_();
};

Stats& stats() {
  static Stats s;
  return s;
}

thread_local bool t_seen = false;

// Times one solve when DDFAST_CAPI_STATS is set.
class Probe {
 public:
  explicit Probe(int tricks) : on_(stats().on), tricks_(tricks) {
    if (!on_) return;
    first_ = !t_seen;
    t_seen = true;
    t0_ = now_ns();
    stats().enter(t0_);
  }
  void done(int rc) {
    if (!on_) return;
    const int64_t t1 = now_ns();
    stats().leave(t1, t1 - t0_, first_, tricks_, rc);
  }

 private:
  bool on_;
  bool first_ = false;
  int tricks_;
  int64_t t0_ = 0;
};

int tricks_left_pbn(const char* pbn) {
  int n = 0;
  for (const char* c = pbn; *c != '\0'; c++)
    if (std::strchr("AKQJT98765432akqjt", *c) != nullptr) n++;
  return (n + 3) / 4;
}

int tricks_left_bin(const unsigned remain[4][4]) {
  int n = 0;
  for (int h = 0; h < 4; h++)
    for (int s = 0; s < 4; s++) n += __builtin_popcount(remain[h][s]);
  return (n + 3) / 4;
}

}  // namespace

#include <unistd.h>
long Stats::getpid_() { return long(::getpid()); }

auto STDCALL SetMaxThreads(int userThreads) -> void { (void)userThreads; }

auto STDCALL SetResources(int maxMemoryMB, int maxThreads) -> void {
  (void)maxMemoryMB;
  (void)maxThreads;
}

auto STDCALL FreeMemory() -> void {}

auto STDCALL SolveBoard(Deal dl, int target, int solutions, int mode, FutureTricks* futp, int threadIndex)
    -> int {
  (void)threadIndex;
  if (futp == nullptr) return RETURN_UNKNOWN_FAULT;
  ddfast_compat::ensure_init();
  Probe probe(stats().on ? tricks_left_bin(dl.remainCards) : 0);
  ddfast_compat::Future f;
  const int rc = ddfast_compat::solve_board(dl.remainCards, dl.trump, dl.first, dl.currentTrickSuit,
                                            dl.currentTrickRank, target, solutions, mode, f);
  probe.done(rc);
  to_future(f, futp);
  return rc;
}

auto STDCALL SolveBoardPBN(DealPBN dlpbn, int target, int solutions, int mode, FutureTricks* futp,
                           int thrId) -> int {
  (void)thrId;
  if (futp == nullptr) return RETURN_UNKNOWN_FAULT;
  // DDS reads remainCards as a C string within its 80 bytes.
  char pbn[81];
  std::memcpy(pbn, dlpbn.remainCards, 80);
  pbn[80] = '\0';
  ddfast_compat::ensure_init();
  Probe probe(stats().on ? tricks_left_pbn(pbn) : 0);
  ddfast_compat::Future f;
  const int rc = ddfast_compat::solve_board_pbn(pbn, dlpbn.trump, dlpbn.first, dlpbn.currentTrickSuit,
                                                dlpbn.currentTrickRank, target, solutions, mode, f);
  probe.done(rc);
  to_future(f, futp);
  return rc;
}

auto STDCALL ErrorMessage(int code, char line[80]) -> void {
  std::strncpy(line, ddfast_compat::error_text(code), 79);
  line[79] = '\0';
}
