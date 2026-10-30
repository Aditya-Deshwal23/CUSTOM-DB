# Disk benchmark baseline

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
