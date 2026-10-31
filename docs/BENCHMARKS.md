# Benchmarks

## Headline comparison with SQLite

`tools/benchmark_sqlite.py` creates the same 10,000-row primary-key table in a
temporary CUSTOM-DB file and a temporary SQLite file, populates both outside
the timed interval, then runs the same deterministic warm point-read keys.
Each run measures 1,000 reads; five runs use seeds 1337–1341. Setup is excluded.
The CUSTOM-DB request goes through Crow, HTTP, JSON, parsing, planning, and the
engine; SQLite uses Python's in-process `sqlite3` interface. This is a useful
user-facing path comparison, **not an apples-to-apples engine-core benchmark**.
It includes CUSTOM-DB's server and serialization overhead.

Observed on 2026-10-31, Apple M5 (`arm64`), macOS 27.0.1, Python 3.14.7,
SQLite 3.53.4, Release CUSTOM-DB build:

| Implementation / path | Median throughput | Median p50 | Median p95 |
|---|---:|---:|---:|
| CUSTOM-DB, HTTP + JSON | 6,000 reads/s | 158.0 us | 207.7 us |
| SQLite, Python `sqlite3` | 671,366 reads/s | 1.38 us | 1.67 us |

Reproduce the exact workload and print per-run results:

```sh
python3 tools/benchmark_sqlite.py --binary build-release/relational_db \
  --rows 10000 --iterations 1000 --repetitions 5 --seed 1337
```

The seed, database type, host, SQLite and Python versions, warm-cache
condition, and per-run statistics are included in the output. These local
measurements are not performance guarantees or CI thresholds.

`bench/bench_disk.cpp` measures sequential/random 4 KiB page reads and writes
plus `Sync()` latency. Read/write benchmarks operate on a 256-page file in the
local temporary directory and are hot-cache measurements, not raw-device
throughput. The sync benchmark writes a dirty page outside the timed interval
before timing each `Sync()` call.

## Local reference run

Command:

```sh
./build-release/reldb_disk_benchmarks --benchmark_min_time=1s
```

Observed on 2026-10-30, Apple M5 (`arm64`), macOS 27.0.1, Apple Clang 21,
Google Benchmark 1.8.3, Release build:

| Operation | Mean latency | Reported throughput |
|---|---:|---:|
| Sequential 4 KiB read | 0.817 us | 4.68 GiB/s |
| Random 4 KiB read | 0.795 us | 4.81 GiB/s |
| Sequential 4 KiB write | 1.72 us | 2.23 GiB/s |
| Random 4 KiB write | 1.73 us | 2.21 GiB/s |
| `Sync()` after a page write | 3.872 ms | n/a |

These are a local reference, not a performance guarantee or CI threshold.
The host had concurrent background load and Google Benchmark reported that
CPU-frequency and thread-affinity metadata were unavailable; rerun the command
on the target filesystem for comparable measurements. Every reported page
operation transfers a 4096-byte on-disk image, including its 16-byte header.

## Container image size

On the local Docker Desktop `linux/arm64` builder, `docker image ls` reported
146 MB for the pre-packaging `custom-db:before` image and 56.9 MB for
`custom-db:release-size-check`, built from the current pinned multi-stage
Dockerfile. That is a 61% reduction in the CLI-reported image size. Rebuild and
measure the current image with:

```sh
docker build -t custom-db:release-size-check .
docker image ls --no-trunc --filter reference=custom-db:release-size-check
```

The before value is a historical local baseline; the Docker CI job records the
current build's measured size in its run summary. Image sizes can vary by
architecture and Docker version.

## Buffer-pool measurements

`bench/bench_buffer_pool.cpp` measures cached hit latency, a sequential miss
stream, shared-pool throughput with 1/2/4/8 worker threads, and hot-set hit
rate after a repeated sequential flood. The miss and throughput workloads use
a 64-frame pool over 512/1024 disk pages. The scan workload uses a 16-page hot
set followed by a 256-page sequential flood, repeated within the run. The
benchmark constructs the pool using `BUFFER_POOL_POLICY=clock` or `lruk`.

Command:

```sh
./build-release/reldb_buffer_pool_benchmarks --benchmark_min_time=0.2s
```

Observed on 2026-10-30, Apple M5 (`arm64`), macOS 27.0.1, Apple Clang 21,
Google Benchmark 1.8.3, Release build:

| Measurement | Clock | LRU-K (K=2) |
|---|---:|---:|
| Cached hit latency | 46.8 ns | 48.8 ns |
| Sequential miss latency | 650 ns | 679 ns |
| Shared pool, 1 thread | 1.579 M ops/s | 1.429 M ops/s |
| Shared pool, 2 threads | 670 k ops/s | 484 k ops/s |
| Shared pool, 4 threads | 385 k ops/s | 223 k ops/s |
| Shared pool, 8 threads | 111 k ops/s | 74.6 k ops/s |
| Hot-set hit rate during sequential flood | 0.066% | 100% |

