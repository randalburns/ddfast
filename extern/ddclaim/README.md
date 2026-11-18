# Endgame tables

The tables ddfast uses for the last 2, 3 and 4 cards of each hand, with their probes and the
code that builds them.

- `include/ddclaim/tables.hpp`: hand-word helpers, the layout of the 2-card table, the 3-card
  table and the 4-card pattern table, and the inline probes the search calls.
- `include/ddclaim/port.hpp`: the bit and vector primitives used throughout ddfast: AVX-512
  and BMI2 on x86-64, Google Highway on Apple silicon.
- `include/ddclaim.hpp`: the position type and thread pinning.
- `src/ddclaim.cpp`: the table data, loading and building. The 2-card table is built in
  memory at load time; the 3-card table solves every 3-card ending; the 4-card pattern table
  is filled by solving random 6-card endings for a fixed time.

The tables are described in the [endgame tables explainer](https://randalburns.github.io/ddfast/endgame-tables.html).
`NOTES.md` here holds the original design notes.

Licensed under the Apache License 2.0 (see LICENSE).
