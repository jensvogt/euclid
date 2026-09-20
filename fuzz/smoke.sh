#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

#
# Runs every fuzz target briefly, over its seed corpus.
#
# This is not a fuzzing campaign - a campaign runs for hours and keeps what it finds. This runs
# long enough to catch a regression: an input that used to be handled and now crashes, and the
# seeds themselves, which are the cases somebody thought worth writing down. Short enough to sit
# in a pull request.
#
# Usage: smoke.sh [-b BUILD_DIR] [-r RUNS]
#
#   -b BUILD_DIR  the build whose fuzz/ directory holds the targets (default: cmake-build-debug)
#   -r RUNS       executions per target (default: 20000)
#

set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="cmake-build-debug"
RUNS=20000

while getopts "b:r:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        r) RUNS="$OPTARG" ;;
        h) sed -n '7,19p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
        *) exit 1 ;;
    esac
done

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

declare -A CORPUS=(
    [euclid-fuzz-signature-input]=signature-input
    [euclid-fuzz-sigv4]=sigv4
    [euclid-fuzz-query]=query
    [euclid-fuzz-upload-key]=upload-key
)

# libFuzzer writes every coverage-increasing input it finds into the *first* corpus directory it
# is given, and reads any further ones without writing to them. Pointed straight at fuzz/corpus
# that turns the seed corpus into an output directory: a few seconds of smoke leaves hundreds of
# hash-named files behind, on every machine that runs it, growing without limit and with nobody
# having chosen any of them. So the writable corpus is scratch under the build directory and the
# seeds are passed read-only - fuzz/corpus holds what somebody wrote down, and nothing else.
SCRATCH="$BUILD_DIR/fuzz-corpus"

failed=0
for target in "${!CORPUS[@]}"; do
    binary="$BUILD_DIR/fuzz/$target"
    [ -x "$binary" ] || { echo "  $target: not built - run fuzz/build.sh" >&2; failed=1; continue; }

    mkdir -p "$SCRATCH/${CORPUS[$target]}"

    printf '  %-28s ' "$target"
    if output=$("$binary" "$SCRATCH/${CORPUS[$target]}" "fuzz/corpus/${CORPUS[$target]}" -runs="$RUNS" -max_len=512 2>&1); then
        echo "ok ($RUNS runs)"
    else
        echo "FINDING"
        echo "$output" | tail -40
        failed=1
    fi
done

exit $failed
