# ddclaim (inside ddfast)

The endgame tables ddfast uses for the last 2, 3 and 4 cards of each hand, with their probes
and the code that builds them.

This directory was the separate repository
[randalburns/ddclaim](https://github.com/randalburns/ddclaim), brought in with `git subtree`
from its `main` at 2a4f416 (the history is kept), and then cut down to what ddfast uses:

- `include/ddclaim/tables.hpp`: hand-word helpers, the layout of the 2-card table, the 3-card
  table and the 4-card pattern table, and the inline probes ddfast's search calls.
- `include/ddclaim/port.hpp`: the AVX-512/BMI2 primitives and their NEON versions (Apple
  silicon), used by ddclaim and ddfast.
- `include/ddclaim.hpp`: the position type and thread pinning.
- `src/ddclaim.cpp`: the table data, loading and building. ddclaim's own search is kept only
  to build the tables: the 3-card table solves every 3-card ending, and the 4-card pattern
  table is filled by solving random 6-card endings (the search above 4 cards, with its own
  transposition table, drives that fill).

Removed: the standalone 1-7-card solver interface (`ddclaim::init`, `ddclaim::solve`), its
command, tests, tools and build script, and the 5- and 6-card pattern tables (they saved ddfast
6% of nodes and no time; see ../../docs/NOTES.md). The original README, with ddclaim's speed
against DDS on 1-7-card endings, and NOTES.md (kept here) describe the standalone solver.

Licensed under the Apache License 2.0 (see LICENSE).
