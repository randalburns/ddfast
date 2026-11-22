# ddfast: design and measurement notes

These are the working notes from ddfast's development, kept as they were written: each round
records what was tried, what was measured and what was kept. ddfast began as "entry 3" in a
speed competition between double-dummy solvers, "ddsfast" (exact answers on full deals, timed
on a single core against DDS); the branch it was developed on and the namespace of its first
interface had that name too. The notes use that history's file names:

| in the notes | in this repository |
|---|---|
| `lean.cpp` | `src/solver.cpp` (table building removed: it is ddclaim's) |
| `dds_port_core.hpp` | `src/dds_port.hpp` |
| `batch.cpp`, `batch.hpp`, `entry.cpp`, `ddclaim.hpp` | `src/api.cpp`, `include/ddfast.hpp` |
| `dev/harness.cpp`, `dev/t.sh`, `dev/ab.sh` | `bench/bench_solve.cpp` (`set` mode prints cycles per deal) |
| `dev/batch_bench.cpp` | `bench/bench_solve.cpp` (`parallel` mode) |
| `dev/table_bench.cpp` | `bench/bench_tables.cpp` |
| the referee, `make_ref`, `public.set` | `tests/make_ref.cpp`, `tests/data/public.set` |
| "the public set" | `tests/data/public.set`: 600 random deals with DDS's answers |

Measurements are cycles per deal from hardware counters on one fast core (CPU 0 of an AMD
Ryzen AI 9 HX 370) unless stated. Node counts come from `-DSTATS` builds: the final code
searches 84,841,515 nodes on the public set with ddclaim's tables as built on that machine
(the pattern table is filled for a fixed time, so a table built elsewhere can differ, and so
can the count; the answers cannot). Some experiment switches mentioned below (ORACLE, MEMN,
TTPICK and others) are still in the source, off by default.

---

# Entry 3 notes: a lean 13-card solver in ddclaim's style

Files: `lean.cpp` (ddclaim's source, widened), `dds_port_core.hpp` (scalar ports of DDS's
quick/later tricks and move weights, from ddscheck/tools/vecdds), `entry.cpp`,
`dev/harness.cpp` + `dev/build.sh` (development harness: random n-card endings or a test
set, checked against DDS).

## Round 1: correct at 13 cards (all 600 public deals), 6x slower than DDS

Done:
- Wide transposition table for 8+ cards per hand: buckets of 8 entries (256 bytes), exact
  key (suit lengths, leader, card count) packed in spare bits, cards-that-mattered mask of
  up to 52 bits and two 52-bit owner strings; 2^18 buckets shared by all searches, tagged
  with a per-deal generation. ddclaim's own small table still serves 5 to 7 cards.
- DDS's QuickTricks, LaterTricks, QuickTricksSecondHand and move weights at levels with
  n >= DMIN cards per hand, ddclaim's own rules below.
- Table files are only read (this copy exits instead of building them). A bug found on the
  way: ddclaim's three-card table path was computed before DDCLAIM_DIR was set, so it was
  silently rebuilt on every start.

Measured (CPU 2, harness; DDS column is SolveBoard on the same positions):

| cards | plain widening | DMIN=8 | DMIN=6 | DMIN=4 | DDS |
|---|---|---|---|---|---|
| 8 | 21,800/s | 20,900 | | | 3,560 |
| 9 | 4,590/s | 5,250 | | | 1,900 |
| 10 | 1,370/s | 1,620-1,900 | 1,690 | 1,400 | 930 |
| 11 | 116/s | | 240 | | 419 |
| 13 (public, 600) | | 13/s | 12/s | | 71 |

Lead nodes per 10-card position: DMIN=8 4,156; DMIN=6 2,667; DMIN=4 2,115.

## Round 2: the table was the problem; now faster than DDS

Referee, public set, CPU 2: **93.2 deals/s (10.7 ms)**; top10 21.2 deals/s (47.3 ms);
top1 10.7 deals/s (93.5 ms). All correct. (DDS: 71.3 / 61.6 ms / 122 ms.)

What changed, in order of effect (600 public deals, total nodes = lead + follow nodes):
- Interleaving 8 deals on one thread (ddclaim's coroutines) made full deals 2x slower: the
  searches evicted each other's table entries. Now one deal at a time (NCTX=1).
- ddclaim's 1,024-bucket per-position table replaced for all levels >= 5 cards.
- A bucket of 8 patterns per suit-length key is far too few on full deals: 80% of stores
  evicted a live entry. Chaining 4 buckets per key: 374M -> 165M nodes; 16: 130M (DDS: 138M
  search calls). So the node gap to DDS was table capacity per key, not ordering.
- Final structure: per-deal directory (key -> chain of 8-pattern blocks) with blocks taken in
  order from a pool that is reset per deal. Nothing is ever evicted, a deal's table is one
  compact region, patterns are 16 bytes up to 7 cards (two 28-bit owner strings) and 24
  bytes from 8 cards (two 52-bit strings). 123M nodes, 9.8-10.7 ms per deal.
- DDS port ordering in follow nodes only from 9 cards (DFOL=9): the port costs more than it
  saves lower down. Lead ordering and quick/later tricks from the port at >= 6 cards.

Sizes now (harness, random endings): 8 cards 24,000/s (DDS 3,560), 9: 8,780 (1,900),
10: 3,240 (930).

## Round 3: 131 deals/s

Referee, public set, CPU 2: **131.0 deals/s (7.63 ms)**; top10 30.5 deals/s (32.8 ms);
top1 15.5 deals/s (64.5 ms). All correct, 1.1 GB. (DDS: 71.3 / 61.6 ms / 122 ms.)

- Per-deal table probed before DDS's quick tricks (half of all lookups hit, and a lookup is
  cheaper than the port's QuickTricks): 9.7 -> 7.0-7.7 ms.
- The 5-card pattern table (1.57 GB) is worth only 6% of nodes on full deals and nothing in
  time: dropped. The 4-card table is worth 58% of nodes (20% of time), the 3-card table 16%
  of nodes (10% of time). 4-card pattern hits are copied into the per-deal table (p4 probes
  6.2M -> 1.7M per 600 deals): 3%.
- Did not help: prefetching the pattern-table directory and entries early in the node (the
  search has no slack to hide a memory miss); interleaving 2-8 deals with per-deal tables
  (NCTX: 1: 9.7 ms, 2: 10.8, 4: 13.0, 8: 12.5); a growable directory (kept, harmless).
- An oracle first target (the true value) is only 8-10% faster than the fitted guess
  (3.16 queries per deal): little to gain from a better first guess.
- Nodes per deal now 207,000 (lead + follow), DDS 230,000 search calls. About 37 ns per
  node. gprof: lead 31% (incl. pattern/3-card table probes, store), follow 31%, table
  lookup 13%, DDS-port ordering 15%, port quick tricks + top cards 9%.
- Timing noise from the other entries is +-10%; use dev/cmp.sh (alternating A/B, best of n).

## Round 4: measuring in cycles; 190 deals/s

The harness now reads hardware counters (cycles and instructions per deal, user mode):
repeatable to about 1%, where wall time moves +-10% with the other entries running.
`dev/t.sh "<flags>"` builds a variant and prints them. Cycles per deal on the public set:

| change | cycles/deal |
|---|---|
| start of round (DMIN=6, DFOL=9) | 31.8M |
| DDS-port bounds and lead ordering at all levels >= 4 cards (DMIN=4) | 30.3M |
| table's best card passed as DDS "best move" (+55), killer as "tt move" (+18) | 29.2M |
| port QuickTricks only when the side's top cards can reach the tricks needed | 28.6M |
| fitted first target for 13 cards (3.16 -> 2.39 queries per deal; oracle: 25.7M) | 28.0M |
| quick/later-trick cutoffs stored in the per-deal table | 25.9M |
| no port ordering in follow nodes at all (DFOL=99), moves generated with pext/pdep | 25.5M |

Did not help (cycles): history heuristic on leads (29.4-30.3M vs 28.9M); forcing the table's
best card first (30.1M vs 29.2M); forcing the killer first (34.6M); port ordering in follow
nodes at any level and per hand (DFOL 6-12: equal or worse); port lead ordering only higher
up (DORD=6: +2%, 8: +10%, none: +69%, so DDS's lead weights are essential and ddclaim's
follow rules are as good as DDS's); second-hand quick tricks only higher up (DQ2=6: +6%);
later-tricks filter; storing the 5-card level in the table from 5 (TT_MIN=5: +7%).

Exactness: 6,000 fresh random full deals (harness `13 3000 7` and `13 3000 8`) and the 600
public deals all agree with DDS.

## Final state (round 5)

Referee, public set, CPU 2 (two runs of `all`): **198.1 and 195.8 deals/s (5.05-5.11 ms per
deal)**; top10 46.9 deals/s (21.3 ms each); top1 23.1-26.1 deals/s (38-43 ms each, 6 deals).
All 600 correct; init 0.4 s; 1.1 GB. DDS baseline: 71.3 deals/s, 61.6 ms, 122 ms.
25.2M cycles and 63.8M instructions per deal; 177,000 nodes per deal (DDS: 230,000 search
calls); 2.39 true/false questions per deal.

The entry does not use DDS at all (build_entry.sh still compiles the library because the
development harness links it for reference answers). It reads only the 3-card table and the
4-card pattern table of ddclaim (read-only; it exits rather than build anything).

### By size (harness, random endings; DDS = one SolveBoard per position, same machine)

| cards | lean /s | DDS /s | ratio | nodes/position | cycles/position |
|---|---|---|---|---|---|
| 8 | 26,900 | 3,560 | 7.6 | 720 | 169k |
| 9 | 10,400 | 1,900 | 5.5 | 2,130 | 431k |
| 10 | 3,970 | 930 | 4.3 | 6,160 | 1.11M |
| 11 | 1,280 | 419 | 3.1 | 21,500 | 3.45M |
| 12 | 460 | 172 | 2.7 | 64,000 | 9.44M |
| 13 (3,000 random deals) | 143 | 63 | 2.3 | 203,000 | 29.7M |

(8-12 use ddclaim's crude first guess, about 2.5 questions per position; only 13 has the
fitted one. The DDS column was timed once while other entries ran, so +-10%.)

Growth per extra card. Before (plain widening of ddclaim, round 1): time x4.7 (8->9), x3.4,
x11.8 (10->11, where the small tables thrashed); 13 cards 6x slower than DDS. After: cycles
x2.5, x2.6, x3.1, x2.7, x3.1; nodes x3.0, x2.9, x3.5, x3.0, x3.2. DDS's time on the same
sets: x1.9, x2.0, x2.2, x2.4, x2.7 (flattered at small sizes by its fixed cost per call).
So the lean solver still grows a little faster than DDS, but from a much lower base.

### Tried in round 5, no gain (cycles per deal, baseline 25.0-25.6M)

- First two patterns of a key inside a 64-byte directory slot: 27.1M (the directory gets 4x
  larger and stops fitting the cache). Dense 16-byte slots + blocks is better.
- 4-pattern single-line blocks for <= 7 cards: 25.6M (top 60 deals: 108.6M vs 104.3M).
- Tightening a matching pattern in place instead of appending: 26.2M vs 25.8M.
- 3-card table results cached in the per-deal table: 25.4M (same).
- 5-card pattern table back on: instructions -2%, cycles +4% (memory misses).
- 4-card pattern table before the bounds: 25.9M; killer per hand: 26.2M; second-hand
  quick-tricks filter: same or worse; QSLACK=1, DIR_MIN=12, PCHUNK=2: same.
- Follow-node ordering path made dead at compile time (kept): instructions -3%, cycles -1%.
- Table reset paths (pool or directory full) checked exact with tiny tables
  (POOL_BITS=10 DIR_MAX=13, and DIR_MAX=11): all 600 correct.

### Where the time is now (gprof, three runs)

lead nodes 23% (own code incl. table store, 3/4-card table probes), follow nodes 29%,
per-deal table lookup 13%, DDS-port lead ordering 15.5% (about 70 ns per expanded lead),
port quick tricks 4.6%, top-cards fill 4.4%, second-hand quick tricks 1%.

### What I would do next

1. Rewrite DDS's lead weights, quick tricks and the top-cards fill in ddclaim's
   bit-parallel style: the ports are a quarter of all cycles and lead ordering is essential
   (without it +69% cycles).
2. Lead ordering better than DDS's: first lead cuts in 91.5% of cut nodes (follow hands:
   97-99%). A simple history bonus did not help; a per-pattern best card might.
3. First target: the oracle is 25.7M against 28.0M cycles when measured (8% left); the
   no-trump model is the weak one (58% in the two-question window, suits 69%).
4. Memory misses of the 3- and 4-card tables (about 6,000 probes per deal): interleaving
   deals did not pay for full deals; a denser 4-card table might.

## Round 5: lead ordering headroom (after the competition)

Nodes over the 600 public deals (STATS build): 106.1M before.

- Ceiling: solving each deal again with every lead node's cutting lead remembered from the
  previous pass (exact position and target, `-DORACLE`): 77.5M nodes (-27%). That is the most
  any lead ordering can give on this tree.
- Kept: extra weight HX=50 for the table's best lead (the per-key hint) on top of DDS's own
  bonus: 104.5M nodes (-1.5%), instructions -0.7%, cycles within noise. Removing the hint
  altogether costs +29% nodes, so it is the most valuable ordering input after DDS's weights.
- No gain (all removed): a best lead stored per pattern and used when the pattern matches
  but its bounds do not decide (present in only 12% of cut nodes; 106.9M); an exact-position
  best-lead memory within one solve (106.6M); trying first the lead that was hardest to
  refute on an earlier visit (108.9-109.8M); a hint per (key, target) (103.4M with HX, -1%,
  not worth a second table); a fallback hint keyed on the leading side's suit lengths
  (103.95M, -0.5%); larger or smaller killer bonus (flat).
- Searching leads in turn with a node budget cannot help: when the first lead fails, the
  failed leads cost about as many nodes as the lead that then cuts (root: 9.5M wasted against
  6.5M for the cutting leads).
- First-lead cut rate by cards: 84% at 4, 90-94% from 5 to 12, 87.7% at the root. Nodes
  wasted before the cutting lead are 7-9% of all nodes at each of the levels 10 to 13.
- A missing hint does not predict a node where every lead fails well enough to skip the
  weights there (30% of such nodes still cut).

## Round 6: DDS's lead weights and second-hand quick tricks on the hand words

- `native_order`: weight_alloc_trump0 and weight_alloc_nt0 rewritten on the 64-bit hand
  words (suit-level terms once per suit, a few terms per card), no port position needed.
  `native_second`: QuickTricksSecondHand likewise. Both give exactly what the port gives:
  `-DVERIFY_ORDER` runs the port beside them on every call and aborts on any difference
  (cards, keys, result, relevant cards); none over the 600 public deals.
- Cycles per deal on the public set: 25.1-25.6M -> 22.7-23.0M (-9%); instructions 63.4M ->
  60.5M. `-DPORT_ORDER` brings the ported weights back.
- Still through the port: QuickTricks, LaterTricksMIN/MAX at lead nodes (with fill_tops and
  fill_holdings, together roughly 8% of the time). QuickTricks is about 900 lines of cases;
  an exact rewrite would save perhaps half of that 8%.

## Round 7 (b-bounds): lower cost per node

Measured on CPU 1 with `t.sh` (cycles and instructions per deal, public set).
Start: 22.8-23.0M cycles, 60.5M instructions; 104.5M nodes over the 600 deals (STATS).

- Lead-node QuickTricks, LaterTricksMIN and LaterTricksMAX rewritten on the hand words
  (`nq_quick`, `nq_ltmin`, `nq_ltmax`: the port's cases mirrored one by one, relevant cards
  kept as one word with the port's win_ranks in lanes; top/second/third cards found with
  lzcnt per lane, owners from three bit tests, lengths from LENS). No port position is built
  any more at lead nodes (fill_holdings/fill_tops gone from the normal build).
  `-DVERIFY_BOUNDS` runs the port beside them and aborts on any difference in quick-trick
  count, decision, or raw relevant cards (after QuickTricks always; after LaterTricks when it
  decides): none over the public set and random positions (harness 13x600, 12x300, 10x2000,
  8x5000, 6x20000, 4x20000; all also correct against DDS). `-DPORT_BOUNDS` restores the port.
  Node counts identical (104,485,611).
  Cycles 22.9M -> 21.5M per deal (-6%), instructions 60.5M -> 55.0M (-9%).
  (Note: the random-position harness must be run from the worktree root; elsewhere it
  segfaults on the relative table path.)
- Component costs measured by calling a function twice per use (`-DDUP_TT`, `-DDUP_BOUNDS`,
  `-DDUP_ORDER`; search unchanged, delta = one more call, warm): table lookup +2.1M cycles
  (+8.1M instructions), lead-node bounds +1.5M (+8.0M; about 470 instructions per call,
  16k calls per deal), native_order +2.7M (+12M; about 1,000 instructions per expanded lead).
  harness: `BRM=1` also prints branch misses (247k per deal, about 1.4 per node) and cache misses.
- Local copies of the hands inside the native bounds (aliasing): no change (55.03M instr).
- `S` (thread_local pointer initialized with the address of a thread_local) needed a TLS
  wrapper call on every access. Now constant-initialized to null and set by `ensure_state()`
  in init, solve_position, solve_one and the table-building threads. Instructions 55.0M ->
  53.0M, cycles 21.2-21.5M -> 20.8-21.25M.
- native_order with per-suit class tables (base, rr and rank coefficients, hint/killer
  bonus) and branch-free per-card arithmetic: exact (VERIFY_ORDER, public set and random
  positions), but instructions 52.98M -> 53.34M and cycles flat (21.3M). Reverted.
- Sampling profiler in the harness (`PCS=<file> EV=0|1`: PC at every cycles / branch-miss
  overflow; build with -fno-pie -no-pie -g, map with addr2line -i). Lines are diffuse since
  everything inlines into lead: table-lookup block loop about 10%, 4-6 card pattern probe
  about 3%, NB::init 1.4%; branch-miss samples look like the cycle samples (large skid).
- Filter and top-card pass of the bounds merged into one loop: instructions 52.98M -> 53.81M.
  Reverted.
- End of round (stopped at the tool-call cap): 21.0-21.8M cycles and 53.0M instructions per
  deal (start 22.8-23.0M and 60.5M). The referee gave 140.7 deals/s, all correct, but the
  machine was shared, so that number cannot be compared with earlier rounds.
- Next: table lookup (directory probe and block loop about 10%); native_order is about 1,000
  instructions per expanded lead and needs a different shape (vector over cards), since class
  tables did not help; branch-free card choice at hands 2-4 of follow.

## Round 7 (agent b-nodes): fewer nodes without touching lead ordering

Baseline at the start (public set, CPU 2, `dev/t.sh`): 22.4-22.6M cycles, 60.54M instructions
per deal; 104.5M nodes over 600 deals (174,000 per deal); 2.39 questions per deal.
New `dev/m.sh "<flags>" [N]` runs a variant N times and prints the lowest cycles (single runs
moved by up to 4% this round with three other agents on the machine).

- Where the nodes go by question (STATS now prints each question's target, result and nodes
  on the GUESS line): question 1 44% of nodes (77k per deal), question 2 45% (79k), 3+ 10%.
  The second question costs as much as the first: it asks for the opposite side's proof.
- Fail-soft bounds (lead and follow return a proven bound beyond the target; the table stores
  it; the root skips questions it already settles): nodes -0.5%, questions not reduced at
  all (1433 both), instructions +3%, cycles +1.8%. Dropped (diff kept out of the tree). The
  bounds that come back are almost never stronger than the target: leaves (quick tricks,
  3-card table, table entries) only prove the target.
- Run rule (RUNOPT=1, now default): the trick winner's card alone is relevant (was: its whole
  run of touching cards); at a node that fails with every card, the run below each tried
  relevant card is added. Nodes 104.5M -> 101.3M (-3.0%), instructions 60.54M -> 59.69M
  (-1.4%), cycles 22.41M -> 22.37M (best of 4; within noise).
- Self-test of relevant cards (`-DSELFTEST=k -DSELFTEST_MAX=m`): every k-th searched lead node
  of 4..m cards re-deals the cards below its relevant ones (same suit lengths per hand) and
  re-solves with a separate empty table; any difference aborts. RUNOPT=1, k=10, m=11 over the
  600 public deals: 695,794 checks, no failure. `-DSELFBREAK` (drop one relevant card) fails
  at once, so the test bites.
- Narrowest deciding table entry (TTPICK=1, now default): when a block holds more than one
  entry that decides the question, the one with the fewest relevant cards is used (vector
  popcount + masked min, only when more than one decides). Nodes 101.3M -> 98.1M (-3.3%),
  instructions 59.69M -> 58.28M; A/B in cycles (`dev/ab.sh`, 6 alternating runs) -1.5%
  (22.41 -> 22.08M min). The first version with a scalar loop on every hit was +1% cycles.
  Scanning all blocks for the narrowest (TTPICK=2): nodes -4.4% but cycles +2.7%. The same
  choice in the 4-card pattern table (PPICK=1): nodes -0.26%, left off.
- Follow-node failures (STATS: first-card category x cutting-card category per hand): the
  biggest avoidable ones were void hands. Third hand discarded while its partner (the leader)
  was winning, although the last hand could beat that card (54k cases); second hand ruffed
  where the third hand could overruff and discarding was right (87k). VOIDR=1 (now default):
  third hand ruffs when the last hand would beat partner's card (above the last hand's trumps
  if it is void too); second hand does not ruff low into an overruff when it can discard.
  Nodes 98.1M -> 95.3M (-2.9%); A/B cycles 22.16 -> 21.41M min, 22.45 -> 22.15M median.
