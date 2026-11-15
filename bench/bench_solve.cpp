// Times ddfast on a test set or on random deals, on one thread with hardware counters (the
// measure used throughout docs/NOTES.md: cycles per deal are far less noisy than time), or
// in parallel. Every answer is checked against the set's answers or DDS's.
//
// usage: bench_solve set FILE [count]                one thread (pin it: taskset -c 0 ...)
//        bench_solve random N SEED                   one thread; DDS answers computed first
//        bench_solve parallel FILE [cpus] [reps]     solve_parallel on cpus (default auto)
//
// Output: cycles and instructions per deal (when the kernel allows counters), deals/s, and
// with a -DSTATS build the nodes searched.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.hpp"

using namespace testing;

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: bench_solve set FILE [count] | random N SEED | parallel FILE [cpus] [reps]\n");
    return 2;
  }
  const std::string mode = argv[1];
  TestSet s;
  if (mode == "set" || mode == "parallel") {
    if (!read_set(argv[2], s)) {
      fprintf(stderr, "cannot read %s\n", argv[2]);
      return 2;
    }
    if (mode == "set" && argc > 3 && size_t(atol(argv[3])) < s.problems.size()) {
      s.problems.resize(size_t(atol(argv[3])));
      s.answers.resize(s.problems.size());
    }
  } else if (mode == "random") {
    SetMaxThreads(1);
    s.problems = random_problems(size_t(atol(argv[2])), argc > 3 ? unsigned(atol(argv[3])) : 1);
    for (auto& p : s.problems) s.answers.push_back(uint8_t(dds_solve(p)));
  } else {
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
  }
  init_or_exit();
  const size_t n = s.problems.size();
  std::vector<uint8_t> got(n, 0xFF);
  if (mode == "parallel") {
    const std::vector<int> cpus = parse_cpus(argc > 3 ? argv[3] : "auto");
    const int reps = argc > 4 ? atoi(argv[4]) : 3;
    ddfast::solve_parallel(s.problems.data(), n < 64 ? n : 64, got.data(), cpus.data(), int(cpus.size()));  // warm-up
    size_t wrong = 0;
    for (int r = 0; r < reps; r++) {
      memset(got.data(), 0xFF, n);
      std::vector<ddfast::WorkerStats> st;
      const double t0 = now();
      ddfast::solve_parallel(s.problems.data(), n, got.data(), cpus.data(), int(cpus.size()), ddfast::BatchOrder::Predicted, &st);
      const double secs = now() - t0;
      for (size_t i = 0; i < n; i++) wrong += got[i] != s.answers[i];
      printf("parallel, %zu threads: %.1f deals/s\n", cpus.size(), n / secs);
    }
    ddfast::stop_pool();
    printf("%s\n", wrong ? "WRONG" : "all correct");
    return wrong ? 1 : 0;
  }
  const int fc = perf_open(PERF_COUNT_HW_CPU_CYCLES), fi = perf_open(PERF_COUNT_HW_INSTRUCTIONS);
  const double t0 = now();
  ddfast::solve(s.problems.data(), n, got.data());
  const double secs = now() - t0;
  const long long cyc = perf_read(fc), ins = perf_read(fi);
  if (cyc >= 0 && ins >= 0) printf("cycles/deal %.0f  instructions/deal %.0f\n", double(cyc) / n, double(ins) / n);
  size_t wrong = 0;
  for (size_t i = 0; i < n; i++) {
    if (got[i] != s.answers[i] && wrong < 5) fprintf(stderr, "  deal %zu: got %d, reference %d\n", i, got[i], s.answers[i]);
    wrong += got[i] != s.answers[i];
  }
  printf("%zu deals: %.1f deals/s (%.0f us each)", n, n / secs, 1e6 * secs / n);
  if (ddfast::stat_nodes()) printf("  nodes %llu (%.0f per deal)", ddfast::stat_nodes(), double(ddfast::stat_nodes()) / n);
  printf("  %s\n", wrong ? "WRONG" : "all correct");
  return wrong ? 1 : 0;
}
