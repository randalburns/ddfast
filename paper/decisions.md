# ddfast: the design decisions and what they say about the problem

This is source material for a paper. It records the decisions that shaped ddfast, why each was
taken, and the measurement behind it, including the ideas that were measured and rejected. The
decisions are split in two:

- **Part I, highly pruned minimax games in general.** Decisions that depend only on the shape
  of the search: a two-player, perfect-information game whose tree is cut so hard by move
  ordering, bounds and a transposition table that the search visits little more than a proof.
  They should carry over to other games with that shape.
- **Part II, bridge.** Decisions that depend on the rules of bridge: suits, ranks, tricks,
  trumps, partnerships and following suit.

Where a bridge decision is an instance of a general one, it points back to it.

- **Part III, more search for less time.** A cross-cutting view: the decisions where ddfast
  deliberately searches more nodes, or declines a saving in nodes, because the solver as a
  whole is faster that way.

## The problem

A double-dummy solver is given all four hands of a bridge deal, a trump suit (or no trumps) and
the hand on lead, and returns the number of tricks each side takes with best play by both
sides. The answer must be exact. The reference solver is [DDS](https://github.com/dds-bridge/dds);
ddfast returns the same answers 4 to 5 times faster on one core.

Seen as a game tree:

- Two sides, perfect information, zero sum. The value is a small integer, 0 to 13 tricks.
- Fixed depth: 52 plies in 13 tricks. The side to move changes only between tricks, with
  whoever won the last one.
- Wide near the root (up to 13 legal leads), but most moves are equivalent to another move or
  irrelevant to the result.
- Positions transpose heavily, and far more so once cards that do not affect the result are
  ignored.

What makes it "highly pruned": at the nodes that cut off, the first move tried is the one that
cuts in 84 to 94% of trick starts and 97 to 99% of the other three plays of a trick. A full
deal is solved in about 138,000 nodes. In that regime the three levers are, in order of what
they turned out to be worth here:

1. the **cost of a node** (a deal is 15.8M cycles, about 115 cycles per node);
2. the **size of each proof** (which of several refuting moves is tried first);
3. **not searching at all** (bounds, table hits, precomputed endings).

Finding a move that cuts is almost never the problem. Much of what follows is a consequence of
that.

## How to read the numbers

- Unless stated, measurements are user-mode hardware cycles per deal on one performance core of
  an AMD Ryzen AI 9 HX 370 (Zen 5), on a fixed set of 600 random deals ("the public set"),
  every answer checked against DDS.
- Node counts are trick-start ("lead") nodes plus the other three plays ("follow" nodes), from a
  counting build. The current build searches 82.9M nodes on the public set.
- The numbers were taken over months against moving baselines. A percentage is against the
  baseline of its own experiment, not against today's build. The leave-one-out factors
  ([docs/LOO.md](../docs/LOO.md)) are the exception: they are all against one build.
- "Slowdown ×" figures from the leave-one-out study say how much slower the whole solver gets
  when one addition is replaced by DDS's way of doing the same job.
- Sources: [docs/NOTES.md](../docs/NOTES.md) (the full history, cited as "round n"),
  [docs/LOO.md](../docs/LOO.md), [docs/TABLES_NOTES.md](../docs/TABLES_NOTES.md) and
  [README.md](../README.md).

## The decisions at a glance

| | Decision | Worth |
|---|---|---|
| **General** | | |
| G1 | Ask true/false questions, starting from a fitted guess | guess: 1.02×; structure: see G1 |
| G2 | Every result carries the part of the position that decided it | basis of the table and the endgame tables |
| G3 | A per-problem transposition table that never evicts | 374M → 123M nodes when it replaced a small one |
| G4 | Probe the table before computing bounds; store bound cutoffs | 9.7 → 7.0-7.7 ms; 1.03× |
| G5 | Order moves for a small proof, not for a cutoff | ceiling 27% of nodes; little of it reachable |
| G6 | Where the first move nearly always cuts, pick it by rule; do not score and sort | **1.66×** |
| G7 | Make the node cheap: one machine word per player, bit-parallel | 1.3-1.4× alone |
| G8 | Rewrite proven heuristics exactly on the new representation, checked side by side | 1.24× and 1.07× |
| G9 | Vectorize across moves and across table entries | 1.29× |
| G10 | Precomputed endings: complete tables when small, learned patterns beyond | **1.30×** as a pair |
| G11 | A probe must cost less than the nodes it saves | several ideas rejected |
| G12 | Parallelism across problems first; inside a problem only for latency | 5.2× DDS on 8 threads; 1.5× on one deal |
| G13 | Measure in cycles, against a control build, with node counts as a check | method |
| G14 | Exactness checked at three levels | method |
| **Bridge** | | |
| B1 | A hand is one 64-bit word, a 16-bit lane per suit, trumps in a fixed lane | with G7 |
| B2 | Ranks are relative: compact them with `pext`, restore with `pdep` | **1.52×** |
| B3 | "Cards that decided" is, per suit, a rank and everything above it | with G2 |
| B4 | Touching cards are one move; the run rule | −3% nodes |
| B5 | The table key is suit lengths and leader; entries are owners of the deciding cards | with G3 |
| B6 | Table and bounds only at the start of a trick | structure |
| B7 | Keep DDS's lead weights and trick bounds | without lead weights +69% |
| B8 | The follow-hand rules themselves | each −2 to −5% nodes |
| B9 | The three endgame tables' layouts | with G10 |
| B10 | The first-guess models, and in full tables the other declarers' results | −7% questions per table |
| B11 | Search compiled twice: no trumps and trumps | part of G7 |
| B12 | Same answers and the same interfaces as DDS | adoption |
| B13 | A pattern is a mask and a value, laid out so eight are matched at once | 1.06× (per-deal table), 1.01× (4-card table) |
| B14 | Fold equivalent layouts together: trumps to one lane, ranks, leader, side suits | what makes the endgame tables small enough |
| **Trade** | | |
| Part III | Search more nodes when each is cheaper | see the table there |

---

# Part I: highly pruned minimax games

## G1. Ask true/false questions, starting from a fitted guess

**Decision.** The search never computes a value. It answers "does the side on lead take at
least *t* tricks?" with a zero-width window, and the root asks a short sequence of such
questions, starting from a predicted value and moving one trick at a time. DDS does the same;
ddfast changes where the sequence starts.

**Why it fits the problem.** The value is one of 14 integers and a good predictor is cheap, so
two questions usually suffice: one answered yes at the value, one answered no just above it.
A true/false search needs no window bookkeeping, and every stored result is a plain bound.

**What was learned.**

- *The two tight questions are two nearly disjoint proofs of fixed total size.* Asking "at
  least v" and "at least v+1" in either order costs the same (184k against 182k nodes per deal
  on the 192 deals where two runs asked them in opposite orders). The first question is cheaper
  only by what it leaves in the table for the second (round 7, b-second).
