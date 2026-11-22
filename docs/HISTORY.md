# ddfast's speed over its history

How ddfast's own speed changed across the commits that moved it, on Linux x86-64 with the
AVX-512 code, measured with one program at every point. [NOTES.md](NOTES.md) has the reasoning
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

Commits before #10 need ddclaim's checkout for their `extern/ddclaim` (`DDCLAIM_REPO`,
default `../ddclaim`). `N` and `SEED` choose the workload (default 300 and 7).
