# syntax=docker/dockerfile:1.7

FROM debian:13-slim@sha256:f6b08b09a05725f9431502d3b5714f5c4576e82c1a84fa1debe0c24a7e0c3ec4 AS builder

ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /src

# Packages track Debian 12 security updates; the OS base is pinned by digest.
# hadolint ignore=DL3008
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        make

COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src

RUN --mount=type=cache,target=/root/.cache/cmake-fetchcontent,sharing=locked \
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
        -DFETCHCONTENT_BASE_DIR=/root/.cache/cmake-fetchcontent \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -flto" \
        -DCMAKE_EXE_LINKER_FLAGS="-flto -static-libstdc++ -static-libgcc" \
        -DRELDB_WERROR=ON \
        -DRELDB_BUILD_TESTS=OFF \
        -DRELDB_BUILD_BENCHMARKS=OFF \
    && cmake --build build --config Release --parallel \
    && strip --strip-unneeded build/relational_db \
    && install -d -o 10001 -g 10001 /data

FROM gcr.io/distroless/cc-debian13:nonroot@sha256:08476c244888bfb425bae1ae4dd7c17f093e972aab8426883d97f3c6f280f7c1 AS runtime-amd64
FROM gcr.io/distroless/cc-debian13:nonroot@sha256:08476c244888bfb425bae1ae4dd7c17f093e972aab8426883d97f3c6f280f7c1 AS runtime-arm64
# Architecture selects one of the pinned minimal runtime manifests above.
# hadolint ignore=DL3006
FROM runtime-${TARGETARCH} AS runtime

ARG VERSION=1.0.0
ARG VCS_REF=unknown

LABEL org.opencontainers.image.title="CUSTOM-DB" \
      org.opencontainers.image.description="C++17 Crow relational storage service" \
      org.opencontainers.image.source="https://github.com/Aditya-Deshwal23/CUSTOM-DB" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${VCS_REF}"

WORKDIR /app
COPY --from=builder --chown=10001:10001 /src/build/relational_db /app/relational_db
COPY --from=builder --chown=10001:10001 /data /data
COPY --chown=10001:10001 dashboard ./dashboard

ENV PORT=8080 \
    DB_PATH=/data/custom.db

USER 10001:10001
VOLUME ["/data"]
EXPOSE 8080
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD ["/app/relational_db", "--healthcheck"]
ENTRYPOINT ["/app/relational_db"]
