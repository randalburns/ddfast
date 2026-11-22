# ddfast's speed over its history

How ddfast's own speed changed across the commits that moved it, on Linux x86-64 with the
AVX-512 code, measured with one program at every point; and from #10 on, on an Apple M5. [NOTES.md](NOTES.md) has the reasoning
and the full measurements behind each step.

## Method

- **Machine:** AMD Ryzen AI 9 HX 370 (Zen 5) laptop, AC power, performance profile; one
  thread pinned to CPU 0, a fast core. Each library built for this machine (`-march=native`,
  so the AVX-512 code).
- **Workload:** 300 random problems (a deal, a strain and a declarer, solved as SolveBoard is)
  and 300 random full deals (`solve_table`, all 20 results), the same deals at every point
  (`random_problems(300, 7)` and `random_deals(300, 7)` of `tests/common.hpp`).
- **Measure:** user-mode cycles per problem and per table, from hardware counters; the best
  of 3 passes in each run, three runs at each point alternating over all points, and the
  table gives the lowest. Cycles still move by about 2% between runs on a laptop whose clock
  swings between 3.0 and 4.2 GHz, so the instructions executed are counted too: they are
  exact and repeat to the last digit, and decide whether a step changed the work.
- **One program for every commit:** `bench/bench_history.cpp` uses only the public API
  (`init`, `solve`, `solve_table`), which has not changed since the first commit, and is
  linked to each commit's own library.
- **Same answers everywhere:** a hash of all 300 answers and 300 tables is identical at every
  point, and the current code's answers agree with DDS (`bin/test_ddfast`).
- **Tables:** every point reads the same endgame-table files (the 4-card table as `p4_v5`
  up to #16, `p4_v6` from #19, both filled on this machine).
- **DDS** (commit 5caf076e, same compiler and flags) on the same workload: 65.9M cycles per
  problem, 709M per table.

## Results

Points in the order they reached `main`. Only the steps that changed instructions or cycles
by 1% or more are kept; the others are listed below the table.

| Point | Commit | Change | Problem: M cycles (M instructions) | Table: M cycles (M instructions) | vs DDS (problem / table) |
|---|---|---|---|---|---|
| start | `03d1b25` | The solver as it entered git (rounds 1-15 of NOTES behind it) | 14.54 (36.48) | 165.1 (403.1) | 4.53x / 4.29x |
| #1 | `8aee46d` | `solve_position` (any trick start) | 14.43 (35.94) | 167.3 (397.7) | 4.56x / 4.24x |
| #2 | `5661a1d` | ddclaim inside ddfast, cut to the 2-, 3- and 4-card tables | 14.47 (35.05) | 162.0 (387.5) | 4.55x / 4.38x |
| #6 | `4eda109` | Faster table directory, micro-optimizations, dead code removed | 14.00 (34.05) | 158.1 (376.5) | 4.70x / 4.48x |
| #11 | `157e688` | Full tables: first questions from the same deal's other declarers | 13.87 (34.05) | 155.5 (371.1) | 4.75x / 4.56x |
| #21 | `8bd213a` | bridge-solver's sure-trick bounds and cut-off cards | 12.08 (30.83) | 130.4 (321.1) | 5.45x / 5.44x |
| #25 | `af908de` | Cut-off cards only from 8 cards a hand | 11.68 (30.39) | 122.6 (319.2) | 5.64x / 5.78x |

From the start to #25: 20% fewer cycles per problem (17% fewer instructions) and 26% fewer per
table (21% fewer instructions).

- **#1 and #2** each take 1.5-2.6% of the instructions. Their cycles move less than that, within
  the 2% the clock allows.
- **#6** takes 3% of the instructions and about 3.5% of the cycles (NOTES: -3.5% on the public
  set).
- **#11** changes full tables only: 1.4% fewer instructions, 2% fewer cycles.
- **#21** is the largest step: 10% fewer instructions per problem and 14% per table, and 13-16%
  fewer cycles (24% fewer nodes on the public set).
- **#25** saves cache misses rather than work: 1.4% fewer instructions per problem and 0.6% per
  table, but 3.4% and 4.3% fewer cycles than #21 measured alongside it (12.09M and 128.1M that
  day). Small searches no longer probe the 512 KB cut-off table.

Left out (instructions within 1% of the point before): #24 (every card's score at a lead, in
the DDS-compatible SolveBoard, which this workload does not call: the instructions of #21),
#5 (the check builds), #7 (AVX-512 and
Highway as two code paths: exactly the instructions of #5, so the Highway path costs the
AVX-512 code nothing), #8 (CI), #10 (tables folded into ddfast), #16 (opt-in mapping of the
table files) and #19 (the 4-card table with all four suits sorted at no trumps: +0.5% here; it
mostly shrinks the table file, 540 MB against 836 MB). The PRs are numbered by when they were
opened, not merged: #7 and #8 reached `main` before #6.

## On an Apple M5

