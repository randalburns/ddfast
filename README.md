# ddfast

ddfast is a performance optimization of the search algorithms of
[DDS](https://github.com/dds-bridge/dds), the widely used exact double-dummy solver for bridge.
Given four 13-card hands, a strain and a declarer, it returns the number of tricks declarer's
side takes with best play by both sides, the same answer as DDS, about four times faster on
one core.

It keeps DDS's approach to the double-dummy search: an alpha-beta search asked a sequence of
"at least t tricks?" questions, a transposition table of positions reduced to the cards that
matter, DDS's quick-tricks and later-tricks bounds, and DDS's move-ordering heuristics (ported
from DDS line by line and checked against it, `src/dds_port.hpp`). What changes is how they
run: positions are 64-bit hand words handled with bit operations and AVX-512 (NEON on Apple silicon), the bounds and
the move ordering work on those words without DDS's per-node bookkeeping, the transposition
table is a compact per-deal table of card patterns, the first question is chosen by a fitted
guess, and the last 2, 3 and 4 cards of each hand are looked up in the precomputed tables of
ddclaim (`extern/ddclaim`, formerly [its own repository](https://github.com/randalburns/ddclaim)). [docs/NOTES.md](docs/NOTES.md) is the
design and the full measurement history.

## Design explainers

Two illustrated explainers of the techniques behind the speed, for programmers who know C++
and bridge basics ([all design pages](https://randalburns.github.io/ddfast/)):

- **[One word per hand](https://randalburns.github.io/ddfast/hand-words.html)**: each hand as
  a single 64-bit word, 16 bits per suit, and the card logic (following suit, trick winners,
  one card per run of touching cards, the cards that decided a result) as a few bit
  operations.
- **[The pattern table search](https://randalburns.github.io/ddfast/table-search.html)**: the
  per-deal transposition table of card patterns, its directory and blocks, and how AVX-512
  matches a block of 8 stored patterns against a position in a few instructions.

The pages are published from the `gh-pages` branch.

## Speed

On one core of an AMD Ryzen AI 9 HX 370 (CPU 0, a Zen 5 core), against DDS at commit
5caf076e (develop) built with the same compiler and flags:

| | ddfast | DDS | ratio |
|---|---|---|---|
| single problems (600 random deals, SolveBoard) | 271 deals/s | 63 deals/s | 4.3x |
| full tables (200 random deals, CalcDDtable on one thread) | 33.9 ms/table | 133.8 ms/table | 3.9x |

On the 600 deals of `tests/data/public.set` ddfast takes 16.1M cycles per deal (313 deals/s).
The parallel calls spread a batch over a pinned thread pool; see `bench/`.

On one performance core of an Apple M5 (macOS; see "The macOS port" below), against
the same DDS built with the same compiler and flags, cycles per deal (min of 3 alternating
runs):

| | ddfast | DDS | ratio |
|---|---|---|---|
| single problems (600 random deals, SolveBoard) | 15.5M cycles (3.7 ms) | 48.5M cycles (11.8 ms) | 3.1x |
| full tables (200 random deals, CalcDDtable on one thread) | 151M cycles (37.5 ms) | 439M cycles (108.7 ms) | 2.9x |

ddfast takes about as many cycles per deal on the M5 as on Zen 5; DDS takes far fewer, which is
most of the smaller ratio.

## Requirements

- Linux on x86-64 with AVX-512 and BMI2 (Zen 4/5, Intel Ice Lake and later): the build uses
  `-march=native`.
- Or macOS on Apple silicon (M1 and later), with the Xcode Command Line Tools: the build uses
  `-mcpu=native`. See "The macOS port" below.
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

### On macOS

Install Apple's compiler once, then build as above:

```sh
xcode-select --install
git clone --recursive https://github.com/randalburns/ddfast.git
cd ddfast
scripts/build.sh
```

`scripts/build.sh` sees `arm64` from
`uname -m` and compiles with `-mcpu=native` in place of `-march=native`. Apple clang may know
a newer CPU only by an older name (it treats an M5 as an M3), which leaves out the scalar
popcount and count-trailing-zeros instructions (CSSC); the script adds them, as
`-mcpu=native+cssc`, when `sysctl hw.optional.arm.FEAT_CSSC` says the CPU has them. That is
worth a few percent, as `popcount` and `ctz` are everywhere in the search. Everything else
(outputs, `CXX`, `LIBONLY`, `XFLAGS`, `OUT`) is as on Linux, and DDS builds unchanged.

For timing on macOS there is no `taskset`: run `bin/bench_solve` on an otherwise quiet
machine and take the best of a few runs. It reports cycles and instructions per deal on macOS
too (from `proc_pid_rusage`); they are steadier than the time when other programs are running.

### The tables

ddfast needs two of ddclaim's table files: the 3-card table (`t3_v2.bin`, 377 MB) and the
4-card pattern table (`p4_v5.ent` and `.idx`, 758 MB). `init()` reads them from the directory
it is given, else `$DDCLAIM_DIR`, else `build/ddclaim` under the current directory, and if
they are missing builds them there first (once): about 10 seconds for the 3-card table (on 8 cores) and up
to 170 seconds for the pattern table, on all cores, using up to about 5 GB of memory. So
the first call of `init()` builds them, whatever it is asked to solve: running `bin/ddfast` on
any deal (or any program that calls `init()`) builds all the tables ddfast needs. To build them
ahead of time, solve one deal; which deal does not matter:

```sh
echo 'N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85' | bin/ddfast
```

The 2-card table is not a file: it is built in memory by every `init()`, in a moment.

These are the only table files: ddclaim's 5- and 6-card pattern tables were removed (they
saved 6% of nodes and no time). A directory that also holds the old `p5_v5` and `p6_v5` files
works as it is; they are not read. The pattern table is filled for a fixed time, so tables
built on different machines can differ in size; the answers are exact either way.

## The macOS port

ddfast and ddclaim were written for AVX-512 and BMI2. The port keeps that code as it was for
x86-64 and adds an AArch64 version of each piece, chosen at compile time. The pieces live in
one header, `extern/ddclaim/include/ddclaim/port.hpp`, used by both ddclaim and ddfast:

- **pext and pdep**, which Arm lacks, are done a byte at a time through two 64 KB tables
  (they fit in a performance core's L1 cache). The eight bytes are independent: each byte's
  output position comes from one NEON popcount and a multiply. A mask used for several calls is
  prepared once (`ddport::BitMask`). This is the largest cost of the port: about 50
  instructions for what is one instruction on x86. So the move generators find the heads of
  runs of touching cards with an add-and-carry bit trick instead, without pext, and constant
  masks become shifts.
- **The AVX-512 blocks** become NEON (two 64-bit or four 32-bit lanes) or plain scalar code:
  the transposition-table and pattern-table block matches, the choice of the best remaining
  card, the per-suit bounds and the lead move ordering. In the move ordering, the per-suit
  weights use four-lane NEON vectors as before, and a branch-free scalar loop over the cards
  takes the place of AVX-512's compress and 16-lane permutes.
- **Thread-local state**: on macOS each thread-local variable costs a function call to find
  its address, once in every function that uses it. The search's hot per-thread variables
  (the hands, the lengths, the search state) are kept in one thread-local struct, so every node
  pays one such call instead of several.
- **The operating system**: macOS has no huge pages and no `MAP_POPULATE`, so the pattern
  table file is mapped with 16 KB pages and read through once at load time. It cannot pin a
  thread to a core either: the worker pool asks for the performance cores by quality-of-service
  class instead, and `available_cpus()` returns worker slots rather than CPU numbers. The
  hardware counters in `WorkerStats` stay 0. The coroutine stack switch (used only in builds
  with `-DNCTX` above 1) has an AArch64 version.

The search itself is unchanged, so the answers are the same: on an M5 the port gave DDS's
answer on the public set, on 1,700 random problems and on 260 random full tables, on one
thread and in parallel. A `-DVERIFY_ORDER -DVERIFY_BOUNDS` build, which checks the move
ordering and the bounds against the line-by-line port of DDS at every node, found no
difference.

Where the time goes: the port runs about 55M instructions per deal of the public set, against
about 40M on x86, mostly for pext and pdep and the narrower vectors. The M5 runs them at about
3.6 instructions per cycle, so ddfast takes about as many cycles per deal as on Zen 5 (15.5M
against 16.1M). DDS gains far more from Apple's core: about 48M cycles per deal on the M5,
against about 80M on Zen 5. That is why the ratio to DDS is about 3x on the Mac and 4x on
Zen 5. Most of what remains is waiting for memory in the per-deal table and in ddclaim's 1.1 GB
of tables (which on macOS cannot use huge pages; on Linux they are worth about 2%).
Profile-guided optimisation saves about 7% of the instructions on the M5 but only about 2% of
the cycles, so the build does not use it.

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

int fast[4] = {0, 1, 2, 3};                        // one deal on several cores (lower latency)
ddfast::pin_thread(fast[0]);                       // the calling thread works as well, on fast[0]
int dtricks = ddfast::solve_deal_parallel(p, fast, 4);
```

Link with `bin/libddfast.a -pthread`, and compile with `-march=native` (or another target with
AVX-512 and BMI2) and `-Iinclude`.

Thread safety: after `init()`, `solve`, `solve_table` and `solve_tables` may be called from any
number of threads at once. The two parallel calls share one pool and must not overlap;
`solve_deal_parallel` has its own pool and must not overlap them either.

## Tests and benchmarks

```sh
export DDCLAIM_DIR=...            # if the tables are not in build/ddclaim
bin/test_ddfast                  # quick: public set, 100 random deals, 10 tables; each also in parallel
bin/test_ddfast random 3000 1    # 3,000 random deals against DDS SolveBoard
bin/test_ddfast tables 200 1     # 200 full tables against DDS CalcDDtable
bin/make_ref my.set 1000 7        # a test set with DDS's answers; then: bin/test_ddfast set my.set
taskset -c 0 bin/bench_solve set tests/data/public.set     # cycles per deal on one core
bin/bench_solve parallel tests/data/public.set auto        # on the thread pool
bin/bench_solve deal tests/data/public.set 3 1t 0-3        # latency of one deal: solve() vs 4 cores
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
| `extern/ddclaim` | ddclaim (git subtree): the endgame tables, their probes (`include/ddclaim/tables.hpp`) and builder |
| `extern/dds` | DDS (submodule), for tests and benchmarks only |
| `tests/`, `bench/`, `tools/` | tests against DDS, benchmarks, the command |
| `docs/NOTES.md` | design and measurements |

## License

Apache License 2.0 (see LICENSE), as ddclaim and DDS. `src/dds_port.hpp` contains code taken
from DDS (Copyright 2006-2014 Bo Haglund, 2014-2018 Bo Haglund and Soren Hein), marked in the
file.
