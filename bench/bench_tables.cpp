// Full tables: DDS CalcDDtable against ddfast's solve_tables and solve_tables_parallel, on
// random deals. Every result is compared with DDS's.
//
// usage: bench_tables [deals] [seed] [cpus for the parallel run, e.g. 0-3,12-15; default auto]
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