The default is LRU-K: this benchmark selects scan resistance (retaining every
hot-set page) over the measured 4% hit-path and 9% single-thread throughput
cost; `BUFFER_POOL_POLICY=clock` remains available for workloads that prioritize
miss-path throughput. The higher-thread LRU-K slowdown is visible and should
be remeasured on deployment hardware.

There was no buffer-pool benchmark in the pre-change phase, so a historical
before/after latency row is unavailable. The table compares both policies in
the new implementation rather than inventing a legacy baseline. Results are
local observations, not CI performance gates; benchmark metadata showed CPU
frequency and affinity were unavailable.

## Table-heap measurements

`bench/storage_bench.cpp` includes three workloads over one million 32-byte
opaque tuples and a 256-frame buffer pool: timed insert, full scan, and random
point get. Dataset setup is excluded from the scan and point-get timers. The
insert benchmark creates a fresh temporary database and times all one million
inserts. File-cache state and buffer-pool behavior are part of these local
measurements; they are not raw-device throughput or CI performance gates.

Command:

```sh
./build-release/reldb_storage_benchmarks --benchmark_filter='BM_TableHeap.*' --benchmark_min_time=0.5s
```

Observed on 2026-10-30, macOS, Google Benchmark 1.8.3, Release build:

| Operation | Measured result |
|---|---:|
| Insert 1M rows | 1.882 s; 535.6 k rows/s; 16.3 MiB/s |
| Full scan 1M rows | 18.17 ms; 55.2 M rows/s; 1.65 GiB/s |
| Random point get | 1.223 us; 820.4 k gets/s |

The insert and scan figures each came from one benchmark iteration. The point
get result was averaged over 573,352 iterations. The host reported a load
average around 4.5 during this run and did not report CPU frequency or
thread-affinity metadata, so these are a local reference only.

## B+ tree workloads

`bench/b_plus_tree_bench.cpp` registers point lookup, insert, and 128-key
range-scan benchmarks for 10,000, 100,000, and 1,000,000 preloaded keys. Each
operation has a warm case using the preloaded 256-frame pool and a cold-buffer
case that opens a new 16-frame pool for each operation. “Cold” means a cold
buffer pool; it does not evict the operating system's file cache. Dataset
construction and the initial flush are excluded from timed intervals.

Command:

```sh
./build/reldb_benchmarks --benchmark_filter='BM_BPlusTree.*' --benchmark_min_time=0.01s
```

Observed on 2026-10-30, local macOS host, Google Benchmark 1.8.3, Release
build. The host could not report CPU frequency or thread affinity. Timings
are mean wall-clock latency and are local observations, not CI thresholds.
Cold insert includes a new buffer pool and its clean-close flush/sync; other
cold cases include construction of a new 16-frame pool and index handle.

| Keys | Point warm | Point cold | Insert warm | Insert cold/close | Range warm | Range cold |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 287 ns | 7.761 us | 433 ns | 3.909 ms | 1.416 us | 9.736 us |
| 100,000 | 1.472 us | 8.910 us | 412 ns | 3.939 ms | 2.835 us | 10.917 us |
| 1,000,000 | 2.335 us | 9.521 us | 390 ns | 3.949 ms | 4.051 us | 11.207 us |

The split regression also reports its leaf fill factors: ascending insertions
reached 89.48% at 40,000 keys; seeded random insertion reached 70.87% at
200,000 keys. These are regression observations, not timing guarantees.

The same Google Benchmark target includes a warm delete benchmark. It deletes
keys once in ascending order and refills the tree with timing paused before
reusing the key range, so each timed operation is a successful delete. Run it
with the command above and a filter such as
`--benchmark_filter='BM_BPlusTreeDeleteWarm/10000$'`.

## WAL commit and checkpoint workloads

`bench/wal_bench.cpp` measures one transaction inserting four distinct B+ tree
keys per statement under `full`, `normal`, and `off`. It reports mean wall
latency and processed-key throughput. Every commit syncs the WAL in all three
modes; `normal` additionally runs a 20 ms periodic sync thread, while `off`
disables that background thread. One fixture initialization and cleanup is
amortized across the iterations. The checkpoint benchmark builds 256 rows
outside the timed interval, then measures `Checkpoint()` including dirty-page
write-back, database sync, and WAL replacement.

Command:

```sh
./build-release/reldb_wal_benchmarks --benchmark_min_time=0.1s
```

Observed on 2026-10-30, Apple M5 (`arm64`), macOS 27.0.1, Apple Clang 21,
Google Benchmark 1.8.3, Release build. CPU frequency and thread-affinity
metadata were unavailable. These are historical local observations from before
every commit was changed to sync the WAL; they are not current performance
measurements or CI thresholds and should not be compared to the new benchmark
run.