- *So a better first guess has a low ceiling.* A perfect first target saved 5 to 10% early on
  and saves at most 4.7 to 6.3% of nodes now. The fitted guess recovers about a third of that: 2.39 →
  2.25 questions per deal, 1.02× in the leave-one-out study.
- *Start one above the estimate, not at it.* Starting at target *t* costs two questions when
  the value is *t* or *t* − 1, so the first target is floor(estimate) + 1 (2.15 → 2.07
  questions on six-card endings before any better model).
- *Missing by one costs about 10%, by two about 14%*, symmetric in direction. Only the
  two-question window matters.
- *Fail-soft does not help.* Returning a bound stronger than the target saved 0.5% of nodes,
  no questions, and cost 1.8% in cycles: the leaves (bounds, endgame tables, table entries)
  only ever prove the target asked for (round 7, b-nodes).
- *Loose questions are nearly free in nodes but not in time*: starting two tricks low costs
  fewer nodes and 3.5% more instructions.

## G2. Every result carries the part of the position that decided it

**Decision.** A search result is a pair: the answer, and the set of features of the position
the proof depended on (in bridge, cards; see B3). Everything else in the position could be
different and the answer would stand.

**Why it fits the problem.** A proof in a highly pruned tree touches a small part of the
position. Recording that part turns one proven position into a statement about a whole class
of positions. This is the idea of Ginsberg's partition search, and DDS is built on it. ddfast
keeps it and leans on it in three places:

- the transposition table stores classes, not positions (G3), as in DDS;
- moves that differ only in features that did not matter are skipped, as in DDS;
- the endgame tables store classes too, so a table that is far from complete still answers
  84% of what it is asked (G10). This use is ddfast's.

**What was learned.** Narrower is not automatically better. When several stored entries decide
a question, choosing the one that depends on the fewest cards saves 3.2% of nodes and no time
at all (leave-one-out, 1.00×). Scanning further for an even narrower one saves another 0.9% of
nodes and costs 1.8% in cycles.

## G3. A per-problem transposition table that never evicts

**Decision.** Each deal gets its own table: a directory from key to a chain of 8-entry blocks,
the blocks taken in order from a pool that is reset when the next deal starts. Nothing is ever
evicted within a deal.

**Why it fits the problem.** In a proof-sized search almost every stored entry is part of the
proof and will be wanted again. Losing one means proving it again.

**Evidence.**

- The first full-deal version used buckets of 8 entries per key and was 6 times slower than
  DDS. 80% of stores evicted a live entry. Chaining 4 buckets per key took it from 374M to 165M
  nodes, 16 buckets to 130M, unbounded chains to 123M. "The node gap to DDS was table capacity
  per key, not ordering" (round 2).
- A table too small for one deal (256 KB, resetting when full) searched four times the nodes
  (LOO).
- Every later attempt to drop or cut off entries lost: scanning only the first 3 or 6 blocks
  of a chain costs +41% and +13.5% nodes; not storing small subtrees costs 1 to 2% (round 15b).
- The working set is small because the table is per problem: 2,544 keys and about 450 KB of
  blocks per deal, which fits L2. That is why memory tuning is worth so little (G13).

**Layout decisions inside it, each measured:**

- A dense 16-byte directory slot plus separate blocks beats putting the first entries inside
  the slot (the directory grows fourfold and falls out of cache).
- The next-block links live in their own small array, not in the blocks. The array stays in
  L1, so the link loads in parallel with the block; a pointer inside the block made every hop
  wait on the block's cache line (+5.7% cycles).
- The directory starts large enough that most deals never grow it. Growing rehashes with
  unpredictable branches: −2.2 to −2.6% cycles from the starting size alone.
- A table shared across consecutive deals is worse (+2.5 to +4%): deals share too few
  positions, and the chains only get longer.

## G4. Probe the table before computing bounds; store bound cutoffs

**Decision.** At a trick start the table is probed first. The static bounds run only on a
miss, and when a bound decides the question the result is stored like a searched one.

**Why.** Half of all lookups hit, and a lookup costs less than the bounds. Probing first took a
deal from 9.7 to 7.0-7.7 ms at the time (round 3). Storing bound cutoffs is worth 1.03× (LOO):
the bound would be recomputed at every transposition otherwise.

## G5. Order moves for a small proof, not for a cutoff

**Decision.** Move ordering at trick starts is DDS's static weights, plus the table's
remembered best move, plus one narrow memory of proof sizes (B7). The many dynamic schemes that
were tried are not in the code.

**Why it fits the problem.** When the first move already cuts about 90% of the time, the
remaining waste is not in finding a cutting move.

**Evidence.**

- *The ceiling is known.* Re-solving each deal with every node's cutting move remembered from
  the previous pass searches 27% fewer nodes. No ordering can do better on this tree (round 5).
- *The waste is proof size.* At root nodes of true questions, 6.6 of 10 legal leads cut. Most
  of the excess is at nodes where the first lead does cut, but another cutting lead has a proof
  half as big. 10% of the nodes hold 71 to 78% of the excess (round 9, s2-root).
