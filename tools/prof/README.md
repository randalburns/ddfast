# Profiling ddfast without perf

`perf` is not always installed, but the kernel's hardware counters are still available to an
unprivileged process for its own user-space code (`kernel.perf_event_paranoid` 2 or lower).
These tools use them directly. Linux x86-64 only.

| File | What it does |
|---|---|
| `bprof.cpp` | Solves a test set on one thread and samples the instruction pointer every N branch misses (or cycles). Prints exact per-deal branch misses, branches and cycles, plus an address histogram. |
| `sym.py` | Maps that histogram to source lines and to functions (inclusive of inlining). |
| `build.sh` | Links `bprof` against an existing build directory. |
| `ab.sh` | Alternating A/B benchmark of two build directories on one core, with branch-miss counts. |

## Where do the branch misses (or cycles) go?
```sh
export DDCLAIM_DIR=/path/to/ddclaim/tables
XFLAGS=-g OUT=bin-prof sh scripts/build.sh        # -g only adds line tables
sh tools/prof/build.sh bin-prof
taskset -c 0 bin-prof/bprof tests/data/public.set bm > bm.txt     # or: cyc
python3 tools/prof/sym.py bin-prof/bprof bm.txt 40
```
The default period (2003 events per sample) gives tens of thousands of samples on the public
set. Lines are attributed to the instruction *after* a mispredicted branch, so check
neighbouring lines in the disassembly (`objdump -d --no-show-raw-insn bin-prof/bprof`) when a
line looks unlikely.

## Is change B faster than A?
```sh
OUT=bin-base sh scripts/build.sh && sh tools/prof/build.sh bin-base   # on the old code
OUT=bin-new  sh scripts/build.sh && sh tools/prof/build.sh bin-new    # on the new code
CPU=0 sh tools/prof/ab.sh bin-base bin-new 9
```
The clock on a laptop moves a lot under load, so cycle medians from alternating runs are the
figure to trust, and differences of 1-3% need many pairs. Branch-miss counts from
`bprof ... none` are nearly deterministic, so they screen a change before the timing runs.