| Mode/workload | Mean latency | Key throughput |
|---|---:|---:|
| `full`, four keys per commit | 34.7 us/commit | 160.961 k keys/s |
| `normal`, four keys per commit | 6.98 us/commit | 642.73 k keys/s |
| `off`, four keys per commit | 6.11 us/commit | 671.999 k keys/s |
| `full`, checkpoint of 256 rows | 3.99 ms/checkpoint | n/a |

## SQL execution workloads

`bench/sql_execution_bench.cpp` loads 10,000, 100,000, or 1,000,000 rows,
then reports mean wall latency for 100 indexed point selects, 100 indexed
100-row range selects, 100 single-row inserts, 100 single-row updates, and
five full-scan aggregate queries. Dataset setup is excluded from timed
intervals. Each scale uses a fresh temporary database and removes it afterward.
The benchmark disables only periodic WAL sync; every statement still syncs its
COMMIT, so these timings include durable commit latency. The existing values
below predate that change and are historical, not current measurements.

Command:

```sh
./build-release/reldb_sql_execution_benchmarks
```

The target prints the exact per-scale results on the host where it is run.
Local results are observations, not CI performance gates.

`bench/sql_parse_bench.cpp` separately measures tokenization and parsing of a
fixed representative SELECT statement. The token source is held constant and
the benchmark consumes the resulting tokens/AST to prevent dead-code
elimination.

Command:

```sh
./build-release/reldb_sql_parse_benchmarks --benchmark_min_time=1s
```

`bench/ycsb_bench.cpp` runs seeded in-process YCSB-style SQL workloads against
100,000 through 1,000,000 rows. A is 50/50 read/update, B is 95/5, C is
read-only, and E issues short (up to 10-row) range scans. Uniform keys use a
uniform integer distribution; Zipfian keys use a precomputed rank CDF with
exponent 0.99. Each invocation accepts a durability mode (`full` or `normal`),
1/2/4/8 worker threads, a frame count, and warm or cold buffer-pool state.
Cold means the pool is reopened after setup and flush; it does not evict the
operating-system file cache. The CSV row reports achieved operation count,
throughput, p50/p95/p99 statement latency, buffer-pool hit rate, and the seed.

Example:

```sh
./build-release/reldb_ycsb_benchmarks --workload A --distribution zipfian \
  --records 100000 --operations 100000 --threads 4 --durability full \
  --cache warm --frames 256 --seed 1337
```

## Statement concurrency

`bench/concurrency_bench.cpp` measures indexed point-SELECT throughput at
1/2/4/8 threads and an 8-thread 95/5 point-SELECT/no-op-UPDATE mix. Each run
uses a fixed seed (`0xC0FFEE`); the read-only runs execute 4,000 operations per
thread and the mixed workload executes 2,000 per thread. Latencies are sampled
per statement. The no-op UPDATE still commits a transaction and syncs its WAL
record, so the mixed p50/p99 include durable-write latency.

Command:

```sh
./build-release/reldb_concurrency_benchmarks
```

Observed on 2026-10-31, Apple M5 (`arm64`), macOS 27.0.1, Apple Clang 21,
Release build:

| Workload | Threads | Throughput | p50 latency | p99 latency |
|---|---:|---:|---:|---:|
| Point SELECT | 1 | 329,820 ops/s | 2.875 us | 5.541 us |
| Point SELECT | 2 | 259,527 ops/s | 6.750 us | 18.833 us |
| Point SELECT | 4 | 239,791 ops/s | 13.709 us | 55.750 us |
| Point SELECT | 8 | 186,741 ops/s | 38.917 us | 112.959 us |
| 95/5 SELECT/UPDATE | 8 | 114,174 ops/s | 43.500 us | 320.792 us |

These are local observations, not performance guarantees or CI thresholds.
Read scaling decreases on this host as threads contend for shared engine
resources; rerun on the deployment hardware before drawing capacity
conclusions.

## Metrics recording overhead

`bench/metrics_overhead_bench.cpp` compares warmed point-SELECT execution with
metrics recording enabled and disabled. Each condition was measured for five
three-second repetitions, with random interleaving enabled.

Command:

```sh
./build-release/reldb_metrics_overhead_benchmarks --benchmark_min_time=3s --benchmark_repetitions=5 --benchmark_enable_random_interleaving=true
```

Observed on 2026-10-31, local Apple Silicon macOS host, Release build:

| Condition | Mean CPU time per point SELECT |
|---|---:|
| Metrics enabled | 2.698 us |
| Metrics disabled | 2.664 us |

This is **1.28% CPU-time overhead**, equivalent to a **1.26% throughput
reduction**. The enabled and disabled coefficients of variation were 0.74%
and 1.92%, respectively. CPU-frequency and thread-affinity metadata were
unavailable, and the host had concurrent background load; treat this as a
local observation rather than a performance guarantee or CI threshold.
