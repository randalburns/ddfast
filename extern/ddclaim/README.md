# ddclaim

An exact double-dummy solver for small bridge endings, built to check claims on end
positions: given the cards left, the trump suit and the hand on lead, how many tricks does
each side take with best play?

It handles endings of 1 to 7 cards per hand. A 6-card ending takes about 2.6 microseconds on
one core, about 39 times faster than [DDS](https://github.com/dds-bridge/dds) on this specific workload. 

_Note:_ DDS is designed to solve all strains from all seats. 


## Speed against DDS

One core of an AMD Ryzen AI 9 HX 370. Positions are uniformly random (random cards, strain
and leader), not seen before, with the tables already built. DDS is with
one `SolveBoard` call per position for the exact trick count, on one thread. Every ddclaim
answer was checked against an independent solver.

| Cards per hand | ddclaim positions/s | Time each | DDS positions/s | Speed-up |
|---:|---:|---:|---:|---:|
| 1 | 18,800,000 | 53 ns | 101,000 | 185× |
| 2 | 9,490,000 | 105 ns | 13,600 | 700× |
| 3 | 12,800,000 | 78 ns | 13,300 | 960× |
| 4 | 5,700,000 | 175 ns | 12,800 | 445× |
| 5 | 1,350,000 | 740 ns | 11,600 | 116× |
| 6 | 392,000 | 2.6 µs | 10,000 | 39× |
| 7 | 111,000 | 9.0 µs | 7,670 | 14× |

Most of the gap at these sizes is not search speed: DDS spends 60 to 70 microseconds
setting up each call, which is nearly all of its time on a short ending, and ddclaim has no
per-position setup. These are figures for large batches; see "Batching" for single calls.

## Limits

- **1 to 7 cards per hand, no more.** This is a hard limit of the design: the tables pack
  who holds each card into 28-bit fields, so 28 cards is the most a position can have. On
  larger positions it returns wrong answers (seen at 9 and 10 cards), silently.
  `ddclaim::cards_per_hand` tells you whether a position is acceptable, and the command-line
  tool refuses those that are not.
- **The start of a trick only.** Every hand holds the same number of cards and nobody has
  played to the current trick.
- **x86-64 with AVX-512 and BMI2 on Linux, or Apple silicon on macOS.** It will not build
  for older x86 CPUs. On Arm, `include/ddclaim/port.hpp` supplies NEON code for the AVX-512
  parts and table lookups for pext and pdep (see "Building").
- **`init` once, from one thread.** After that `solve` may be called from several threads
  at once.
- **Tables:** 2.6 GB on disk, about 2.8 GB in memory while solving, and about 3 minutes
  (5.2 GB of memory at the peak) to build the first time.

## Design

ddclaim is an ordinary alpha-beta search made as lean as possible, resting on tables of
endings that are already solved. `NOTES.md` is the design log, with what was measured and
what was tried and dropped.

- **True/false search.** The search answers "can North-South take at least `t` tricks?".
  The exact count comes from a few such questions; a first guess at the answer, from small
  fitted tables (`src/guess_tables.inc`, made by `tools/guess/`), keeps that to about 1.9
  questions per position.
- **A hand in one word.** Each hand is a 64-bit word with 16 bits per suit, so one
  instruction works on all four suits. Runs of touching cards, which play alike, are found
  with bit-extract and bit-deposit instructions. The trump suit is always moved to the same
  place, so there is no per-strain code.
- **Bounds and ordering.** Quick tricks and later tricks stop a search early when the
  outcome is already certain; the first card to try is chosen by rule, with no sorting.
- **Relevant cards.** Each result records which cards it depended on, as DDS does. A
  result that did not depend on the low cards holds for every position that agrees on the
  high ones, which is what lets one table entry answer many positions.
- **Exact tables for 2- and 3-card endings.** Every such ending is stored with its answer
  and the smallest set of cards the answer depends on. A search that reaches three cards
  per hand is one lookup.
- **Learned tables for 4- and 5-card endings.** While building, ddclaim solves about 75
  million random 6-card endings and keeps the patterns it proves about 4- and 5-card
  endings. Each pattern is a proof for a whole class of positions, so an answer taken from
  one is exact; the sampling only decides how often the search can stop early. On random
  6-card endings they answer about 95% of 4-card questions and 49% of 5-card ones.
- **A small table per position** for results found during its own search.
- **Interleaved searches.** `solve` runs eight searches in turn on one thread, each on its
  own small stack. When a search needs a table entry it asks for the memory and hands over
  to the next search, so the entry has arrived by the time it resumes. This is what makes
  2.6 GB of tables fast to use, and it is why batches are faster than single calls.
- **Few unpredictable branches.** Choosing the lead, finding a trick's winner and scanning
  a table bucket are written without branches.

Its only use of vector instructions is in the table probes (eight entries compared at once)
and in reordering suits; the search itself is scalar.

## Interface

`include/ddclaim.hpp`:

```cpp
namespace ddclaim {

struct Position {
  uint16_t hand[4][4];  // [seat][suit]
  uint8_t trump;        // 0..3, or 4 for no trumps
  uint8_t leader;       // the seat to lead
};

int cards_per_hand(const Position& p);   // 1..7, or -1 if solve() cannot take the position
void init();                             // once, before solving
void init(const char* dir);              // the same, with the table directory
void solve(const Position* positions, size_t n, uint8_t* tricks);
bool pin_thread(int cpu);                // optional: pin the calling thread to a CPU

}
```

- **Seats** are 0 North, 1 East, 2 South, 3 West. **Suits** are 0 spades, 1 hearts,
  2 diamonds, 3 clubs.
- **A holding** is a bit mask of ranks: bit 0 is the two, bit 8 the ten, bit 12 the ace.
- **`solve`** writes to `tricks[i]` the number of tricks North-South take in
  `positions[i]`. East-West take the rest. The positions of one batch may have different
  numbers of cards.
- **`cards_per_hand`** returns -1 for a position `solve` must not be given: hands of
  different sizes, more than 7 cards or none, a card held twice, or a trump or leader out
  of range. `solve` does not check.
- **`init`** loads the tables, building them first if they are missing (see "Tables").
  `init(dir)` reads them from `dir` instead of `$DDCLAIM_DIR`.
- **`pin_thread`** pins the calling thread to one CPU; see "Threads".

### Calling it

```cpp
#include "ddclaim.hpp"

int main() {
  ddclaim::init();

  // Spades are trumps and West leads.
  //   North AK of spades, two of hearts      East  QJ of spades, three of diamonds
  //   South T9 of spades, four of hearts     West  87 of spades, five of clubs
  ddclaim::Position p{};
  p.hand[0][0] = 1 << 12 | 1 << 11;  p.hand[0][1] = 1 << 0;   // North
  p.hand[1][0] = 1 << 10 | 1 << 9;   p.hand[1][2] = 1 << 1;   // East
  p.hand[2][0] = 1 << 8 | 1 << 7;    p.hand[2][1] = 1 << 2;   // South
  p.hand[3][0] = 1 << 6 | 1 << 5;    p.hand[3][3] = 1 << 3;   // West
  p.trump = 0;
  p.leader = 3;

  if (ddclaim::cards_per_hand(p) < 0) return 1;
  uint8_t ns;
  ddclaim::solve(&p, 1, &ns);        // ns == 2: North-South take two of the three tricks
}
```

Build your program with `src/ddclaim.cpp`:

```sh
clang++ -std=c++20 -O3 -march=native -Iinclude your_program.cpp src/ddclaim.cpp -pthread
```

### Checking a claim

To check a claim that the side on lead takes `k` of the remaining tricks, solve the position
and compare: North-South's tricks are `tricks[i]`, East-West's are
`cards_per_hand(p) - tricks[i]`. The answer is the exact count, so it settles "at least",
"exactly" and "at most" claims alike.

## Batching

Give `solve` many positions at once. It keeps eight searches going in turn, so that while
one waits for a table entry to arrive from memory the others work; with a single position
there is nothing else to do and it waits.

Measured on random 6-card endings (`bin/batch_sizes 6 100000`):

| Positions per call | Positions/s | Time each |
|---:|---:|---:|
| 1 | 195,000 | 5.1 µs |
| 2 | 231,000 | 4.3 µs |
| 4 | 308,000 | 3.3 µs |
| 8 | 345,000 | 2.9 µs |
| 16 | 363,000 | 2.8 µs |
| 64 | 383,000 | 2.6 µs |
| 1,000 or more | 390,000 | 2.6 µs |

At 4 cards the effect is larger: 2.6 million per second one at a time, 5.7 million in large
batches.

How to batch:

- **Collect positions into one array and make one call.** Results come back in the same
  order, in `tricks[0..n)`. Sixty-four positions per call already gets nearly all of the
  speed; there is no benefit in splitting a large batch, and no upper limit on `n`.
- **Mix freely.** One batch may hold endings of different sizes, strains and leaders. There
  is no need to sort or group them.
- **If positions arrive one at a time,** buffer them and flush when the buffer holds a few
  dozen or when you need an answer. A single call still takes about 5 microseconds for six
  cards, so batching matters only when throughput does.
- **To use several cores,** call `solve` from several threads, each with its own batch
  (see "Threads" below).

```cpp
std::vector<ddclaim::Position> batch;      // fill from your source of positions
std::vector<uint8_t> tricks(batch.size());
ddclaim::solve(batch.data(), batch.size(), tricks.data());
```

## Threads

`solve` is safe to call from several threads at once, each on its own batch, once `init`
has returned. The tables are shared and only read; everything a search writes (its small
table, its stacks, its place in the batch) is kept per thread, about 2 MB each. `init`
itself must be called once, from one thread, before any `solve`. Do not call `solve` from
inside another `solve` on the same thread.

Pinning each thread to its own core can help: otherwise the operating system may put two
of them on the same core. The library supports it with `ddclaim::pin_thread(cpu)`, called
from the thread to be pinned.

```cpp
ddclaim::init();
std::vector<std::thread> pool;
for (int t = 0; t < threads; t++)
  pool.emplace_back([&, t] {
    ddclaim::pin_thread(t);        // optional: this thread runs on CPU t
    ddclaim::solve(batch[t].data(), batch[t].size(), tricks[t].data());
  });
for (std::thread& th : pool) th.join();
```

`bin/threads <cards> <positions> <threads> [cpus]` checks thread safety: every thread's
answers must equal those of a single thread. With a comma-separated list of CPU numbers it
pins the threads to them.

## Command line

`bin/ddclaim` solves positions written as text.

```
ddclaim <trump> <leader> <north> <east> <south> <west>
ddclaim [file]
```

A position is six words:

- the **trump suit**: `S`, `H`, `D`, `C`, or `N` for no trumps;
- the **hand on lead**: `N`, `E`, `S` or `W`;
- the **four hands**, North, East, South, West. A hand is its spades, hearts, diamonds and
  clubs separated by dots, each suit written with ranks from `AKQJT98765432`. An empty suit
  is left empty or written `-`. So `AK.Q..T9` is the ace and king of spades, the queen of
  hearts, no diamonds, and the ten and nine of clubs.

The output is one line per position: North-South's tricks, then East-West's.

One position as arguments:

```sh
$ bin/ddclaim S W AK.2.. QJ..3. T9.4.. 87...5
2 1
```

Many positions, one per line, from a file or standard input. They are solved as one batch,
so this is the fast way to check many. Blank lines and lines starting with `#` are skipped.

```sh
$ cat endings.txt
# trump leader north east south west
S W AK.2.. QJ..3. T9.4.. 87...5
N S AQ.K.. KJ..A. 2.3.4. .A.2.3
$ bin/ddclaim endings.txt
2 1
2 1
$ bin/ddclaim < endings.txt        # the same
```

A position that is not acceptable (hands of different sizes, more than 7 cards, a card
twice, a word it cannot read) stops the run with a message naming the line, and exit status
2; nothing is solved. Run the command from the directory that holds `build/ddclaim`, or set
`DDCLAIM_DIR`.

## Building

```sh
scripts/build.sh
```

This makes `bin/ddclaim` (the command), `bin/bench` (timing and checking),
`bin/batch_sizes` and `bin/threads`. It needs a C++20 compiler (clang++ by default; set `CXX` to change it)
and a CPU with AVX-512 and BMI2, because it compiles with `-march=native`.

On macOS (Apple silicon, with the Xcode Command Line Tools: `xcode-select --install`) the same
command works: the script sees `arm64` and compiles with `-mcpu=native`, adding the CSSC
instructions (scalar popcount and count-trailing-zeros) when `sysctl hw.optional.arm.FEAT_CSSC`
reports them, since Apple clang may know a new CPU only by an older name. The bit and vector
operations then come from `include/ddclaim/port.hpp`: NEON in place of AVX-512, byte-table
lookups in place of pext and pdep. macOS has no huge pages or `MAP_POPULATE` (the pattern
tables are mapped with 16 KB pages and read through once at load), and cannot pin threads
(`pin_thread` asks for the performance cores by quality-of-service class and returns false).
The answers are the same: `bin/bench` checks every answer against altsolve.

## Tables

`init` looks for its table files in `$DDCLAIM_DIR`, or in `build/ddclaim` under the current
directory if that is not set.

- **If they are there,** it reads them into memory: about 0.6 seconds.
- **If they are not,** it builds them first: about 3 minutes on a 24-thread machine, 5.2 GB
  of memory at the peak, 2.6 GB of files. The build runs in rounds and merges each round
  into the files on disk. It uses its own fixed random seeds.

Settings (environment variables):

| Variable | Effect |
|---|---|
| `DDCLAIM_DIR` | Where the table files are read from and built. Read by `init`, so a program may set it just before. |
| `DDCLAIM_LEVELS` | How many levels of learned tables to use and build: 0 to 3 for the 4-, 5- and 6-card tables (default 2). `DDCLAIM_LEVELS=1` builds only the 3-card table and the 4-card table (about 1 GB of files instead of 2.6 GB). |
| `DDCLAIM_MAP=1` | Memory-map the files instead of reading them in. Starts a little faster, but solves about 12% slower. |
| `DDCLAIM_SECONDS` | Time allowed for filling the learned tables when building (default 170). Less gives smaller tables and a slower solver; the answers stay exact. |

### Using the tables from another program

`include/ddclaim/tables.hpp` exports the tables to programs that search larger positions
themselves (as [ddfast](https://github.com/randalburns/ddfast) does for full deals): the
hand-word helpers, the layout of the 2-card, 3-card and pattern tables, `init_tables(dir,
levels)` to load or build them, and the probes. The probes are inline functions in the header,
so they compile into the caller's search; ddclaim's own solver uses the same header. Link with
ddclaim's `src/ddclaim.cpp`, which holds the table data.

## Checking it yourself

```sh
bin/bench <cards> <positions> [seed] [batches]
```

`bench` deals random positions in several different batches, times `solve` on each once,
and compares every answer with altsolve (`test/altsolve.hpp`), an independent search that
uses no tables. It reports any difference and exits with status 1.

ddclaim was exact on every position tried from 1 to 7 cards per hand: several hundred
thousand per size from 3 to 7 while it was developed, and the runs behind the table above
(20,000 positions for each size, 100,000 for six cards). The reference is slow at 7 cards
(about 1,100 positions per second); its answers are cached under `build/`.

## Origin

ddclaim is the winning entry ("open1") of a small competition between solver designs, and
`NOTES.md` is that entry's own log, so it refers to the competition's rules and benchmark.
It is licensed under Apache-2.0.