- *The cheap proof cannot be found by trying.* Leads that fail cost about as much as leads
  that cut, and there are several of them. Simulated on measured per-move costs, any
  round-robin or budgeted search costs 1.05 to 6 times the plain order, even if aborted work
  were free.
- *Classic dynamic heuristics do not help here.* A history heuristic and forcing the killer
  move first both cost cycles (the killer 18%). Trying first the move that was hardest to
  refute, a best move stored per pattern, and a hint per (key, target) each moved nodes by
  between −1% and +3%. None earned its cost.
- *The same static order is best for refutations too.* When every move must be refuted,
  reversing the order costs +5 to +18% nodes: the refutations of the strongest moves refute
  the rest.
- *"Waste" statistics mislead.* Nodes spent under a first move that failed pointed at cases
  where the first move was wrong only 1 to 2% of the time. What found real gains was measuring
  the proof size of every move from the same table state (save the table, search each move,
  restore) and comparing against the cheapest (round 11).

## G6. Where the first move nearly always cuts, pick it by rule; do not score and sort

**Decision.** For the second, third and fourth plays of a trick, ddfast does not generate a
scored move list. A short chain of tests picks the first card. If it fails, the rest are tried
in a fixed order (lowest first, or highest first for third hand).

**Why it fits the problem.** At these plies the first card cuts 97 to 99% of the time. A
weight for every legal move, then a sort, is paid at every node and used at one node in fifty.
A rule costs a few compares and one bit scan.

**Evidence.** This is the largest single addition: with DDS's follow weights in place of the
rules, the search is 3% smaller in nodes and the solver is **1.66× slower** (LOO). The rules
choose about as well; they are just far cheaper. The rules themselves are bridge (B8).

The same finding in another form: running DDS's weights at follow nodes "at any level and per
hand" was equal or worse in every configuration (round 4), and a branch-free version of the
rule chain was slower too (fewer branch misses, more instructions, +1.4% cycles).

## G7. Make the node cheap: one machine word per player, bit-parallel

**Decision.** The whole position is four 64-bit words (one per hand) plus one word of suit
lengths. Playing a card clears a bit and adjusts a length; undoing sets them back. DDS keeps a
position record with per-suit arrays, rank tables and a move list per ply.