- Other measured follow waste (not acted on): second hand playing a covering card where low
  was right and the reverse, both large at 10+ cards; DDS-port follow ordering was already
  found no better (round 4).
- Exactness of the final state: 600 public deals and 1,500 fresh random deals
  (`harness 13 1500 4242`) all equal to DDS; the self-test above covered the run rule.
  TTPICK and VOIDR do not change what is proven (any deciding entry is valid; VOIDR only
  reorders moves).
- Not done (budget): enhanced transposition cutoffs at lead nodes. A lead's child positions
  are follow nodes with no table; the first table position is after the full trick, so a
  lookup needs a guessed 3-card continuation per lead (cheapest proxy: each follow hand's
  first-choice card, which cuts 97-99% of the time at hands 2-4).

Totals this round: nodes 104.5M -> 95.3M over 600 deals (174k -> 159k per deal, -8.8%);
cycles about 22.4M -> 21.4-22.1M per deal (-2 to -4%; noisy machine).

## Round: a better first guess for full deals (branch comp/b-guess)

The fitted first target (guess_full) is replaced by a small MLP over richer features
(dev/fit_guess2.py; weights in guess_full.inc; the old model stays under -DOLD_GUESS).
- Training data: 106,000 random full deals with DDS answers (harness DUMP mode, seeds
  201-204 x 25,000 plus the earlier 6,000), held-out test: 3,000 random deals (seed 4242).
  No public or hidden deals were used for training.
- Features (leader's side minus the other side, per suit, summed over side suits with the
  trump suit apart; all suits together in NT): owners of A K Q J T 9, the top run held by one
  side capped by its longest hand, length beyond the other side's longest hand (also when the
  other side has no stopper A/Kx/Qxx/Jxxx), stoppers, pooled-holding suit tricks per side
  (k-th best card against k-th best), length beyond seven, cards beyond the fourth; NT also
  best suit per side, all-suits-stopped, leader's longest suit; suits also trump lengths,
  ruffing value per hand (shortness capped by trumps), trump fit beyond eight.
- Model: one tanh hidden layer of 24 units, softmax over the leader's side's tricks 0..13;
  the first target minimises expected extra cost. Cost of a miss, measured with ORACLE offsets
  on 1,500 random deals (instructions/deal, millions): offset (NS target - value) -2: 68.7,
  -1: 66.5, 0: 60.4, 1: 60.3, 2: 67.9, 3: 70.6 (fitted guess: 64.0). Symmetric: only the
  two-question window matters much; out by one costs ~+10%, out by two ~+14%.
- Share inside the window, held-out random deals (suit / NT): old 0.717 / 0.556; linear
  regression with the new features 0.753 / 0.608; softmax 0.765 / 0.631; MLP-24 0.801 / 0.693;
  MLP-48 0.802 / 0.701 (overfits a little, not taken). Questions per deal (suit / NT):
  2.304 / 2.577 -> 2.189 / 2.377.
- Public set: window 0.686 / 0.582 -> 0.789 / 0.709; questions per deal 2.39 -> 2.25.
  t.sh (CPU 3): instructions 60.54M -> 59.53M per deal (-1.7%); cycles 22.5-24.0M -> 22.0-22.2M
  (2-7%, noisy, alternating runs). The guess costs a few thousand cycles, nothing next to 22M.
- Exact: referee on the public set all correct (175 deals/s on CPU 3 under load); harness
  13 3000 9191 and 4242 all correct.
- What did not help: a softmax on the 6,000-deal set (no better than linear); an MLP on 6,000
  deals (overfits badly, NT 0.51). Data volume was what unlocked the MLP.
- Remaining gap to the oracle: the oracle saves ~5.6% of instructions over the old guess; the
  new guess recovers about 1.7 points of that. More data or features (entries, NT suit
  combinations solved exactly per suit) might lift the window to ~0.85, worth ~1-2% more.

## Round 7 (agent b-second): can the second question be made cheaper?

