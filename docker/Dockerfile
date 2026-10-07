# syntax=docker/dockerfile:1
FROM alpine:latest AS build

RUN apk add --no-cache build-base ninja git perl zip unzip curl tar pkgconfig linux-headers bash python3 autoconf \
     autoconf-archive automake libtool m4 make gettext-dev patch pkgconf ncurses-dev ncurses-terminfo-base doxygen pandoc \
     coreutils openssl-dev zlib-dev bzip2-dev readline-dev sqlite-dev xz-dev libffi-dev util-linux-dev musl-dev

# Alpine's stable apk repo lags vcpkg's CMake requirement; pull from edge instead,
# without touching the base image's repo list.
RUN apk add --no-cache --repository=https://dl-cdn.alpinelinux.org/alpine/edge/main cmake

ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_ROOT=/usr/local/vcpkg
ENV VCPKG_MAX_CONCURRENCY=4
ENV CMAKE_BUILD_PARALLEL_LEVEL=4
ENV ACLOCAL_PATH=/usr/share/aclocal
ENV VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache
RUN mkdir -p /usr/share/aclocal && mkdir -p /vcpkg-cache

# This clone has to contain the commit euclid's vcpkg.json names as its baseline: vcpkg resolves
# every port version by running `git show <baseline>:versions/baseline.json` inside it. Left on
# master and unpinned, the layer is cached from whenever it was first built and never refreshed -
# the instruction text does not change, so Docker never re-runs it - and the first baseline bump
# afterwards fails the build with "path 'versions/baseline.json' exists on disk, but not in
# <commit>". The clone was simply older than the manifest.
#
# Pinning it puts the commit in the layer's cache key, so moving the baseline rebuilds the clone.
# Keep this equal to "builtin-baseline" in vcpkg.json; the build stage below checks that it is and
# recovers if it is not.
ARG VCPKG_BASELINE=319504a5326aa870edde46438c5455fa76305a56
WORKDIR /usr/local/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git . && git checkout "$VCPKG_BASELINE"
RUN ./bootstrap-vcpkg.sh

WORKDIR /usr/src
ARG BUILD_VERSION
RUN git clone https://github.com/jensvogt/euclid.git
RUN --mount=type=cache,target=/vcpkg-cache \
    cd /usr/src/euclid && \
    BASELINE="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{40\}\)".*/\1/p' vcpkg.json)" && \
    if [ -n "$BASELINE" ] && ! git -C "$VCPKG_ROOT" cat-file -e "$BASELINE^{commit}" 2>/dev/null; then \
        echo "vcpkg clone predates the manifest baseline $BASELINE - fetching" && \
        git -C "$VCPKG_ROOT" fetch --quiet origin && \
        git -C "$VCPKG_ROOT" checkout --quiet "$BASELINE"; \
    fi && \
    mkdir build && \
    cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -Wno-dev && \
    cmake --build build && \
    rm -f build/bin/*-tests

# Debian rather than alpine, for a stage that is discarded anyway: this is the libc and the node
# that the release pipeline builds euclid-web on (actions/setup-node on ubuntu), so a frontend that
# builds for a release builds here. euclid-web's lockfile does carry the musl binaries that
# Angular's esbuild and rollup need, so alpine would very likely work too - this is about building
# on the toolchain the frontend is actually tested on, not about alpine being unable to.
#
# Nothing from this stage but the compiled bundle reaches the runtime image, so its base costs
# nothing in what ships.
FROM node:24 AS buildfrontend

# The frontend the gateway serves. It lives in its own repository - a separate Angular project,
# released on its own schedule - so the image fetches and compiles it rather than carrying a built
# copy that would go stale silently.
#
# euclid-web's default branch is master. Override to pin a tag or another branch; changing it is
# also what invalidates this layer, since Docker cannot see that the remote has moved.
ARG FRONTEND_VERSION=master
WORKDIR /usr/src
RUN git clone --depth 1 --branch "$FRONTEND_VERSION" https://github.com/jensvogt/euclid-web.git

