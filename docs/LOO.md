# Leave-one-out study: what ddfast adds to DDS's design

November 2026. ddfast at `main` 5661a1d plus the study switches (`ABL_*`, all off by default).

## Summary

On one core, ddfast solves the 600-deal public set **4.25× faster than DDS** (15.9M cycles per
deal against 67.5M). This study looks only at what ddfast does that DDS doesn't. Each addition
was turned off on its own, going back to DDS's way of doing that job, or to a plain table
without the improvement, and the solver was timed again. Every variant still gave the right
answer on all 600 deals.

Things DDS also has are not turned off: the transposition table itself, DDS's lead weights,
the quick-trick and later-trick bounds, second-hand quick tricks, and skipping cards below a
suit's lowest winning rank. How much the hand representation is worth as a whole is measured
separately, by running ddfast with DDS's search (see below).

- **The largest additions are the follow-hand rules (1.66× over DDS's follow weights),
  hardware pext/pdep (1.52×) and the two endgame tables together (1.30×).** All of the AVX-512
  code together is worth 1.29×.
- **The follow-hand rules search about as many nodes as DDS's follow weights** (DDS's weights
  search 3% fewer). Five cheap rules choose a follower's card about as well as DDS's weights,
  at a fraction of the cost.
- **Rewriting DDS's lead weights and bounds on the hand words** is worth 1.24× and 1.07×,
  compared with running DDS's code through the port.
- **The two endgame tables stand in for each other.** Either one alone saves 3-4%; both
  together save 30%.
- **The transposition-table improvements are small:** storing bound cutoffs (3%), copying
  4-card table hits (4-5%) and matching 8 entries at once (6%). Choosing the matching entry
  with the fewest relevant cards is worth nothing.
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
- **Nodes:** from a `-DSTATS` build of each variant, counting lead and follow nodes. The full
  build searches 82,915,440.
- **Correctness:** every timed run is checked against the set's DDS answers.
- **Columns:** slowdown is variant cycles ÷ full cycles. "Left of 4.25×" is what remains of
  ddfast's lead over DDS. Share is log(slowdown) ÷ log(4.25): the addition's part of the
  speed-up on a log scale. Additions interact, so the shares don't add up to 1.

## Results

Each row is an addition DDS doesn't have. "Back to" is what replaces it in the variant.

| Addition | Back to | Switch | Nodes | Slowdown | Left of 4.25× | Share |
|---|---|---|---:|---:|---:|---:|
| **Transposition table** | | | | | | |
| Bound cutoffs stored in the table | not stored, as in DDS | `ABL_NO_BOUNDSTORE` | +1.5% | 1.03× | 4.13× | 0.02 |
| 4-card table hits copied into the table | not copied | `ABL_NO_P4STORE` | −0.5% | 1.04× | 4.07× | 0.03 |
| Of several matching entries, the one with fewest relevant cards | first match | `TTPICK=0` | +3.2% | 1.00× | 4.27× | 0.00 |
| 8 entries matched at once (AVX-512) | scalar loop | `ABL_SCALAR_TT` | same | 1.06× | 4.02× | 0.04 |
| **Search** | | | | | | |
| Follow-hand rules (cover, discard, void ruff) | DDS's follow weights | `DFOL=2`, rules off | −3.4% | **1.66×** | 2.57× | 0.35 |
| Endgame tables, 3-card and 4-card | searched | `LEAN_NOT3=1 LEAN_NOP4=1` | +56% | 1.30× | 3.26× | 0.18 |
| ↳ 3-card table alone | searched | `LEAN_NOT3=1` | +7.3% | 1.04× | 4.07× | 0.03 |
| ↳ 4-card pattern table alone | searched | `LEAN_NOP4=1` | +7.5% | 1.03× | 4.12× | 0.02 |
| Fitted first guess | DDS's first target | `ABL_GUESS_DDS` | +1.2% | 1.02× | 4.18× | 0.01 |
| Extra weight for the table's best lead | DDS's best-move bonus only | `HX=0` | +1.2% | 1.02× | 4.18× | 0.01 |
| Lead memory at 12 cards | none | `NOMEM` | +0.3% | 1.00× | 4.24× | 0.00 |
| Run extension of relevant cards | none | `RUNOPT=0` | +3.5% | 1.00× | 4.25× | 0.00 |
| **Representation and instructions** | | | | | | |
| Hardware pext/pdep (BMI2) | byte tables, as on Arm | `ABL_SOFT_BITS` | same | **1.52×** | 2.80× | 0.29 |
| DDS's lead weights rewritten on hand words | DDS's code through the port | `PORT_ORDER` | same | 1.24× | 3.42× | 0.15 |
| DDS's bounds rewritten on hand words | DDS's code through the port | `PORT_BOUNDS` | same | 1.07× | 3.96× | 0.05 |
| Quick tricks on whole words | per-suit loops | `OLD_QT OLD_COMM` | same | 1.02× | 4.15× | 0.02 |
| **AVX-512** | | | | | | |
| All AVX-512 (scalar paths, `-mno-avx512f`) | scalar | five switches | same | 1.29× | 3.30× | 0.18 |
| ↳ Lead weights for all cards at once | scalar | `OLD_ORDER` | same | 1.09× | 3.91× | 0.06 |
| ↳ Move pick: highest of 16 keys at once | scalar | `ABL_SCALAR_PICK` | same | 1.08× | 3.95× | 0.05 |
| ↳ Per-deal table: 8 entries at once | scalar | `ABL_SCALAR_TT` | same | 1.06× | 4.02× | 0.04 |
| ↳ 4-card pattern table: 8 entries at once | scalar | `ABL_SCALAR_PMATCH` | same | 1.01× | 4.21× | 0.01 |
| **Memory** | | | | | | |
| Huge pages everywhere | 4 KB pages | `LEAN_NOHUGE=1` | same | 1.01× | 4.22× | 0.01 |
| Huge pages for the per-deal table | 4 KB pages | `LEAN_NOHUGE_TT=1` | same | 1.01× | 4.23× | 0.00 |
| Software prefetch | none | `ABL_NOPREFETCH` | same | 1.00× | 4.24× | 0.00 |

