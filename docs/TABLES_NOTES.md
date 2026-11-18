> These are the design notes of the competition entry ("open1") that became ddclaim, as its
> author left them. Paths and settings named `open1` / `OPEN1_*` are now `ddclaim` /
> `DDCLAIM_*`, the tables live in `$DDCLAIM_DIR` (default `build/ddclaim`), and
> `entries/open1/entry.cpp` is `src/ddclaim.cpp`. "The bench" and "the rules" are the
> competition's.

# open1

An open entry: a lean zero-window search on top of two exact tables of small endings and
two learned tables of patterns, with several positions searched in turn on one thread so
that table reads are prefetched.

Measured on this machine (Zen 5 core, `taskset -c 2`), all answers checked by the bench.
These are the numbers after the second round of work (see "Second round" below); the bench
now times different batches once each.

| Cards | Command | Positions/s |
|---|---|---:|
| 3 | `build/bench_open1 3 20000 3 1` | 13,100,000 |
| 4 | `build/bench_open1 4 20000 4 1` | 4,150,000 |
| 5 | `build/bench_open1 5 20000 5 1` | 1,080,000 |
| 6 | `build/bench_open1 6 100000 1 3` | 312,800 (three runs: 312,918, 312,715, 312,838) |
| 7 | `build/bench_open1 7 20000 7 1` | 91,800 |

Also correct on a second seed (11) for 3, 4, 5, 6 and 7 cards (20,000 positions each).
About 14,800 cycles, 25,400 instructions and 236 branch mispredictions per six-card
position. The same build measured 333,000 earlier in the session; right after the
three-minute table build on all cores the core ran slower.

## The search

- A hand is one 64-bit word (16 bits per suit); the trump suit is moved to lane 3 and the
  search is compiled twice (trumps, no trumps).
- True/false questions ("can North-South take t tricks?"), starting from a guess (a linear
  fit on top-card counts, trump lengths and the side on lead) and walking up or down. About
  2.15 questions per six-card position, against 2.85 starting from the middle.
- Every result carries the cards that mattered (per suit: a rank and everything above it),
  as in DDS. Results are stored as patterns: the suit lengths of all four hands plus the
  owners of the cards that mattered. A card whose rank did not matter makes the hand's other
  low cards of that suit equivalent, so they are skipped.
- A subtle point that cost a wrong answer early on: touching cards are tried once (the top
  one), and when such a card wins its trick on rank, the whole run down to its bottom card
  must count as mattering (DDS does the same).
- Bounds at the start of a trick: my own quick tricks (the leader cashing top cards, with
  trumps drawn or ruffs excluded) and a simple later-tricks bound (one trick for the top
  trump, or for holding the top card of every suit the leader has at no trumps).
- Ordering: one rule-chosen card first for the second, third and fourth hands, a small
  score for leads. Of the nodes that cut off, the first card does so in 90% (leads) to 99%
  (fourth hand).
- A small per-position transposition table (192-byte buckets keyed on suit lengths, compared
  with one AVX-512 instruction) for five or more cards.

## Tables built by entry_init

None of them depends on the benchmark's seed or positions.

1. **Two-card endings** (3.3 MB): every ending, leader-relative, with the exact answer for
   both targets and a smallest exact set of mattering cards (found by checking every set of
   top cards against all endings with the same suit lengths). Built in 0.3 s.
2. **Three-card endings** (377 MB, `build/open1/t3_v2.bin`): all 369,600 arrangements of 102
   suit-length configurations (side suits sorted) for trumps and no trumps: the exact trick
   count and, for each target, a minimal exact set of mattering cards. Five bytes per
   ending. A search at three cards left is one lookup.
