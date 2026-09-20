#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

#
# Builds the fuzz targets with clang and libFuzzer.
#
# Not part of the CMake build, and not for want of trying: libFuzzer is a clang runtime, euclid is
# built with GCC 14, and configuring the project with clang makes vcpkg rebuild every dependency
# under the new compiler - which takes an hour and fails on boost-cobalt. So this compiles the few
# euclid sources each target needs directly, and reuses the headers an ordinary build already
# installed. Nothing of euclid's is taken from the GCC build: every file under test is compiled
# here, by clang, instrumented.
#
# Usage: build.sh [-b BUILD_DIR] [-o OUT_DIR]
#
#   -b BUILD_DIR  an existing euclid build, for its vcpkg headers (default: cmake-build-debug)
#   -o OUT_DIR    where the fuzzers are written (default: <build dir>/fuzz)
#
# Then:
#   ./cmake-build-debug/fuzz/euclid-fuzz-upload-key fuzz/corpus/upload-key -runs=1000000
#
# Requires: clang++ with libFuzzer, and one ordinary build already configured.

set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="cmake-build-debug"
OUT_DIR=""

while getopts "b:o:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        o) OUT_DIR="$OPTARG" ;;
        h) sed -n '7,24p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
        *) exit 1 ;;
    esac
done

OUT_DIR="${OUT_DIR:-$BUILD_DIR/fuzz}"

command -v clang++ >/dev/null 2>&1 || { echo "error: clang++ not found" >&2; exit 1; }

VCPKG="$BUILD_DIR/vcpkg_installed/x64-linux"
[ -d "$VCPKG/include" ] || {
    echo "error: no vcpkg headers under $VCPKG - configure an ordinary build first, or pass -b" >&2
    exit 1
}

mkdir -p "$OUT_DIR"

# AddressSanitizer alongside the fuzzer, always. A fuzzer on its own finds the bugs that crash;
# ASan is what turns "read one byte past the end" into a finding rather than a value that happened
# to be there. UBSan catches the signed overflow that produced a plausible-looking size.
FLAGS=(-std=c++23 -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
INCLUDE=(-I core/include -I "$VCPKG/include")

# The mongo driver installs its headers under a version directory, so the path a source writes is
# not the path they are at.
BSON=(-I "$VCPKG/include/bsoncxx/v_noabi" -I "$VCPKG/include/mongocxx/v_noabi")

build() {
    local name="$1"; shift
    printf '  %-28s' "$name"
    if clang++ "${FLAGS[@]}" "${INCLUDE[@]}" "$@" -o "$OUT_DIR/$name" 2>"$OUT_DIR/$name.log"; then
        echo "ok"
    else
        echo "FAILED (see $OUT_DIR/$name.log)"
        return 1
    fi
}

echo "Building fuzz targets into $OUT_DIR"

build euclid-fuzz-signature-input fuzz/FuzzSignatureInput.cpp \
      core/src/HttpSignature.cpp core/src/CryptoUtils.cpp -lssl -lcrypto

build euclid-fuzz-sigv4 fuzz/FuzzSigV4Authorization.cpp \
      core/src/SigV4.cpp core/src/CryptoUtils.cpp -lssl -lcrypto

build euclid-fuzz-query fuzz/FuzzQueryParameters.cpp core/src/HttpUtils.cpp

build euclid-fuzz-upload-key fuzz/FuzzUploadKey.cpp modules/eag/src/UploadTarget.cpp \
      -I modules/eag/include -I database/include -I dto/include "${BSON[@]}"

echo
echo "Run one for a while:"
echo "  $OUT_DIR/euclid-fuzz-upload-key fuzz/corpus/upload-key -runs=1000000"
echo
echo "Or all of them briefly, as CI does:"
echo "  fuzz/smoke.sh -b $BUILD_DIR"
