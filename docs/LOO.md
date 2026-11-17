# Leave-one-out study: what each part of ddfast's design is worth

November 2026. ddfast at `main` 5661a1d plus the study switches (`ABL_*`, all off by default).

## Summary

On one core, ddfast solves the 600-deal public set **4.29× faster than DDS** (15.9M cycles per
deal against 68.2M). Each feature below was turned off on its own while everything else stayed
on, and the solver was timed again. Every variant still gave the right answer on all 600 deals.

- **The search design is most of the win.** Three features stand out: the per-deal
  transposition table of card patterns, keeping the whole deal in it, and DDS's lead weights.
  Without the table at all, ddfast is 16× *slower* than DDS. Next come skipping cards outside
  the relevant set (1.39×), the bounds (1.20×) and the follow-hand rules (1.16×).
- **Hardware pext/pdep is the most valuable low-level feature (1.52×)**, more than all the
  AVX-512 code together (1.29×).
- **The two endgame tables stand in for each other.** Each one alone saves 3-4%; both together
  save 30%.
- **Memory tuning hardly matters.** Huge pages and software prefetch are each worth under 1%.

## Method

- **Machine:** AMD Ryzen AI 9 HX 370 (Zen 5), performance power mode. Measured on CPU 0 (a
  fast core), with builds on CPUs 4-11.
- **Deals:** `tests/data/public.set`, 600 deals; one `solve()` per deal for the declarer's
  tricks.
- **Cycles:** user-mode hardware cycles per deal. Each variant was run three times,
  alternating with the full build, and the best run of each was kept. The full build varied by
  about ±1% across the study, so differences under about 2% are within noise.
- **DDS:** `SolveBoard` (target -1, one solution, one thread) on the same deals, timed the same
  way (`bench_solve dds`).
- **Nodes:** from a `-DSTATS` build of each variant. The full build searches 82,915,440 nodes.
- **Correctness:** every timed run is checked against the set's DDS answers.
- **Columns:** slowdown is variant cycles ÷ full cycles. "Left of 4.29×" is what remains of
  ddfast's lead over DDS. Share is log(slowdown) ÷ log(4.29): the feature's part of the
  speed-up on a log scale. Features interact, so the shares don't add up to 1.

## Results

