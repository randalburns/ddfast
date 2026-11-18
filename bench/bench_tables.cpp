// Full tables: DDS CalcDDtable against ddfast's solve_tables and solve_tables_parallel, on
// random deals. Every result is compared with DDS's.
//
// usage: bench_tables [deals] [seed] [cpus for the parallel run, e.g. 0-3,12-15; default auto]
//        DDFAST_ONLY=1 bench_tables ...: ddfast only, no DDS (cycles per table on one thread,
//        tables/s and cycles per table on the parallel workers, a hash of all results, and
//        nodes per table in a STATS build); DDFAST_ONLY=2: the parallel run only
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.hpp"

using namespace testing;

int main(int argc, char** argv) {
  const size_t n = argc > 1 ? size_t(atol(argv[1])) : 200;
  const unsigned long long seed = argc > 2 ? strtoull(argv[2], nullptr, 10) : 4242;
  const std::vector<int> cpus = parse_cpus(argc > 3 ? argv[3] : "auto");
  const auto deals = random_deals(n, seed);
  if (getenv("DDFAST_ONLY")) {
    init_or_exit();
    std::vector<ddfast::Table> tab(n), tab2(n);
    const int fc = perf_open(PERF_COUNT_HW_CPU_CYCLES);
    const unsigned long long n0 = ddfast::stat_nodes();
    const long long c0 = perf_read(fc);
    double t = now();
    const bool par_only = getenv("DDFAST_ONLY")[0] == '2';  // DDFAST_ONLY=2: the parallel run only
    if (!par_only) ddfast::solve_tables(deals.data(), n, tab.data());
    const double t1 = now() - t;
    const long long c1 = perf_read(fc) - c0;
    const unsigned long long nn = ddfast::stat_nodes() - n0;
    unsigned long long h = 1469598103934665603ull;
    for (auto& x : tab)
      for (auto& r : x.tricks)
        for (auto v : r) h = (h ^ v) * 1099511628211ull;
    printf("1 thread: %.1f tables/s  %.3fM cycles/table  nodes/table %.0f  hash %016llx\n", n / t1, c1 / 1e6 / n, double(nn) / n, h);
    ddfast::solve_tables_parallel(deals.data(), n < 16 ? n : 16, tab2.data(), cpus.data(), int(cpus.size()));  // warm-up
    std::vector<ddfast::WorkerStats> st;
    t = now();
    ddfast::solve_tables_parallel(deals.data(), n, tab2.data(), cpus.data(), int(cpus.size()), &st);
    const double t2 = now() - t;
    double cyc = 0;
    for (auto& w : st) cyc += w.cycles;
    printf("%zu threads: %.1f tables/s  %.3fM thread-cycles/table  %s\n", cpus.size(), n / t2, cyc / 1e6 / n,
           par_only ? "" : memcmp(tab.data(), tab2.data(), sizeof(ddfast::Table) * n) ? "DIFFER" : "same");
    ddfast::stop_pool();
    return 0;
  }
  SetMaxThreads(1);
  std::vector<ddfast::Table> ref(n);
  double t = now();
  for (size_t k = 0; k < n; k++) dds_table(deals[k], ref[k]);
  const double t_dds = now() - t;
  printf("DDS CalcDDtable, 1 thread:      %7.2f ms/table  %7.1f tables/s\n", 1e3 * t_dds / n, n / t_dds);
  auto check = [&](const std::vector<ddfast::Table>& tab) {
    int bad = 0;
    for (size_t k = 0; k < n; k++) bad += memcmp(tab[k].tricks, ref[k].tricks, sizeof ref[k].tricks) != 0;
    return bad;
  };
  init_or_exit();
  std::vector<ddfast::Table> tab(n);
  t = now();
  ddfast::solve_tables(deals.data(), n, tab.data());
  const double t_one = now() - t;
  printf("ddfast solve_tables, 1 thread: %7.2f ms/table  %7.1f tables/s  %d tables differ\n", 1e3 * t_one / n, n / t_one, check(tab));
  std::vector<ddfast::Table> tab2(n);
  ddfast::solve_tables_parallel(deals.data(), n < 16 ? n : 16, tab2.data(), cpus.data(), int(cpus.size()));  // warm-up
  memset(tab2.data(), 0, sizeof(ddfast::Table) * n);
  t = now();
  ddfast::solve_tables_parallel(deals.data(), n, tab2.data(), cpus.data(), int(cpus.size()));
  const double t_par = now() - t;
  printf("ddfast solve_tables_parallel, %zu threads: %7.2f ms/table  %7.1f tables/s  %d tables differ\n", cpus.size(),
         1e3 * t_par / n, n / t_par, check(tab2));
  ddfast::stop_pool();
  return check(tab) || check(tab2) ? 1 : 0;
}