**Evidence.** With ddfast made to search exactly as DDS does (same bounds, weights, first
target, no endgame tables, none of ddfast's additions), it visits 5% more nodes than DDS and
runs **1.32× faster**, 1.39× per node. That is the representation alone, about a fifth of the
total speed-up on a log scale. The other 3.2× is search (LOO, second study).

The caveat matters for the paper: the advantage depends on two instructions (B2). With them
emulated in software, the cost per node equals DDS's.

## G8. Rewrite proven heuristics exactly on the new representation, checked side by side

**Decision.** DDS's lead weights, quick-tricks and later-tricks bounds were first ported line
by line, scalar, on DDS's own data layout. Each was then rewritten on the hand words to give
bit-for-bit the same output, with a build mode that runs the port beside the rewrite at every
call and aborts on any difference.

**Why.** It separates two questions that are usually tangled: is the heuristic good, and is it
cheap. Because the rewrite is exact, node counts are identical before and after, so the whole
gain is cost per node and the comparison is clean.

**Evidence.** Lead weights on hand words 1.24×, bounds 1.07× (LOO). Along the way: the ported
code was a quarter of all cycles when the rewrite started. The check bites: a version that
left out one fallback aborted on the first run.

## G9. Vectorize across moves and across table entries

**Decision.** Three places use wide vectors: the weights of all legal leads at once (one lane
per card), the choice of the highest of up to 16 keys, and matching 8 table entries per
compare.

**Evidence.** All vector code together 1.29×; lead weights 1.09×, move pick 1.08×, table match
1.06×, endgame-pattern match 1.01× (LOO). How the patterns are encoded so that they can be
matched this way is B13.

**Portability decision.** There are two implementations of one small header of primitives:
AVX-512 intrinsics, and Google Highway (128-bit, static dispatch) for everything else. Every
path gives the same answers and the same node counts. Relative cost on one Zen 5 core: AVX-512
1.00, AVX2 with BMI2 1.11, SSE4 with table-driven `pext` 1.47, SSE2 1.62. On an Apple M5 the
Highway build takes as many cycles per deal as AVX-512 does on Zen 5.

## G10. Precomputed endings: complete tables when small, learned patterns beyond

**Decision.** The last plies are looked up, in three tables of different kinds:

- 2 cards a hand: every ending, built in memory at start-up.
- 3 cards a hand: every ending, complete, on disk.
- 4 cards a hand: not complete. A table of *patterns* (G2) proved while solving random 6-card
  endings for a fixed time budget. A pattern is a true statement about a class of endings, so
  any hit is exact; a miss falls back to search.

**Why it fits the problem.** Completeness stops being affordable one size up, but the proofs
at that size still depend on few cards, so a modest number of patterns covers most endings.

**Evidence.**

- *The tables substitute for each other.* Without the 3-card table: 1.04×. Without the 4-card
  table: 1.03×. Without both: **1.30×**, +56% nodes. Either alone is caught by the other, the
  per-deal table and the bounds (LOO).
- *One size further did not pay.* A 5-card pattern table (1.57 GB) saved 6% of nodes on full
  deals and nothing in time; turned back on later it cut instructions 2% and added 4% in
  cycles from memory misses (rounds 3 and 5).
- *The 4-card probe is the dearest memory access left*: about 1,750 probes per deal, 16% miss,
  roughly 30% of all sampled cache misses.
- *Hits are copied into the per-deal table* (1.04×): the next probe of that ending stays in
  cache.
- *Smaller positions gain far more*, since more of their tree is in the tables. Against DDS: 4.3× at 13 cards a hand, 4.8× at 10, 8× at 8,
  13× at 7, 58× at 5.

## G11. A probe must cost less than the nodes it saves

**Decision.** Several standard techniques that reduce nodes were measured and left out,
because at about 115 cycles per node a table probe that may miss the cache costs as much as
the handful of nodes it saves.

**Evidence.**

- *Enhanced transposition cutoffs* (look up the positions after each move before searching
  any): −8.8% nodes, +14% cycles in the strongest form; every variant lost (round 9).
- *Prefetching* table lines early in the node: "the search has no slack to hide a memory
  miss." Software prefetch as a whole is 1.00× (LOO).
- *Interleaving several searches on one thread* (coroutines that switch at a table probe so
  the miss is over on return) pays for small endings, where the big tables dominate: batches of
  small positions gain 1.2 to 1.5× from it. On full deals it loses 3 to 10% in every configuration, because two live per-deal tables no longer
  fit L2. SMT overlaps the same stalls without the footprint (round 16b).
- *Profile-guided optimisation* removed 9% of instructions and at first 3% of cycles; after
  the vector rewrites, nothing. The search is bound by branch misses and table latency, which a
  static layout does not fix.

## G12. Parallelism across problems first; inside a problem only for latency

**Decision.** The throughput path is one deal per thread: a persistent pool, one pinned worker
per CPU, deals handed out one at a time from a counter, predicted-costliest first, the endgame
tables shared read-only. A separate call splits one deal across cores.

**Why.** Deals are independent and their cost has a heavy tail (mean 4.6 ms, 99th percentile
33.7 ms, maximum 134 ms on one core), so handing out single deals wastes almost nothing.
Splitting one proof across cores throws away transpositions.

**Evidence.**

- *Batch:* 8 threads on 4 cores, 1,419 deals/s against DDS's 272 (5.2×). The second SMT
  thread adds 1.4 to 1.55× per core, for this branchy code, at equal clock.
- *Deal order matters only for small batches.* Idle time at the end of a 600-deal batch on 5
  threads: 1.6-2.4% in the given order, 0.4-1.7% with a fitted cost predictor, 0.1-0.2% with
  the true costs.
- *One deal on 4 cores:* 1.45 to 1.57× lower latency, for 1.6× the nodes. Root leads are the
  work items; the remaining replies to a lead are split only after its first reply fails to
  refute it. A shared table is striped 16 ways with lock-free readers that treat a changed
  sequence number as a miss. A shared table from 6 cards up trades fewer nodes for dearer ones
  and comes out even with one from 8 cards up (round 13).
- *Threads, not processes*, unless the tables are mapped: a process that reads the tables
  holds 1.27 GB of its own; with the files mapped read-only it holds 56 MB and searches at the
  same speed (measured on an Apple M5). Mapping is opt-in because on the Linux kernel used,
  file mappings got 4 KB pages and searched several percent slower.

## G13. Measure in cycles, against a control build, with node counts as a check

The method changed the conclusions often enough to count as a design decision.

- **Cycles from hardware counters, not time.** Repeatable to about 1% where wall time moved
  10% on a shared laptop whose clock ranges from 3.0 to 5.1 GHz.
- **Alternating A/B runs, best or median of several.** Sequential runs drift.
- **A control build.** Two builds of the same search with harmless source differences differ
  by 1.3 to 1.4% from code layout alone. A candidate is compared with the same source with the
  feature switched off, never with the previous commit's binary.
- **Screen with instructions and branch misses, decide with cycles.** The first two are nearly
  deterministic. Here a cut in instructions buys about half as much in cycles.
- **Node counts as a determinism check.** A change meant to affect only speed must leave the
  node count identical, to the node. This caught an ordering heuristic that depended on timing
  (it ranked moves by measured cycles), which was then changed to count nodes.
- **Cost of a component by calling it twice.** A build that calls a function twice per use,
  with the search unchanged, gives that function's warm cost directly.
- **Leave-one-out, on a log scale.** Each addition is replaced by the baseline's way of doing
  the same job and the whole solver is timed. Shares are log(slowdown) / log(total); they do
  not add to 1, because additions interact (the two endgame tables are the clear case).
- **What turned out not to matter:** huge pages (1.01×), software prefetch (1.00×), table size
  parameters within a wide range, and the layout of the hot structures.

## G14. Exactness checked at three levels

- **Answers.** Every timed run compares every answer with the reference solver's.
- **Heuristics.** Rewritten bounds and weights run beside the originals and abort on any
  difference (G8).
- **Proof supports.** A self-test build takes every *k*-th searched node, re-deals the cards
  its result claims did not matter (keeping suit lengths), re-solves with an empty table and
  aborts if the answer changes: about 600,000 such checks per run of the public set. Dropping
  one card from a support on purpose fails at once, so the test has teeth.

A useful invariant throughout: a change that only reorders moves cannot change an answer, so
ordering experiments need only node counts and timing, not a new correctness argument.

---

# Part II: bridge

## B1. A hand is one 64-bit word, a 16-bit lane per suit, trumps in a fixed lane

**Decision.** Each hand is 64 bits: four 16-bit lanes, one per suit, bit *r* of a lane for
rank *r*, the top bit of each lane kept free as a guard. The trump suit is always moved to the
last lane. A fifth word holds the 16 suit lengths (four hands by four suits, 4 bits each).

**Why.** A hand has at most 13 cards per suit and four suits, so it fits a register with room
for a guard bit that stops carries crossing suits. Whole-hand questions become one operation:
"cards of the suit led", "does this hand beat that card", "who holds the top card of each
suit". With trumps in a fixed lane, trump masks are constants and the endgame tables have one
canonical layout. Suit lengths are kept, not recounted, because the table key (B5) and the
discard rule (B8) read them at every node.

## B2. Ranks are relative: compact them with `pext`, restore with `pdep`

**Decision.** Wherever the search reasons about a suit, it first packs the cards still in play
into adjacent bits (`pext` with the mask of remaining cards). Played cards drop out, and only
the order of the rest is left. `pdep` puts a result back at the true ranks.

**Why.** In bridge only the relative order of the cards still in play matters: once the king
is gone, the queen is as good as the king was. After compaction, "touching cards" are adjacent
bits, "the *j*-th highest card" is a bit position, and two positions that differ only in
played cards are the same bit pattern, which is what the tables index on.

**Evidence.** With `pext` and `pdep` done by byte tables, the same search (identical nodes) is
**1.52× slower**, and the representation's whole advantage over DDS disappears (0.99× per
node). This is the second largest factor and it is two instructions. It is also the reason the
ratio to DDS is lower on Arm, which has neither (3.1× on an Apple M5, where DDS itself also
runs faster).

