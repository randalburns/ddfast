#!/bin/sh
# ddfast running DDS's search, against DDS: what is left is mostly the hand representation
# (one word per hand, trumps in a fixed lane). See docs/LOO.md.
# usage: sh bench/dds_study.sh OUTDIR [SET]       (from the repository root; DDCLAIM_DIR set)
#
# DDS's search in ddfast: DDS's first target (7 for the leader's side, 6 when E-W lead), DDS's
# bounds and second-hand quick tricks at every level, DDS's lead and follow weights, no follow
# rules, the relevant-set skip kept (DDS has it: MoveGen::MakeNext skips cards below the lowest
# winning rank of a suit once a card of that suit has failed), no run extension, no lead memory, no extra weight for the
# table's best card, no endgame tables (2, 3 and 4 cards searched), scalar table matching and
# move pick (no 8-wide lookups), no prefetch, no huge pages; one thread. The transposition
# table stays ddfast's (it cannot be made DDS's), so node counts differ somewhat.
S="-DABL_GUESS_DDS -DDMIN=2 -DDFOL=2 -DHX=0 -DCOV2=0 -DCOVB=0 -DCOVPL=0 -DDISC=0 -DVOIDR=0 -DRUNOPT=0 -DNOMEM -DABL_SCALAR_TT -DABL_SCALAR_PICK -DOLD_ORDER -DABL_NOPREFETCH"
E="LEAN_NOT2=1 LEAN_NOT3=1 LEAN_NOP4=1 LEAN_NOHUGE=1"
LOO_VARIANTS="dds_search|$S|$E|3
dds_search_soft_bits|$S -DABL_SOFT_BITS|$E|3
dds_search_port_code|$S -DPORT_BOUNDS -DPORT_ORDER|$E|3" exec sh "$(dirname "$0")/loo.sh" "$@"
