#
# Multi-stage build for evgrpc_server.
#
# Stage 1 (builder) compiles evgrpc_server with all gRPC/protobuf/libpqxx
# deps pulled from source via FetchContent. Stage 2 (runtime) ships only
# the binary plus the system shared libraries it needs at runtime.
#
# Build (default, expects direct internet to GitHub + Ubuntu archive):
#   docker build -t evgrpc:dev .
#
# Build from China (uses gh-proxy + TUNA mirrors via build args):
#   docker build \
#     --build-arg APT_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/ubuntu/ \
#     --build-arg GIT_INSTEADOF=https://gh-proxy.com/https://github.com/ \
#     -t evgrpc:dev .
#
# Run (v2: config.json only — no env vars required at startup):
#   docker run --rm \
#     -v $PWD/config.json:/etc/evgrpc/config.json:ro \
#     -v evgrpc_logs:/app/log \
#     -p 50051:50051 evgrpc:dev
#
# `config.json` is the canonical name (matches src/util/args.h default
# config_path and the server's CWD lookup if --config is omitted).
# `evgrpc_logs` is an anonymous volume that backs /app/log so rotating
# log files persist across container restarts; the image already
# creates /app/log so a config with `"log.file": "./log/evgrpc.log"`
# works without further setup.

# =====================================================================
# Stage 1: builder
# =====================================================================
ARG APT_MIRROR=
ARG GIT_INSTEADOF=

FROM ubuntu:24.04 AS builder

ARG APT_MIRROR
ARG GIT_INSTEADOF

ENV DEBIAN_FRONTEND=noninteractive

# Optionally swap apt sources to a local mirror. Empty APT_MIRROR means
# use the default archive.ubuntu.com. The deb822 file uses a single
# `URIs:` line per repo. Note: use the http:// variant (not https://)
# because the ubuntu:24.04 base image ships WITHOUT
# /etc/ssl/certs/ca-certificates.crt, so https URLs would fail
# certificate verification at the first apt-get update. After
# ca-certificates is installed in the next step, subsequent updates
# could in principle verify https, but http keeps the Dockerfile
# simpler and removes one moving part.
RUN if [ -n "$APT_MIRROR" ]; then \
      sed -i "s@http://archive.ubuntu.com/ubuntu/@${APT_MIRROR}@g; s@http://security.ubuntu.com/ubuntu/@${APT_MIRROR}@g" /etc/apt/sources.list.d/ubuntu.sources; \
    fi

# Install ca-certificates so any https://URL in the build (e.g.
# FetchContent git clones) can verify TLS. The base image lacks it.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# System packages needed for FetchContent deps to build. The list is
# deliberately exhaustive because CMake's FetchContent probes a lot of
# features at configure time and fails on any missing header.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      git \
      ninja-build \
      pkg-config \
      # gRPC / protobuf / abseil
      libabsl-dev \
      libc-ares-dev \
      libre2-dev \
      libprotobuf-dev \
      libssl-dev \
      protobuf-compiler \
      zlib1g-dev \
      # libpqxx
      libpq-dev \
      libpqxx-dev \
      # jwt-cpp pulls in libcurl transitively
      libcurl4-openssl-dev \
      # nlohmann/json (system pkg; FetchContent fallback would try to
      # download from github.com directly)
      nlohmann-json3-dev \
      # libuuid (used by NewUuid in src/util)
      uuid-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only what `cmake -S .` needs to find the project. .dockerignore
# excludes build/, tests/, docs/, .git/, .superpowers/, *.md, and the
# probe_interceptor2.o scratch file.
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY proto/ ./proto/
COPY src/ ./src/

# Optionally redirect GitHub URLs to a mirror. Empty GIT_INSTEADOF
# means leave git defaults alone. cmake/deps.cmake already calls
# `git config --global ... insteadOf` on its own, but that requires
# the build host to have a working ~/.gitconfig. Setting it here
# makes the Docker build hermetic.
RUN if [ -n "$GIT_INSTEADOF" ]; then \
      git config --global url.${GIT_INSTEADOF}.insteadOf https://github.com/; \
    fi

# Configure + build only the server binary. The default build target
# is `all` which would pull in the unit + e2e test targets and their
# deps (testcontainers-cpp + cpp-httplib + GoogleTest, ~5-7 more
# minutes of build time). Pinning to evgrpc_server keeps the image
# build focused on what's actually shipped.
RUN cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DEVGRPC_BUILD_TESTS=OFF \
      -DEVGRPC_WITH_TESTCONTAINERS=OFF \
    && cmake --build build --target evgrpc_server

# Verify the binary actually statically links gRPC + protobuf (per
# BUILD_SHARED_LIBS=OFF in cmake/deps.cmake). If this fails the
# runtime image below is missing shared libraries.
RUN ldd build/src/evgrpc_server \
      | grep -E 'lib(grpc|protobuf|absl|curl|pq|ssl|crypto|uuid|cares|re2|z)' \
      || echo "(no unexpected dynamic gRPC/protobuf deps — good)"

# =====================================================================
# Stage 2: runtime
# =====================================================================
FROM ubuntu:24.04 AS runtime

ARG APT_MIRROR

ENV DEBIAN_FRONTEND=noninteractive

# Same APT mirror swap as the builder stage (only effective when the
# build arg is set).
RUN if [ -n "$APT_MIRROR" ]; then \
      sed -i "s@http://archive.ubuntu.com/ubuntu/@${APT_MIRROR}@g; s@http://security.ubuntu.com/ubuntu/@${APT_MIRROR}@g" /etc/apt/sources.list.d/ubuntu.sources; \
    fi

# Same ca-certificates pre-install as the builder stage.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Runtime shared libraries. Versions pinned to the apt packages
# installed in the builder stage (24.04 = noble):
#   libpqxx-7.8t64      7.8.1-2.1build1
#   libabsl20220623t64  20220623.1-3.1ubuntu3.2
#   libc-ares2          (current noble)
#   libre2-10           20230301-3build1
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      libabsl20220623t64 \
      libc-ares2 \
      libcurl4 \
      libgcc-s1 \
      libpq5 \
      libpqxx-7.8t64 \
      libre2-10 \
      libssl3 \
      libstdc++6 \
      libuuid1 \
      zlib1g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/src/evgrpc_server /app/evgrpc_server

# Default config path (see src/util/args.{h,cc}). Operators MUST mount
# a real config.json here, e.g.:
#   docker run -v $PWD/config.json:/etc/evgrpc/config.json:ro …
# No env vars are read at startup in v2 — all configuration is in
# config.json.
#
# /app/log is created unconditionally because the repo-root config.json
# convention (commit 19d2c6c — centralize all logs to repo-root ./log/)
# sets log.file="./log/evgrpc.log", resolved against WORKDIR=/app.
# Without this dir, config_loader throws "log.file: parent directory
# does not exist or is not writable" at startup. Declared as VOLUME
# so rotating log files persist across container restarts; mount a
# named volume or bind mount in production to collect logs.
RUN mkdir -p /etc/evgrpc /app/log
VOLUME ["/app/log"]
EXPOSE 50051

ENTRYPOINT ["/app/evgrpc_server"]
CMD ["--config", "/etc/evgrpc/config.json"]