## B3. "Cards that decided" is, per suit, a rank and everything above it

**Decision.** The proof support of G2 is, for each suit, the lowest rank that mattered. All
cards of that suit at or above it are taken to matter; all below are interchangeable small
cards. This is DDS's form and ddfast keeps it, as one word with one lane per suit.

**Why.** A trick is won on rank, so if a card mattered, the cards above it mattered for the
same reason. The form is closed under the operations the search needs (union is a per-suit
minimum) and costs 4 small numbers. A hand's cards below the threshold are equivalent to each
other, so only one is tried.

## B4. Touching cards are one move; the run rule

**Decision.** Adjacent cards in the same hand (after compaction) are one move; only the top of
each run is tried. When a card wins a trick on rank, only that card is marked as deciding. At
a node where every card fails, the run below each tried deciding card is added.

**Why.** The first half is standard. The second half is where exactness is easy to lose: an
early version gave a wrong answer by counting only the winning card when the whole run had to
count. The current rule is the narrowest that is still correct, verified by the self-test of
G14.

**Evidence.** −3.0% nodes when introduced; no measurable time in the final build (LOO 1.00×).
It is kept because narrower supports make the stored patterns more general.

## B5. The table key is suit lengths and leader; entries are owners of the deciding cards

**Decision.** The transposition key is the 16 suit lengths, the hand on lead and the number of
cards. Under a key, each entry holds which hand owns each deciding card, as two packed owner
strings, plus the targets it decides. A lookup compares the current owners against 8 entries
at once.

**Why.** Suit lengths fix which tricks are possible at all (who can follow, who can ruff), so
they cannot be abstracted away, and they are already at hand in one word (B1). Within a key,
positions differ only in who holds which relative rank, and by B3 only the top few ranks of
each suit are compared.

**Details that were measured.**

- Entries for up to 7 cards a hand are 16 bytes, from 8 cards 24 bytes.
- Small entries store a mask of the targets they decide and the answer in one bit, so the test
  is one vector instruction per block (−1.1%).
- Chains are short: a hit scans about 2 blocks at 6 cards a hand, a miss about 3.

## B6. Table and bounds only at the start of a trick

**Decision.** Only trick starts are stored and probed. The other three plays of a trick have
no table, no bounds and no stored state beyond the trick so far.

**Why.** A position in mid-trick is described by more than the hands (the cards on the table,
who is winning), transposes far less, and is one to three cheap plies from a trick start. It
also means the unit of work is the trick, which is why enhanced transposition cutoffs are so
awkward here (G11): the first position that can be looked up is three plies down.

## B7. Keep DDS's lead weights and trick bounds

**Decision.** The choice of lead and the static bounds are DDS's, unchanged in what they
compute (G8 changed how). ddfast adds two small things to lead ordering: extra weight for
the lead the table remembers as best for this key, and a per-deal memory at 12 cards a hand of
which leads cut before and how large their proofs were.

**Why.** They encode decades of bridge knowledge, and every attempt to replace or bypass them
lost.

**Evidence.**

- Without DDS's lead weights: **+69% cycles**. Using them only higher in the tree: +2% (from 6
  cards) to +10% (from 8).
- Without the table's remembered lead altogether: +29% nodes. The extra weight on top of DDS's
  own bonus is worth 1.02×.
- The 12-card lead memory: −0.3% nodes, about −1% cycles. Extending it to other depths or to
  the defenders made things worse.
- Quick tricks are computed only when the side's top cards could reach the tricks needed. A
  cheap upper bound to skip more calls skipped only 4.7% of them: the calls that fail, fail on
  entries and blockages a cheap bound cannot see.

## B8. The follow-hand rules themselves

G6 is the decision to use rules. These are the rules as they stand in `pick_follow` and
`pick_void` (`src/solver.cpp`), in the order they are tested for each hand, with the change in
nodes when each was introduced; "(base)" rows were there from the start. Seats are named by
order of play in the trick, so second hand's partner plays fourth and third hand's partner
led. The later rules were found by measuring proof sizes (G5).