"Same" means exactly 82,915,440 nodes: the addition changes how fast each node is handled,
not what is searched.

## Findings

**The follow-hand rules are ddfast's biggest search addition.** DDS gives every card of the
second, third and fourth hands a weight and sorts by it. ddfast picks each follower's first card
with five cheap rules instead: cover an honour, play low second hand, discard from length, ruff
when void, and play above third hand when partner is long. With DDS's weights the search is
slightly smaller (3% fewer nodes), but every follow node costs much more, so the whole solver is
1.66× slower.

**pext/pdep is what makes one word per hand pay off.** Software pext/pdep, using the byte tables
of the Arm port, costs 52% with exactly the same search. Rewriting DDS's lead weights and bounds
on the hand words (1.24× and 1.07× over DDS's code through the port) depends on it too.

**AVX-512 helps, but less than pext/pdep.** The four kernels measured one at a time come to
about 24%. Removing AVX-512 entirely costs 29%; the difference is most likely the AVX-512 code
the compiler generates on its own.

**The tables are worth keeping, but only as a pair.** Either table alone is cheap to lose,
because the other one, the per-deal table and the bounds catch most of the same endings. With
both gone, nodes rise 56% and time 30%.

**ddfast's table improvements over a plain table are modest.** Storing bound cutoffs and copying
4-card table hits each save 3-5% of time. Matching 8 entries at once saves 6%. Choosing the entry
with the fewest relevant cards saves nodes but not time, and could be dropped. The table's size
matters more than any of these: in an earlier run, a table too small to hold one deal (256 KB,
resetting) searched four times the nodes and was 3.8× slower. DDS's table differs in structure
as well, which this study doesn't measure.

**Small additions near the noise:** the fitted first guess, the extra weight for the table's
best lead, lead memory and the run extension are each worth 2% or less. The run extension saves
3.5% of nodes but no time.

**Memory latency is not the bottleneck.** Huge pages and prefetch are each worth under 1%. A
deal's working set is small enough for the caches.

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

- **Noise:** about ±1% from run to run. The per-suit quick-tricks row has come out at 1.024×,
  1.068× and 1.024× in three runs, so treat it as 2-7%.
- **The leave-one-out table doesn't measure the representation as a whole.** It changes the
  instructions ddfast uses on its hand words, not the hand words themselves. For the whole
  representation, see the comparison with DDS's search above.
- **Trump placement** (trumps always in lane 3) has no switch of its own. It's part of the
  representation; see the comparison with DDS's search.
- **Not covered:** parallel solving (`solve_parallel`, `solve_deal_parallel`) and interleaved
  searches (`NCTX`, already off in ddfast).
- Earlier runs (normal power mode, and an earlier list that also turned off features DDS has)
  gave the same factors within 1-2% for the rows they share. ddfast measured 4.21-4.34× DDS
  across runs.

## Reproducing

```sh
export DDCLAIM_DIR=/path/to/ddclaim/tables
sh scripts/build.sh && XFLAGS=-DSTATS OUT=bin-stats sh scripts/build.sh
sh bench/loo.sh OUTDIR                    # about an hour: two builds per variant, then timing
ONLY="no_guess no_skip" sh bench/loo.sh OUTDIR   # rerun a few variants
```

Results are written to `OUTDIR/results.tsv`. The variants and their switches are listed at the
top of `bench/loo.sh`.
