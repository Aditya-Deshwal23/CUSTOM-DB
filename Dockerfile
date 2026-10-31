# syntax=docker/dockerfile:1.7

FROM debian:12-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3 AS builder

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
        -DRELDB_BUILD_TESTS=OFF \
        -DRELDB_BUILD_BENCHMARKS=OFF \
    && cmake --build build --config Release --parallel \
    && strip --strip-unneeded build/relational_db \
    && install -d -o 10001 -g 10001 /data

FROM gcr.io/distroless/cc-debian13:nonroot@sha256:732d2fe38bb1996cb47f8218b48dc840967684d73c5483894ff2ed638068c1b6 AS runtime-amd64
FROM gcr.io/distroless/cc-debian13:nonroot@sha256:91e80685018de611a20450bed6b011c3d3242b3d9cc4452208121f342703ada0 AS runtime-arm64
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