| Hand | Situation | First card | Nodes |
|---|---|---|---|
| Second | Cannot beat the lead | Low | (base) |
| Second | Partner holds a card above the lead and third hand's best, is long (3+) and holds a card above second hand's best | Lowest card above third hand's best | −2.3% |
| Second | Partner holds a card above the lead and third hand's best | Low | −3.1% |
| Second | Can beat third hand's best (or third hand is void) | Lowest card that does | (base) |
| Second | Can beat the lead but not third hand's best, and partner cannot beat the lead | Cover with the lowest winner | −3.8% |
| Second | Otherwise | Low | (base) |
| Third | Partner is winning and fourth hand cannot beat it | Low | (base) |
| Third | Partner is winning and fourth hand can beat it | Lowest card above fourth hand's best, else low | (base) |
| Third | An opponent is winning | Lowest card above fourth hand's best, else the lowest winner, else low | (base) |
| Fourth | Partner is winning, or cannot win | Low | (base) |
| Fourth | Can win the trick | Cheapest winner | (base) |
| Void, second | Third hand could overruff and a discard is available | Discard, do not ruff low | −2.9% with next row |
| Void, third | Partner is winning but fourth hand would beat it | Ruff (above fourth hand's trumps if it is void too) | |
| Void, any | Not ruffing | Lowest card of the longest side suit | −1.0% |

Observations worth keeping for the paper:

- *Second hand is where the gains were.* Rules for third and fourth hand have measurable
  oracle gaps (third hand 17.1M against 14.0M nodes in one log) but no simple feature
  separated the cases, and one rule that the offline analysis predicted would help made things
  worse in the real search.
- *Some textbook rules cost nodes.* "Cover an honour with an honour" helped until the partner
  rule above existed, then cost 1.2%. Third hand playing low when fourth hand can ruff: +4.5%.
- *Gains were checked on fresh deals*, not only the public set; one discard rule gained on the
  public set and lost on fresh deals, and was dropped.
- *The discard rule is cheap only because suit lengths are kept in a word* (B1): a first
  version that counted cards per suit cost what it saved.

## B9. The three endgame tables' layouts

- **Canonical form.** The 3- and 4-card tables store positions relative to the leader, with
  the trump suit in its fixed lane and the side suits sorted by length (B14). Sorted side suits, minimal 3-card supports
  and a longer fill together took the 4-card table's hit rate from 45% to 85% in early
  versions, and a larger fill later to 95% (on random 6-card endings).
- **2 cards a hand.** 2,520 arrangements per suit-length configuration, both targets, with a
  smallest exact set of deciding cards. 3.3 MB, built in memory.
- **3 cards a hand.** 369,600 arrangements × 102 suit-length configurations × 2 (trumps, no
  trumps), 5 bytes each: the trick count and, for each target, a minimal exact set of deciding
  cards. 377 MB. One lookup replaces the search.
- **4 cards a hand.** A directory from suit lengths to a run of 16-byte patterns (owners of the
  deciding cards and a bound), most-hit first, at most 256 per run, matched 8 at a time.
  758 MB. Filled by solving random 6-card endings against the table as it stands, in rounds,
  merging the new patterns in by streaming files so the table is never assembled in memory.
- **Minimal supports.** The 2- and 3-card tables store a smallest exact set of deciding cards,
  found by exhaustive checking, not whatever a search happened to touch. That makes the
  patterns built on top of them more general. For the 2-card table alone it was worth only 2%
  of nodes.
- **What did not help:** dropping patterns never hit during the fill (tables 3× smaller, 8%
  slower); longer runs; a 6-card table (6.6% hits).

## B10. The first-guess models, and in full tables the other declarers' results

**Decision.** The first target of G1 comes from one of three sources:

- *Full deals:* a small neural network (one hidden layer of 24 units) over bridge features of
  the two sides: who holds the top six ranks of each suit, top runs, length beyond the
  opponents' longest holding, stoppers, trump length and fit, ruffing values. Trained on
  106,000 random deals solved by DDS. The value falls in the two-question window for 79% of
  suit contracts and 71% of no-trump contracts.
- *Endings of 4 to 7 cards a hand:* an additive model, one table entry per suit keyed on its
  lengths and the owners of its top four cards. 91% in the window at six cards, 1.92 questions
  against a floor of 1.83.
- *Full tables (all 20 declarer and strain combinations):* the result already found for the
  same strain. The same side's other declarer has the same result 93% of the time; North-South
  on lead take the same or one more than when East-West lead 96% of the time. Leaders are
  solved in the order East, West, South, North, and the per-deal table is kept across the four
  leaders of a strain.

**Evidence.** Network: 2.39 → 2.25 questions per deal. Data volume is what made it work: on
6,000 deals a network overfitted badly and a linear model was as good as anything. Full
tables: 44.7 → 41.7 questions per table, −1.3 to −2.5% nodes and cycles.

**What is not provable.** "The side on lead takes at least as many tricks as when the other
side leads" is false in about 2 deals of 2,000, so it can set the first question but cannot be
used as a bound. A strain's result says little about another strain's.

## B11. Search compiled twice: no trumps and trumps

The search is a template on "trumps or not" and compiled twice. Ruffing logic, trump masks and
the trump terms of the weights vanish from the no-trump copy. DDS likewise has separate weight
functions for the two cases. This is counted as part of the representation in G7.

## B12. Same answers and the same interfaces as DDS

**Decision.** ddfast is not a new solver with new semantics. It returns DDS's answers, takes
DDS's deals, and ships DDS's C interface and DDS 3's Python functions on top of its own API,
so existing programs can switch without changes.

**Why it matters for the paper.** It fixes the evaluation: there is one reference, every run is
checked against it, and the comparison is like for like (same compiler, same flags, same
deals, same number of threads). It also sets the scope: the contribution is how the search
runs, not what it computes.

## B13. A pattern is a mask and a value, laid out so eight are matched at once

**Decision.** Every stored result, in the per-deal table and in the 4-card table, is a
*pattern*: a statement "any position with these suit lengths in which these cards are held by
these hands has this result". It is encoded as two words, and both tables are laid out so that
one position is tested against eight patterns in a handful of vector instructions with no
branch per entry.

**The encoding.**

- *The query.* The cards still in play are compacted with `pext` (B2) into adjacent bits, so a
  position with *n* cards a hand has 4*n* card slots, highest first within each suit. Each
  card's owner is one of four hands, relative to the leader: two bits. They are kept as two bit
  strings over the slots: "held by the leader's left or right hand opponent", and "held by
  partner or the right hand opponent". For up to 7 cards a hand both strings fit one word
  (2 × 28 bits), called `own`.
- *The pattern.* `b` is the mask of the cards that decided (B3), repeated under both strings.
  `a` is the owners of exactly those cards. A position matches when `(b & own) == a`: a
  ternary match, where a card that did not decide is "don't care".
- *The bound, in the same word.* The spare top bits of `b` say which questions the entry
  answers. In the per-deal table that is one bit per target plus the answer, so "does this
  entry decide my question" is one more AND against a constant. In the 4-card table it is a
  low and a high bound of 3 bits each.
- *The key is not in the entry.* Suit lengths, leader and card count select the chain or run
  (B5), so an entry spends all its bits on the pattern.

**The layout.**

- *Structure of arrays, one cache line per field.* A block of the per-deal table is `a[8]`
  then `b[8]`: two 64-byte lines, each one vector load. From 8 cards a hand a pattern needs 52
  slots, so the two owner strings get a word each and a block is three lines.
- *One compare per block.* AND the query with the eight masks, compare with the eight values,
  AND with the test of the bound bits: the result is an 8-bit mask of entries that match *and*
  decide. If it is zero, go to the next block; otherwise the lowest set bit is the answer. One
  branch per block, none per entry.
- *The narrowest match without a loop.* When more than one entry decides, the one with the
  fewest deciding cards is taken (G2) with a vector popcount of the masks and a masked minimum
  over (popcount, index).
- *Back to cards.* The winning entry's mask goes through `pdep` to the true ranks and is
  filled upward per suit, which gives the "cards that decided" to return (B3).
- *The 4-card table adds canonical order.* Its side suits are sorted by length, so the query's
  suit lanes are permuted with one byte shuffle before compaction and the result is permuted
  back. A run stores all its `a` lines and then all its `b` lines, most-hit entries first, and
  is prefetched 8 groups at a time.
- *Unused slots never match* (`a` all ones, `b` zero), so a partly filled block needs no count.

**Why it fits the problem.** A lookup is not "find this position" but "find any stored proof
whose conditions this position satisfies". That is a search within a small set per key (most
keys hold one to four entries, chains are two or three blocks), which a hash table cannot do
and a vector compare does in one step. It works because of the choices before it: relative
ranks make the slots line up between positions (B2), the threshold form of B3 keeps the mask
to one word, and exact keys keep the sets small (B5).

