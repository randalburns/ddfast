// bprof: a sampling profiler for ddfast without perf. Linux x86-64 only.
//
// Solves a test set with ddfast::solve on one thread and samples the instruction pointer on
// every `period`-th hardware event (branch misses by default, or cycles) through a
// perf_event_open overflow signal. Prints per-deal totals (branch misses, branches, cycles)
// to stderr and an "address count" histogram to stdout; tools/prof/sym.py maps it to source
// lines. Needs only user-space counters (kernel.perf_event_paranoid <= 2).
//
//   bprof SET [bm|cyc|none] [period]
//     bm    sample branch misses (default)
//     cyc   sample cycles
//     none  no sampling: just the exact per-deal counts (low noise; good for A/B screening)
//
// Build and use: see tools/prof/README.md.
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <ucontext.h>

#include <map>
#include <string>

#include "common.hpp"
using namespace testing;

static uint64_t g_ips[1 << 22];
static volatile size_t g_n = 0;
static void handler(int, siginfo_t*, void* uc) {
  const size_t n = g_n;
  if (n < (1u << 22)) {
    g_ips[n] = ((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
    g_n = n + 1;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: bprof SET [bm|cyc|none] [period]\n");
    return 2;
  }
  TestSet s;
  if (!read_set(argv[1], s)) return 1;
  const std::string mode = argc > 2 ? argv[2] : "bm";
  // cyc: cycles; l1m: L1 data-cache read misses; cm: cache misses (as the kernel maps them); else branch misses.
  const bool hwc = mode == "l1m";
  const unsigned long long ev = mode == "cyc" ? PERF_COUNT_HW_CPU_CYCLES
                                : hwc        ? (PERF_COUNT_HW_CACHE_L1D | PERF_COUNT_HW_CACHE_OP_READ << 8 | PERF_COUNT_HW_CACHE_RESULT_MISS << 16)
                                : mode == "cm" ? PERF_COUNT_HW_CACHE_MISSES
                                               : PERF_COUNT_HW_BRANCH_MISSES;
  const unsigned long long period = argc > 3 ? atoll(argv[3]) : 2003;
  const bool prof = mode != "none";
  init_or_exit();
  std::vector<uint8_t> got(s.problems.size());
  ddfast::solve(s.problems.data(), std::min<size_t>(20, s.problems.size()), got.data());  // warm-up

  struct sigaction sa{};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGIO, &sa, nullptr);
  perf_event_attr a{};
  a.type = hwc ? PERF_TYPE_HW_CACHE : PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = ev;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  a.sample_period = period;
  a.wakeup_events = 1;
  a.disabled = 1;
  const int fd = prof ? int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0)) : -1;
  if (prof && fd < 0) {
    perror("perf_event_open");
    return 1;
  }
  if (prof) {  // deliver the overflow signal to this thread
    fcntl(fd, F_SETFL, O_ASYNC);
    fcntl(fd, F_SETSIG, SIGIO);
    f_owner_ex o{F_OWNER_TID, (pid_t)syscall(SYS_gettid)};
    fcntl(fd, F_SETOWN_EX, &o);
  }
  const int fb = perf_open(PERF_COUNT_HW_BRANCH_MISSES), fbr = perf_open(PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
            fc = perf_open(PERF_COUNT_HW_CPU_CYCLES);
  const long long b0 = perf_read(fb), br0 = perf_read(fbr), c0 = perf_read(fc);
  if (prof) ioctl(fd, PERF_EVENT_IOC_REFRESH, 1 << 30), ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  ddfast::solve(s.problems.data(), s.problems.size(), got.data());
  if (prof) ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

  const double n = double(s.problems.size());
  size_t wrong = 0;
  for (size_t i = 0; i < s.problems.size(); i++) wrong += got[i] != s.answers[i];
  fprintf(stderr, "per deal: branch-misses %.0f branches %.0f cycles %.0f samples %zu %s\n",
          (perf_read(fb) - b0) / n, (perf_read(fbr) - br0) / n, (perf_read(fc) - c0) / n, (size_t)g_n,
          wrong ? "WRONG" : "ok");
  std::map<uint64_t, size_t> h;
  for (size_t i = 0; i < g_n; i++) h[g_ips[i]]++;
  for (auto& [ip, c] : h) printf("%lx %zu\n", (unsigned long)ip, c);
  return wrong != 0;
}