# npm ci, not "npm install --force": the lockfile is what CI builds from, and --force resolved a
# fresh tree every build - so the image could ship a dependency set nobody had tested. The Angular
# CLI is a devDependency, so "npm run build" uses that pinned copy; installing one globally was a
# second version to disagree with it.
#
# --configuration docker, because euclid-web's angular.json sets defaultConfiguration to
# "development": a plain "ng build" leaves optimization off and source maps on, which is a bundle
# several times the size it needs to be and an environment that still says production: false. The
# "docker" configuration is the one that project keeps for being served from a container - it
# swaps in environment.docker.ts and turns on output hashing - so the image follows whatever it
# decides that should mean rather than pinning a guess here.
#
# What makes it work without the nginx that configuration was written around: euclidEndpoint is the
# same-origin path /euclid, and euclid's own gateway routes a request by its x-euclid-target header
# rather than by path (see detectEuclidService in GatewayServer.cpp). The gateway serving both the
# bundle and the API on one port is what the path was chosen to avoid a CORS preflight for.
WORKDIR /usr/src/euclid-web
RUN npm ci && npm run build -- --configuration docker

# Angular's "application" builder writes to <outputPath>/browser, and euclid-web's angular.json sets
# outputPath to "dist/euclid-web". Checked here rather than found in a browser: for as long as this
# image has existed it shipped an empty frontend directory, because the stage cloned a repository
# that does not exist (euclid-ui) and fell back to creating the empty output path it was looking
# for. A missing frontend and a broken one both answer 404, and the gateway's line saying which is
# logged at info - below the "warning" this image's configuration sets - so nothing said so.
RUN test -f dist/euclid-web/browser/index.html \
    || (echo "euclid-web produced no dist/euclid-web/browser/index.html" && exit 1)

# Stick to alpine:latest
FROM alpine:latest

RUN apk update && apk upgrade && apk add --no-cache libstdc++ libgcc unzip libarchive libmagic curl unbound

# Make directories
RUN mkdir -p /usr/local/euclid/data/tmp && \
    mkdir -p /usr/local/euclid/data/backup && \
    mkdir -p /usr/local/euclid/data/eam && \
    mkdir -p /usr/local/euclid/data/eqs && \
    mkdir -p /usr/local/euclid/data/esm && \
    mkdir -p /usr/local/euclid/data/ens && \
    mkdir -p /usr/local/euclid/bin && \
    mkdir -p /usr/local/euclid/etc && \
    mkdir -p /usr/local/euclid/init && \
    mkdir -p /usr/local/euclid/log && \
    mkdir -p /usr/local/euclid/frontend && \
    mkdir -p /var/run/euclid

# Install frontend. The bundle alone - not the checkout around it, not node_modules, not the
# sources - into the directory euclid.gateway.frontend.directory names.
COPY --from=buildfrontend /usr/src/euclid-web/dist/euclid-web/browser/ /usr/local/euclid/frontend/

# Install euclid.
#
# Everything the build produced, rather than a list that has to be extended by hand whenever a
# module is added - which is how euclid-ead came to be missing while euclid.json still pointed at
# it, leaving the manager restart-looping for the life of the container on a binary that was never
# installed. The same drift the macOS package hit, fixed the same way. The test executables land in
# this directory too and are removed in the build stage above, so they are gone before the layer
# that would otherwise carry them: deleting them here would leave them in the image.
#
# Includes euclid-mgr and euclid-cli, the document store euclid-emd - the image runs on it rather
# than on MongoDB, see euclid.database.backend in the bundled configuration, so nothing else here
# starts without it - and the two transfer executables ETS spawns.
COPY --from=build /usr/src/euclid/build/bin/ /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/vcpkg_installed/x64-linux/share/libmagic/misc/magic.mgc /usr/local/euclid/etc/magic.mgc
COPY --from=build /usr/src/euclid/dist/docker/etc/ssh_host_key /usr/local/euclid/etc/ssh_host_key
# No gateway certificate: one baked into the image would be every container's, key included. The
# manager generates one for the container on first start - mount /usr/local/euclid/etc to keep it
# across containers, and name the host's address in euclid.gateway.tls.alt-names so clients
# outside can check it.
COPY --from=build /usr/src/euclid/dist/docker/etc/euclid.json /usr/local/euclid/etc/euclid.json

# euclid-manager resolves euclid-eam/euclid-queues/euclid-emo via PATH
ENV PATH="/usr/local/euclid/bin:${PATH}"
RUN ln -s /usr/local/euclid/bin/euclid-cli /usr/bin/euclid-cli || true
RUN ln -s /usr/local/euclid/bin/euclid-mgr /usr/bin/euclid-mgr || true

# Gateway HTTP / frontend / websocket ports
EXPOSE 5566
EXPOSE 4567
EXPOSE 4569

CMD ["/usr/local/euclid/bin/euclid-mgr", "--config", "/usr/local/euclid/etc/euclid.json"]
