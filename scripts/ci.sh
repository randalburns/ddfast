#!/bin/sh
# Continuous integration (.github/workflows/ci.yml): builds ddfast, then runs the exactness
# tests against DDS. The endgame tables are built on first use into $DDFAST_TABLES with a short
# fill (a sparser 4-card table only means more search, not different answers); CI caches them.
# Environment:
#   ARCH="..."      target flags (scripts/build.sh); default the runner's own CPU
#   RUN="..."       a prefix for the test binaries, e.g. "sde64 -icx --" to emulate AVX-512
#   TESTS="..."     test_ddfast's arguments (default: quick)
#   PLAY="..."      test_play's arguments (default: 100 deals, seed 1)
set -eu
cd "$(dirname "$0")/.."
export DDFAST_TABLES="${DDFAST_TABLES:-$PWD/build/tables}"
export DDFAST_TABLE_SECONDS="${DDFAST_TABLE_SECONDS:-20}" DDFAST_TABLE_MB="${DDFAST_TABLE_MB:-1500}"
mkdir -p "$DDFAST_TABLES"
sh scripts/build.sh
RUN=${RUN:-}
# shellcheck disable=SC2086
$RUN bin/test_ddfast ${TESTS:-quick}
# shellcheck disable=SC2086
$RUN bin/test_play ${PLAY:-100 1}
