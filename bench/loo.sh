#!/bin/sh
# Leave-one-out study: the full solver with one design feature turned off at a time, timed
# against the full solver and against DDS on the same deals.
#
# usage: sh bench/loo.sh OUTDIR [SET]        (from the repository root; DDCLAIM_DIR set)
#        ONLY="name name" sh bench/loo.sh ...  only those variants
#
# Builds one variant per feature (in OUTDIR/b-NAME, with a -DSTATS twin for node counts) on
# CPUs 4-11, then measures on CPU 0: DDS's cycles per deal (bench_solve dds, best of 3), and
# for each variant its nodes, its answers against the set's, and its cycles per deal (best of
# REPS runs, alternating with the full build). Writes OUTDIR/results.tsv:
#   slowdown = variant cycles / full cycles
#   speedup  = DDS cycles / variant cycles (what is left of ddfast's lead over DDS)
#   share    = log(slowdown) / log(DDS / full): the feature's part of the speed-up
# Factors multiply, so shares of features measured one at a time need not add up to 1.
set -u
O=${1:?usage: bench/loo.sh OUTDIR [SET]}
SET=${2:-tests/data/public.set}
mkdir -p "$O"

# name|compile flags|environment|timing reps. The flags and environment turn the feature OFF.
VARIANTS='scalar_tt|-DABL_SCALAR_TT||3
scalar_pmatch|-DABL_SCALAR_PMATCH||3
scalar_pick|-DABL_SCALAR_PICK||3
scalar_order|-DOLD_ORDER||3
no_avx512|-DABL_SCALAR_TT -DABL_SCALAR_PMATCH -DABL_SCALAR_PICK -DOLD_ORDER -DOLD_QT -DOLD_COMM -mno-avx512f||3
soft_bits|-DABL_SOFT_BITS||3
per_suit|-DOLD_QT -DOLD_COMM||3
port_bounds|-DPORT_BOUNDS||3
no_huge_tt||LEAN_NOHUGE_TT=1|3
no_huge||LEAN_NOHUGE=1|3
no_prefetch|-DABL_NOPREFETCH||3
small_tt|-DPOOL_BITS=12||3
no_t3||LEAN_NOT3=1|3
no_p4||LEAN_NOP4=1|3
no_tables||LEAN_NOT3=1 LEAN_NOP4=1|3
no_tt|-DTT_MIN_N=14||1
no_dds_order|-DDORD=99||3
no_bounds|-DDMIN=99 -DDORD=4 -DDQ2=4||3
no_follow_rules|-DCOV2=0 -DCOVB=0 -DCOVPL=0 -DDISC=0 -DVOIDR=0||3
no_guess|-DABL_GUESS_MID||3
no_skip|-DABL_SW=7||3
no_runopt|-DRUNOPT=0||3'

[ -n "${LOO_VARIANTS:-}" ] && VARIANTS=$LOO_VARIANTS  # another study (bench/dds_study.sh)

# Builds. Variants with only an environment use the full build (bin, bin-stats).
[ -x bin/bench_solve ] && [ -x bin-stats/bench_solve ] || { echo "build bin and bin-stats first"; exit 1; }
echo "$VARIANTS" | while IFS='|' read -r n f e r; do
  case " ${ONLY:-$n} " in *" $n "*) ;; *) continue ;; esac
  [ -n "$f" ] || continue
  [ -x "$O/b-$n/bench_solve" ] || XFLAGS="$f" OUT="$O/b-$n" taskset -c 4-11 sh scripts/build.sh > "$O/build-$n.log" 2>&1 || echo "BUILD FAIL $n"
  [ -x "$O/b-$n-s/bench_solve" ] || XFLAGS="$f -DSTATS" OUT="$O/b-$n-s" taskset -c 4-11 sh scripts/build.sh > "$O/build-$n-s.log" 2>&1 || echo "BUILD FAIL $n-s"
done
echo "builds done $(date +%T)"

cycles() {  # dir env mode -> "cycles ok|WRONG"
  env $2 taskset -c 0 "$1/bench_solve" "$3" "$SET" 2>&1 |
    awk '/cycles\/deal/{c=$2} /deals/{ok=($0 ~ /all correct/)?"ok":"WRONG"} END{print c, ok}'
}
nodes() { env $2 taskset -c 0 "$1/bench_solve" set "$SET" 2>&1 | awk '/^queries/{print $4; exit}'; }
best() { m=$1; shift; for x in "$@"; do [ "$x" -lt "$m" ] && m=$x; done; echo "$m"; }

dds=999999999999
for i in 1 2 3; do set -- $(cycles bin "" dds); dds=$(best $dds $1); done
dnodes=$(taskset -c 0 bin/bench_solve dds "$SET" 2>&1 | awk '{for (i = 1; i < NF; i++) if ($i == "nodes") print $(i + 1)}')
full=999999999999
for i in 1 2 3; do set -- $(cycles bin "" set); full=$(best $full $1); done
fnodes=$(nodes bin-stats "")
printf "variant\tnodes\tcorrect\tfull_cycles\tvariant_cycles\tslowdown\tspeedup_vs_dds\tshare\n" > "$O/results.tsv"
printf "DDS\t%s\t-\t-\t%s\t-\t1.000\t-\n" "$dnodes" "$dds" >> "$O/results.tsv"
printf "full\t%s\tok\t%s\t%s\t1.000\t%s\t0.000\n" "$fnodes" "$full" "$full" "$(awk -v a=$dds -v b=$full 'BEGIN{printf "%.3f", a/b}')" >> "$O/results.tsv"
echo "DDS $dds, full $full cycles/deal $(date +%T)"

echo "$VARIANTS" | while IFS='|' read -r n f e r; do
  case " ${ONLY:-$n} " in *" $n "*) ;; *) continue ;; esac
  if [ -n "$f" ]; then b="$O/b-$n"; s="$O/b-$n-s"; else b=bin; s=bin-stats; fi
  [ -x "$b/bench_solve" ] || { printf "%s\tBUILD_FAIL\n" "$n" >> "$O/results.tsv"; continue; }
  nd=$(nodes "$s" "$e")
  bf=999999999999; bv=999999999999; ok=ok
  i=0
  while [ $i -lt "$r" ]; do
    set -- $(cycles bin "" set); bf=$(best $bf $1)
    set -- $(cycles "$b" "$e" set); bv=$(best $bv $1); [ "$2" = ok ] || ok=$2
    i=$((i + 1))
  done
  awk -v n="$n" -v nd="$nd" -v ok="$ok" -v bf="$bf" -v bv="$bv" -v d="$dds" 'BEGIN{
    printf "%s\t%s\t%s\t%d\t%d\t%.3f\t%.3f\t%.3f\n", n, nd, ok, bf, bv, bv / bf, d / bv, log(bv / bf) / log(d / bf)}' >> "$O/results.tsv"
  echo "$n done $(date +%T)"
done
echo ALLDONE
