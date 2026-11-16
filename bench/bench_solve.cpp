// Times ddfast on a test set or on random deals, on one thread with hardware counters (the
// measure used throughout docs/NOTES.md: cycles per deal are far less noisy than time), or
// in parallel. Every answer is checked against the set's answers or DDS's.
//
// usage: bench_solve set FILE [count]                one thread (pin it: taskset -c 0 ...)
//        bench_solve random N SEED                   one thread; DDS answers computed first
//        bench_solve parallel FILE [cpus] [reps]     solve_parallel on cpus (default auto)
//        bench_solve deal FILE reps CFG...           latency of one deal: each CFG is "1t" (solve()
//                                                    on this thread) or a cpu list for
//                                                    solve_deal_parallel (this thread pinned to its
//                                                    first cpu); configurations alternate pass by
//                                                    pass, the best of reps runs per deal is kept
//
// Output: cycles and instructions per deal (when the kernel allows counters), deals/s, and
// with a -DSTATS build the nodes searched.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "common.hpp"

using namespace testing;

// Latency per deal of several configurations (see the usage above).
int bench_deal(const TestSet& s, int reps, const std::vector<std::string>& cfgs) {
  const size_t n = s.problems.size(), nc = cfgs.size();
  std::vector<std::vector<int>> cpus(nc);
  for (size_t k = 0; k < nc; k++) cpus[k] = cfgs[k] == "1t" ? std::vector<int>{} : parse_cpus(cfgs[k]);
  const int fc = perf_open(PERF_COUNT_HW_CPU_CYCLES);
  // Per configuration and deal: best time; lead nodes and cycles (this thread) of that run.
  std::vector<std::vector<double>> best(nc, std::vector<double>(n, 1e30)), cyc(nc, std::vector<double>(n, 0));
  std::vector<std::vector<unsigned long long>> nodes(nc, std::vector<unsigned long long>(n, 0));
  size_t wrong = 0;
  for (int r = 0; r < reps; r++)
    for (size_t k = 0; k < nc; k++) {
      const size_t kk = (k + size_t(r)) % nc;  // rotate the order of the configurations
      const auto& cp = cpus[kk];
      ddfast::pin_thread(cp.empty() ? 0 : cp[0]);
      uint8_t w;
      if (!cp.empty()) ddfast::solve_deal_parallel(s.problems[0], cp.data(), int(cp.size()));  // start the pool
      else ddfast::solve(&s.problems[0], 1, &w);
      for (size_t i = 0; i < n; i++) {
        uint8_t got = 0xFF;
        unsigned long long ln = 0;
        const long long c0 = perf_read(fc);
        const double t0 = now();
        if (cp.empty()) {
          const unsigned long long l0 = ddfast::stat_lead_nodes();
          ddfast::solve(&s.problems[i], 1, &got);
          ln = ddfast::stat_lead_nodes() - l0;
        } else {
          got = uint8_t(ddfast::solve_deal_parallel(s.problems[i], cp.data(), int(cp.size()), &ln));
        }
        const double t = now() - t0;
        const long long c1 = perf_read(fc);
        if (got != s.answers[i]) {
          if (wrong < 5) fprintf(stderr, "  deal %zu cfg %s: got %d, reference %d\n", i, cfgs[kk].c_str(), got, s.answers[i]);
          wrong++;
        }
        if (t < best[kk][i]) best[kk][i] = t, nodes[kk][i] = ln, cyc[kk][i] = c0 < 0 ? 0 : double(c1 - c0);
      }
    }
  // Deals by the first configuration's time, costliest first.
  std::vector<size_t> ord(n);
  std::iota(ord.begin(), ord.end(), size_t(0));
  std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return best[0][a] > best[0][b]; });
  const size_t parts[3] = {n, std::max<size_t>(1, n / 10), std::max<size_t>(1, n / 100)};
  const char* names[3] = {"all", "top10%", "top1%"};
  printf("%zu deals, best of %d; times in ms per deal; speedup and nodes against %s\n", n, reps, cfgs[0].c_str());
  for (size_t k = 0; k < nc; k++) {
    printf("%-14s", cfgs[k].c_str());
    for (int q = 0; q < 3; q++) {
      double t = 0, t0 = 0, nd = 0, nd0 = 0;
      for (size_t j = 0; j < parts[q]; j++) {
        const size_t i = ord[j];
        t += best[k][i], t0 += best[0][i], nd += double(nodes[k][i]), nd0 += double(nodes[0][i]);
      }
      printf("  %s %.3f ms x%.2f nodes %.3f", names[q], 1e3 * t / double(parts[q]), t0 / t, nd0 > 0 ? nd / nd0 : 0.0);
    }
    double ct = 0, tt = 0;
    for (size_t i = 0; i < n; i++) ct += cyc[k][i], tt += best[k][i];
    double c0 = 0;
    for (size_t i = 0; i < n; i++) c0 += cyc[0][i];
    // Cycles of this thread per deal (the clock-independent latency) and the speedup in cycles.
    printf("  GHz(caller) %.2f  Mcycles/deal %.2f x%.2f\n", ct / tt * 1e-9, ct / double(n) * 1e-6, c0 / ct);
  }
  ddfast::stop_pool();
  printf("%s\n", wrong ? "WRONG" : "all correct");
  return wrong ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "deal") {
    TestSet s;
    if (!read_set(argv[2], s)) {
      fprintf(stderr, "cannot read %s\n", argv[2]);
      return 2;
    }
    init_or_exit();
    return bench_deal(s, atoi(argv[3]), std::vector<std::string>(argv + 4, argv + argc));
  }
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
  const long long cyc0 = perf_read(fc), ins0 = perf_read(fi);
  const double t0 = now();
  ddfast::solve(s.problems.data(), n, got.data());
  const double secs = now() - t0;
  const long long cyc = cyc0 < 0 ? -1 : perf_read(fc) - cyc0, ins = ins0 < 0 ? -1 : perf_read(fi) - ins0;
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
