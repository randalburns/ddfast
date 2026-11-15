// Makes a test set with reference answers from DDS (extern/dds): random full deals, each with
// a strain and a declarer, solved with SolveBoard (target -1). The dealing is the competition
// referee's, so a given seed gives the same deals.
//
// usage: make_ref <output file> <deals> <seed>
//
// File: uint32 n; n Problems; n answers (declarer's tricks, uint8); n DDS node counts (uint32).
#include <cstdio>
#include <cstdlib>

#include "common.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: make_ref <output file> <deals> <seed>\n");
    return 2;
  }
  SetMaxThreads(1);
  testing::TestSet s;
  s.problems = testing::random_problems(size_t(atol(argv[2])), unsigned(atol(argv[3])));
  const size_t n = s.problems.size();
  s.answers.resize(n), s.nodes.resize(n);
  const double t0 = testing::now();
  for (size_t i = 0; i < n; i++) s.answers[i] = uint8_t(testing::dds_solve(s.problems[i], &s.nodes[i]));
  const double secs = testing::now() - t0;
  if (!testing::write_set(argv[1], s)) {
    fprintf(stderr, "cannot write %s\n", argv[1]);
    return 1;
  }
  printf("%zu deals, DDS %.2f ms each\n", n, 1e3 * secs / double(n));
  return 0;
}