**Evidence.**

- Matching 8 at once against a scalar loop over the same entries: 1.06× for the per-deal table,
  1.01× for the 4-card table (LOO, identical nodes).
- Target bits tested with one vector instruction, in place of two shifts, two ANDs and two
  compares on packed bounds: −1.1% cycles (round 9).
- In the 4-card table, combining the match and bound tests into one mask per group was one of
  the changes that took branch misses on 6-card endings from 319 to 236 per position.
- Choosing the narrowest entry with a scalar loop on every hit cost 1% in cycles; the vector
  form used only when two or more entries decide is free.
- On machines without AVX-512 the same compare runs through Highway on 128-bit vectors, four
  steps per block, with the same results.

**What did not help.** Narrower blocks of 4 patterns in one line for small positions; the
first block of a chain placed inside the directory slot; tightening a matching pattern in
place where a new one would be appended; a per-slot summary of bounds to skip chains that
cannot decide (only 2 to 9% of misses would have been skipped).

## B14. Fold equivalent layouts together: trumps to one lane, ranks, leader, side suits

**Decision.** Many different bridge positions are the same position under a relabelling. ddfast
removes each relabelling at the cheapest point it can, and only where it pays. There are four,
and they are applied in different places.

| Equivalence | What is folded | Where it is applied | How |
|---|---|---|---|
| Which suit is trumps | 4 trump suits → 1 layout | Once per problem, on entry | The trump suit is moved to lane 3 |
| Which ranks are left | Any ranks → the top *k* of the suit | At every use | `pext` over the cards in play (B2) |
| Who is on lead | 4 leaders, 2 sides → "the leader's side" | Endgame tables only | Hands taken in order from the leader; the target flipped for the other side |
| Which side suit is which | 6 orders of the three side suits → 1 | 3- and 4-card tables only | Lanes sorted by length with one byte shuffle |

**Trumps in the third lane (lane 3).** A problem in hearts and the same cards in spades are
one problem once the suits are renamed. So when a position is loaded into the search, the
trump suit is swapped with whatever suit was in the last lane (`solve_position` in
`src/solver.cpp`). Everything after that sees one layout:

- The trump mask is a compile-time constant, in the search, in the bounds and in the weights.
  With B11 (the search compiled once for trumps and once for no trumps) there is no "which
  suit is trumps" variable anywhere in the inner loop.
- Deciding a trick needs no lookup of the trump suit: a card beats the current best if it is
  in the same lane and higher, or if it has a bit in the constant trump lane.
- The tables need one copy for trump contracts, not four, and the ports of DDS's bounds and
  weights (which take the trump suit as a parameter) are called with a constant.

At no trumps nothing is moved: lane 3 is simply the fourth suit.

**Relative ranks.** B2. It is listed here because it is the same kind of step: a position
holding A, Q, 7 of a suit with the K, J, 10, 9, 8 already played is the position holding A, K,
Q. Every table index and every pattern is built on compacted ranks.

**The leader and the sides.** In the endgame tables a position is read starting from the hand
on lead: the query takes the hands in the order leader, left-hand opponent, partner,
right-hand opponent, and the entry answers "does the leader's side take at least *t*". When
the leader is on the other side, the question is turned round: the leader's side takes at
least *n* + 1 − *t* exactly when the first side does not take *t*. So four leaders and two
sides are one entry.

The per-deal table does not do this. It keys on the leader and stores owners by absolute seat.
Within one deal a position and its rotation never both occur, so rotating would cost work at
every lookup and match nothing.

**The side suits.** Swapping two side suits changes nothing about a position, so the 3- and
4-card tables keep only one order: the three side suits sorted by length, the trump lane left
in place. A probe does this with three comparisons, which select one of six byte-shuffle
controls from a small table; the shuffle moves the 16-bit lanes of the hand words into sorted
order before the ranks are compacted, and the inverse shuffle moves the result's cards back.

- In the 3-card table the suits are ordered by how many cards of the suit are left. That
  takes the suit-length configurations from 455 (all ways to split 12 cards over four suits)
  to 102, so the file is 377 MB where an unsorted one would be about 1.7 GB.
- In the 4-card table they are ordered by the suit's four hand lengths taken as one number,
  which also separates suits of equal total length, and the sorted lengths are the directory
  key. Each stored pattern then serves every relabelling of its side suits. Sorting, together
  with minimal 3-card supports and a longer fill, took the hit rate from 45% to 85% in early
  versions.
- The 2-card table is not sorted. It holds all 165 suit-length configurations in 3.3 MB, so
  there was nothing to gain.
