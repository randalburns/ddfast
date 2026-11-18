# ddfast

ddfast is a performance optimization of the search algorithms of
[DDS](https://github.com/dds-bridge/dds), the widely used exact double-dummy solver for bridge.
Given a deal, a strain and a declarer, it returns the number of tricks declarer's side takes
with best play by both sides, the same answer as DDS, 4-5 times faster on one core.

It isn't limited to full deals. Like DDS, it solves any position reached in play: a trick start
with 1 to 13 cards in each hand, or a position in the middle of a trick. It also gives every
legal card with its result, analyses a played sequence card by card, answers single "at least
k tricks?" questions (claims), and computes full tables and par (see
[Positions, play and par](#positions-play-and-par)).

It keeps DDS's approach to the double-dummy search: an alpha-beta search asked a sequence of
"at least t tricks?" questions, a transposition table of positions reduced to the cards that
matter, DDS's quick-tricks and later-tricks bounds, and DDS's move-ordering heuristics (ported
from DDS line by line and checked against it, `src/dds_port.hpp`). What changes is how they
run: positions are 64-bit hand words handled with bit operations and AVX-512 (Google Highway on Apple silicon), the bounds and
the move ordering work on those words without DDS's per-node bookkeeping, the transposition
table is a compact per-deal table of card patterns, the first question is chosen by a fitted
guess, and the last 2, 3 and 4 cards of each hand are looked up in precomputed endgame tables
(`src/tables.cpp`). [docs/NOTES.md](docs/NOTES.md) is the design and the full measurement
history.

## Design explainers

Illustrated explainers of the techniques behind the speed, for programmers who know C++ and
bridge basics:

- **[ddfast design](https://randalburns.github.io/ddfast/)**: the search in brief, where the
  speed comes from (what each part is worth, largest first) and the speed against DDS.
  - **[Follow-hand rules](https://randalburns.github.io/ddfast/follow-hand.html)** (1.66x):
    the second, third and fourth hands pick their first card with a few cheap rules instead
    of DDS's per-card weights, with worked examples.
  - **[One word per hand (with pext/pdep)](https://randalburns.github.io/ddfast/hand-words.html)**
    (1.52x): each hand as a single 64-bit word and the card logic as a few bit operations,
    which pays because of the `pext` and `pdep` instructions.
  - **[Endgame tables](https://randalburns.github.io/ddfast/endgame-tables.html)** (1.30x):
    every 2- and 3-card ending as one lookup, and 4-card endings matched against learned card
    patterns, with examples from the real tables.

The pages are published from the `gh-pages` branch.

## Speed

On an AMD Ryzen AI 9 HX 370 (Zen 5) laptop, using only its four performance cores (CPUs 0-3,
SMT siblings 12-15), against DDS at commit 5caf076e (develop) built with the same compiler and
flags. Every answer is checked against DDS.

One core (CPU 0):

| | ddfast | DDS | ratio |
|---|---|---|---|
| single problems (`tests/data/public.set`, 600 deals, SolveBoard; cycles per deal, 3 alternating runs) | 15.7-15.9M | 70.0-71.4M | 4.5x |
| full tables (100 random deals, CalcDDtable on one thread) | 30.7 ms/table | 132.5 ms/table | 4.3x |

Batches (`bench/bench_batch.cpp`: SolveAllBoards and CalcAllTables against `solve_parallel` and
`solve_tables_parallel`, same deals, same thread count, best of 3; the 600 deals of the public
set and tables of its first 120):

| | threads | ddfast | DDS | ratio |
|---|---|---|---|---|
| deals | 4 (CPUs 0-3) | 1,018 deals/s | 207 deals/s | 4.9x |
| tables | 4 (CPUs 0-3) | 122.0 tables/s | 27.7 tables/s | 4.4x |
| deals | 8 (CPUs 0-3, 12-15) | 1,419 deals/s | 272 deals/s | 5.2x |
| tables | 8 (CPUs 0-3, 12-15) | 165.3 tables/s | 34.8 tables/s | 4.75x |

ddfast pins one worker per CPU and hands out one problem at a time; DDS's pool is confined to
the same CPUs with `taskset` but its threads are not pinned individually. A laptop's clock moves
a lot under load (3.0-4.2 GHz here), so the cycle counts are the steadier figure.

Positions with fewer cards are faster still, relative to DDS (random trick starts, one at a
time, `bench_positions`): 4.3x DDS at 13 cards a hand, 4.8x at 10, 8x at 8, 13x
at 7 and 58x at 5. Batches of small positions (`solve_positions`) gain another 1.2-1.5x.

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

This makes, in `bin/`: `libddfast.a` (the library, endgame tables included), `ddfast` (the
command), `test_ddfast`, `make_ref`, `bench_solve` and `bench_tables` (tests and benchmarks,
linked with DDS from `extern/dds`, which is used for nothing else). Set `CXX` to choose the
compiler; `LIBONLY=1` skips DDS, the tests and the benchmarks. See the comments in
`scripts/build.sh` for the development builds (`-DSTATS`, `-DVERIFY_ORDER -DVERIFY_BOUNDS`,
`-DSELFTEST`).

### On macOS

Install Apple's compiler and Google Highway (headers only) once, then build as above:

```sh
xcode-select --install
brew install highway
git clone --recursive https://github.com/randalburns/ddfast.git
cd ddfast
scripts/build.sh
```

`scripts/build.sh` sees `arm64` from
`uname -m` and compiles with `-mcpu=native` in place of `-march=native`, and with Highway's
headers from Homebrew (set `HWY_INC` to the directory holding `hwy/` to use others). Apple clang may know
a newer CPU only by an older name (it treats an M5 as an M3), which leaves out the scalar
popcount and count-trailing-zeros instructions (CSSC); the script adds them, as
`-mcpu=native+cssc`, when `sysctl hw.optional.arm.FEAT_CSSC` says the CPU has them. That is
worth a few percent, as `popcount` and `ctz` are everywhere in the search. Everything else
(outputs, `CXX`, `LIBONLY`, `XFLAGS`, `OUT`) is as on Linux, and DDS builds unchanged.

For timing on macOS there is no `taskset`: run `bin/bench_solve` on an otherwise quiet
machine and take the best of a few runs. It reports cycles and instructions per deal on macOS
too (from `proc_pid_rusage`); they are steadier than the time when other programs are running.

### The tables

ddfast needs two table files: the 3-card table (`t3_v2.bin`, 377 MB) and the
4-card pattern table (`p4_v5.ent` and `.idx`, 758 MB). `init()` reads them from the directory
it is given, else `$DDFAST_TABLES`, else `build/tables` under the current directory, and if
they are missing builds them there first (once): about 10 seconds for the 3-card table (on 8 cores) and up
to 170 seconds for the pattern table, on all cores, using up to about 5 GB of memory. So
the first call of `init()` builds them, whatever it is asked to solve: running `bin/ddfast` on
any deal (or any program that calls `init()`) builds all the tables ddfast needs. To build them
ahead of time, solve one deal; which deal does not matter:

```sh
echo 'N:AKQ2.J93.T84.Q76 J96.A84.AQ752.T3 T8754.KT.K9.K942 3.Q7652.J63.AJ85' | bin/ddfast
```

The 2-card table is not a file: it is built in memory by every `init()`, in a moment.

The pattern table is filled for a fixed time, so tables built on different machines can differ
in size; the answers are exact either way.

## The macOS port

The bit and vector primitives live in one header, `src/port.hpp`,
with two implementations chosen at compile time:

- **AVX-512 and BMI2** intrinsics where the CPU has them (x86-64).
- **Google Highway**, the portable implementation (static dispatch, 128-bit vectors), on
  Apple silicon. `pext` and `pdep`, which Arm lacks, are done a byte at a time through two
  64 KB tables. x86-64 builds still require AVX-512.

macOS also has no huge pages and cannot pin threads: the tables are read through 16 KB pages,
and the worker pool asks for the performance cores by quality-of-service class instead. The
search is unchanged, so the answers are the same. On an M5, ddfast takes about as many cycles
per deal as on Zen 5 (15.5M against 16.1M), but DDS runs much faster there, so the ratio to
DDS is about 3x.

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

### Positions, play and par

Besides whole deals, ddfast covers the rest of DDS's interface. All of these answer from
North-South's side, and all are checked against DDS (`bin/test_play`).

```cpp
ddfast::Position q = {};                           // a trick start, 1..13 cards a hand
int ns = ddfast::solve_position(q);                // North-South's tricks from here
ddfast::solve_positions(qs, n, out);               // a batch: small positions much faster per position

ddfast::PlayPosition pp = {};                      // anywhere in the play: cards still held, the
pp.played = 1;                                     // leader of the current trick and the 0..3
pp.trick[0] = {0, 14};                             // cards already played to it (here the spade ace)
int v = ddfast::solve_play(pp);                    // North-South's tricks, this trick included
int ok = ddfast::at_least(pp, 9);                  // one yes/no question: a claim of 9 tricks
ddfast::CardResult cr[13];
int nc = ddfast::card_results(pp, cr);             // every legal card with its result (solutions 3)
uint8_t after[53];
ddfast::analyse_play(q13, cards, ncards, after);   // North-South's total after each card played

ddfast::ParResult pr;
ddfast::par(t, dealer, vulnerable, pr);            // par score and contracts from a table
```

| ddfast | DDS |
|---|---|
| `solve`, `solve_parallel` | `SolveBoard`, `SolveAllBoards` |
| `solve_table(s)`, `solve_tables_parallel` | `CalcDDtable`, `CalcAllTables` |
| `solve_position`, `solve_positions` | `SolveBoard` at the start of a trick |
| `solve_play` | `SolveBoard` in the middle of a trick (`currentTrickSuit`/`Rank`) |
| `at_least` | `SolveBoard` with a target |
| `card_results` | `SolveBoard` with `solutions` 3 |
| `analyse_play` | `AnalysePlayBin` |
| `par` | `DealerPar` (the score; contracts as structs rather than text) |

`solve_positions` searches positions of up to 10 cards a hand four at a time, interleaved on
the calling thread: a search waiting for an endgame table hands over to the next. On random
4-7-card positions that is 1.2-1.5x faster than one `solve_position` at a time. In the middle of a trick the cards still to come are
searched card by card and each completed trick goes to the trick-start solver; kept simple,
it still solves a deal after its opening lead in about 4 ms (DDS: 13 ms). Par is computed as
the auction between two sides who know the table: from the dealer's side, each bids any higher
contract or passes, contracts that go down are doubled, and two passes end it.

Thread safety: after `init()`, `solve`, `solve_table` and `solve_tables` may be called from any
number of threads at once, as may the calls of this section. The two parallel calls share one pool and must not overlap;
`solve_deal_parallel` has its own pool and must not overlap them either.

## Tests and benchmarks

```sh
export DDFAST_TABLES=...            # if the tables are not in build/tables
bin/test_ddfast                  # quick: public set, 100 random deals, 10 tables; each also in parallel
bin/test_ddfast random 3000 1    # 3,000 random deals against DDS SolveBoard
bin/test_ddfast tables 200 1     # 200 full tables against DDS CalcDDtable
bin/make_ref my.set 1000 7        # a test set with DDS's answers; then: bin/test_ddfast set my.set
taskset -c 0 bin/bench_solve set tests/data/public.set     # cycles per deal on one core
bin/bench_solve parallel tests/data/public.set auto        # on the thread pool
bin/bench_solve deal tests/data/public.set 3 1t 0-3        # latency of one deal: solve() vs 4 cores
bin/bench_tables 200 4242                                  # tables against DDS CalcDDtable
taskset -c 0-3 bin/bench_batch tests/data/public.set 120 4 0-3   # batches vs SolveAllBoards / CalcAllTables
bin/test_play 1000 1             # play positions, card results, play analysis and par against DDS
taskset -c 0 bin/bench_positions 6 20000 21 x.bin dds      # positions of 6 cards a hand vs DDS
sh bench/loo.sh OUTDIR                                     # the leave-one-out study (docs/LOO.md)
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
| `src/play.cpp` | play positions, card results, play analysis, targets and par |
| `src/guess_*.inc` | fitted first guesses of the result |
| `src/tables.hpp`, `src/tables.cpp` | the endgame tables: their layout and probes, and the code that loads and builds them |
| `src/port.hpp` | the bit and vector primitives: AVX-512 and BMI2 on x86-64, Google Highway on Apple silicon |
| `extern/dds` | DDS (submodule), for tests and benchmarks only |
| `tests/`, `bench/`, `tools/` | tests against DDS, benchmarks, the command |
| `docs/NOTES.md` | design and measurements |
| `docs/LOO.md` | what each part of the design is worth (leave-one-out study) |
| `docs/TABLES_NOTES.md` | the original design notes of the endgame tables |

## License

Apache License 2.0 (see LICENSE), as is DDS. `src/dds_port.hpp` contains code taken
from DDS (Copyright 2006-2014 Bo Haglund, 2014-2018 Bo Haglund and Soren Hein), marked in the
file.
