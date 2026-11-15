#!/usr/bin/env bash
# Builds protocol-printer fully static in an Alpine (musl) container: one binary with no
# dependencies - OpenSSL included - which runs on the Alpine workers of examples/docker and on any
# other x86-64 Linux alike. Nothing is installed on this machine; only docker is.
#
#   OUT_DIR      where the binary goes (default: ../../docker/apps, next to file-delivery)
#   ALPINE       the Alpine image (default: alpine:3.24 - the workers' release)
set -euo pipefail

export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

here="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${OUT_DIR:-$here/../../docker/apps}"
ALPINE="${ALPINE:-alpine:3.24}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

echo "Building protocol-printer in $ALPINE into $OUT_DIR"

# The source mounted read-only and built under /tmp in the container, so no target/ directory is
# left in the checkout. The result is handed back owned by whoever ran this, not by root.
docker run --rm \
    -v "$here:/src:ro" \
    -v "$OUT_DIR:/out" \
    -e OWNER="$(id -u):$(id -g)" \
    "$ALPINE" sh -ec '
        # Alpine'"'"'s own Rust targets musl. OpenSSL is linked in statically (OPENSSL_STATIC, from
        # openssl-libs-static) and so is the C library (crt-static): the binary loads nothing.
        apk add --no-cache rust cargo musl-dev pkgconf openssl-dev openssl-libs-static >/dev/null

        cp -r /src /tmp/build
        rm -rf /tmp/build/target
        cd /tmp/build
        # The target named explicitly, so RUSTFLAGS applies to it alone: with host and target the
        # same, crt-static would reach the proc-macros as well, which cannot be static.
        target=$(rustc -vV | sed -n "s/^host: //p")
        OPENSSL_STATIC=1 RUSTFLAGS="-C target-feature=+crt-static" cargo build --release --target "$target"

        cp "target/$target/release/protocol-printer" /out/protocol-printer
        chown "$OWNER" /out/protocol-printer
    '

echo "Done: $OUT_DIR/protocol-printer"