3. **Four- and five-card pattern tables** (750 MB and 1,572 MB, `p4_v5.ent`, `p5_v5.ent`,
   plus small `.idx` files): not complete. entry_init solves random six-card endings (its
   own generator and fixed seeds, unrelated to the bench's) for 170 seconds, about 75
   million of them on this machine, and keeps every pattern the search proved for a four-
   or five-card ending: suit lengths of all hands, owners of the mattering cards, and a
   bound. A pattern is a true statement about a whole class of endings, so a hit is exact.
   Stored leader-relative with side suits sorted; a directory (built in memory from the
   `.idx` file) maps the suit lengths to a run of 16-byte entries, most-hit first, at most
   256 (four cards) or 64 (five cards) per run, scanned eight at a time with AVX-512. On
   the benchmark the four-card table answers 95% of the questions put to it and the
   five-card table 49%.

   The fill works in rounds (up to 3 million endings each, 22 threads): the endings are
   solved against the table files as they stand, and the patterns they prove are then merged
   in by streaming the old file and the sorted new patterns into a new file (one merge per
   table, side by side). No table is assembled in memory. The search uses the files
   memory-mapped. The `.idx` file is written last, so an interrupted build is redone.

From an empty `build/open1/` entry_init took 185 s here (three-card table about 6 s, the
fill 170 s by its time budget, then the last merge), with a peak of 5.2 GB resident during
the fill (mapped table files, their hit counts and one round of new patterns). With the
files present it takes 0.4 s and 2.8 GB stays in use. Files total 2.6 GB. Environment
variables for experiments: `OPEN1_WARM` (endings, forces a rebuild), `OPEN1_SECONDS`,
`OPEN1_MB`, `OPEN1_ROUND`, `OPEN1_CAP4`, `OPEN1_CAP5`, `OPEN1_VERBOSE`, `OPEN1_MAP`.

## Coroutines

The big tables do not fit in cache. entry_solve runs 8 searches in turn on one thread, each
on its own small stack (a 15-instruction context switch, no threads). A search that needs a
table entry prefetches it and switches to the next search; when its turn returns the entry
is in cache.

## What each step gave (six cards, positions/s when the machine was quiet)

| Step | Positions/s |
|---|---:|
| Plain search with quick/later tricks and pattern table | 88,000 |
| Two-card table | about 100,000 |
| First guess from a fit | 127,000 |
| Three-card table with coroutines | 190,000 |
| Four-card pattern table (first version, 1M endings) | 250,000 at best |
| Four- and five-card tables, 8M endings, minimal three-card patterns | 258,000 |

The four-card table went from 45% to 85% hits with sorted side suits, minimal three-card
patterns and 8M endings, but the lookups themselves (two switches each, a scan) cost about
as much as they then saved, so the last row gained little.

## Tried, did not help

- Six-card (root) pattern table: 6.6% hits, 0.5 GB, no gain. Left out.
- Dropping patterns never hit while filling: tables 3x smaller, but hits fell (85% to 79%,
  36% to 24%) and it was 8% slower.
- Larger runs (512 and 256 entries): no change.
- Asking the four-card table before quick tricks: fewer mispredictions, more instructions,
  no change.
- Profile-guided optimization: no change.
- Prefetching a run in chunks, 4 to 64 coroutines: within noise (8 coroutines, chunks of 4
  groups kept).
- Minimal patterns for the two-card table reduced search nodes by only 2%.

## Where the time goes now

Before the second round: about 30,000 instructions and 400 branch mispredictions per six-card position, spread out:
roughly 45% in the start-of-trick work (bounds, table lookups), 30% in the other three
hands, the rest in switches and setup. No single hot spot is left.

## Not done

- Putting the first entries of a run in the directory slot to save one switch per lookup.
- DDS-strength quick tricks (through partner's entries) and later tricks, and DDS's lead
  ordering, for the five- and six-card levels where the search still runs.
- A better first guess: 2.15 questions per position against a floor of about 1.85.

## Second round

Headline command `taskset -c 2 build/bench_open1 6 100000 1 3`, quiet machine.

| Step | Positions/s | Cycles/position | Mispredictions/position |
|---|---:|---:|---:|
| Start (8M endings, tables 0.4 + 0.6 GB, hits 85% / 36%) | 246,000 | about 19,000 | about 400 |
| Larger tables built on disk (63M to 75M endings, 0.75 + 1.57 GB, hits 95% / 49%) | 278,000 to 295,000 | 16,700 | 319 |
| Branch-free rewrites | 312,800 to 333,000 | 14,800 | 236 |

### Larger tables

- The merge now streams to disk (see above), so the build no longer holds two copies of a
  table. What limits the tables now is the 170 s fill budget, not memory or the file limit.
- A larger five-card cap (160 entries per run instead of 64: 2.26 GB, 58% hits instead of
  49%) was not faster: 309,000 mapped, and 357,000 against 367,000 when read into huge
  pages. Longer runs cost more to scan on a miss and touch more pages. So more bytes for the
  five-card table do not buy speed at this point; the cap stayed at 64.
- From 8 to 63 million endings gave 13% to 18%. The hit rates are flattening (the
  four-card table gains about a point per doubling now), so doubling the fill time should
  give a few percent at most.
- **Mapped file against huge pages:** the mapped files are backed by 4 KB pages (this kernel
  has no huge pages for file mappings; the code asks with MADV_COLLAPSE and is refused).
  Reading the same files into anonymous huge pages at init (`OPEN1_COPY=1`, 0.7 s, same
  memory in use) is 6% to 10% faster: 332,000 against 313,000, and 367,000 against 333,000
  earlier. The default is the mapped files, as directed.

### Branch mispredictions

No `perf` binary here; I sampled the branch-miss counter with `perf_event_open` (a signal
every 499 misses, attributed to source lines). Changes kept:

- choosing the next lead: scores in a 16-lane vector, masked maximum (was a loop with
  compares);
- the trick's winning card, winner and "won on rank" flag computed with conditional moves;
- the pattern-table scan: match and bound tests combined into one mask per group, one
  branch per group (was a loop over matches with a test each);
- lowest-rank discard, top-card counting, the j-th top card and the bottom of a run:
  `lzcnt`/`pdep` forms without loops or tests;
- runs prefetched 8 groups at a time (fewer switches).

319 to 236 mispredictions per position, 16,700 to 14,800 cycles. About 60 of the remaining
236 come from the coroutine switches (measured by running one coroutine): after a switch
the return address stack belongs to another search. The rest are spread over the search's
own decisions (cutoff tests, quick tricks, card-picking rules).

Tried and not kept: dropping the per-position table at five cards (1% either way, and it
is needed for seven cards); 6 or 12 coroutines (no change).

## Default table loading (changed after the second round)

The table files are now read into anonymous huge pages at init by default, the faster of the
two ways measured (353,600 against 323,400 six-card positions/s on an unseen seed). Set
`OPEN1_MAP=1` to memory-map the files instead; `OPEN1_COPY` is no longer read.

## Third round: a better first guess (in ddclaim)

Questions per position on random endings (bench seed 1), and speed with
`taskset -c 2 bin/bench <cards> <positions> 1 3`, old and new run alternately on a machine
with some other load:

| Cards | Questions before | Questions after | Floor | Positions/s before | Positions/s after |
|---:|---:|---:|---:|---:|---:|
| 4 | 1.98 | 1.72 | 1.68 | 4,840,000 | 5,740,000 to 5,830,000 |
| 5 | 2.07 | 1.83 | 1.76 | 1,220,000 to 1,280,000 | 1,220,000 to 1,380,000 |
| 6 | 2.15 | 1.92 | 1.83 | 344,000 to 352,000 | 358,000 to 379,000 (390,000 to 392,000 when quiet) |
| 7 | 2.24 | 1.99 | 1.87 | 101,000 to 102,000 | 108,000 |

"Before" questions for 4, 5 and 7 cards and all the floors are from the offline simulation
(which reproduces the measured 2.15 at six cards); "after" is counted by the solver. The
floor is a perfect guess: two questions, one when the value is 0 or all the tricks.
Searched positions per six-card position fell from 84 to 78.

Two changes, both only in which target is asked first:

1. **Where to start.** Starting at target t costs two questions when the value is t or
   t - 1, so the first target should be floor(estimate) + 1, not round(estimate). With the
   old estimate alone this gives 2.07 questions at six cards instead of 2.15.
2. **A better estimate.** An additive model of the leader's side's tricks: one table entry
   per suit, keyed on the suit's length in each hand (leader-relative, capped at 3) and
   which side holds each of its top four cards (4,096 keys; separate entries for a side
   suit and the trump suit), plus a term for the difference in trump length. Separate
   tables for no trumps and trumps and for 4, 5, 6 and 7 cards, 16-bit weights, 173 KB of
   source (`src/guess_tables.inc`). Fitted by least squares on random endings solved by
   ddclaim itself (3M six-card, 2M five-card, 1.5M four-card, 0.6M seven-card; its own
   generator): `tools/guess/dump.cpp` writes the data, `tools/guess/export.py` fits and
   writes the tables. Root-mean-square error 0.60 tricks at six cards against 0.84 for the
   old linear fit; the value is the first target or one less in 91% of positions (71%
   before). Sizes 1 to 3 keep the old guess (they are table lookups anyway).

No table file changed. Exactness does not depend on the guess.

Tried, not kept:

- Keys that also say which hand (not just which side) holds the top one, two or three
  cards: 1.902 to 1.908 questions against 1.915 in the simulation, for tables two to four
  times larger.
- Lengths capped at 4 instead of 3: 1.913 against 1.915.

Not done: narrowing the range with bounds before the first question. The quick-tricks and
later-tricks bounds already answer such a question at once when it is asked, so they save a
cheap question, not a search. What is left to gain is about 0.09 questions per position.

## The tables as a header (include/ddclaim/tables.hpp)

ddfast (an exact solver for full deals) finishes its endings in these tables. Rather than
keep a copy of the probes there, the tables are exported: the hand-word helpers, the table
layout, `init_tables(dir, levels)`, and the 2-card, 3-card and pattern-table probes as inline
function templates. The
probe takes the coroutine switch as a callable, so ddclaim passes its `yield` and a caller
without coroutines passes an empty lambda; a pattern probe returns the key it computed (for
the filling search) and the index of the entry that answered (for hit counts). ddclaim's
`lead` calls the same functions, so there is one copy.

The table state the probes read (the flags, the small lookup arrays and the pointers to the
tables) is declared in the header in an unnamed namespace, so every file that includes it has
its own copy with internal linkage; a file other than ddclaim's fills its copy from
`snapshot()` with `attach()`. A first version declared the state `extern`, with one
definition in ddclaim.cpp: ddclaim became about 2% slower at seven cards (110-111k against
113-114k positions/s, alternating runs), and declaring the same variables `static` again gave
the speed back. With internal linkage the compiler knows nothing outside the file writes them,
so it need not reload them after calls it cannot see into (the coroutine switch).

Same answers (bench against altsolve), and the same speed within noise: six and seven cards,
the old and the new binary alternating, pinned to CPU 0 (numbers in the commit message).

Fixed at the same time: the path of the 3-card table was a global computed at start-up, so a
program that set `DDCLAIM_DIR` in `main` (or later) before `init` still read, and if missing
rebuilt, the table in the old directory. It is now computed when the table is loaded.
`init(dir)` and `init_tables(dir, levels)` take the directory directly; `DDCLAIM_LEVELS` (which
already chose the pattern levels) is documented, and with 1 only the 3-card and 4-card tables
are built.
