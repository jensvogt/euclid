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

WORKDIR /usr/local/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git .
RUN ./bootstrap-vcpkg.sh

WORKDIR /usr/src
ARG BUILD_VERSION
RUN git clone https://github.com/jensvogt/euclid.git
RUN --mount=type=cache,target=/vcpkg-cache \
    cd /usr/src/euclid && \
    mkdir build && \
    cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -Wno-dev && \
    cmake --build build

FROM node:24-alpine AS buildfrontend

RUN apk update && apk upgrade && apk add --no-cache git

# Build euclid-ui, if it exists yet - falls back to an empty storage so the runtime
# image still builds with no bundled frontend until that repo is created.
WORKDIR /usr/src
RUN if git ls-remote https://github.com/jensvogt/euclid-ui.git >/dev/null 2>&1; then \
      git clone https://github.com/jensvogt/euclid-ui.git && \
      cd euclid-ui && \
      npm install -g @angular/cli && \
      npm install --force && \
      npm run build --prod; \
    else \
      mkdir -p euclid-ui/dist/euclid-ui/browser; \
    fi

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
    mkdir -p /usr/local/euclid/frontend

# Install frontend
COPY --from=buildfrontend /usr/src/euclid-ui/dist/euclid-ui/browser/ /usr/local/euclid/frontend/

# Install euclid
COPY --from=build /usr/src/euclid/build/bin/euclid-mgr /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/bin/euclid-cli /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/bin/euclid-eam /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/bin/euclid-eqs /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/bin/euclid-emo /usr/local/euclid/bin/
COPY --from=build /usr/src/euclid/build/vcpkg_installed/x64-linux/share/libmagic/misc/magic.mgc /usr/local/euclid/etc/magic.mgc
COPY --from=build /usr/src/euclid/dist/docker/etc/ssh_host_key /usr/local/euclid/etc/ssh_host_key
COPY --from=build /usr/src/euclid/dist/docker/etc/euclid_cert.crt /usr/local/euclid/etc/euclid_cert.crt
COPY --from=build /usr/src/euclid/dist/docker/etc/euclid_cert.key /usr/local/euclid/etc/euclid_cert.key
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