| Feature turned off | How | Nodes | Slowdown | Left of 4.29× | Share |
|---|---|---:|---:|---:|---:|
| **Search** | | | | | |
| Per-deal transposition table | `TT_MIN_N=14` | ×99.6 | ×69.7 | 0.06× | 2.91 |
| Table holds the whole deal (256 KB pool, resets) | `POOL_BITS=12` | ×3.96 | ×3.76 | 1.14× | 0.91 |
| DDS's lead weights | `DORD=99` | ×1.94 | 1.96× | 2.19× | 0.46 |
| Skip cards outside the relevant set | `ABL_SW=7` | ×1.75 | 1.39× | 3.11× | 0.23 |
| Bounds at lead nodes (quick and later tricks) | `DMIN=99 DORD=4 DQ2=4` | ×1.27 | 1.20× | 3.60× | 0.12 |
| Follow-hand rules (five rules together) | `COV2=0 COVB=0 COVPL=0 DISC=0 VOIDR=0` | ×1.16 | 1.16× | 3.71× | 0.10 |
| Fitted first guess (middle target instead) | `ABL_GUESS_MID` | ×1.02 | 1.03× | 4.25× | 0.02 |
| Run extension of relevant cards | `RUNOPT=0` | ×1.04 | 1.01× | 4.28× | 0.01 |
| **Representation** | | | | | |
| Hardware pext/pdep (BMI2) | `ABL_SOFT_BITS` (byte tables, as on Arm) | same | 1.52× | 2.83× | 0.29 |
| Whole-word quick tricks (per-suit loops instead) | `OLD_QT OLD_COMM` | same | 1.07× | 4.02× | 0.05 |
| **Vector computing (AVX-512)** | | | | | |
| All AVX-512 (scalar paths, `-mno-avx512f`) | five switches below | same | 1.29× | 3.34× | 0.17 |
| ↳ DDS's lead weights for all cards at once | `OLD_ORDER` | same | 1.09× | 3.95× | 0.06 |
| ↳ Move pick: highest of 16 keys at once | `ABL_SCALAR_PICK` | same | 1.08× | 4.00× | 0.05 |
| ↳ Per-deal table: 8 entries matched at once | `ABL_SCALAR_TT` | same | 1.06× | 4.06× | 0.04 |
| ↳ 4-card pattern table: 8 entries at once | `ABL_SCALAR_PMATCH` | same | 1.01× | 4.26× | 0.01 |
| **DDS's code rewritten on hand words** | | | | | |
| Bounds (DDS's code, ported, instead) | `PORT_BOUNDS` | same | 1.08× | 4.00× | 0.05 |
| **Endgame tables (ddclaim)** | | | | | |
| 3-card and 4-card tables both | `LEAN_NOT3=1 LEAN_NOP4=1` | ×1.56 | 1.30× | 3.31× | 0.18 |
| ↳ 3-card table alone | `LEAN_NOT3=1` | ×1.07 | 1.04× | 4.12× | 0.03 |
| ↳ 4-card pattern table alone | `LEAN_NOP4=1` | ×1.07 | 1.03× | 4.16× | 0.02 |
| **Memory** | | | | | |
| Huge pages for the per-deal table | `LEAN_NOHUGE_TT=1` | same | 1.01× | 4.26× | 0.01 |
| Huge pages everywhere | `LEAN_NOHUGE=1` | same | 1.01× | 4.27× | 0.01 |
| Software prefetch | `ABL_NOPREFETCH` | same | 1.00× | 4.29× | 0.00 |

"Same" means exactly 82,915,440 nodes: the feature changes how fast each node is handled,
not what is searched.

## Findings

**The per-deal table carries the design.** Without it the search visits 100 times as many
nodes. Keeping a whole deal in it matters almost as much. With a 256 KB pool that has to
reset, nodes rise fourfold and only 1.14× of the lead over DDS is left. The table's size and
its never-evict policy are as important as having it at all.

**DDS's lead weights are essential, and ddfast's own rules take the place of DDS's follow
weights.** Without DDS's weights at lead nodes, nodes almost double. At follow nodes ddfast
uses no DDS ordering at all. Five cheap rules (second-hand cover, discards, ruffing when void)
do that job, and they are worth 16%.

**Skipping cards outside the relevant set is worth more than any single low-level
optimisation except pext/pdep (1.39×).** When a card fails and was not among the cards that
decided the result, its equals in the same suit can't do better, so they are skipped.

**pext/pdep is what makes one word per hand pay off.** Software pext/pdep, using the byte
tables of the Arm port, costs 52% with exactly the same search. The four AVX-512 kernels
measured one at a time come to about 24%. Removing AVX-512 entirely costs 29%; the difference
is most likely the AVX-512 code the compiler generates on its own.

**The tables are worth keeping, but only as a pair.** Either table alone is cheap to lose,
because the other one, the per-deal table and the bounds catch most of the same endings. With
both gone, nodes rise 56% and time 30%.

**Memory latency is not the bottleneck.** Huge pages and prefetch are each worth under 1%. A
deal's working set is small enough for the caches.

**Some features save nodes but not time.** The run extension saves 3.5% of nodes but only
about 1% of cycles. The second-hand quick tricks and the fitted guess are similar.

## ddfast running DDS's search, against DDS

To separate the representation from the search, ddfast was set up to search the way DDS does
(`bench/dds_study.sh`):
- DDS's first target: 7 tricks for the leader's side, or 6 when East or West leads, then one
  trick at a time;
- DDS's bounds and second-hand quick tricks at every level;
- DDS's lead and follow weights;
- DDS's relevant-card skip (DDS's `MoveGen::MakeNext` skips cards below a suit's lowest winning
  rank);
