# ddfast

ddfast is a performance optimisation of the search algorithms of
[DDS](https://github.com/dds-bridge/dds), the widely used exact double-dummy solver for bridge.
Given four 13-card hands, a strain and a declarer, it returns the number of tricks declarer's
side takes with best play by both sides, the same answer as DDS, about four times faster on
one core.

It keeps DDS's approach to the double-dummy search: an alpha-beta search asked a sequence of
"at least t tricks?" questions, a transposition table of positions reduced to the cards that
matter, DDS's quick-tricks and later-tricks bounds, and DDS's move-ordering heuristics (ported
from DDS line by line and checked against it, `src/dds_port.hpp`). What changes is how they
run: positions are 64-bit hand words handled with bit operations and AVX-512, the bounds and
the move ordering work on those words without DDS's per-node bookkeeping, the transposition
table is a compact per-deal table of card patterns, the first question is chosen by a fitted
guess, and the last 2, 3 and 4 cards of each hand are looked up in the precomputed tables of
[ddclaim](https://github.com/randalburns/ddclaim). [docs/NOTES.md](docs/NOTES.md) is the
design and the full measurement history.

## Speed

On one core of an AMD Ryzen AI 9 HX 370 (CPU 0, a Zen 5 core), against DDS at commit
5caf076e (develop) built with the same compiler and flags:

| | ddfast | DDS | ratio |
|---|---|---|---|
| single problems (600 random deals, SolveBoard) | 271 deals/s | 63 deals/s | 4.3x |
| full tables (200 random deals, CalcDDtable on one thread) | 33.9 ms/table | 133.8 ms/table | 3.9x |

On the 600 deals of `tests/data/public.set` ddfast takes 16.1M cycles per deal (313 deals/s).
The parallel calls spread a batch over a pinned thread pool; see `bench/`.

## Requirements

- Linux on x86-64 with AVX-512 and BMI2 (Zen 4/5, Intel Ice Lake and later): the build uses
  `-march=native`.
- A C++20 compiler: clang++ (preferred) or g++. No cmake or other build system.
- About 1.2 GB of memory for the tables, and 1.1 GB of disk for their files.

## Building

```sh
git clone --recursive https://github.com/randalburns/ddfast.git
cd ddfast
scripts/build.sh
```

This makes, in `bin/`: `libddfast.a` (the library, ddclaim included), `ddfast` (the
command), `test_ddfast`, `make_ref`, `bench_solve` and `bench_tables` (tests and benchmarks,
linked with DDS from `extern/dds`, which is used for nothing else). Set `CXX` to choose the
compiler; `LIBONLY=1` skips DDS, the tests and the benchmarks. See the comments in
`scripts/build.sh` for the development builds (`-DSTATS`, `-DVERIFY_ORDER -DVERIFY_BOUNDS`,
`-DSELFTEST`).

### The tables

ddfast needs two of ddclaim's table files: the 3-card table (`t3_v2.bin`, 377 MB) and the
4-card pattern table (`p4_v5.ent` and `.idx`, 758 MB). `init()` reads them from the directory
it is given, else `$DDCLAIM_DIR`, else `build/ddclaim` under the current directory, and if
they are missing builds them there first (once): about 10 seconds for the 3-card table (on 8 cores) and up
to 170 seconds for the pattern table, on all cores, using up to about 5 GB of memory. To
build them ahead of time:

```sh
echo 'N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85' | bin/ddfast
```

ddfast asks ddclaim for the 4-card pattern level only (`DDCLAIM_LEVELS` overrides it), so
only the files above are built. A directory that already holds ddclaim's full tables, as ddclaim's own build makes them, works
as it is: point `DDCLAIM_DIR` at it.) The pattern table is filled for a fixed time, so tables
built on different machines can differ in size; the answers are exact either way.

## The command

```sh
bin/ddfast [-t threads] [-d table-dir] [-s strain] [-l declarer] [file ...]
```

One deal per line, in PBN notation (optionally inside a `[Deal "..."]` tag). A deal alone
gives its full table; a deal followed by a strain and a declarer gives declarer's tricks:

```
$ echo 'N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85' | bin/ddfast
N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85 | S 9 4 9 4 | H 4 9 4 8 | D 4 9 4 9 | C 7 6 7 6 | N 7 6 7 6
$ echo 'N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85 S N' | bin/ddfast
N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85 S N 9
```

Each strain (S H D C N) is followed by the tricks for declarers N E S W. `-t 0` uses every
available CPU.

## The library

`include/ddfast.hpp` is the whole interface:

```cpp
#include "ddfast.hpp"

ddfast::init();                                    // once: load (or build) the tables
ddfast::Problem p = {};                            // hand[seat][suit], bit r = rank 2 + r
// ... fill p.hand, p.trump (0..3 = S H D C, 4 = NT), p.declarer (0..3 = N E S W)
uint8_t tricks;
ddfast::solve(&p, 1, &tricks);                     // declarer's tricks, on this thread

ddfast::Deal d;                                    // just the hands
ddfast::Table t;
ddfast::solve_table(d, t);                         // t.tricks[strain][declarer], as DDS's res_table

std::vector<int> cpus = ddfast::available_cpus();  // fastest cores first
ddfast::solve_parallel(problems, n, out, cpus.data(), int(cpus.size()));
ddfast::solve_tables_parallel(deals, n, tables, cpus.data(), int(cpus.size()));
```

Link with `bin/libddfast.a -pthread`, and compile with `-march=native` (or another target with
AVX-512 and BMI2) and `-Iinclude`.

Thread safety: after `init()`, `solve`, `solve_table` and `solve_tables` may be called from any
number of threads at once. The two parallel calls share one pool and must not overlap.

## Tests and benchmarks

```sh
export DDCLAIM_DIR=...            # if the tables are not in build/ddclaim
bin/test_ddfast                  # quick: public set, 100 random deals, 10 tables; each also in parallel
bin/test_ddfast random 3000 1    # 3,000 random deals against DDS SolveBoard
bin/test_ddfast tables 200 1     # 200 full tables against DDS CalcDDtable
bin/make_ref my.set 1000 7        # a test set with DDS's answers; then: bin/test_ddfast set my.set
taskset -c 0 bin/bench_solve set tests/data/public.set     # cycles per deal on one core
bin/bench_solve parallel tests/data/public.set auto        # on the thread pool
bin/bench_tables 200 4242                                  # tables against DDS CalcDDtable
```

Every test and benchmark checks every answer against DDS. `tests/data/public.set` holds 600
random deals, each with a strain, a declarer and DDS's answer (format in `tests/common.hpp`).

## Layout

| | |
|---|---|
| `include/ddfast.hpp` | the public interface |
| `src/solver.cpp` | the search |
| `src/dds_port.hpp` | DDS's quick tricks, later tricks and move ordering, ported (used by the search) |
| `src/api.cpp` | the interface and the thread pool |
| `src/guess_*.inc` | fitted first guesses of the result |
| `extern/ddclaim` | ddclaim (submodule): the endgame tables and their probes (`include/ddclaim/tables.hpp`) |
| `extern/dds` | DDS (submodule), for tests and benchmarks only |
| `tests/`, `bench/`, `tools/` | tests against DDS, benchmarks, the command |
| `docs/NOTES.md` | design and measurements |

## License

Apache License 2.0 (see LICENSE), as ddclaim and DDS. `src/dds_port.hpp` contains code taken
from DDS (Copyright 2006-2014 Bo Haglund, 2014-2018 Bo Haglund and Soren Hein), marked in the
file.
