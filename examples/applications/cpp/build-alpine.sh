#!/usr/bin/env bash
# Builds file-delivery fully static in an Alpine (musl) container: one binary with no dependencies,
# which runs on the Alpine workers of examples/docker and on any other x86-64 Linux alike.
#
# euclid-cdk is built from source inside the container - there is no Alpine package of it - so a
# checkout of it is needed. Nothing is installed on this machine; only docker is.
#
#   CDK_SRC      the euclid-cdk checkout (default: ../../../../euclid-cdk, next to this repository)
#   OUT_DIR      where the binary goes (default: ../../docker/apps, where setup-infrastructure.sh
#                picks it up)
#   ALPINE       the Alpine image (default: alpine:3.24 - the workers' release)
set -euo pipefail

export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

here="$(cd "$(dirname "$0")" && pwd)"
CDK_SRC="$(cd "${CDK_SRC:-$here/../../../../euclid-cdk}" && pwd)"
OUT_DIR="${OUT_DIR:-$here/../../docker/apps}"
ALPINE="${ALPINE:-alpine:3.24}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

echo "Building file-delivery in $ALPINE, euclid-cdk from $CDK_SRC, into $OUT_DIR"

# Both sources mounted read-only and built under /tmp in the container, so no build directory is
# left in either checkout. The result is handed back owned by whoever ran this, not by root.
docker run --rm \
    -v "$CDK_SRC:/src/euclid-cdk:ro" \
    -v "$here:/src/app:ro" \
    -v "$OUT_DIR:/out" \
    -e OWNER="$(id -u):$(id -g)" \
    "$ALPINE" sh -ec '
        # The compiler and, for every library, its static archive: libarchive-static needs the
        # libraries libarchive itself is built against, and CMakeLists.txt looks each one up.
        apk add --no-cache build-base cmake ninja pkgconf \
            boost-dev boost-static openssl-dev openssl-libs-static \
            libarchive-dev libarchive-static zlib-static zstd-static xz-static bzip2-static \
            lz4-static acl-static libxml2-static nettle-static expat-static \
            file-dev libmagic-static >/dev/null

        echo "== euclid-cdk"
        cmake -S /src/euclid-cdk -B /tmp/cdk -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DEUCLID_CDK_BUILD_SHARED=OFF -DEUCLID_CDK_BUILD_TESTS=OFF -DEUCLID_CDK_BUILD_EXAMPLES=OFF \
            -DEUCLID_CDK_INSTALL=ON >/dev/null
        cmake --build /tmp/cdk
        cmake --install /tmp/cdk --prefix /usr/local >/dev/null

        echo "== file-delivery"
        cmake -S /src/app -B /tmp/app -G Ninja -DCMAKE_BUILD_TYPE=Release -DFILE_DELIVERY_FULLY_STATIC=ON >/dev/null
        cmake --build /tmp/app

        strip -o /out/file-delivery /tmp/app/file-delivery
        chown "$OWNER" /out/file-delivery
    '

echo "Done: $OUT_DIR/file-delivery"