- no endgame tables (2, 3 and 4 cards are searched);
- none of ddfast's additions: follow rules, run extension, lead memory, extra weight for the
  table's best card;
- scalar table matching and move pick (no 8-wide lookups), no prefetch, no huge pages, one
  thread.

The transposition table remains ddfast's, because it can't be made DDS's. DDS's `nodes` counts
completed tricks, so it is compared with ddfast's lead nodes.

| | Trick-start nodes | Cycles per deal | Cycles per trick node | Speed vs DDS |
|---|---:|---:|---:|---:|
| DDS | 39.5M | 67.5M | 1,025 | 1.00× |
| ddfast, DDS's search | 41.6M (+5%) | 51.2M | 738 | **1.32×** (1.39× per node) |
| ↳ with software pext/pdep | 41.6M | 72.1M | 1,039 | 0.94× (0.99× per node) |
| ↳ with DDS's ported code for bounds and lead weights | 41.6M | 55.6M | 802 | 1.21× (1.28× per node) |
| ddfast, full | 27.1M | 15.8M | n/a | 4.27× |

All answers were correct. With the searches within 5% of each other:

- **The hand representation is worth about 1.3-1.4× on its own.** That is one word per hand,
  with trumps in a fixed lane, and DDS's bounds and weights rewritten on those words. It's
  about a fifth of ddfast's 4.27× on a log scale. **The other 3.2× comes from the search:**
  the endgame tables, the follow rules, the fitted guess, the table and the rest of the
  search features above.
- **Without BMI2 the representation's advantage disappears:** with software pext/pdep, cost
  per node matches DDS's.
- **Rewriting DDS's bounds and lead weights on the hand words** (rather than running DDS's
  code through the port) is worth about 9% of this.
- An earlier run that also turned off the relevant-card skip searched 2.3× DDS's nodes. That
  run measured the missing skip, not the representation.

The fixed trump lane is counted as part of the representation, deliberately. A design that
puts a whole hand in one word puts trumps in a fixed lane: that's what gives it constant trump
masks and the tables their canonical layout. A variant with trumps left in their own suits
would be invasive and is not a design anyone would build, so it wasn't measured.

Still mixed in with the representation: the differences between the two transposition tables,
and ddfast's compile-time specialisation for no-trumps against trumps (DDS also has separate
trump and no-trump weight functions).

## Caveats

- **Noise:** about ±1% from run to run. The per-suit quick-tricks row came out at 1.024× in an
  earlier run in normal power mode and 1.068× here, so its true value lies somewhere in that
  range.
- **The leave-one-out table doesn't measure the representation as a whole.** It changes the
  instructions ddfast uses on its hand words, not the hand words themselves. For the whole
  representation, see the comparison with DDS's search above.
- **Trump placement** (trumps always in lane 3) has no switch of its own. It's part of the
  representation; see the comparison with DDS's search.
- **Not covered:** parallel solving (`solve_parallel`, `solve_deal_parallel`) and interleaved
  searches (`NCTX`, already off in ddfast).
- An earlier run in normal power mode gave the same order of features, with factors within
  1-2%. In that run ddfast was 4.21× faster than DDS.

## Reproducing

```sh
export DDCLAIM_DIR=/path/to/ddclaim/tables
sh scripts/build.sh && XFLAGS=-DSTATS OUT=bin-stats sh scripts/build.sh
sh bench/loo.sh OUTDIR                    # about an hour: two builds per variant, then timing
ONLY="no_guess no_skip" sh bench/loo.sh OUTDIR   # rerun a few variants
```

Results are written to `OUTDIR/results.tsv`. The variants and their switches are listed at the
top of `bench/loo.sh`.