The same program and deals on an Apple M5 Mac (4 performance and 6 efficiency cores, 16 GB),
each library built for it (`-mcpu=native`: the Highway NEON code, and table-driven pext/pdep,
which Arm lacks). Measured 2026-11-21.

- **Points:** from #10 on. Before #7 the solver was AVX-512 only, and commits before #10 need
  ddclaim's checkout.
- **Counters:** the process's cycles and instructions from `proc_pid_rusage`, which count
  system time as well as user time, so the instructions repeat to about 0.1% and not to the
  last digit.
- **Cores:** macOS cannot pin a thread. The program asks for a performance core (user-interactive
  quality of service), as ddfast's pool threads do.
- **Four threads:** the same 300 problems and 300 tables as batches (`solve_parallel`,
  `solve_tables_parallel`) on four threads, `THREADS=4 REPS=5`. Cycles are summed over the
  threads; problems and tables per second are the best of three rounds.
- **Tables:** `p4_v5` up to #16 and `p4_v6` from #19, both filled on this machine.

Three rounds alternating over all points; cycles are the lowest. The hash of the answers is the
same at every point and the same as on the Zen 5.

| Point | Commit | One thread, problem: M cycles (M instructions) | One thread, table: M cycles (M instructions) | Four threads, problem / table: M cycles | Four threads: problems/s, tables/s |
|---|---|---|---|---|---|
| #10 | `f8319fa` | 16.12 (59.97) | 142.7 (535.1) | 16.12 / 143.4 | 990, 104.9 |
| #11 | `157e688` | 16.13 (59.98) | 140.7 (527.1) | 16.04 / 140.3 | 934, 102.2 |
| #16 | `702b50b` | 16.10 (59.97) | 140.4 (527.1) | 16.00 / 139.8 | 953, 100.6 |
| #19 | `58c41aa` | 16.04 (59.79) | 139.8 (525.8) | 16.02 / 139.2 | 957, 99.2 |
| #21 | `8bd213a` | 13.85 (52.57) | 114.9 (441.4) | 13.78 / 114.3 | 1,093, 119.7 |
| #24 | `9c9d2ca` | 13.86 (52.57) | 115.1 (441.4) | 14.03 / 116.3 | 1,042, 114.2 |
| #25 | `af908de` | 13.42 (52.53) | 110.9 (436.9) | 13.61 / 112.2 | 1,083, 119.0 |

From #10 to #25: 17% fewer cycles per problem and 22% fewer per table on one thread, 16% and
22% on four.

- **The steps are the Zen 5's.** #21 takes 14% of the cycles per problem and 18% per table (12%
  and 16% of the instructions); #25 another 3% and 3.5% with hardly any fewer instructions;
  #11 moves tables only; #16, #19 and #24 are within 1%.
- **Four threads cost the same cycles as one**, within 1.5% at every point: the threads do not
  slow each other.
- **Throughput on four threads is less steady than cycles.** Every point lost about 10% between
  the first round and the later ones (#10: 990, 878, 899 problems per second), as the machine
  warmed. Within one round #25 is 13-18% ahead of #19 on problems and 20-25% on tables.
- **Smaller positions gain less.** Trick starts in batches of 32 with two hands fixed (as in
  NOTES round 19), one thread, #10 to #25: no change at 7 to 9 cards a hand (#21 cost about 6%
  at 8 cards and #25 took most of it back), -3% at 10, -10% at 11, -7% at 12.

## Before git

The solver entered git (`03d1b25`) after fifteen rounds of work recorded in NOTES, which are
not separate commits and are not re-measured here. As NOTES records them on the public set:
round 1 was correct at 13 cards but 6 times slower than DDS; round 2 replaced the
transposition table and was already faster than DDS; rounds 3 and 4 reached 131 and 190 deals
per second; the rounds after brought it to about 4 times DDS. The repository's first commit,
`c925d49`, is ddclaim's history (the endgame-table project, merged in with #2), not ddfast's.

## Reproducing

```sh
export DDFAST_TABLES=/path/to/tables
sh bench/history.sh 3 start:03d1b25 pr01:8aee46d pr02:5661a1d pr06:4eda109 pr11:157e688 pr21:8bd213a \
  pr25:af908de
```

On macOS (`taskset` is not used; `THREADS` and `REPS` as above):

```sh
export DDFAST_TABLES=/path/to/tables
sh bench/history.sh 3 pr10:f8319fa pr11:157e688 pr16:702b50b pr19:58c41aa pr21:8bd213a pr24:9c9d2ca \
  pr25:af908de
THREADS=4 REPS=5 sh bench/history.sh 3 pr10:f8319fa pr11:157e688 pr16:702b50b pr19:58c41aa pr21:8bd213a \
  pr24:9c9d2ca pr25:af908de
```

Commits before #10 need ddclaim's checkout for their `extern/ddclaim` (`DDCLAIM_REPO`,
default `../ddclaim`). `N` and `SEED` choose the workload (default 300 and 7).