- At no trumps only three of the four suits are sorted, because lane 3 stays in place. A full
  sort over four suits would shrink the no-trump half further. It has not been done.

The per-deal table does not sort suits either, for the same reason it does not rotate: within
one deal the suits are fixed, and a position's suit-swapped twin does not arise. A table
shared across deals, where the twins would arise, was measured and is slower (G3).

**Why it fits the problem.** Each folding multiplies the reach of everything stored: a pattern
(B13) already stands for every position that agrees on the deciding cards, and with these four
steps it also stands for those positions in any trump suit, at any ranks, from any leader, and
with the side suits in any order. For the precomputed tables that is the difference between
feasible and not. For the search it is free: the trump move is paid once per problem, the
rank compaction is one instruction, and the other two are paid only on an endgame probe.

**What is not folded.** Suit order at no trumps beyond the three sorted lanes; suit order and
rotation in the per-deal table; and anything across strains, since a result in one strain says
little about another (B10).

---

# Part III: more search for less time

Nodes are the usual measure of a search. Here they are a poor one. Again and again the better
decision was to search *more* nodes, or to turn down a technique that saves nodes, because the
work needed to save a node cost more than the node.

## The exchange rate

A node costs about 115 cycles on average. Three facts follow from that number:

- **Anything paid at every node is expensive.** There are 138,000 nodes per deal. Ten cycles
  of extra work per node is 9% of the run, and must remove more than 9% of the nodes to pay.
- **A table probe costs several nodes.** A probe that leaves the cache costs as much as the
  subtree it might save: with enhanced transposition cutoffs at trick starts, about 5% of the
  probes hit and a hit saved a subtree of about 7 nodes.
- **The nodes a refinement saves tend to be the cheap ones.** This is an interpretation, not
  a measurement: the search is already close to a proof, so what a refinement removes is
  mostly small subtrees that would have ended quickly in a table hit or a bound, while the
  work to remove them is paid everywhere.

## Decisions that search more and run faster

"Nodes" is what the rejected alternative would have saved. "Time" is what it cost.

| Decision taken | Alternative that searches less | Nodes | Time | See |
|---|---|---|---|---|
| Follow hands pick one card by rule | DDS's weight for every card, then sort | −3.4% | **1.66× slower** | G6 |
| No lookahead into the table before searching a trick | Enhanced transposition cutoffs, fourth hand | −8.8% | +14% | G11 |
| | The same at trick starts, forced replies only | −2.4% | +8% | G11 |
| | Lookahead only from 8 cards a hand | −3.6% | +0.8% | G11 |
| Stop at the first block holding a deciding entry | Narrowest deciding entry in the whole chain | −0.9% | +1.8% | G2 |
| Results prove only the target asked for | Fail-soft: return and store the stronger bound | −0.5% | +1.8% | G1 |
| No 5-card endgame table | A 1.57 GB table of 5-card patterns | −6% | +4% | G10 |
| First question at the predicted value | Start two tricks low, where questions are nearly free | −1.0% | +3.5% instructions | G1 |
| A hit deep in a chain is left where it is | Copy it to the newest block | −0.2% | +1 to +2% | G3 |
| One deal split across 4 cores, when latency matters | The same deal on one core | −38% (the split searches 1.6× the nodes) | 1.45-1.57× longer to answer | G12 |

Two more of the same kind, where the decision is structural and has no single measurement:

- **No table and no bounds in mid-trick (B6).** Three plays in four are made without looking
  anything up. A table there would catch transpositions the trick-start table misses, at the
  price of a probe on every play.
- **Bounds only where they can decide.** Quick tricks are computed only when the side's top
  cards could reach the tricks needed. A cheap test to skip still more calls would have
  skipped 4.7% of them and cost more than it saved.

## The opposite cases

The rule is not "nodes do not matter". Two things mark the cases where spending work to save
nodes did pay:

- **The saving is large and lands on expensive subtrees.** DDS's lead weights cost about 1,000
  instructions per expanded trick start before they were vectorized (G9), then the dearest
  single step in the search, and without them the solver is 69% slower. The endgame tables cost the worst memory misses in the
  search and remove 56% of the nodes as a pair.
- **The extra work is paid rarely.** The per-deal table is probed once per trick, not once per
  play. The second and later follow cards are ordered only after the first has failed, which
  is one node in fifty.

And there is a middle group: refinements that save nodes and cost exactly as much as they
save. Choosing the narrowest of several deciding entries in a block saves 3.2% of nodes, and
the run rule (B4) 3.5%; each is 1.00× in time. They are kept only because narrower proofs make
stored patterns apply to more positions.

## What this says about method

- **Nodes cannot be the measure of merit** for a search this close to a proof. Every row of
  the table above would have been decided the other way on node counts.
- **Instructions are not the measure either.** Profile-guided optimisation removed 9% of
  instructions and, in the end, no cycles (G11); a cut in instructions bought about half as
  much in cycles throughout (G13).
- **Node counts are still the best check.** A change that should affect only cost must leave
  them identical, and one that should affect only ordering must leave the answers identical.
  They are how each row above was shown to be a pure trade.

---

# Open questions

- **Proof-size prediction.** Root and 12-card proofs could be about half as large (roughly 20%
  of all nodes) if the cheapest cutting lead were known. Nothing cheap has predicted it.
  Untested: probes that estimate proof size instead of result, and a model trained on the
  cheapest cutter instead of on DDS's choice.
- **Third- and fourth-hand rules.** The oracle gaps are real; no simple feature separates the
  cases.
- **The 4-card probe's memory misses.** The largest remaining memory cost.
- **NUMA.** On two-socket servers the endgame tables sit on one node; replicating them per
  node is unmeasured.
- **How far the general part carries.** Part I has been tested on one game. The claims most
  likely to transfer are G3 (never evict within a proof), G5 (order for proof size), G6 (rules
  where the first move nearly always cuts) and G11 (a probe must beat the nodes it saves).
