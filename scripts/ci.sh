#!/bin/sh
# Continuous integration (.github/workflows/ci.yml): builds ddfast, then runs the exactness
# tests against DDS. The ddclaim tables are built on first use into $DDCLAIM_DIR with a short
# fill (a sparser 4-card table only means more search, not different answers); CI caches them.
# Environment:
#   ARCH="..."      target flags (scripts/build.sh); default the runner's own CPU
#   RUN="..."       a prefix for the test binaries, e.g. "sde64 -icx --" to emulate AVX-512
#   TESTS="..."     test_ddfast's arguments (default: quick)
#   PLAY="..."      test_play's arguments (default: 100 deals, seed 1)
set -eu
cd "$(dirname "$0")/.."
export DDCLAIM_DIR="${DDCLAIM_DIR:-$PWD/build/ddclaim}"
export DDCLAIM_SECONDS="${DDCLAIM_SECONDS:-20}" DDCLAIM_MB="${DDCLAIM_MB:-1500}"
mkdir -p "$DDCLAIM_DIR"
sh scripts/build.sh
RUN=${RUN:-}
# shellcheck disable=SC2086
$RUN bin/test_ddfast ${TESTS:-quick}
# shellcheck disable=SC2086
$RUN bin/test_play ${PLAY:-100 1}