Base: entry 3 as committed (08e588c1; without b-nodes' round 7 changes). Public set: 174,064
nodes per deal (STATS, all questions), 2.39 questions per deal, 60.54M instructions, 22.4-22.7M
cycles (`dev/t.sh`, three alternating runs). STATS now prints on the GUESS line the leader,
and for each question its target, result and nodes; and per side (the root leader's side or
the opponents) the cut rate, the waste and the nodes under all-nodes at every level.

Where the nodes are, in the leader's terms (v = value, question "the leader's side makes t"):
- t = v+1 (false, tight): 129k when asked first, 85k when asked after another; t = v (true,
  tight): 67k first, 60k later; t = v-1: 35k first, 11k later; t = v+2: 51k. Questions whose
  root leads all fail (the false ones) hold 61% of all nodes (64M of 104M lead nodes).
- But the order of the two tight questions does not matter: on the 192 deals asked {v, v+1} in
  opposite orders by two runs, 184k (false first) against 182k (true first). The first question
  is cheaper only by what it leaves in the table for the second; the pair costs the same.
- Asking loose questions first is almost free: starting two tricks below the guess for the
  leader (GOFF=-2, 3.4 questions per deal) gives 172.4k nodes, but instructions +3.5%.

Tried:
- Which question first, by expected cost: GB (the rounding of the fitted guess, in 1/256 trick;
  was 256, i.e. the upper of the two likely values). Public set nodes: GB 64 173.8k, 128 172.9k,
  192 172.9k, 256 174.1k, 320 176.0k; same shape for suits and no trump separately. Fresh random
  deals (`harness 13 1500 777`): 256 176.2k, 192 174.5k (-1.0%), 128 173.7k (-1.4%, but 2.49
  questions). Kept GB=192: instructions 60.54M -> 60.13M (-0.7%), cycles three A/B pairs
  22.43/22.69/22.54M -> 22.25/22.51/22.19M (-1.1% min to min).
- Root leads of a true question that follows a false one, ordered by what each lead cost to
  refute in the false one (cycles, rdtsc; `-DROOTORD=1`, off): root waste 9.21M -> 8.93M but
  total nodes 174.1k -> 177.6k (+2%): the hardest-to-refute lead does cut, but with a larger
  proof than the lead DDS's weights choose. Same finding as round 5's "hardest to refute first".
- Lead-by-lead at the root with windows from what is known: no room. The root is one max
  layer; leads that fail in a true question are already settled by the table for the next
  question (their positions after trick 1 hold the bound), and in the false question every
  lead must be refuted anyway. Root waste (leads tried before the cutting one in true
  questions) is 9.2M nodes, 8.8%: first lead cuts in 87.6% of root cut nodes, 88.8% at 12
  cards for the root leader, 96-97% for the opponents at 8-12 cards.

Conclusion: the two tight proofs are nearly disjoint trees of fixed total size; their order
and the first guess move totals by about 1%. Savings must come from making each proof smaller
(root and 12-card lead ordering for the root leader's side: 12.5M wasted nodes, 12%).
Exactness: referee on the public set and `harness 13 3000 9191`, all correct.

## Round B: batch throughput on the whole machine (agent b-batch)

All numbers in this round were taken while three other agents loaded CPUs 0-2 and 4-7 (load
average 4-9; unpinned compiles and a 3-thread python also landed on my CPUs at times). I
used only CPUs 3, 8-11 and SMT siblings 15, 20-23. Wall-clock deals/s varied up to 40%
between identical runs, so comparisons below use **user-mode cycles per deal per thread**
(read from hardware counters in each worker), which are much steadier (about +-3%).

Cache topology matters here: CPUs 0-3 (Zen 5) share a 16 MB L3; CPUs 4-11 (Zen 5c) share
one 8 MB L3, so my 8-11 shared L3 with the other agents' 4-7 throughout.

### The batch call

- `competition/batch.hpp/.cpp`: `ddsfast::solve_parallel(problems, n, tricks, cpus, ncpu,
  order, stats)`. A persistent pool of worker threads, one pinned per listed CPU, kept between
  calls while the CPU list is unchanged (per-thread tables stay mapped). Deals are handed out
  one at a time from an atomic counter, predicted-costliest first. The ddclaim tables (about
  1.1 GB) are shared; each worker adds about 10 MB (peak RSS 1119 MB with 1 worker, 1209 MB
  with 10). `entry_solve` is unchanged and still single-threaded.
- `competition/dev/batch_bench.cpp` (+ `build_batch.sh`): runs the call on a CPU list and
  checks every answer against the sets' reference answers; prints total deals/s, idle share
  at the batch end, per core type deals/s per thread, cycles/deal and effective GHz.
- `competition/dev/batch_bench.sh [full|dev]`: all configurations. Sets: public.set plus
  four 1,000-deal sets from the referee's `make_ref` (unmodified DDS), seeds 9100-9103
  (made on first run into `competition/dev/sets/`, not committed).
- Exactness: every run in this round checked every answer (4,600 deals per call in most
  runs, about 100k deal solutions in all): all correct. Referee on public.set, CPU 3:
  all correct (136 deals/s under load).

### Cost predictor for the deal order

Per-deal times of all 4,600 deals on one core: mean 4.6 ms, 99th percentile 33.7 ms, max
134 ms. log(time) correlates 0.94 with log(DDS nodes), so the reference node counts are a
good "oracle" order. A first hand-written predictor (NT, balanced suits, HCP split)
correlated 0.05: useless. A linear fit of log(time) on 12 cheap features (NT, declarer HCP
and its square, declarer's trump fit and its square, shape, voids, singletons, best fit,
HCP split within sides) reaches rank correlation 0.43 on held-out deals; kept in
`predict_cost`.

Idle share at the end of the batch, 5 threads (3, 8-11), public set (600 deals), three runs:
given order 1.6-2.4%, predicted 0.4-1.7%, oracle 0.1-0.2%. Deals/s moved by the same amounts
(+1% predicted, +2-4% oracle), inside the noise. With 4,600 deals the given order already
idles only 0.2-1%: for large batches the order hardly matters; it matters for batches of a
few hundred deals per thread or less.

### Configurations (4,600 deals per call, dev CPUs, under load)

| CPUs | threads | deals/s | per thread | cycles/deal | GHz |
|---|---|---|---|---|---|
| 3 | 1 | 216 (128-165 in later runs) | | 24.5M | 3.1-4.0 |
| 3,15 (SMT) | 2 | 240 (179-262) | | 34.0M | 3.1-4.4 |
| 8-11 | 4 | 383 (363-373) | 92-96 | 25.1M | 2.3 |
| 8-11,20-23 (SMT) | 8 | 493 (444-536) | 57-69 | 34.7-37.9M | 2.2-2.4 |
| 3,8-11 | 5 | 456-692 | fast 116-182, slow 85-128 | | |
| 3,8-11,15,20-23 | 10 | 620 | fast 89, slow 56 | | |

- Slow cores are not slower per cycle: 25M cycles/deal, like a fast core (24.5M). They are
  slower by clock only, and under this load they ran at 2.2-2.4 GHz, not 3.29 (the first,
  quieter run: 128 deals/s per slow core vs 182 per fast core, ratio 0.70).
- **SMT pays here, at equal clock**: cycles per deal per thread rise only 1.36-1.51x with two
  threads on a core, i.e. +32-44% per core on the slow cores and +40-44% on fast core 3. The
  earlier "+3% with 8 processes on 4 fast cores" was with processes; see below.
- **Threads versus processes**: 8 processes (one per CPU on 8-11,20-23) cost about the same
  cycles per deal per thread as 8 threads (first run 34.8M vs 34.7M), but each process holds
  its own 1.1 GB copy of the tables (the pattern tables are read into private huge pages).
  In a second run the 8 processes pushed the machine into memory pressure and ran at half
  speed (apparent 1.1 GHz). 24 processes would need about 26 GB on a 14 GB machine; 24
  threads need about 1.4 GB. So one process with a thread pool is what makes the full machine
  usable at all, and SMT should be re-measured with threads on an idle machine.

### Per-thread table sizes and huge pages (cycles/deal, 4 threads on 8-11, 1,000 deals)

All within noise (+-3%): default 24.4/25.4M; TT_BITS_N=9 24.5/24.9M; TT_BITS_N=11
24.8/24.8M; DIR_MIN=10 24.6/24.3M; DIR_MIN=13 25.2/24.3M; PCHUNK=4 26.2/24.3M. Huge pages
(new switches `LEAN_NOHUGE`, all 4 KB pages, and `LEAN_NOHUGE_TT`, per-thread tables only):
default 23.9/23.8M, no huge pages for the per-thread tables 24.1/24.0M, none at all
24.5/24.8M: huge pages give about 2%, the per-thread tables' share of it nothing.
Nothing amortises across deals inside a core beyond what entry 3 already does (the table is
reset by a generation bump; the per-deal clears are about 2 KB).

### Round B, part 2: the 4 fast cores only (goal changed by the user)

Measured on CPU 3 and its sibling 15 only, other agents on 0-2 and 4-7 (fast core clock
3.1-4.4 GHz instead of 5.16, so compare cycles). New counters per worker: branch misses, the
kernel's generic cache-misses event, dTLB read misses.

- One thread on core 3: 23.6-23.9M cycles/deal, 64.4M instructions, 260k branch misses,
  55k cache misses, about 0 dTLB misses per deal (b0.set).
- Two threads on 3,15 (SMT): 32.8-34.2M cycles/deal per thread, 284-293k branch misses,
  66-73k cache misses. **Per core that is +38-45% throughput at equal clock**: SMT does pay
  for this branchy code. 2 processes on 3 and 15 cost the same cycles (32.9/34.1M) as 2
  threads (34.0/34.2M), so private table copies are not what limits SMT on one core either.
- So the earlier "+3% from 8 workers on 4 fast cores" is not a per-core SMT effect. Most
  likely causes, to check on the idle machine (batch_bench prints GHz and cycles/deal per
  run, which separates them): the package power/thermal limit lowering the all-core clock
  when 8 hardware threads run (this is a laptop; single-core 205/s vs 174/s per core with 4
  cores already shows the clock dropping), and/or the L3 (16 MB shared by the 4 cores) holding
  four private 1.1 GB table copies' hot lines instead of one shared copy.
- Working set per thread: the 256 MB pool is virtual; peak RSS grows by about 10 MB per
  worker in total (1119 MB with 1 worker, 1129 MB with 2). With huge pages dTLB misses are
  about 0; with all 4 KB pages (`LEAN_NOHUGE=1`) 3-5k per deal and +2-3% cycles (24.2-24.5M vs
  23.6-23.9M; SMT 33.7-33.9M vs 32.8-32.9M). The shared tables are already read into
  anonymous huge pages (alloc_huge) and shared by all threads of the batch process;
  per-thread table sizes (TT_BITS_N 9/11, DIR_MIN 10/13, PCHUNK 4) were flat. Nothing left
  to shrink that matters at this scale.
- Pinned vs unpinned workers (taskset to the same CPUs): pinned 413-446 vs unpinned 364-423
  deals/s on 3,8-11 under load; keep pinning.
- PGO+LTO (dropped, another agent owns it; one quick look): trained on public.set, measured
  on b0+b1: instructions 66.0M -> 61.0M (-7.5%), cycles/deal within noise (24.4-24.8M vs
  24.4-25.6M); PGO alone 61.1M instructions, LTO alone 65.2M. `PGO=gen|use LTO=1` in
  build_batch.sh.
- For the coordinator: `sh competition/dev/batch_bench.sh pcores` (1 core, 1 core + SMT, 4
  cores, 4 cores + SMT twice each, 4,600 deals per call, all answers checked; then deal
  order on 600 deals).

## Round 7: profile-guided and link-time optimisation (comp/b-pgo)

clang 18 instrumented PGO, trained on random deals made by the harness (seeds 900000+, never
the public or hidden sets), plus LTO through the gold plugin (no lld here). Only
competition/*.cpp gets the profile; the DDS library objects are unchanged (the lean solver
does not call them). Cycles and instructions per deal on the public set, CPU 1, harness
linked from libentry.a as the referee links bench; repeated interleaved runs, the machine was
loaded by other agents so only same-run comparisons count:

| build                                   | Mcycles/deal       | Minstr/deal |
|-----------------------------------------|--------------------|-------------|
| plain -O3                               | 22.0-22.4          | 60.54       |
| thin LTO alone                          | 22.1-22.5          | 60.09       |
| front-end PGO alone                     | 22.1-22.4          | 56.29       |
| PGO + full LTO                          | 22.1-22.2          | 55.99       |
| PGO + thin LTO                          | 21.3-21.7 (-3%)    | 54.84       |
| PGO + thin LTO + -fsplit-machine-functions | 21.2-21.5 (-3.5%) | 54.84     |
| IR PGO (-fprofile-generate) + thin LTO  | 21.5-21.6          | 55.78       |
| IR PGO + context-sensitive PGO + thin   | 21.55-21.6         | 55.26       |
| training on 13/11/9/7-card mix + thin   | 21.4-21.5          | 54.51       |

- PGO removes 9-10% of the instructions but only 3% of the cycles: the search is bound by
  memory (table probes) and branch misses that a static layout cannot fix.
- Referee, public set, CPU 1, alternating runs: PGO + thin LTO + split 184.8-187.0 deals/s
  against plain 180.7-183.1 (+2.3%). All correct; harness_pgo on 3000 fresh 13-card deals
  (seed 424242): all correct.
- No gain: LTO without PGO, full LTO (worse than thin with PGO), IR-level and
  context-sensitive PGO (CS profile needs -fprofile-use on the thin-LTO link to apply at
  all; then no better), a mix of ending sizes in training (same as full deals only), ext-TSP
  block placement (the -mllvm flag does not reach the gold LTO backend; not pursued).
- Build: `sh competition/dev/build_pgo.sh` (about a minute on CPUs 4-11) trains, writes
  competition/pgo/lean.profdata, builds competition/libentry.a with PGO + thin LTO + split and
  links competition/dev/harness_pgo. build_entry.sh takes PGO=<profdata> LTO=thin|full
  XFLAGS=..., all off by default, and with LTO writes competition/link_flags for the referee's
  link. Score with
  `PGO=$PWD/competition/pgo/lean.profdata LTO=thin XFLAGS=-fsplit-machine-functions sh <referee>/run.sh $PWD <set> 1 all`.
  The profile must be regenerated after any change to lean.cpp (stale functions just lose
  their profile; warnings are silenced).

## Round 8 (merge): b-bounds + b-nodes + b-guess + b-second + b-batch + b-pgo (branch comp/merged)

Merged in that order onto ddsfast (08e588c1); comp/b-lead left out (no gain). SHARE and OR_MIN
stay as switches, default 0. Conflicts were all adjacent additions: DUP_TT next to
PPICK/TTPICK before tt_lookup (kept both); the tail of lead (RUNOPT's add_runs beside
b-second's ROOTORD and per-side STATS); the STATS question log in solve (b-second's guarded
256-byte buffer, "q<k> <t> <result> <nodes>" for up to 4 root questions, and L on the GUESS
line, replacing b-nodes' unguarded 200-byte log); NOTES.md and .gitignore (kept both).
b-second's GB rounding offset landed only in the old linear guess (-DOLD_GUESS, GB=192); the
default MLP guess (b-guess) has no rounding offset. t.sh now pins to CPU 0; link_flags
(written by build_entry.sh with LTO) is ignored by git.

Verification (public set unless stated; all on the merged tree):
- Builds with no warnings at the default flags (-Wall -Wextra shows only unused
  variables/parameters in switched-off code).
- -DVERIFY_BOUNDS -DVERIFY_ORDER: 600 deals, no abort, all correct.
- -DSELFTEST=20 -DSELFTEST_MAX=10: 319,932 checks, no failure, all correct.
- Referee, CPU 0: plain 238.1 deals/s, PGO + thin LTO + split 245.7 deals/s, all correct.
- harness 13 3000 515151 and 13 3000 626262 (fresh random deals): all correct.
- batch_bench pcores (all 4,600 answers checked in every call): all correct.

Nodes (STATS, 600 deals): new guess 93.70M (156.2k per deal, 1,349 questions = 2.25 per
deal); old guess (-DOLD_GUESS, GB=192) 94.65M (1,449 questions). b-nodes alone: 95.3M; round
6 start: 104.5M.

Cycles and instructions per deal (t.sh, CPU 0, idle machine):
- plain -O3: 20.34-20.49M cycles, 50.04M instructions (round 7 start: 22.4-23.0M, 60.5M).
- PGO + thin LTO + -fsplit-machine-functions (dev/build_pgo.sh, profile regenerated on the
  merged code, 4,000 random deals): 19.96-19.99M cycles, 46.09M instructions (-2.2%, -7.9%).

Batch, 4 fast cores (`sh competition/dev/batch_bench.sh pcores`, CPUs 0-3 and 12-15). The
script allows PGO through build_batch.sh: `PGO=use PROF=competition/pgo/lean.profdata LTO=1
sh competition/dev/batch_bench.sh pcores -flto=thin -fsplit-machine-functions` (lean.cpp
profiled; batch.cpp and entry.cpp unprofiled). The laptop's all-core clock varies a lot
between runs (3.3-4.3 GHz at 4 threads), so cycles per deal is the comparable number:

| build, run | 1 thr | 0,12 | 4 thr (0-3) | 8 thr (0-3,12-15) | cyc/deal 1/4/8 thr | GHz at 4 thr |
|---|---|---|---|---|---|---|
| PGO+thin, run 1 | 241.6 | 356.0 | 806.2 / 796.6 | 1074.5 / 1078.7 | 20.96 / 20.9-21.1 / 28.6M | 4.2-4.3 |
| plain | 244.2 | 336.8 | 654.6 / 638.9 | 847.0 / 829.8 | 20.67 / 20.7-20.8 / 28.4-28.6M | 3.3-3.4 |
| PGO+thin, run 2 | 234.4 | 328.5 | 630.1 / 628.5 | 828.1 / 820.5 | 20.95 / 20.8-21.0 / 28.5-28.9M | 3.3 |

Instructions per deal on the 4,600-deal mix: plain 51.0M, PGO 48.4M; PGO cycles per deal
are no lower than plain in the batch (20.8-21.1M vs 20.7-20.8M). The pre-merge reference
(4 threads 700-710, 8 threads 923-934 deals/s) sits between the fast-clock and slow-clock
runs, so deals/s alone cannot rank the merge here; per-thread cycles per deal fell from about
23.6-23.9M (b-batch, one thread, b0.set, before the merge) to 20.7-21.0M. SMT on the fast
cores: +34-36% per core at 4 vs 8 threads, same as before.

## Round 9 (s2-cost): lower cost per node, same nodes (branch comp/s2-cost)

All numbers t.sh-style (public set, CPU 0); node counts checked with -DSTATS after every
change: 93,701,458 throughout. The machine was shared with two other agents, so cycles drift
by up to 10% between runs; only alternating A/B runs (`ab.sh`, or two source files) are
compared. ab.sh and m.sh now run on `$CPU` (default 0) instead of CPU 2.

Profile at the start (PCS sampler, non-PIE -g build, innermost inlined function): lead/follow
body 45%, tt_lookup + TT::find 21%, native_order 13.6%, tt_store 4.3%, bounds (NB, nq_*) ~5%,
native_second 3%. 240k branch misses and 46k cache misses per deal.

Kept:
- native_order, per-suit part in vector lanes (one 32-bit lane per suit, __m128i with
  AVX-512VL masks): lengths (vpopcntd), top three cards (vplzcntd + vpsrlvd), every per-suit
  condition as a mask, and from them for each suit and card class (top card, second card
  heading a sequence, other sequence head, other) the base weight, its coefficients on rr and
  rank, and the hint and killer bonuses. The division by 13 or 19 is a multiply and shift
  (exact for the range). The scalar version stays as native_order_scalar (-DOLD_ORDER).
  50.04M -> 46.07M instructions, A/B 20.41M -> 19.32M cycles (-5.3%).
- native_order, per-card part in one zmm (one 32-bit lane per card): the cards in the order
  suit up, rank down come from bit-reversing each suit (gf2p8affine + pshufb) and compressing
  the bit positions (vpcompressb); class from rr (==1, ==2) and the sequence bit, weights by
  vpermd from the 16-entry class tables, bonuses, clamp, key and the cards array (vpsllvq)
  written as vectors. 45.67M -> 43.38M instructions, A/B 18.97M -> 18.36M cycles (-3.2%).
  VERIFY_ORDER (port beside it, aborts on any difference) clean on the public set and on
  random positions (13x400, 13x300, 10x2000, 9x3000, 8x2000, 7x3000, 6x2000, 5x3000).
- Narrow table entries (up to 7 cards) store, instead of lo/hi, a mask of the targets they
  decide (bit 55 + t, t = 1..7) and the answer in bit 63: an entry from tt_store is either
  "reaches target" (decides t <= target) or "does not" (decides t >= target), so this is the
  same test. One vptestmq per block instead of two shifts, two ands, two compares.
  46.07M -> 45.67M instructions, A/B 19.38M -> 19.17M (-1.1%).

Tried, no gain (reverted):
- Prefetch the next lead's directory slot in follow<2> (assuming the last hand follows suit;
  current winner, or both possible winners): 20.33M -> 20.58-20.69M.
- Prefetch the next block of the chain while scanning a block: no difference within noise.
- The newest block of each key inside its directory slot (256-byte slots: key line + 3 lines),
  so a lookup's first block needs no dependent load: node counts unchanged, cycles equal
  (A/B 18.30M vs 18.32M), cache misses 47.6k -> 75.5k per deal (the latency saved is eaten by
  the bigger footprint). With the directory filled to 3/4 instead of 1/2: worse (18.51M).
- pick_follow without branches (lowest card of a preferred set or a fallback set): branch
  misses 203k -> 195k per deal, but instructions 43.38M -> 44.28M and cycles +1.4%.
- A summary of lo/hi per directory slot to skip chains that cannot decide: measured with
  STATS first, only 2-9% of the misses would be skipped; not built.

Table statistics (new STATS line per n): hits scan 1.2 blocks at n=4, 2.2 at n=6, 2.7 at n=7;
misses 3.3 blocks at n=6.

Component costs now (double-call builds, warm second call): native_order +4.3M instructions
per deal (was +12M), bounds +6.0M, table lookup +6.5M instructions and +1.9M cycles; the
sampled table time (about 23% of cycles) is about half latency.

End of round (alternating runs, same session):
- plain -O3: 20.35-20.60M -> 18.37-18.67M cycles per deal (-9.8%), 50.04M -> 43.38M
  instructions (-13.3%).
- PGO + thin LTO + split (build_pgo.sh, profile retrained on this code, CPUs 4-5):
  18.48-18.70M cycles, 40.09M instructions; PGO no longer beats the plain build in cycles
  (round 8: 19.96-19.99M PGO vs 20.34-20.49M plain).
- Referee (plain, CLEAN=1, CPU 0): all correct, 187.3 deals/s (machine loaded, clock low;
  not comparable with round 8's 238 deals/s). harness 13 3000 818181 and 13 3000 929292: all
  correct. Batch path not rerun (needs CPUs used by other agents).

Next: the table is now the biggest single cost (~23% of cycles, about half memory latency,
and neither prefetching nor moving the first block next to the key helped); the lead-node
bounds (nq_quick, ~380 instructions per call, 16k calls per deal) could be vectorized over
suits the same way as native_order; native_second (3%) likewise.
## Round 9 (s2-nodes): fewer nodes from follow choices; ETC measured and left off

Base: comp/merged (633e8af9). Public set, CPU 1 (t.sh, m.sh, ab.sh now pin to CPU 1): 93.70M
nodes (156.2k per deal), 50.04M instructions, 20.2-20.5M cycles per deal (plain -O3).

Kept (move ordering only; what is proven does not change):
- COV2=1: second hand plays low when its partner (playing last) holds a card in the suit
  above both the lead and the third hand's highest card. Before, second hand never looked at
  its partner and won the trick whenever it could beat the third hand's top. Nodes 93.70M ->
  90.82M (-3.1%); A/B 20.18 -> 19.66M cycles (min of 5). Variants: also low when partner can
  ruff (COV2=2) 91.46M; only when partner's card is above all of second hand's (3) 92.84M;
  only when the third hand follows (4) 91.21M; cover an honour lead from a sequence even when
  partner beats (COVRH) 89.57-89.77M against 89.42M. Off.
- COVB=1: when second hand can beat the lead but not the third hand's top, it covers (lowest
  winner) if the lead is the jack or higher (was: low). Nodes 90.82M -> 90.03M; ten or higher
  90.91M, queen 90.22M, by count of higher cards (COVB=4) 91.5M. The reverse (low instead of
  a cover above the third hand's top when the lead is small, COVB=2) is +30% nodes.
- DISC=3: a hand that cannot follow and does not ruff discards the lowest card of its longest
  side suit (from LENS; was the lowest-ranked side card in any suit). Nodes 90.82M -> 89.91M
  alone. Keeping suits where the hand holds the top card (DISC=1) 93.63M, stoppers (2) 91.25M,
  longest suit then lowest rank (5) 90.02M, longest suit without stoppers (4) 90.96M. The
  same rule for VOIDR's "discard rather than ruff into an overruff" (DISCV=1) 89.73M; off.
  The first version (loop over suits with popcounts) cost what it saved; the LENS version is
  4 branch-free steps.
- Together: 93.70M -> 89.42M nodes on the public set (-4.6%, 149.0k per deal); fresh random
  deals (`harness 13 1500 8181`, STATS) 239.7M -> 220.3M (-8.1%). Instructions 50.04M ->
  48.61M; A/B (ab.sh, 7 alternating runs, old = `-DCOV2=0 -DCOVB=0 -DDISC=0`) 20.30M ->
  19.32M min, 20.33 -> 19.36M median (-4.8%).

Measured, not kept (all left in the tree as switches, default off):
- ETC at lead nodes (ETCL=n): for each lead after which hands 2 and 4 each have a single
  candidate (one run), and every candidate of partner, look up the position after the trick;
  a stored win for the leader proves the lead (R = add_runs through the forced follow nodes,
  as the search would return). Hit rate about 5% of probes; nodes -2.4% at ETCL=5
  (91.47M), cycles +8%; ETCL=7 +3.5%. The saved subtrees average ~7 nodes per hit.
- ETC at hand-4 nodes: probe every card's position after the trick before searching (first
  version): nodes 85.5M (-8.8%) but cycles +14% (n>=5), +5.5% (n>=7), +0.5% (n>=9). Second
  version (ETC1/ETCF): hand 4 looks up its first choice's position itself (the child lead
  then skips its lookup, taking the hint from the probe; dropping the hint cost +7% nodes),
  and only when that misses probes the others. ETC1=5 alone: instructions -1.2%, cycles
  +0.7%. ETC1=ETCF=8 on the new ordering: 86.20M nodes (-3.6%) but cycles 19.32 -> 19.47M.
  Prefetching the siblings' directory slots first (ETCPF) did not help. Probing only cards
  that change which side wins the trick (ETCW) keeps almost none of the node saving: the
  hits are mostly siblings with the same trick winner. A table probe costs about as much as
  the nodes it saves.
- Third hand low (instead of winning) when the last hand is void and can ruff (THR=1):
  +4.5% nodes. Second hand void: discard when partner beats the lead and the third hand
  (VOIDP): -0.1% nodes; off.

Exactness: referee on the public set (plain build) all correct after each kept change;
`harness 13 3000` seeds 9393, 4747, 5858 all correct; SELFTEST (-DSELFTEST=10
-DSELFTEST_MAX=11) on the defaults 609,675 checks and with ETCL=ETC1=ETCF=6 555,456 checks,
no failure.

Where follow waste remains (STATS, public set, cut-late cases / nodes under them, nested):
second hand low first when a cover was right 54.7k / 7.4M (of which COV2's low at 9+ cards
4.8k / 5.0M); void-hand discards choosing the wrong suit 55k (hand 2), 55k (hand 3), 82k
(hand 4), about 2M nodes each; fourth hand winning with its lowest winner when low was right
7.8k / 1.05M.
## Round 9 (s2-root): wasted leads at the root and at 12 cards

Base: comp/merged (633e8af9). Public set, STATS: 93.70M nodes (156.2k per deal), 1,349
questions. Root leader's side, cut nodes: root 674, first lead cuts 87.0%, waste 7.41M; 12 cards
1,550, 88.1%, waste 3.09M (10.5M together, 11% of nodes). t.sh now pins to CPU 2.

New measurement switches (all off by default, STATS builds only):
- `-DLEADLOG`: one line per lead node of the root leader's side at 12-13 cards with the nodes of
  every lead tried. `-DLLALL=n`: also searches the leads after the cutter (pollutes the table).
- `-DSNAP=n`: at each such node, saves the per-deal table (directory, pool, chains, killers),
  searches every lead from that same state, restores, then searches as usual (checked: the
  first lead's cost equals the cost the normal search then pays). Logs per lead: DDS rank, nodes,
  cut?, DDS weight, 2nd/3rd/4th hand option counts, card, hint, killer.
  `-DFSNAP=n -DFSNAPK=k`: the same at follow nodes (hand k).

Findings (independent per-lead costs from SNAP, summed over cut nodes):
- Proof size, not finding a cutter, is the waste. In true questions 6.6 of 10.05 root leads cut
  (12 cards: 6.1 of 9.2). Cost of DDS's order 45.0M at the root, 41.3M at 12; the cheapest
  cutting lead per node (oracle) 21.1M and 22.0M. Most of the excess (14.0M of 24.2M at the root)
  is at nodes where DDS's first lead does cut, but another cutting lead has a proof half as big.
  The excess is concentrated: 10% of the nodes hold 71-78% of it. The cheapest cutter's DDS rank
  is spread over all ranks (root: rank 0 170 times, 1 92, 2 84, 3 71, ... 9 19).
- Budgeted search is ruled out. Failing leads cost about as much as cutters each, and there are
  several of them, so any round-robin spends close to (number of leads) x (cheapest cutter).
  Simulated on the independent costs, root cut nodes (DDS 45.0M): round-robin with doubling
  budgets 113-128M even if aborted work were free, 187-270M if it is all redone; round-robin
  among only DDS's top 2 leads 57.6M (free) / 109M (redone); a first lead given 16x budget
  47.3M at best. 12 cards the same (41.3M -> 52.8-268M). One probe round with a small budget
  (100-3000 nodes per lead) before DDS's order: under 1% even before charging the probes.
  Second hand at trick 1 (FSNAP 13, opponents): DDS 88.8M, oracle 68.4M, round-robin 97-159M.
- Predictors of the cheap cutter tested on the SNAP data (no gain): option counts of the
  opponents at trick 1 (any weight: 44.6-58.9M vs 45.0M); higher card first within the suit
  (the cheapest cutter, when in the same suit as DDS's cutter, is higher in 135 of 144 cases,
  but promoting higher cards loses: 46.1-50.2M); refutation costs in the preceding false
  question (ascending 25.7M, descending 18.2M vs 16.3M on 288 pairs; demoting leads refuted for
  under 20% of the costliest refutation 15.4M simulated, but in the solver nodes +0.2 to +1.0%
  for thresholds 5-30%, `-DROOTORD=2 -DROOTTH=`).
- Table-only probes (enhanced transposition cutoffs, `-DETC=n`, `-DETCRE=n`): each lead's trick
  enumerated (all opponent cards, every partner card), the positions after the trick looked up
  in the per-deal table, leads proven to cut moved first and proven failures last (ordering
  only). At node entry at 12-13 cards: 93.70M either way (the table holds no lower bounds for
  the target at that point); at 11+ cards, both sides: 92.92M (-0.8%) for 9M lookups. Probing
  again after every failed lead: 93.0-93.7M. Not worth the lookups.
- Root lead order for questions expected to fail (after a true answer), `-DROOTALL`: reversed
  DDS order at the root +5.0% nodes (root and 12 cards +7.4%; every question +17.9%), suit-grouped order +0.05 to +0.8%. DDS's order
  is also the best order for refutations: the strongest leads' refutations refute the rest.
- Kept: per-deal lead memory at 12 cards for the root leader's side (MEMN=12, default on,
  `-DNOMEM` off). For each card, the result and cycles of its last search as a lead there; cards
  that cut before go first, cheapest proof first; otherwise DDS's order. Simulated on SNAP data
  -5.6% of the 12-card cut cost; in the solver: 12-card first-lead cut rate 88.1% -> 90.0%, waste
  3.09M -> 2.90M, nodes 93.70M -> 93.44M (-0.28%); fresh deals (harness 13 3000 777) 470.80M ->
  469.84M (-0.20%). Cycles (ab.sh, CPU 2): 20.15/20.24M -> 19.88/20.05M (min/median, 7 runs) and
  20.29/20.56M -> 20.09/20.27M (9 runs, reversed order): about -1%. Same memory at 11 cards
  93.57M, at 10 94.67M, both sides at 11+ 98.59M, demoting cards that failed (MEMMODE=1) 93.88M,
  root included 93.44M, reset per question 93.43M. The state lives in SState (per batch context).
- Exactness: referee public set, CPU 2: 233.5 deals/s, all correct; harness 13 3000 919191,
  12 2000 4141, 9 2000 4242: all correct. Ordering only; nothing stored changes.

What more could buy: the oracle says root and 12-card proofs could be about half as big (about
20M of 93.7M nodes) if the cheap cutter were known; nothing cheap has predicted it. Untested:
probes that estimate proof size rather than result (e.g. counting the nodes of a depth-limited
search with the quick-trick bounds at the horizon), and a cutter choice learned on SNAP
labels (cheapest cutter, not DDS's cutter, as the target; earlier learned scorers were trained
on DDS's own choices and outcomes).

## Round 10 (merge 2): s2-cost + s2-nodes + s2-root (branch comp/merged2)

Base comp/merged (633e8af9); merged in that order with --no-ff. lean.cpp merged without
conflict for s2-cost + s2-nodes; s2-root conflicted in two places, both pairs of independent
#if blocks (s2-nodes' ETC declarations / probe_after beside s2-root's Snap struct; s2-nodes'
hand-4 ETC block followed by s2-root's FSNAP block), kept both. Lead memory (MEMN=12) rewrites
the keys that the vectorized native_order produces, before the sort, so it applies on top of
it unchanged. ab.sh, m.sh and t.sh run on `$CPU` (default 0).

Defaults now: vectorized native_order (-DOLD_ORDER scalar), decide-mask narrow table entries,
COV2=1 COVB=1 DISC=3, MEMN=12 (-DNOMEM off); ETCL/ETC1/ETCF, ETC, SNAP, LEADLOG, ROOTORD off.

Checks (public set unless said):
- Every build clean (plain, VERIFY, STATS, SELFTEST, ETCL=ETC1=ETCF=6, OLD_ORDER+NOMEM+COV2=0
  COVB=0 DISC=0, STATS+SNAP+FSNAP+LEADLOG+ETC=11+ROOTALL+ROOTORD=2, STATS+NOMEM).
- -DVERIFY_ORDER -DVERIFY_BOUNDS: no aborts, all correct.
- -DSTATS: 89,230,405 nodes (148.7k per deal); with -DNOMEM 89,419,459 (= s2-nodes' 89.42M),
  so the memory saves 0.21% on top of the new follow rules.
- SELFTEST=10 SELFTEST_MAX=11: 608,205 checks, no failure. ETC switches at 6 and the old
  configuration: all correct.
- Referee (plain, CLEAN=1, CPU 0): all correct, 283.2 deals/s.
- harness 13 3000 515151 and 13 3000 626262: all correct (ratio to DDS 4.24 and 4.32).

Cost, public set, CPU 0, 7 alternating runs (comp/merged's lean.cpp built the same way):
- plain -O3: comp/merged 20.30M min / 20.36M median cycles, 50.04M instructions per deal;
  comp/merged2 17.40M / 17.43M cycles (-14.4%), 42.97M instructions (-14.1%).
- PGO + thin LTO + split (build_pgo.sh, profile regenerated on this code): 17.53M / 17.61M
  cycles, 39.21M instructions, against plain 17.40M / 17.41M in the same alternation. PGO
  no longer helps (about +1% cycles despite -8.7% instructions); the plain build is the entry.

Batch (batch_bench.sh pcores, plain; load average 3.5-4 at start from the preceding runs, the
clock differs a lot between the two runs):
- run 1: 4 threads (0-3) 753.3 / 747.1 deals/s at 3.30 / 3.25 GHz; 8 threads (0-3,12-15)
  991.5 / 990.4 deals/s at 2.97 / 2.99 GHz; 1 thread 280.0 deals/s at 4.88 GHz.
- run 2: 4 threads 928.9 / 873.9 deals/s at 4.20 / 3.82 GHz; 8 threads 1261.2 / 1244.7 deals/s
  at 3.75 / 3.69 GHz; 1 thread 271.3 at 4.71 GHz.
- Cycles per deal are steady (17.4-18.0M at 4 threads, 23.6-24.0M at 8 with SMT), so the
  difference between runs is the clock alone.

## Round 11 (s3-cost): cheaper lead-node bounds (branch comp/s3-cost)

Base comp/merged2 (d173b4bd): 42.97M instructions per deal, 89,230,405 nodes (public set,
plain build). CPU 0 measurements were taken while CPU 12 (likely CPU 0's SMT sibling) ran
another job at 100%, so absolute cycles were 17.5-19.5M on the same code; only alternating
A/B runs are compared.

Cost of the bounds, measured by calling each part twice (`-DDUP_QT`, `-DDUP_LT`, `-DDUP_BOUNDS`):
dds_bounds as a whole 6.04M instructions per deal (13.5k calls per deal, after the table
misses), nq_quick 3.58M of that (4.08M calls over the set, 6.8k per deal, about 526
instructions each; 27% of them decide), nq_ltmin/ltmax 0.56M, the rest the top-count filter
and NB::init.

Kept (scalar code under `-DOLD_QT`):
- NB::init computes the top three cards of every suit and their hands at once (one 64-bit
  lane per suit: vplzcntq + vpsrlvq, owners from vptestmq masks, vpmovqd into int[4]); the
  lengths are a byte array (unpacked LENS). sb/tb/owner_of in nq_quick, nq_ltmin, nq_ltmax
  become loads. 43.01M -> 41.53M instructions.
- The top-count filter (u) for all suits in one ymm (cards above the other side's highest
  card: andnot of srlv(ones, lzcnt(other))), one popcount of the union. -0.05M instructions;
  checked against the scalar loop (`-DCHK_U`, aborts on difference): equal on the public set.
- The communication suit (commPartner) from 4-bit suit masks (leader holds a card / two,
  partner holds the top / second card, opponents follow or are out of trumps): the first
  suit of the candidate mask instead of the loop (`-DOLD_COMM` for the loop alone).
  +0.27M instructions but A/B 19.00/19.60M (loop) vs 18.96/19.23M (masks), min/median of 7;
  kept, small. Computing these masks in NB::init instead (for every bounds call, not only
  nq_quick's) cost +0.43M instructions.
- Together: 41.75M instructions (-2.9% against the base's 42.97M); A/B against `-DOLD_QT`
  (= the base code) 18.84/18.99M -> 18.04/18.25M cycles (min/median of 7, -4.2%/-3.9%).

Tried, not kept:
- QUB: an exact upper bound on nq_quick's count (per suit: 0 unless the side holds the top or
  the opponents are void, at most the longer hand's length, at most 2 when an opponent holds
  3+ cards, +1 for the trump ruff) to skip calls. Skipped only 4.7% of the calls (4.08M ->
  3.89M); +1.43M instructions per deal net. The failing calls fail on entries and blockages,
  which a cheap bound does not see.
- The third card (tb) on demand instead of in NB::init: +0.03M instructions.
- native_second and the table were not changed (native_second's NT loop is small and its
  trump path returns at once; first-block-in-slot cannot keep node counts unless it scans
  the whole first block anyway, because the lookup takes the matching entry with the fewest
  cards).

Node counts: with -DNOMEM, 89,419,459 before and after (exact). With the lead memory on
(default), 89,230,442 instead of 89,230,405 (+37, 0.00004%): MEMN orders 12-card leads by
the log2 of the cycles (rdtsc) their last search took, so a change in code speed can flip a
bucket. -DOLD_QT on the new tree gives 89,230,405. The answers do not depend on it.

Exactness: -DVERIFY_BOUNDS -DVERIFY_ORDER on the public set and on harness 10 2000 31,
8 3000 32, 6 5000 33: no aborts, all correct (an intermediate version that left out the
trump-suit fallback of the communication test aborted at once, so the check covers it).
Referee (plain, CLEAN=1, CPU 0, loaded machine): all correct, 220.0 deals/s. harness 13 3000
717171 and 13 3000 838383: all correct.

## Round 11 (s3-nodes): second hand covers when its partner cannot beat the lead

Base comp/merged2 (d173b4bd). STATS: public set 89,230,405 nodes (148.7k per deal); fresh deals
(`harness 13 1500 8181`) 219,914,326. ab.sh on CPU 1 (7 alternating runs): 18.10M min / 18.27M
median cycles per deal (this round's machine state; the round-10 figure was 17.40M on CPU 0).

Method. The "waste" STATS (nodes under a failed first card) turned out to be a poor guide:
logging every follow cut node at 9+ cards (`-DFLOG=9`, one line per node: hands, trick, first
card, cutter, waste) shows the current first card is wrong in only 1.7% of COV2's "partner
wins" lows, 0.25% of fourth hand's cheap wins and 1-2% of discards, and no feature tested
(partner length, cards above partner's top in the leader's hand, third hand void/ruffing,
lead an honour, n, ...) found a group with a wrong rate anywhere near 50%. The waste sits in
a few huge subtrees at 12-13 cards. What does matter is the size of the proof when the first
card cuts: FSNAP (per-card costs from the same table state, now with the hands on the FS
line) at 11 cards, second hand, showed the current order at 75.6M against an oracle of 61.0M,
and in the "can beat the lead but not the third hand's top" case (why 5), covering was
cheaper than low when the partner was short.

Kept (ordering only; nothing proven or stored changes):
- COVPA=13: second hand that can beat the lead but not the third hand's top covers with its
  lowest winner when its partner (playing last) holds no card above the lead. Was: low (or
  cover if the lead was the jack or higher, COVB's honour rule). Nodes public 89.23M ->
  85.86M (-3.8%), fresh 219.91M -> 207.49M (-5.6%).
- COVRA=99: drops COVB's honour cover, which now costs nodes: 85.86M -> 84.84M public, 207.49M
  -> 206.38M fresh (COVRA=10 or 11 give the same counts; only the jack case mattered).
- Together: public 89.23M -> 84.84M (-4.9%, 141.4k per deal), fresh 219.91M -> 206.38M
  (-6.2%). A/B (ab.sh, CPU 1, 7 alternating runs) 18.10M / 18.27M -> 17.25M / 17.48M
  cycles (min / median, -4.7% / -4.3%).

Measured, not kept (switches left in the tree, default off; nodes public / fresh 8181):
- Cover when partner holds at most COVP cards in the suit: COVP=0 88.50M / 214.95M, 1
  87.74M / 213.36M, 2 87.85M / 213.21M, 3 88.76M / 215.73M; on top of COVPA: COVP=1 86.44M /
  209.66M (worse), COVP=0 identical. Partner holds nothing above second hand's lowest winner
  (COVPB=1, a superset of COVPA): 87.54M / 211.96M.
- Third hand low instead of its lowest winner when it cannot beat the fourth hand's top and
  the fourth hand has a card between the lead and that winner (K2L=1, with COVPA): 91.59M /
  222.55M.
- Discard scores per hand (DSKk, k = hand 2..4 as K=1..3; base 89.23M / 219.91M): K=1
  partner-length tie-breaks or (mine - partner's) 89.4-90.2M, non-stoppers first 91.4M,
  partner at least as long first 97.6M; K=2 (third hand) ties to non-stoppers (DSK2=6)
  89.19M / 219.71M (-0.04% / -0.1%, cycles within noise: 18.29/18.82M vs 18.45/18.69M),
  non-stoppers first 88.65M but 221.27M fresh (public-only gain); K=3 all worse on the
  public set (89.34-97.83M).

Exactness (final code, defaults): referee on the public set (CLEAN=1 plain build, CPU 1) all
correct; `harness 13 3000` seeds 31337, 271828, 9001 all correct (ratio to DDS 4.18, 3.79,
4.11); SELFTEST (-DSELFTEST=10 -DSELFTEST_MAX=11, public set) 582,141 checks, no failure,
all correct. Every node count above also compared all answers with DDS (public and fresh).

Next: FSNAP proof-size data (per-card costs from the same table state) is the measurement
that found this; the same analysis for third and fourth hand and for COV2's "partner wins"
low at 12-13 cards has not been done.

## Round 12 (final merge): s3-cost + s3-nodes, deterministic lead memory (branch comp/merged3)

Base comp/merged2 (d173b4bd). Merged comp/s3-cost (vector-lane lead bounds, NB::init; old
code under -DOLD_QT / -DOLD_COMM) and comp/s3-nodes (COVPA=13, COVRA=99). lean.cpp merged
without conflicts (bounds vs pick_follow); only NOTES.md conflicted (both Round 11 sections kept).

Lead memory (MEMN=12) made deterministic. It ordered 12-card leads of the root leader's side
that cut earlier by log2 of the rdtsc cycles of their last search, so node counts depended on
timing: the unchanged merged2 code gives 89,230,405 or 89,230,442 nodes on the public set
from build to build. New switch MEMCOST: 1 (default) = lead nodes searched under the card
(a thread_local counter bumped once per lead() call; plain builds count it too), 2 = lead +
follow nodes, 0 = the old rdtsc cycles. Same log2 buckets.
- Nodes (STATS, public): MEMCOST=1 84,841,515, MEMCOST=2 84,841,515, MEMCOST=0 84,841,523.
  Fresh (harness 13 1500 8181): 206,380,195 for both 1 and 0.
- Cycles (ab.sh, CPU 0, 7 alternating): MEMCOST=0 17.41M min / 17.54M median vs MEMCOST=1
  17.32M / 17.55M; MEMCOST=2 17.60M / 17.77M vs MEMCOST=1 17.67M / 17.79M. Equal within
  noise; MEMCOST=1 kept.

Checks (final code; clean build, CLEAN=1, link_flags removed):
- -DSTATS public set: 84,841,515 nodes, twice identical; -DOLD_QT 84,841,515 (identical).
  (-DNOMEM: 85,009,499.)
- -DVERIFY_BOUNDS -DVERIFY_ORDER: public set, harness 10 2000 41, 8 3000 42, 6 5000 43: no
  aborts, all correct.
- -DSELFTEST=10 -DSELFTEST_MAX=11 public set: 582,141 checks, all correct.
- Referee (public set, CPU 0, plain): all correct, 277.8 deals/s (builds were running on
  other CPUs at the time).
- harness 13 3000 515151 and 929292: all correct (ratio to DDS 3.95, 3.85).

Measurement (public set, CPU 0, 7 alternating runs; CPUs 0 and 12 idle, three unrelated
harness_fs* jobs on CPUs 8-10): comp/merged2 lean.cpp built from a scratch copy against the final code.
- comp/merged2: 18.67M min / 18.76M median cycles, 42.97M instructions, 89.23M nodes
  (148.7k per deal).
- comp/merged3: 17.40M / 17.76M cycles (-6.8% / -5.3%), 40.05M instructions (-6.8%),
  84.84M nodes (141.4k per deal, -4.9%).

## Round 13: one deal on several cores (branch parallel-deal)

Goal: lower latency for a single deal on the 4 fast cores (0-3) and their SMT siblings
(12-15), exact, without slowing the batch path. New call `solve_deal_parallel(problem, cpus,
ncpu, &lead_nodes)`; the calling thread works as participant 0 (pin it to cpus[0]), helpers are
pinned to cpus[1..). Measured with `bench_solve deal FILE reps CFG...` (configurations alternate
pass by pass, best of reps per deal; the costliest 10% / 1% by single-thread time; "nodes" are
lead nodes summed over threads against solve(), counted by the lead-memory counter that plain
builds keep anyway; Mcycles = the calling thread's cycles per deal, i.e. latency in cycles).

Design (final):
- `src/par_deal.cpp` includes `solver.cpp` with `DDF_PAR`: a second copy of the search with its
  own thread-local state and three hooks, all under `#ifdef DDF_PAR`: an abort flag checked at
  every lead node and before every table store; a mask on hand 2's replies at trick 1; and a
  split point after hand 2's first reply at trick 1. The default object `solver.o` compiles to
  the same machine code as before (objdump diff: only the new `lead_nodes()` accessor);
  STATS node count 84,841,515, twice.
- The calling thread runs solve_one's question sequence (same guess, same moves of t). Each
  question: the root leads in DDS's order (hint = last cutting root lead) become work items;
  an item searches its lead against hand 2's first reply only; if that fails to refute, the
  other replies become one item each, queued ahead of the leads not yet started (young
  brothers wait, at trick 1). A lead cuts when all its items cut (question decided: abort
  everything); one refuted item refutes the lead (abort its other items); the question is
  false when every lead is refuted. Items come from a small locked list, lowest priority first.
- Aborts: the flag only ever goes from clear to set within a question, and a node stores only
  if it is still clear when the node finishes, so every stored entry comes from a complete
  search. Aborted items' finished subtrees stay in the tables and serve later items.
- Tables: each thread keeps its own per-deal table across the questions of the deal; results
  from 6 cards up also go into a shared table of 16 stripes (independent TT instances, a
  key's stripe by a second hash). Writers lock the stripe and make its sequence number odd;
  readers take no lock and treat a changed or odd sequence as a miss (chains always point to
  lower block numbers, so a torn read terminates; its answer is dropped).
- Helpers spin 3 ms for the next question, then sleep on a condition variable.

Measurements, public set, idle machine (before another agent started on CPUs 8-11), best of
2-3 per deal, single thread 3.18-3.19 ms (all), 14.2-14.3 ms (top 10%), 28.9-29.0 ms (top 1%):

| design | threads | all | top 10% | top 1% | nodes |
|---|---|---|---|---|---|
| first lead split by hand 2, private tables | 2 / 4 / 8 | x1.09 / x1.27 / x1.14 | x1.13 / x1.32 / x1.20 | x1.17 / x1.31 / x1.18 | 1.67 / 2.36 / 3.00 |
| root leads only, private tables | 4 | x1.30 | x1.34 | x1.31 | 2.36 |
| + one shared table, single lock, from 8 cards | 4 / 8 | x1.33 / x1.26 | x1.42 / x1.35 | x1.33 / x1.24 | 1.94 / 2.36 |
| same from 6 cards (lock contention) | 4 / 8 | x0.96 / x0.66 | | | 1.64 / 1.78 |
| root only, 16 stripes, from 8 / 6 / 4 cards | 4 | x1.49 / x1.48 / x1.37 | x1.52 / x1.54 / x1.45 | x1.44 / x1.45 / x1.38 | 1.84 / 1.60 / 1.53 |
| final (YBW at trick 1, 16 stripes, from 6) | 4 | x1.45-1.57 | x1.51-1.66 | x1.40-1.69 | 1.60 |
| final | 8 | x1.44 | x1.49 | x1.35 | 1.90 |

In cycles of the calling thread (clock-independent) the final design is x1.65 at 4 threads;
the 4-core load costs about 7% clock (caller 5.06 -> 4.71 GHz), the SMT siblings more (4.2-4.6).
Final design measured again later with the machine loaded by other work (load average 10-24,
clock 3.0-3.7 GHz; single thread 4.54 / 20.2 / 41.3 ms):

| threads | all | top 10% | top 1% | nodes | Mcycles/deal (x) |
|---|---|---|---|---|---|
| 1 (solve) | 4.536 ms | 20.21 ms | 41.26 ms | 1.000 | 16.98 |
| 2 (0-1) | 4.381 ms x1.04 | 19.16 ms x1.05 | 40.21 ms x1.03 | 1.353 | 14.79 (x1.15) |
| 4 (0-3) | 3.600 ms x1.26 | 15.53 ms x1.30 | 33.61 ms x1.23 | 1.612 | 10.86 (x1.56) |
| 8 (0-3,12-15) | 3.673 ms x1.23 | 15.83 ms x1.28 | 36.00 ms x1.15 | 1.899 | 11.01 (x1.54) |

Where the time goes (final, 4 threads): threads are in work items 75-84% of the question time
(the rest waits for the last item); 15-17% of thread time is in items later aborted. Node
overhead 1.6x. Per lead node, private tables alone cost 1.04x the single-thread time per node;
with the shared table from 6 cards 1.33x (locking and cache-line transfers between cores), so
the shared table trades fewer nodes for dearer ones: from 8 cards and from 6 cards come out
equal in time.

Tried, no gain: splitting every root lead by hand 2's replies up front (x1.26, nodes 2.30: the
replies transpose into each other, and searched at once none finds the others' entries);
YBW against root-only splitting (equal within noise; YBW kept for the costliest deals, where a
cutting first lead otherwise runs on one thread); seqlock readers instead of locked readers
(no measurable change; kept, as readers no longer write shared lines); storing into the shared
table only results whose subtree had 8-32+ lead nodes (`PAR_SHCOST`; more nodes, no faster).
Experiment switches (environment, par build only): `PAR_SPLIT` (0 root leads, 1 first lead by
replies, 2 every lead by replies, 3 YBW, default), `PAR_SHMIN` (shared table from n cards; 99
off), `PAR_SHCOST`, `PAR_STATS` (prints item and waste counters at exit).

Batch path: unchanged. bench_solve set on CPU 0, branch vs main built from main's sources,
alternating: 39,390,560 instructions per deal in both; cycles 17.13/17.06/17.31M vs
17.14/17.07/16.97M. bench_solve parallel 0-3: 658.7/648.2 vs 657.3/654.7 deals/s.

Exactness: test_ddfast now also checks solve_deal_parallel on every set. All exact: quick
(public set, 100 random, 10 tables); the public set 10 times (5 x 4 threads, 5 x 8 threads);
random 3000 seed 20261115 (4 threads) and 2000 seed 777001 (8 threads) against DDS; the 100,000
reference deals (ddfast-refs chunk0-7, 4 threads).

What more could buy: (1) the 1.6x node overhead is mostly lost transpositions between items
running at once; a cheaper shared table (lock-free entry publication, so it can go down to 4
cards) or keeping each root lead on the thread that searched it in the previous question
(its private table holds the small entries) should cut it. (2) 16-25% idle time behind the
last item: split deeper (hand 3/4 replies at trick 1, or trick 2's leads) when threads are
idle, or abort and re-split a long item (its finished subtrees stay in the tables). (3) SMT
siblings do not pay with this design; a table variant (solve_table, 20 problems) would
parallelise far better by problems than inside one search.

## Round 14 (follow-rules): second hand plays above third hand when its long partner has one winner

Base main (71f41c6). STATS: public set 84,841,515 nodes; fresh deals (`bench_solve random 2000
SEED`, STATS build) 288,468,161 (seed 1001), 287,588,571 (2002), 265,608,447 (3003). Also fixed
the STATS build, which no longer compiled (a `for` inside `STAT(...)` in tt_lookup).

Method (as round 11). FSNAP per-card proof sizes on the public set: second hand at 12 cards
(33,407 nodes), third and fourth hand at 11 cards (about 104k nodes each; runs stopped early).
Oracle (cheapest cutting card) against the current first card, cut nodes only:
- second hand, COV2 "partner wins, play low" at 12 cards: 31.4M vs 22.2M; the other second-hand
  cases 71.3M vs 57.7M.
- third hand: 17.1M vs 14.0M (partial log); the biggest gap is "can beat the fourth hand's top"
  (lowest such card, 6.6M vs 5.1M), then "partner wins, fourth hand can beat it" and "can beat
  the lead but not the fourth hand".
- fourth hand: 29.8M vs 27.1M where it can win; spread thin, no group worth a rule.
A split-half search (every single feature and pair of features, gain required on both halves of
the log) ranked the candidates; the gains it found are small (0.3-1.5% of a group) and the
predicted ones did not always hold in the real search (K2W below).

Kept (ordering only):
- COVPL=3: in COV2's "partner wins" case, second hand plays its lowest card above the third
  hand's top (instead of its lowest card) when its partner (playing last) holds 3+ cards in the
  suit and a card above second hand's top. Nodes public 84.84M -> 82.92M (-2.27%); fresh 1001
  288.47M -> 284.93M (-1.23%), 2002 287.59M -> 281.98M (-1.95%), 3003 265.61M -> 262.21M
  (-1.28%); fresh total -1.49%. The gain is all in the case where the partner holds exactly one
  card above the bar (COVPLX=8 gives the same counts: 82.89M / 284.92M).
- Cycles (public set, CPU 11, 7 alternating runs old/new; CPUs 9-10 busy with checks, so
  noisy): old 16.17M min / 16.68M median, new 15.98M / 16.56M (-1.2% / -0.7%).

Measured, not kept (switches in the tree, default off; nodes public / fresh 1001 unless noted):
- COVPL=1 84.99M / 289.10M; 2 83.14M / 285.08M; 4 83.21M / 285.86M.
- COVPLX=1 (no partner-above-my-top test) 83.21M / 290.53M; =2 (also when third hand is void)
  82.59M / 285.01M but 283.08M on 2002 (vs 281.98M); =4 (partner holds 2+ above the bar)
  85.03M / 288.85M; COVPL=3 COVPLX=9 82.77M / 287.48M.
- COVPL=2 COVPLX=8: 82.58M / 284.43M, 2002 283.60M, 3003 262.15M: fresh total -1.36% against
  -1.49% for plain COVPL=3.
- Third hand that can beat the fourth hand's top, when second hand still holds 4+ cards: lowest
  winner (K2W=1) 86.19M / 293.28M (worse; predicted better by FSNAP); second-lowest card (K2W=2,
  guarded for a single candidate) 86.48M public (worse).

Exactness (final code, defaults): bin/test_ddfast all exact; STATS public 82,915,440 nodes all
correct; plain `bench_solve random 3000` seeds 31337, 271828, 9001 all correct against DDS;
every node count above also compared all answers with DDS; SELFTEST (-DSELFTEST=10
-DSELFTEST_MAX=11, public set) 568,425 checks, all correct.

Next: the third-hand gaps are real but no simple feature separated them; FSNAP at 12-13 cards
(slow) or a two-level oracle (which accounts for table reuse) may be needed.

## Round 15 (branch perf-arena): micro-optimization agents, table directory, profiler

Two rounds of parallel agents, each in its own worktree, building on CPUs 4-11 and measuring
on its own fast core (1, 2 or 3). Net result: about -3.5% cycles per deal on one core
(estimate: -1.3% from 15a, which may be partly layout, plus -2.2 to -2.6% from 15b, measured
against a control), node count unchanged at 82,915,440 on the public set, all exact. Also:
`tools/prof/` (a sampling profiler on hardware counters, since perf is not installed) and fixes
that make the VERIFY_ORDER and SELFTEST builds work again (branch verify).

### 15a: registers, branches, dead code (merged)

| Branch | Kept | Agent's figure | Notes |
|---|---|---|---|
| perf-arena-registers | tt_lookup always inlined into lead (it was a call with stack out-parameters); S->trk stores only when DFOL <= 13 (only the port's follow ordering reads them; follow 1 has the lead card as `best`) | -2.0% cycles, -3.8% instructions | Dropped: lead returning {R, result} by value (+3.1% instructions), force-inlined tt_store (-1.9% instructions, cycles in noise), play/unplay from saved values |
| perf-arena-branchless | the table's lead hint set by a mask select (`hb < 52` compiled to a mispredicting branch) | about -1%, branch misses -4.7% | Dropped: reading slot and first block before branching (TTFAST, +3%), loop-free add_runs (+2%), lone follow card played directly (more misses), fused occupied/key tests |
| perf-arena-prune | 821 lines of dead experiment switches removed from solver.cpp (ETC*, ROOT*, ORACLE, MEM*, SNAP, DFOL1-3, DQ2 and others); study switches used by bench/loo.sh, bench/dds_study.sh and docs/LOO.md kept | -4% (ranges overlap) | Dropped: [[unlikely]] / cold wrappers on rare paths (lead<false> grew 18.4 -> 22.4 KB, cycles neutral) |

Merged (clean A/B, 9 alternating pairs, CPU 0, nothing else running), base 6fc8392:
cycles/deal 16.82M -> 16.60M median (-1.3%, ranges overlap), instructions 38.42M -> 37.49M
(-2.4%), branch misses 170.5k -> 164.2k (-3.7%). Other modes (single runs unless noted):
solve_parallel on 8 threads 1,076 -> 1,108 deals/s (+3.0%, two runs each), full tables on 8
threads 136.0 -> 140.6 tables/s, one-deal latency 16.69M -> 16.26M cycles. DDS on the same
core: 72.4M cycles per deal, so 4.30x -> 4.36x. The agents' separate figures (about -7% in
sum) did not add up: they were measured while three agents loaded memory at once.

Profile (bprof, public set): about 1.2 branch misses per node; lead<false> holds 64% of
branch-miss samples (follow 1-3 inlined into it; one 18 KB function with about 740 stack
references, which Zen 5 handles cheaply, so spills barely matter); about 22% of misses in
tt_lookup.

### 15b: table directory (merged), nodes and joint agents (not merged)

**Kept (perf-arena-memory, cc1c817): the directory starts at 2^15 slots (DIR_MIN 11 -> 15).**
Most deals grew the directory twice (2^11 -> 2^13 -> 2^15), and each growth rehashes with
unpredictable branches. Control-build A/B (CPU 0, 7 rounds, three builds in turn; control =
same source with -DDIR_MIN=11):

| Set | Base | Control | DIR_MIN 15 | vs control |
|---|---|---|---|---|
| public | 15.80M (15.75-16.06) | 15.79M (15.71-15.95) | 15.38M (15.35-15.53) | -2.6% |
| chunk1, 600 deals | 16.56M (16.55-16.63) | 16.52M (16.50-16.69) | 16.16M (16.15-16.18) | -2.2% |

Branch misses 160.7k / 159.9k / 148.1k. Nodes identical in all three (82,915,440 public,
87,730,725 chunk1). The branchless compaction in grow (also in cc1c817) is worth nothing on
its own (control vs base). Also measured by the agent: DIR_MIN 13 and 16-17 worse than 15;
growing at 1/4 full no change; huge pages are already in use for the pool, directory and ddclaim
tables; alignas(64) on Hot/SState within noise.

**The next-block pointer inside the block is worse (+5.7% cycles, +1M instructions).** The
separate `next` array is small and stays in L1, so it loads in parallel with the block; inside
the block each hop waits on the block's line. The "two dependent cache misses per chain step"
assumption from 15a does not hold at this table size. Two-line blocks on a 128-byte boundary
were also worse (+1%).

Where the remaining cost is (bprof cycle, L1-miss and cache-miss modes): about 30% of sampled
cache misses are in ddclaim's 4-card pattern probe (probe_pattern, pmatch8; about 1,750 probes
per deal, 5-7% of cycle samples); only about 3% are in the transposition table, whose block
reads mostly hit L2 (about 12.7k entries per deal). About 9.5% of cycle samples land on the
first loads of H on entry to lead, which looks like restart cost after mispredicted branches in
the caller rather than memory. Chain walks (STATS): hits scan about 2 blocks at 6 cards and 3 at
7; misses about 3.2.

**Joint agent (perf-arena-joint, 60939d4, not merged):** the directory grows at 1/8 full
(DIRLOAD=3, DIR_MIN 12, DIR_MAX 22): -1.7% public / -1.3% chunk2 cycles, branch misses -7.5%,
nodes unchanged. Same mechanism as DIR_MIN 15 (a sparser directory, fewer mispredicted probe
loops), so the two overlap; DIR_MIN 15 was adopted. Its trade-off survey (one-pass bprof
screens unless noted), all dropped:

| Experiment | Nodes | Cycles |
|---|---|---|
| 4-card pattern table probed before dds_bounds at 4 cards | | about +1% |
| No store at 4-5 cards when the subtree had fewer than 2 lead nodes | | +1 to +2% |
| A hint stored at failed nodes (lead with the largest subtree) | | +1 to +4% |
| A hit found past the newest block copied into the newest block | -0.2% | +1 to +2% |
| Only the first 3 / 6 blocks of a chain scanned | +41% / +13.5% | |
| Stored masks widened by each suit's top card (to test splitting chains by top-card owner) | x2.9 | |
| TTPICK=0 (first matching entry) | +3.2% | equal |
| TTPICK=2 for 8+ cards only | -0.02% | equal |
| H[1], H[2] from a 16-byte load in lead; -fno-slp-vectorize | | no gain |

Conclusion: dropping or cutting off entries costs far more nodes than it saves in memory
traffic, and getting more out of the table (scanning further, copying hits forward) saves fewer
nodes than the extra scanning costs.

**Nodes agent (perf-arena-nodes): node-reduction sweep, nothing kept.**

Base perf-arena + verify. STATS nodes: public set 82,915,440; chunk1.set first 600 deals
87,730,725. Cycles: alternating runs on CPU 2 (`bench_solve set`), 7 pairs. All variants gave
all answers equal to DDS on both sets. The code of the switches below was not kept (the diff
is small; each is a one-line condition).

| Variant | Public nodes | chunk1[600] nodes | Cycles |
|---|---:|---:|---|
| HX=25 (table hint weight, default 50) | 83,249,566 (+0.40%) | 87,684,016 (-0.05%) | not timed |
| HX=100 | 83,031,658 (+0.14%) | 88,397,726 (+0.76%) | not timed |
| HX=200 | 83,181,350 (+0.32%) | 88,640,474 (+1.04%) | not timed |
| Lead memory also for the opponents at 12 cards | 84,540,990 (+1.96%) | 89,353,701 (+1.85%) | not timed |
| Follow, after the first card fails: hand 2 also highest (default only hand 3) | 84,253,740 (+1.61%) | 88,627,094 (+1.02%) | not timed |
| hands 3 and 4 highest | 83,236,710 (+0.39%) | 88,116,548 (+0.44%) | not timed |
| hands 2, 3, 4 highest | 84,555,457 (+1.98%) | 88,997,210 (+1.44%) | not timed |
| all hands lowest | 83,847,636 (+1.12%) | 88,161,784 (+0.49%) | not timed |
| TTPICK=2 (narrowest deciding entry over the whole chain) | 82,164,753 (-0.91%) | 86,943,889 (-0.90%) | |
| ... narrow entries only (n <= 7) | 82,183,156 (-0.88%) | 86,957,430 (-0.88%) | public +1.8% (16.02M -> 16.31M median) |
| ... wide entries only (n >= 8) | 82,902,275 (-0.02%) | 87,719,642 (-0.01%) | not timed |
| P4WIDE: a 4-card table hit is stored with the entry's whole bound (decides more targets) | 82,925,907 (+0.01%) | 87,721,199 (-0.01%) | same-layout control: public 15.876M -> 15.874M, chunk1 16.71M -> 16.68M (noise) |

Code layout: a build of the same search with harmless source changes (default-off switches,
one extra captured variable in tt_store) ran 1.3-1.4% faster than the base build on both sets
(public 16.26M -> 16.04M, chunk1 16.95M -> 16.71M median), with identical node counts. Differences
of this size between two builds can be layout alone; compare a candidate against a control
build that carries the same source edits with the feature switched off.

### 15c: lessons and open leads

- Measure against a control build. Equivalent builds differ by 1.3-1.4% in cycles from code
  layout alone, so a candidate is compared with a build of the same source with the feature
  switched off (or forced to the old value), alternating, on an idle core. Agents measuring at
  the same time inflate each other's noise to 2-3%.
- Branch-miss and instruction counts (bprof ... none) are nearly deterministic: screen with
  them, decide with cycles. Cutting instructions buys about half as much in cycles here.
- Micro-tuning (registers, branches, code size) is close to used up; so is the transposition
  table's layout.
- Open: the 4-card pattern probe's DRAM misses (a cheaper path for the 16% of probes that miss,
  or skipping them); a branchless or multi-slot directory probe (the remaining misses in
  TT::find are "key not in the table yet"); 8-byte directory slots (needs a new key/head
  split); smaller first blocks for keys with few entries (changes which entry is picked, so the
  search); proof-size predictors for root and 12-card leads (about 20% of nodes on paper, no new
  predictor yet).

## Round 16: batch throughput, aimed at servers (four agents, 2026-11-18)

Latency stopped mattering; the goal became deals (and tables) per second per physical core with
both SMT threads busy, on server-class machines (EPYC Zen 5 "Turin", Xeon Granite Rapids). Four
agents, one physical core each (CPU n and its SMT sibling, builds on the slow cores), every
change A/B against a control build of the same source. Outcome:

| Agent | Result |
|---|---|
| 16a table sizes (tp-tables-size) | no solver change: the per-deal table's footprint does not limit SMT throughput; DIR_MIN 15 does no harm. Tooling kept (worker counters in `bench_solve parallel`, `bench_solve tables`, raw PMU events in bprof). NUMA placement of the endgame tables is the open lead for two-socket servers (issue #12). |
| 16b interleaving (tp-interleave) | 3-10% slower in every configuration: two deals' tables per thread overflow L2, and SMT already overlaps the stalls. Kept off by default behind `DDFAST_NCTX` so servers can re-check. |
| 16c questions (tp-questions) | single problems: no headroom (a perfect first guess saves at most 4.7-6.3% of nodes); full tables: first targets from the strain's earlier declarers, -1.1 to -2.4% cycles. Not merged: 16d does the same more fully. |
| 16d strains (tp-strains, PR #11, merged) | full tables: first targets from the same deal's other declarers and leaders E, W, S, N: nodes -1.3 to -2.5%, cycles -1.3 to -2.5% (-3.3% after rebasing onto main). No effect on single problems. |

The second SMT thread adds 1.4-1.55x per core in every agent's measurements. Follow-ups outside
the agents: thread placement by NUMA node and core class, and no spin-waiting between questions in
solve_deal_parallel (issue #4: 2.2x better with four calls sharing the CPUs).

### 16a: table sizes and memory footprint for SMT throughput (tp-tables-size)

Goal changed to batch throughput per physical core with both SMT threads busy. Measured on
core 0 (CPUs 0 and 12) with `bench_solve parallel ... 0,12` and the new `bench_solve tables`
mode, both of which now print the worker threads' summed cycles per deal (or table); with two
threads on one core, core cycles per deal are about half that figure. Other agents were
loading the other cores, so single runs vary by up to 5%; the figures below are paired medians
of alternating runs. No change to the solver was kept.

**Footprint per thread (TTSTAT counters, public set, DIR_MIN 15).** Single problems: 2,544
keys per deal (max 11,921), so the 2^15-slot directory (512 KB) is touched at about 2,500
random lines (160 KB) and never grows on 99% of deals; the pool hands out 7,036 lines per deal
(450 KB; max 2.7 MB), reused from line 0 by every deal. Full tables (one reset per strain):
3,786 keys and 11,047 lines (700 KB, max 4.4 MB) per strain. Blocks are about 45% full: 37%
of the narrow keys hold one entry, 58% at most two, 76% at most four. Process RSS with two
workers is 1.15 GB, all of it on transparent huge pages (the ddclaim tables are 1.13 GB of it).

**SMT behaviour.** One thread: 15.6M cycles per deal; two threads on the core: 20.1M thread
cycles per deal (10.05M core cycles, x1.55). Tables: 128.8M -> 169.4M thread cycles per table
(x1.52). The cycle profile (bprof cyc, with a second solver running on CPU 12) keeps the same
shape under SMT: the slowdown is spread evenly, i.e. shared execution resources, not a memory
hot spot. Fills per thread and deal (AMD raw event 0x44, LsAnyFillsFromSys, through the new
`DDF_RAW_CM` / `bprof raw:` hooks): from L2 142k -> 206k, from L3 19k -> 29k, from DRAM 25k ->
25k. So the second thread costs about 10k extra L3 fills per deal (an estimated 2% of thread
cycles at most) and no extra DRAM traffic. The DRAM fills are mostly the ddclaim probes (the
4-card pattern probe holds about 38% of the samples; the rest skids into lead<false>).

**Directory size under SMT** (paired medians, thread cycles, 2 threads on core 0):

| DIR_MIN (control 15) | public, 7-11 pairs | chunk1[800] | tables public[150] |
|---|---|---|---|
| 11 | +0.4 / +0.9% | -1.1% | +0.4% |
| 13 | +0.3 / +1.4% | | |
| 14 | -1.0 / -1.4% | -1.4 / +0.0% | -0.2% |
| 16 | +0.7% | | |
| 12, grow at 1/8 full, DIR_MAX 22 | -0.4% | -1.2% | |

All within noise (repeats of DIR_MIN 14 moved from -1.4% to +2.4% on one thread); DIR_MIN 15
does not hurt two threads per core, and the directory size is not worth tuning per thread count.

**Sensitivity to the pool's footprint:** TTPAD=k wrote k extra lines per new narrow block (same
search, pool footprint +38% for k=1, about x2 for k=3). Two threads: public +1.0% / +1.1%
(k = 1 / 3), chunk1 +0.5% (k=3), tables +1.8% (k=3, all 5 pairs worse). So doubling the table
costs 0.5-1.8%; any compaction (a one-line first block for keys with at most 4 entries saves
about 18% of the lines without changing the search; 8-byte directory slots) is worth an
estimated 0.1-0.4%, below what can be measured here. Huge pages off for the table
(LEAN_NOHUGE_TT): no measurable change.

Expectation on servers: Turin has the same 1 MB L2 per core and 4 MB L3 per core as this
machine's Zen 5 cluster, so the same conclusions; Granite Rapids' 2 MB L2 halves the small
L2 effect. What will differ is DRAM: 25k fills (1.6 MB) per deal per thread go to the ddclaim
tables, with higher latency on servers and, on two-socket machines, to a remote node for half
of the threads (the tables are read into anonymous memory by the loading thread, so first
touch puts them all on one node). Replicating the 1.1 GB tables per NUMA node (or at least
interleaving them) is the footprint change most likely to matter there; it needs a
multi-socket machine to measure.

### 16b: interleaving searches on one thread (tp-interleave)

Goal: deals/s and tables/s per physical core with both SMT threads busy. Question: do several
searches per thread (the coroutines of `run_batch`, NCTX), handing over at the prefetch of a
table probe, raise throughput on 13-card deals and full tables? Answer: no, on this machine
(Zen 5 laptop, 1 MB L2 per core); every variant lost 3-10%. Measured on core 1 (CPUs 1 and
13), alternating, cycles from the hardware counters (sum over the core's threads per deal or
table); the interleaving variants run in the same binary as the control (`DDFAST_NCTX`).

| Configuration | public (600) | chunk3 (first 600) |
|---|---|---|
| deals, 1 thread, nctx 1 / 2 / 4 | 15.58M / 16.13M (+3.5%) / +8% (set mode) | 15.50M / 16.11M (+3.9%) (7 pairs) |
| deals, 2 threads (SMT), nctx 1 / 2 / 4 | 20.91M / 21.78M (+4.2%) / 324.6 -> 294.7 deals/s (-9%) | 19.89M / 21.09M (+6.0%) / 21.51M (+8.1%) |
| tables (first 200), 1 thread, nctx 1 / 2 | 135.3M / 141.2M (+4.4%) | |
| tables (first 200), 2 threads, nctx 1 / 2 / 4 | 180.0M / 188.4M (+4.7%) / 199.0M (+10.6%) | |

(2-thread figures are thread-cycles summed over both threads: a core's cycles per deal are
half of that. 1 thread 15.5M against 2 threads 10.45M core cycles per deal: SMT +48%.)

Where the yields go (compile switches YIELD3, YIELDP; NCTX=2 against an NCTX=1 control of the
same source, set mode, 5 rounds): yields at the three-card table only +4.4%; pattern table
directory only +5.7%; pattern directory and entry chunks +5.1%; chunks only +5.6%; all
(default) +2.6%. With 2 contexts the cost is nearly flat in where (and how often) the searches
hand over: yielding at only every 256th three-card probe (about 4 switches per deal) costs the
same +4% as at every probe (980 per deal), and L2-to-L3 misses rise 20-26% in every variant.
So the loss is capacity, not the switch: each live deal's table (directory 2^15 x 16 B plus
its blocks, roughly 0.6-1 MB) is about the size of L2, and two live deals per thread (four per
core with SMT) evict each other. The yields hide about what they cost: about 80 extra
instructions per yield (save/load of the hot state, the stack switch, a mispredicted return)
against a three-card probe that mostly hits L3. The probes that do miss further out (the
four-card pattern table: about 4% of cycle samples, ~1,750 probes per deal) are too small a
target to pay for a second deal's footprint. SMT already overlaps these stalls without the
footprint or switch costs.

Also tried and dropped:
- Sharing one transposition table across consecutive deals (the entries are rank-relative, so
  valid for any deal of the same strain class; reset at 2^15, 2^17 or 2^20 lines): +2.5 to
  +4% cycles, instructions +0.3-0.4%. Deals share too few positions; the chains only get
  longer. (So a shared table does not remove the footprint cost of interleaving either.)
- The four declarers of a strain interleaved on one thread sharing the strain's table (no
  extra footprint): +7% cycles per table, branch misses +17%; running them together loses the
  reuse the sequential order gets (later declarers find the earlier declarers' entries).

Kept for re-measuring on other machines (off by default, the default path is unchanged:
public-set nodes 82,915,440): `DDFAST_NCTX=k` (and `DDFAST_NCTX_CHUNK`, default 4k) makes each
`solve_parallel` / `solve_tables_parallel` worker interleave k searches (k whole tables for
tables); `bench_solve tables FILE cpus reps` (ddfast-only table throughput on a set's deals);
`BENCH_N` (first N problems of a set); per-deal cycles, cache and branch misses in the
`bench_solve parallel` output; YIELD3 / YIELDP compile switches.

Expectation for servers: EPYC Turin has the same 1 MB L2 per core, so the same result
(interleaving loses; with 16 threads per CCD sharing 32 MB L3 the evicted lines come back from
a busier L3, if anything worse). Granite Rapids has 2 MB L2: one thread with two live deals
might roughly break even, but with SMT on (the target) the core already holds two deals'
tables, and a third and fourth would spill. Worth one run there (`DDFAST_NCTX=2 bench_solve
parallel ...` on one core's two threads) before closing it.

### 16c: scheduling the questions across a batch (tp-questions; not merged)

Goal: batch throughput (deals/s and tables/s per physical core with both SMT threads busy);
latency no longer matters. Measured on core 3 (CPUs 3, 15), alternating A/B against a control
build (same source, switch off), 7 pairs, other agents loading the machine.

Where the question nodes are (STATS, single problems): public 2.248 questions and 138.2k nodes
per deal, the fitted guess inside the two-question window for 77.0% of deals; chunk6 (first 1,500)
2.226 questions, 141.2k nodes, window 78.1%. Questions outside the window hold 6.3% / 4.7% of
all nodes: the ceiling for any better first question (an oracle saves less, as the in-window
questions after a miss reuse the table).

- Per-batch calibration of the first guess (shift or per-class correction learned from the
  deals already solved): no room. The offsets first - value are centred on the window (chunk6:
  -1: 149, 0: 583, +1: 589, +2: 153), so any shift loses; a batch of a few thousand deals adds
  nothing to a model fitted on 106k deals. Not built.
- Step rules that use the first question's cost (a cheap answer means a loose question): a
  larger second step helps only when the value is two or more beyond the first target (under
  1% of deals). Not built.
- Phase scheduling (all first questions, then all follow-ups) would keep every deal's table
  alive between its questions for no gain in the guess; not built. The test sets hold no
  repeated deal (or deal + strain), so grouping problems of one deal on a thread has nothing
  to share there; for tables that grouping is solve_table itself.
- **Kept: first targets in solve_table from the strain's earlier declarers** (`TGUESS`, default
  1). North-South's value with the other side on lead moves by 0..+1 when North-South gain the
  lead, and the same side leading from the other seat nearly always gives the same value: d1
  (South leads) asks v0+1 first, d2 (West) v0, d3 (North) v1+1. Window, 100 random tables
  (suits / NT): d1 0.97 / 0.75, d2 0.95 / 0.96, d3 0.99 / 0.95, against 0.80 / 0.62 for the
  fitted guess. Questions per table 44.84 -> 41.79, nodes per table 1,349,111 -> 1,339,403
  (-0.7%: the later declarers' questions are cheap, the table is kept). Cycles per table
  (all threads): 2 threads chunk6 240.2M -> 234.4M (-2.4%), public 170.1M -> 167.0M (-1.8%);
  1 thread chunk6 192.1M -> 189.5M (-1.4%), public 118.9M -> 117.5M (-1.1%). Tables/s (2 threads):
  chunk6 25.32 -> 25.96, public 45.17 -> 45.70. Single problems unchanged (public STATS
  82,915,440 nodes). The earlier strains say little about a strain's first declarer (NT from
  the suits: window 0.65 against 0.62), so that is left to the fitted guess.
- Re-weighting the miss costs that pick the first question (kGF_COST, the too-low side
  scaled 0.6x-1.6x against the too-high side; dev switch, not kept): chunk6 nodes 211.83M at
  1x, 211.69M at 0.6x (-0.07%), 212.0M at 0.8x, 213.0M at 1.25x, 213.8M at 1.6x. Flat: the
  table is already at its optimum.

### 16d: first targets in full tables from the same deal (tp-strains, PR #11)

Merged; see the commit message of 9d86e38 and PR #11. In a full table each later declarer's first
target comes from the strain's results already found: the same side's other declarer (same tricks
93% of the time), and North-South on lead take as many tricks as when East or West leads, or one
more (96%); leaders are solved in the order E, W, S, N. Questions per table 44.7 -> 41.7; nodes per
table -1.3% / -2.5% / -2.3% (seeds 4242, 777, 31337); cycles per table -1.3% / -2.5% against a
`-DTPGUESS=0` control, -3.3% after rebasing onto main (seed 777, CPU 0, 5 pairs). Ceiling: the first
declarer of each strain starts with an empty table and costs about 54% of a table's nodes; an
oracle first target there saves only about 4% of the total, and no bound across leaders or strains
is provable (North-South on lead taking at least as many tricks fails in about 2 of 2,000 cases).

## Round 17: sorting all four suits at no trumps; a 5-card pattern table again (branch patterns)

Questions: the 3-card table and the 4-card pattern table sort the three side suits and leave
lane 3 (the trump suit) in place. At no trumps lane 3 is an ordinary suit, so all four can be
sorted. Does that help, and does it make a table for 5-card endings worth having?

Two compile switches were added for the experiment and removed afterwards (see "Adopted"
below). With both off the search was unchanged: 82,288,956 nodes on the public set with this
machine's tables, the same as main.
- `-DSORT4=1`: at no trumps the 3-card probe and the pattern-table key sort all four lanes
  (six comparisons choose one of 24 byte shuffles; `g_shuf` now has 64 entries, `g_lrank` gives
  each lane's place). The 3-card file is unchanged: the no-trump probe now reads only the 34
  configurations with the lengths in descending order, of the 102 stored.
- `-DP_LEVELS_N=2` with `DDFAST_TABLE_LEVELS=2`: a second pattern table for 5 cards a hand
  (`p5_v5`, at most 64 patterns per run, `DDFAST_TABLE_CAP5`), filled from the same 6-card
  endings.

Both switches must reach every source file, so they go in `CXX` (`CXX="clang++ -DSORT4=1"`):
`tables.cpp` is compiled without `XFLAGS`.

Measured on an Apple M5 (Highway build), public set split into its 141 no-trump and 459 suit
deals. Two sets of tables were filled to the same 48.2M six-card endings, one unsorted and
one with SORT4. Other jobs were loading the machine throughout (load average 12-20), so the
cycle counts are rough; node counts do not depend on load. All answers correct in every run.

Suit-length configurations (side suits sorted, then all four sorted) and complete-table sizes:

| cards a hand | arrangements | unsorted | side suits sorted | all four sorted |
|---|---:|---:|---:|---:|
| 2 | 2,520 | 165 | 41 | 15 |
| 3 | 369,600 | 455 | 102 | 34 |
| 4 | 63,063,000 | 929 | 193 | 60 |
| 5 | 11,732,745,024 | 1,435 | 285 | 85 |

A complete table is out of reach beyond 3 cards whatever the sorting: 4 cards is 63M x (193
+ 60) = 16 billion endings (16 GB at one byte each, without the deciding cards that the
search needs), 5 cards 4.3 x 10^12. So "a table for 5 cards" can only mean patterns.

Table sizes after the fill (unsorted -> SORT4): 4-card 501,843 runs, 688 MB -> 315,568 runs,
483 MB (-30%); 5-card 2.78M runs, 1,348 MB -> 1.86M runs, 1,058 MB (-21%). The 3-card file
would shrink from 377 MB to 251 MB if the 68 unused no-trump configurations were dropped (not
done).

Nodes and pattern-table hits (STATS builds):

| deals | tables | 4-card hits | 5-card hits | nodes | vs first row |
|---|---|---|---|---:|---:|
| no trumps (141) | unsorted, 4-card only | 65.8% | | 17,242,805 | |
| | SORT4, 4-card only | 79.2% | | 16,832,708 | -2.4% |
| | unsorted, + 5-card | 65.4% | 4.5% | 17,067,182 | -1.0% |
| | SORT4, + 5-card | 78.6% | 8.7% | 16,500,090 | -4.3% |
| suits (459) | unsorted, 4-card only | 85.8% | | 66,286,986 | |
| | SORT4, 4-card only | 85.8% | | 66,287,482 | 0.0% |
| | unsorted, + 5-card | 84.9% | 15.8% | 64,778,445 | -2.3% |
| all (600) | unsorted, 4-card only | 81.1% | | 83,421,397 | |
| | SORT4, 4-card only | 84.2% | | 83,005,947 | -0.5% |
| | unsorted, + 5-card | 80.1% | 13.3% | 81,705,418 | -2.1% |
| | SORT4, + 5-card | 83.4% | 14.3% | 81,149,919 | -2.7% |

Cost (plain builds, alternating runs, instructions per deal steady to about 1%):
- SORT4, no-trump deals: 46.3-47.9M -> 45.1-45.3M instructions (-2.6% min to min); cycles
  15.8-20.0M -> 15.1-16.7M (min -4%). Suit deals as a control: instructions equal (55.5-56.0M
  against 55.3-55.9M), cycles 15.3-18.4M against 14.8-18.4M, so the noise is about 3%. Over
  the whole set that is under 1%.
- 5-card table, all deals: instructions 53.4-54.1M -> 56.4-57.5M (+6%); cycles 18.3-19.1M ->
  21.7-23.5M (+15 to +25%), and the same with SORT4 (21.2-22.5M).

Findings:
- **Sorting all four suits at no trumps is a real but small gain**: the no-trump half of the
  4-card table needs a quarter of the runs, its hit rate on full deals goes from 66% to 79%,
  and no-trump deals take 2.4% fewer nodes and about 3% fewer instructions. No-trump problems
  are a fifth of a full table. The 4-card table was 20 points worse at no trumps than in suits;
  sorting closes two thirds of that gap.
- **It does not make a 5-card table worth having.** Sorting doubles the 5-card hit rate at no
  trumps (4.5% -> 8.7%) and does nothing for suits (15.8%); 86% of 5-card probes still miss.
  About 2,350 probes per deal save about 2,900 nodes per deal (2%) and cost about 3.5M
  instructions per deal, some 1,500 per probe: a miss scans a whole run. The same result as
  rounds 3 and 5 on Zen 5 (-6% nodes, +4% cycles there, with a table filled from 75M endings).
- **Why the hit rate is low.** The table is filled from random 6-card endings, where the same
  table answered 49% of the 5-card questions (on Zen 5, TABLES_NOTES); in a full-deal search the 5-card positions that
  reach the probe are the ones the per-deal table and the bounds have not already answered.
- An unsorted table is still exact under the SORT4 probe (an entry is true for every relabelling
  of the suits at no trumps), it just holds fewer of the keys asked for. Checked: `test_ddfast
  quick` passes with the SORT4 build on the unsorted tables.

Exactness, SORT4 build on its own tables, with and without the 5-card level: `test_ddfast
quick` all exact; `test_play 300 7` all agree with DDS; `bench_solve random 2000 4242` all
correct.

### Adopted: all four suits sorted at no trumps in the 4-card table (`p4_v6`)

The sort is now unconditional for the 4-card pattern table, and the file version is v6 so that
existing tables are rebuilt on first use. The 3-card probe is as before: the gain measured
above was in the 4-card table's hits, and the 3-card file would need a new layout to get
smaller. The 5-card level and both switches are removed again: `P_LEVELS` is 1.

Measured again on a quiet machine (Apple M5), each build on its own table filled with the
default settings (about 190M six-card endings in 170 s), five alternating runs, against main:

| deals | 4-card hits | nodes | instructions per deal | cycles per deal |
|---|---|---|---|---|
| no trumps (141) | 79.2% -> 84.9% | 16,829,253 -> 16,630,597 (-1.2%) | 44.86M -> 44.34M (-1.2%) | 12.76-12.98M -> 12.51-12.61M (-2.0% min, -2.5% median) |
| suits (459) | 89.6% -> 89.8% | 65,562,250 -> 65,545,595 | 54.44M -> 54.42M | 14.57-14.61M -> 14.56-14.62M |
| all (600) | 87.1% -> 88.6% | 82,288,956 -> 82,061,772 (-0.3%) | 52.12M -> 51.98M | 14.16-14.21M -> 14.05-14.14M (-0.5%) |

The pattern file is 540 MB against 836 MB (-35%), 315,756 runs. With a table filled this far
the unsorted one already answered 79% at no trumps, so the gain in nodes is half of what the
short fill above showed; the saving in size is the larger effect.

Exactness of the adopted code, on a table built from nothing: `test_ddfast quick`, `random
3000 20261120` (solve, solve_parallel, solve_deal_parallel) and `tables 100 77` all exact;
`test_play 1000 1` all agree with DDS; `-DSELFTEST=10 -DSELFTEST_MAX=11` 561,130 checks on the
public set, no failure; `-DVERIFY_ORDER -DVERIFY_BOUNDS` on the public set, no abort; 20,000
random positions each of 4, 5, 6 and 8 cards a hand all correct against DDS.

Not done: the same sort in the 3-card table (its no-trump half would shrink from 102 to 34
configurations, 377 MB to 251 MB, with a new file layout); a 5-card table filled from deeper
endings or probed only where a hit is likely; canonical order among suits of equal length in
the 3-card table.

## Round 18: ideas from bridge-solver (branch bsolver-ideas, 2026-11-21)

bridge-solver (Hanhong Xue's, github.com/macroxue/bridge-solver; measured as Robert Salita's
fork at `faster-search` 61a6e59, the build BEN ships, whose bounds have since been merged into
bridge-solver) is 2x DDS. The cut-off cards and the slow-trump, trump-length, top-trump and
slow no-trump rules are Xue's; trump tricks by rank, draw trumps then cash and "cannot take
every trick" at no trumps are Salita's. Counted on the
public set with the same definition (positions entered at a trick start): DDS 65,900 per
problem, bridge-solver 48,300 (42,300 with a perfect first guess; it asks 5.6 questions per
problem from a crude guess), ddfast 45,100 at 2.25 questions. So its search is about as
node-efficient as ddfast's, without endgame tables; ddfast's lead over it is cheaper nodes
(about 350 cycles per trick-start node against 640) and the fitted guess. Three of its ideas
were tried here. Baseline: main with a `p4_v6` table built on this machine (83,579,555 nodes;
the M5 table in round 17 gives 82,061,772: fill depends on machine speed). Every figure below
uses the same table.

| | nodes (public set) | trick-start nodes |
|---|---|---|
| main | 83,579,555 | 27,295,213 |
| + bridge-solver's sure-trick bounds (`BSB`) | 74,795,891 (-10.5%) | 24,393,030 |
| + cut-off cards at leads (`CUTC`) | 71,325,309 (-4.6%) | 23,436,591 |
| + cut-off cards at followers (`CUTF`) | 63,461,268 (-11.0%) | 20,736,865 |

Cycles, one core (Zen 5, CPU 0), against a control with both switches off (`-DBSB=0
-DCUTC=0`), alternating: single problems 15.72-15.93M -> 13.55-13.92M per deal (-13.3%), full
tables 150.7-154.2M -> 131.2-131.7M per table (-13.8%). The bounds alone: -4.9% and -2.5%.

- **Sure-trick bounds (`bs_bounds`, after DDS's quick and later tricks):** slow trump tricks
  (Kx behind the ace, KQ or KQJ against it, Qxx behind or against AK), trump length, trump
  tricks guaranteed by rank, draw trumps then cash, the other side's top trumps, and at no
  trumps (or none left) the other side's slow tricks and "the leader's side cannot take every
  trick" (too short in the suits it tops, or held up by stoppers). Each proof's cards are top
  slices of suits, so they go into the table as DDS's winning ranks do (with the suit lengths
  in the key). Where bridge-solver records only some of a suit's winners, all are kept. They
  decide 237,000 trick starts on the public set. Without side-suit fast tricks combined with
  trump tricks (bridge-solver's `side_tricks`), which would need a per-suit quick-trick count.
- **Cut-off cards (`CUTC`, `CUTF`):** a per-thread table of 2^16 entries (`CUT_BITS`), the card
  that last cut off, tried first. Keys as bridge-solver's: at a lead, the leader's exact hand,
  the parity of partner's suit lengths and (suits) the target; following suit, the led suit's
  cards at the trick start, the card winning so far and the leader; void, the hand, the
  winning card (no trumps: its hand) and (suits) the target. Tagged with the deal's
  generation; an ordering hint only. 2^14 entries: 63.85M nodes, 2^18: 63.34M.
- **Not adopted: fewer stored cards for quick tricks** (bridge-solver's `MinimizeFastTricks`:
  trump length alone, or the top cards of one suit): -0.1% nodes, removed. ddfast already
  extends runs only where the proof needs them (`RUNOPT`), prunes irrelevant cards of a
  suit, and plays the top card of each run; bridge-solver's other pattern work (extending
  runs as needed, lifting a run's winner) makes up for what ddfast does already.

Exactness: `test_ddfast quick`, chunks 1-3 of the 100,000-deal reference set (37,500 deals),
`tables 200 4242` all exact; `test_play 300 3` all agree with DDS; `-DSELFTEST=10` 461,012
checks on the public set, no failure; `-DVERIFY_ORDER -DVERIFY_BOUNDS` on the public set, no
abort; 20,000 random positions each of 6 and 9 cards a hand all correct against DDS.

### Follow-up: where cut-off cards pay (branch cutoff-persist)

With cut-off cards from 4 cards a hand (`CUTC_MIN` 4, as merged in #21), small searches pay
for the table: 7-card positions took 8% more cycles than with both techniques off (54.7-55.5k
against 50.6-50.8k per position), and BEN-style batches (`bench_libdds ... boards`) slowed by
9-17% at tricks 4 and 7. Probing the 512 KB table, often a cache miss, costs more there than
the better order saves. Full deals, the target, `CUTC_MIN` against cycles (best of 3
alternating runs) and nodes:

| from | problem | table | nodes |
|---|---|---|---|
| 4 cards | 13.67M | 132.0M | 63,461,268 |
| 6 | 13.57M | 130.3M | 63,637,252 |
| 8 (adopted) | 13.13M (-4%) | 126.9M (-4%) | 64,025,914 |
| 10 | 13.75M | 131.3M | 67,291,800 |

The gain is mostly in cache misses, not work: on the public set the instructions hardly move
(33,387,532 -> 33,338,120 per deal, -0.15%) while cycles fall in every alternating pair
(13.85-14.77M -> 13.33-13.39M); on docs/HISTORY.md's workload (300 problems and 300 tables,
seed 7) instructions -1.4% per problem and -0.6% per table, cycles 12.15M -> 11.97M (-1.5%) and
130.5M -> 127.2M (-2.5%), answers identical. With 8, 7-card positions are back to 51.6k (the
bounds' part remains). A 2^12-entry table (32
KB) also removed most of the small-search cost but was slower on full deals. Keeping entries
across deals (no generation in the key) is worse: 68,916,396 nodes on the public set (+8.6%),
and much slower on BEN-style batches, whose samples differ only in the hidden hands. Exact:
`test_ddfast quick`, chunk 4, `tables 100 77`, `test_play 200 5`, 5,000 positions each of 7
and 9 cards against DDS.

## Round 19: every card's score at a lead (branch all-cards-value-first, 2026-11-21)

Aim: BEN's calls at 8 to 11 cards a hand in batches of 32 (two hands fixed, the other two
re-dealt). Measured on an Apple M5, one thread, `bench_libdds LIB boards 32 1` with `STAGES`
(new: the stages of play to run) and bridge-solver at 61a6e59 as LIB.

`SolveBoard` with `solutions` 3 (every card's score, what BEN asks for) did a full solve per
card, each from a fresh guess. At a lead it cost 2 to 2.6 times a `solutions` 1 call, and
there bridge-solver came within 1.2-1.4x of ddfast, against 1.6-2.5x elsewhere. Now, at a
lead, the position is solved first: no card scores more than its value, so each card is asked
"at least the value?" and one less until the answer is yes. Of the cards at a lead 66% score
the value, 24% one less, 7% two less (79%, 14%, 5% in the middle of a trick): about 1.5
questions a card.

ms per batch of 32, every card scored, 12 positions a row, before -> after:

| cards a hand | at a lead | 1 card played | 2 played | 3 played |
|---|---|---|---|---|
| 13 | 252.4 -> 197.5 (-22%) | 0% | 0% | +10% |
| 12 | 136.1 -> 108.6 (-20%) | -6% | -2% | -3% |
| 11 | 46.1 -> 34.3 (-26%) | -3% | -1% | -2% |
| 10 | 24.6 -> 19.4 (-21%) | -3% | 0% | +14% |
| 9 | 6.07 -> 4.93 (-19%) | -1% | -1% | +2% |
| 8 | 2.73 -> 2.16 (-21%) | -8% | +6% | +17% |
| 7 | 0.85 -> 0.62 (-27%) | -2% | +5% | +11% |
| 6 | 0.30 -> 0.22 (-27%) | | | |
| 5 | 0.10 -> 0.07 (-30%) | | | |

So it is used at a lead, at every size, and not in the middle of a trick, where the extra
solve is not repaid. `DDFAST_OLD3` gives a full solve per card everywhere. `solutions` 1 and 2,
tables and `ddfast::card_results` (which still solves per card) are unchanged.

Tried for the same workload and not adopted (cycles per position, trick starts, `solutions`
1, batches of 32):

- Switched off in turn at 8, 9, 10, 11 cards: cut-off cards -6%, -6%, -2%, 0 to +2% (they
  cost at 8 and 9 cards: a call that small ends before the 512 KB table is reused; a size
  gate or a smaller table for small calls is not tried yet); `BSB` -1%, -1%, +5%, +10%; the
  4-card table +23%, +17%, +15%, +13%; the 3-card table 0 to +2%.
- A perfect first target: -6 to -9%. The previous sample's value or the median of the batch's
  first four as the first target: no gain (values within a batch span 4.6 to 5.8 tricks).
- The search table kept across the batch's samples: 6 to 30% slower, with more nodes.
- Interleaved searches on one thread: 1 to 5% at 2 or 4 contexts, slower beyond.
- Four threads on a batch of 32: 3.3x.

