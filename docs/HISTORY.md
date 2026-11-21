# ddfast's speed over its history

How ddfast's own speed changed across its main commits, on Linux x86-64 with the AVX-512 code,
measured with one program at every point. [NOTES.md](NOTES.md) has the reasoning and the full
measurements behind each step.

## Method

- **Machine:** AMD Ryzen AI 9 HX 370 (Zen 5) laptop, AC power, performance profile; one
  thread pinned to CPU 0, a fast core. Each library built for this machine (`-march=native`,
  so the AVX-512 code).
- **Workload (light):** 100 random problems (a deal, a strain and a declarer, solved as
  SolveBoard is: one question set per problem) and 100 random full deals (`solve_table`, all
  20 results). The same deals at every point (`random_problems(100, 1)` and
  `random_deals(100, 1)` of `tests/common.hpp`).
- **Measure:** user-mode cycles per problem and per table, from hardware counters, which
  vary far less than time on a laptop whose clock moves between 3.0 and 4.2 GHz. Each run
  keeps the best of 3 passes; three runs at each point, alternating over all points, and the
  table gives the lowest. At this size, differences under about 3% are within the noise.
- **One program for every commit:** `bench/bench_history.cpp` uses only the public API
  (`init`, `solve`, `solve_table`), which has not changed since the first commit, and is
  linked to each commit's own library.
- **Same answers everywhere:** a hash of all 100 answers and 100 tables is identical at every
  point, and `test_ddfast random 100 1` and `tables 100 1` check those answers against DDS.
- **Tables:** every point reads the same endgame-table files (the 4-card table as `p4_v5`
  up to #16, `p4_v6` from #19, both filled on this machine).
- **DDS** (commit 5caf076e, same compiler and flags) on the same workload: 88.1M cycles per
  problem, 728M per table.

## Results

| Point | Commit | Change | Problem (M cycles) | Table (M cycles) | vs DDS (problem / table) |
|---|---|---|---|---|---|
| start | `03d1b25` | The solver as it entered git (rounds 1-15 of NOTES behind it) | 19.14 | 175.5 | 4.60x / 4.15x |
| #1 | `8aee46d` | `solve_position` (any trick start) | 18.91 | 173.8 | 4.66x / 4.19x |
| #2 | `5661a1d` | ddclaim inside ddfast, cut to the 2-, 3- and 4-card tables | 18.04 | 171.3 | 4.88x / 4.25x |
| #6 | `4eda109` | Faster table directory, micro-optimizations, dead code removed | 17.31 | 167.3 | 5.09x / 4.35x |
| #7 | `4b2f7c2` | AVX-512 and Highway as two code paths | 18.01 | 172.5 | 4.89x / 4.22x |
| #8 | `8d4c21a` | CI (no solver change) | 18.26 | 176.8 | 4.82x / 4.12x |
| #10 | `f8319fa` | Endgame tables folded into ddfast; ddclaim removed | 17.60 | 168.8 | 5.01x / 4.31x |
| #11 | `157e688` | Full tables: first questions from the same deal's other declarers | 17.41 | 161.3 | 5.06x / 4.51x |
| #16 | `702b50b` | Opt-in mapping of the table files (off by default) | 17.34 | 164.0 | 5.08x / 4.44x |
| #19 | `58c41aa` | 4-card table with all four suits sorted at no trumps (`p4_v6`) | 17.48 | 169.4 | 5.04x / 4.30x |
| #21 | `8bd213a` | bridge-solver's sure-trick bounds and cut-off cards | 15.26 | 146.0 | 5.77x / 4.99x |

From the start to #21: 20% fewer cycles per problem and 17% fewer per table.

- **#2 and #6** are the gains before #21 (about 4% each on problems). #6's NOTES figure on the
  public set was -3.5%.
- **#7** looks about 4% slower than #6, partly recovered by #10. It reorganised the vector
  code into two paths; the difference is near the noise of this light test, and #8 (no solver
  change) is as slow as #7, so code layout is a likely part of it.
- **#11** speeds up full tables only (-4% here; NOTES: about -2% per table).
- **#1, #8 and #16** change no search code; their differences are noise. **#19** mostly
  shrinks the 4-card table (540 MB against 836 MB); NOTES measured -0.3% nodes and -0.5%
  cycles on the M5.
- **#21** is the largest single step: 24% fewer nodes on the public set, 13% fewer cycles.

## Before git

The solver entered git (`03d1b25`) after fifteen rounds of work recorded in NOTES, which are
not separate commits and are not re-measured here. As NOTES records them on the public set:
round 1 was correct at 13 cards but 6 times slower than DDS; round 2 replaced the
transposition table and was already faster than DDS; rounds 3 and 4 reached 131 and 190 deals
per second; the rounds after brought it to about 4 times DDS. The first commit's own,
`c925d49`, is ddclaim's history (the endgame-table project, merged in with #2), not ddfast's.

## Reproducing

```sh
export DDFAST_TABLES=/path/to/tables
sh bench/history.sh 3 start:03d1b25 pr01:8aee46d pr02:5661a1d pr06:4eda109 pr07:4b2f7c2 \
  pr08:8d4c21a pr10:f8319fa pr11:157e688 pr16:702b50b pr19:58c41aa pr21:8bd213a
```

Commits before #10 need ddclaim's checkout for their `extern/ddclaim` (`DDCLAIM_REPO`,
default `../ddclaim`).
