// Exactness tests against DDS (extern/dds). Every answer ddfast gives is compared with DDS's.
//
// usage: test_ddfast [command] [--cpus LIST]
//   quick (default)    the public set, 100 random deals, 10 random tables, each also in parallel
//   set FILE           a test set with answers (tests/data/public.set, or one from make_ref)
//   random N SEED      N random problems (the referee's dealing), DDS SolveBoard as reference
//   tables N SEED      N random deals: solve_table, solve_tables and solve_tables_parallel
//                      against DDS CalcDDtable
// Problems are checked with solve() on this thread, then with solve_parallel() on LIST (default:
// available_cpus()), then one at a time with solve_deal_parallel() on the first N of LIST
// (--deal-threads N, default 8). Exit status 0 if everything is exact. The tables are loaded from
// $DDCLAIM_DIR (default build/ddclaim), built there first if missing.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.hpp"

using namespace testing;

namespace {

std::vector<int> g_cpus;
size_t g_deal_threads = 8;  // solve_deal_parallel uses the first this many of g_cpus (--deal-threads)

int check_problems(const char* what, const std::vector<ddfast::Problem>& ps, const std::vector<uint8_t>& ref) {
  const size_t n = ps.size();
  std::vector<uint8_t> got(n, 0xFF);
  const unsigned long long nodes0 = ddfast::stat_nodes();
  double t = now();
  ddfast::solve(ps.data(), n, got.data());
  t = now() - t;
  const unsigned long long nodes = ddfast::stat_nodes() - nodes0;
  int bad = 0;
  for (size_t i = 0; i < n; i++)
    if (got[i] != ref[i]) {
      if (bad < 5) fprintf(stderr, "  %s: problem %zu: ddfast %d, DDS %d\n", what, i, got[i], ref[i]);
      bad++;
    }
  printf("%-28s %6zu problems  solve:          %s  (%.1f deals/s)", what, n, bad ? "WRONG" : "exact", n / t);
  if (nodes) printf("  nodes %llu", nodes);
  printf("\n");
  std::vector<uint8_t> par(n, 0xFF);
  t = now();
  ddfast::solve_parallel(ps.data(), n, par.data(), g_cpus.data(), int(g_cpus.size()));
  t = now() - t;
  int badp = 0;
  for (size_t i = 0; i < n; i++) badp += par[i] != ref[i];
  printf("%-28s %6zu problems  solve_parallel: %s  (%zu threads, %.1f deals/s)\n", what, n, badp ? "WRONG" : "exact",
         g_cpus.size(), n / t);
  // One deal at a time on all the CPUs (this thread pinned to the first).
  std::vector<int> dc(g_cpus.begin(), g_cpus.begin() + std::min<size_t>(g_cpus.size(), g_deal_threads));
  if (dc[0] >= 0) ddfast::pin_thread(dc[0]);
  int badd = 0;
  t = now();
  for (size_t i = 0; i < n; i++) {
    const int r = ddfast::solve_deal_parallel(ps[i], dc.data(), int(dc.size()));
    if (r != ref[i]) {
      if (badd < 5) fprintf(stderr, "  %s: problem %zu: solve_deal_parallel %d, DDS %d\n", what, i, r, ref[i]);
      badd++;
    }
  }
  t = now() - t;
  printf("%-28s %6zu problems  solve_deal_parallel: %s  (%zu threads, %.1f deals/s)\n", what, n, badd ? "WRONG" : "exact",
         dc.size(), n / t);
  return bad + badp + badd;
}

int test_set(const char* path) {
  TestSet s;
  if (!read_set(path, s)) {
    fprintf(stderr, "cannot read %s\n", path);
    exit(2);
  }
  return check_problems(path, s.problems, s.answers);
}

int test_random(size_t n, unsigned seed) {
  const auto ps = random_problems(n, seed);
  std::vector<uint8_t> ref(n);
  double t = now();
  for (size_t i = 0; i < n; i++) ref[i] = uint8_t(dds_solve(ps[i]));
  t = now() - t;
  fprintf(stderr, "DDS: %zu problems, %.1f deals/s\n", n, n / t);
  const std::string what = "random " + std::to_string(n) + " seed " + std::to_string(seed);
  return check_problems(what.c_str(), ps, ref);
}

int test_tables(size_t n, unsigned long long seed) {
  const auto ds = random_deals(n, seed);
  std::vector<ddfast::Table> ref(n), a(n), b(n), c(n);
  double t = now();
  for (size_t i = 0; i < n; i++) dds_table(ds[i], ref[i]);
  const double t_dds = now() - t;
  t = now();
  for (size_t i = 0; i < n; i++) ddfast::solve_table(ds[i], a[i]);
  const double t_one = now() - t;
  ddfast::solve_tables(ds.data(), n, b.data());
  t = now();
  ddfast::solve_tables_parallel(ds.data(), n, c.data(), g_cpus.data(), int(g_cpus.size()));
  const double t_par = now() - t;
  int bad[3] = {0, 0, 0};
  for (size_t i = 0; i < n; i++)
    for (int st = 0; st < 5; st++)
      for (int d = 0; d < 4; d++) {
        const int r = ref[i].tricks[st][d];
        if (a[i].tricks[st][d] != r && bad[0] < 5)
          fprintf(stderr, "  table %zu strain %d declarer %d: ddfast %d, DDS %d\n", i, st, d, a[i].tricks[st][d], r);
        bad[0] += a[i].tricks[st][d] != r, bad[1] += b[i].tricks[st][d] != r, bad[2] += c[i].tricks[st][d] != r;
      }
  printf("tables %zu seed %llu: solve_table %s, solve_tables %s, solve_tables_parallel %s  (DDS %.2f ms/table, ddfast %.2f ms/table, "
         "parallel %.2f ms/table on %zu threads)\n",
         n, seed, bad[0] ? "WRONG" : "exact", bad[1] ? "WRONG" : "exact", bad[2] ? "WRONG" : "exact", 1e3 * t_dds / n,
         1e3 * t_one / n, 1e3 * t_par / n, g_cpus.size());
  return bad[0] + bad[1] + bad[2];
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  std::string cpus = "auto";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--cpus") && i + 1 < argc) cpus = argv[++i];
    else if (!strcmp(argv[i], "--deal-threads") && i + 1 < argc) g_deal_threads = size_t(atol(argv[++i]));
    else args.push_back(argv[i]);
  }
  const std::string cmd = args.empty() ? "quick" : args[0];
  auto arg = [&](size_t k, const char* dflt) { return args.size() > k ? args[k] : std::string(dflt); };
  SetMaxThreads(1);
  init_or_exit();
  g_cpus = parse_cpus(cpus);
  if (g_cpus.empty()) g_cpus.push_back(-1);
  int bad = 0;
  if (cmd == "quick") {
    // The public set: found next to this source in a checkout (run from the repository root).
    bad += test_set(arg(1, "tests/data/public.set").c_str());
    bad += test_random(100, 1);
    bad += test_tables(10, 1);
  } else if (cmd == "set" && args.size() > 1) {
    bad += test_set(args[1].c_str());
  } else if (cmd == "random") {
    bad += test_random(size_t(atol(arg(1, "1000").c_str())), unsigned(atol(arg(2, "1").c_str())));
  } else if (cmd == "tables") {
    bad += test_tables(size_t(atol(arg(1, "200").c_str())), strtoull(arg(2, "4242").c_str(), nullptr, 10));
  } else {
    fprintf(stderr, "usage: test_ddfast [quick | set FILE | random N SEED | tables N SEED] [--cpus LIST]\n");
    return 2;
  }
  ddfast::stop_pool();
  printf("%s\n", bad ? "FAILED" : "all exact");
  return bad ? 1 : 0;
}
