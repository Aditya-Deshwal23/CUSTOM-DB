FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# hadolint ignore=DL3008
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DRELDB_BUILD_TESTS=OFF \
        -DRELDB_BUILD_BENCHMARKS=OFF \
    && cmake --build build --config Release -j

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# hadolint ignore=DL3008
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /app /data

WORKDIR /app

COPY --from=builder /src/build/relational_db /app/relational_db
COPY dashboard ./dashboard

ENV PORT=8080
ENV DB_PATH=/data/custom.db

EXPOSE 8080

CMD ["/app/relational_db"]
