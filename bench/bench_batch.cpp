// Batch solving against DDS on the same deals and the same number of threads: SolveAllBoards
// against solve_parallel (the problems of a test set) and CalcAllTables against
// solve_tables_parallel (the deals of its first NTABLES problems). DDS and ddfast alternate;
// each figure is the best of REPS passes. Every answer is compared with the set's answers and
// every table with DDS's.
//
// usage: bench_batch SET [ntables] [threads] [cpus] [reps]     (defaults 120 8 auto 3)
//   e.g. taskset -c 0-3,12-15 bin/bench_batch tests/data/public.set 120 8 0-3,12-15
// ddfast pins one worker to each CPU of the list; DDS's pool cannot be pinned per thread, so
// run the whole process under taskset with the same list to keep DDS on the same CPUs.
#include "common.hpp"
using namespace testing;

static double dds_boards(const std::vector<ddfast::Problem>& ps, std::vector<uint8_t>& out) {
  static Boards b;
  static SolvedBoards sb;
  const double t0 = now();
  for (size_t i0 = 0; i0 < ps.size(); i0 += MAXNOOFBOARDS) {
    const int m = int(std::min<size_t>(MAXNOOFBOARDS, ps.size() - i0));
    b.no_of_boards = m;
    for (int k = 0; k < m; k++) {
      const auto& p = ps[i0 + k];
      Deal& dl = b.deals[k];
      dl = Deal{};
      dl.trump = p.trump;
      dl.first = (p.declarer + 1) & 3;
      for (int q = 0; q < 4; q++)
        for (int s = 0; s < 4; s++) dl.remainCards[q][s] = unsigned(p.hand[q][s]) << 2;
      b.target[k] = -1, b.solutions[k] = 1, b.mode[k] = 1;
    }
    if (SolveAllBoardsBin(&b, &sb) != 1) { fprintf(stderr, "SolveAllBoardsBin failed\n"); exit(1); }
    for (int k = 0; k < m; k++) out[i0 + k] = uint8_t(13 - sb.solved_board[k].score[0]);
  }
  return now() - t0;
}

static double dds_tables(const std::vector<ddfast::Deal>& ds, std::vector<ddfast::Table>& out, int threads) {
  static DdTableDeals d;
  static DdTablesRes r;
  static AllParResults par;
  int filter[DDS_STRAINS] = {0, 0, 0, 0, 0};
  const double t0 = now();
  for (size_t i0 = 0; i0 < ds.size(); i0 += MAXNOOFTABLES) {
    const int m = int(std::min<size_t>(MAXNOOFTABLES, ds.size() - i0));
    d.no_of_tables = m;
    for (int k = 0; k < m; k++)
      for (int h = 0; h < 4; h++)
        for (int s = 0; s < 4; s++) d.deals[k].cards[h][s] = unsigned(ds[i0 + k].hand[h][s]) << 2;
    if (CalcAllTablesN(&d, -1, filter, &r, &par, threads) != 1) { fprintf(stderr, "CalcAllTablesN failed\n"); exit(1); }
    for (int k = 0; k < m; k++)
      for (int st = 0; st < 5; st++)
        for (int h = 0; h < 4; h++) out[i0 + k].tricks[st][h] = uint8_t(r.results[k].res_table[st][h]);
  }
  return now() - t0;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: bench_batch SET [ntables] [threads] [cpus] [reps]\n"); return 2; }
  TestSet s;
  if (!read_set(argv[1], s)) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  const size_t nt = std::min(s.problems.size(), size_t(argc > 2 ? atol(argv[2]) : 120));
  const int threads = argc > 3 ? atoi(argv[3]) : 8;
  const std::string cpu_arg = argc > 4 ? argv[4] : "auto";
  const std::vector<int> cpus = parse_cpus(cpu_arg);
  const int reps = argc > 5 ? atoi(argv[5]) : 3;
  init_or_exit();
  SetMaxThreads(threads);
  const auto& ps = s.problems;
  std::vector<ddfast::Deal> ds(nt);
  for (size_t i = 0; i < nt; i++) memcpy(ds[i].hand, ps[i].hand, sizeof ds[i].hand);

  std::vector<uint8_t> a(ps.size()), b(ps.size());
  std::vector<ddfast::Table> ta(nt), tb(nt);
  double bd = 1e9, bf = 1e9, td = 1e9, tf = 1e9;
  size_t wrong = 0;
  for (int r = 0; r < reps; r++) {
    bd = std::min(bd, dds_boards(ps, a));
    double t0 = now();
    ddfast::solve_parallel(ps.data(), ps.size(), b.data(), cpus.data(), int(cpus.size()));
    bf = std::min(bf, now() - t0);
    for (size_t i = 0; i < ps.size(); i++) wrong += a[i] != s.answers[i] || b[i] != s.answers[i];
    td = std::min(td, dds_tables(ds, ta, threads));
    t0 = now();
    ddfast::solve_tables_parallel(ds.data(), nt, tb.data(), cpus.data(), int(cpus.size()));
    tf = std::min(tf, now() - t0);
    for (size_t i = 0; i < nt; i++) wrong += memcmp(ta[i].tricks, tb[i].tricks, sizeof ta[i].tricks) != 0;
  }
  const double n = double(ps.size());
  printf("threads %d, cpus %s (best of %d)\n", threads, cpu_arg.c_str(), reps);
  printf("  boards: DDS %7.1f deals/s   ddfast %7.1f deals/s   %.2fx\n", n / bd, n / bf, bd / bf);
  printf("  tables: DDS %7.1f tables/s  ddfast %7.1f tables/s  %.2fx\n", nt / td, nt / tf, td / tf);
  printf("  %s\n", wrong ? "WRONG" : "all answers agree (set answers and DDS tables)");
  return wrong != 0;
}
