# Phase 0 report

## Changes

- Added repository instructions, pinned CMake dependencies, core/server/test/benchmark targets, sanitizer and warning options, developer tooling, and CI workflows.
- Added eight disabled regression cases and a runner that prints their current status without making ordinary CTest runs fail.
- Replaced unsupported README and architecture claims with implemented/prototype/not-implemented status; added claim, file-format, and concurrency notes.
- Marked synthetic metrics as `synthetic_workload`, removed unsupported metrics, and labeled dashboard simulation and traces.
- Did not fix storage-engine behavior. Storage-core edits are warning fixes needed for warning-clean builds; pre-existing API startup and request-validation edits were retained.

## Validation and observed output

| Command | Result |
|---|---|
| `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DRELDB_WERROR=ON && cmake --build build-release --parallel && ctest --test-dir build-release --output-on-failure` | Passed; Release build completed and CTest reported `100% tests passed out of 1`. |
| `scripts/check.sh` | Exit 0; Debug and ASan+UBSan CTest each reported `100% tests passed out of 1`. The TSan stress run detected the known race at `BPlusTreePage::IncreaseSize(int)` (`include/b_plus_tree_page.h:37`). |
| `cmake -S . -B build-prod -DCMAKE_BUILD_TYPE=Release -DRELDB_BUILD_TESTS=OFF -DRELDB_BUILD_BENCHMARKS=OFF -DRELDB_WERROR=ON && cmake --build build-prod --parallel` | Passed; production configuration built `reldb_core` and `relational_db`. |
| `scripts/run_known_bugs.sh build/reldb_regression_tests` | Printed 4 failures, 2 platform skips, and 2 passes on macOS. Details below. |
| `scripts/verify_resume_claims.sh` | Passed; verified health, dashboard assets, metrics source, integer INSERT/SELECT, invalid request handling, and SIGTERM shutdown. |
| `hadolint Dockerfile .devcontainer/Dockerfile` | Passed. |
| `git diff --check` | Passed. |

### Known-regression results on this macOS host

| Test | Result |
|---|---|
| `PersistenceTest.DISABLED_RestartKeepsData` | Failed: key 5 not found after reopen. |
| `DurabilityTest.DISABLED_HardKillKeepsAckedWrites` | Skipped: Linux-only. |
| `BPlusTreeTest.DISABLED_LookupsNeverFalseMissAfterManyInserts` | Failed: 3,341 misses, first at lookup 64, seed 1337. |
| `BPlusTreeTest.DISABLED_RangeScanTerminatesAfterSplit` | Skipped: Linux-only resource-limited process test. |
| `WalTest.DISABLED_RecordAppendedAfterTornTailIsRecoverable` | Failed: recovery returned 2 instead of 3 records. |
| `ConcurrencyTest.DISABLED_ReaderWriterTsanClean` | Passed without TSan; the TSan run separately reported a data race in `IncreaseSize(int)`. Seed: 20261030. |
| `SqlTest.DISABLED_DocumentedExamplesWork` | Failed: `SELECT * FROM Users` is not supported. |
| `MetricsTest.DISABLED_NoFabricatedValues` | Passed after the honesty patch removed the fabricated metrics. |

The measured B+ tree miss count differs from the original report (3,341, first lookup 64 rather than 3,387/54). Values are recorded as observed, not adjusted to match an expected output. The metrics test passes intentionally after the required fix, so the eight cases do not all fail.

## Limitations

- Linux-only hard-kill and resource-limited range regressions were not runnable on this macOS host.
- Docker image build and actual Linux CI were not run locally; the Docker daemon is unavailable. CI is configured to run both.
- TSan does not pass cleanly by design in this phase: it reproduces the documented race for a later fix.

## Phase 1: disk persistence foundation

### Changes

- Documented the v2 file format before implementing it: a little-endian page-zero database header, common 16-byte page headers, CRC32C checksums, and 4080-byte buffer-facing page bodies.
- Replaced the stream-backed disk manager with `IDiskManager` and `FileDiskManager`. POSIX uses positional short-I/O/EINTR loops, chunked `ftruncate` growth, parent-directory `fsync` on creation, and `fdatasync` for `Sync()`; macOS uses `F_FULLFSYNC`. The Windows implementation uses `_read`, `_write`, and `_commit`.
- Added header/version validation, per-page checksum verification, typed `DbError`s, atomic 64-bit disk counters and fixed log2 latency buckets. Page IDs are reserved by the buffer pool through `SizeInPages()`/`Extend()`; the disk manager has no allocation or free-list policy.
- Added runtime-dispatched CRC32C hardware paths with a table-driven fallback, test-only fault injection, a Linux syscall trace check, concurrent page I/O tests, and `bench/bench_disk.cpp`.
- Removed standard C++ file streams from engine file I/O. WAL remains a separate prototype and is not integrated into recovery or the normal API write path.
- Updated [docs/CLAIMS.md](docs/CLAIMS.md), [docs/CONCURRENCY.md](docs/CONCURRENCY.md), [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), [docs/benchmarks.md](docs/benchmarks.md), [ARCHITECTURE.md](ARCHITECTURE.md), and [README.md](README.md).

### Validation on this macOS host

| Command | Result |
|---|---|
| Debug configure/build with `RELDB_WERROR=ON` and CTest | Passed; `100% tests passed`. |
| Release configure/build with `RELDB_WERROR=ON` and CTest | Passed; `100% tests passed`. |
| ASan+UBSan configure/build and CTest | Passed; `100% tests passed`. |
| TSan `DiskManagerTest.ConcurrentDistinctPageIo` (seed `20261030`) | Passed without a reported race. |
| Exhaustive `RELDB_EXHAUSTIVE_PAGE_CHECKSUM=1` bit-flip test | Passed; detected all 32,768 single-bit flips. |
| `scripts/verify_resume_claims.sh` | Passed; health, static assets, API commands, validation, and SIGTERM shutdown verified. |
| Release `reldb_disk_benchmarks --benchmark_min_time=1s` | Read: 0.817/0.795 us sequential/random; write: 1.72/1.73 us sequential/random; dirty-page `Sync()`: 3.872 ms. Full local baseline and caveats are in [docs/benchmarks.md](docs/benchmarks.md). |
| `git diff --check` and sync script `bash -n` | Passed. |

### Limitations

- This host is macOS, so the Linux-only `strace` script was not run locally. CI installs `strace` and checks two `fdatasync` calls (header initialization plus the explicit `Sync()`) and one parent-directory `fsync`.
- B+ tree payloads and WAL records remain host-native. API tree roots are not restored, no WAL recovery/transactional allocation was added, and writes are not automatically synced.
- At the Phase 1 checkpoint, `BufferPoolManager` still held its global pool latch across disk operations. Phase 2 replaces that path as described below.

## Phase 2: observable buffer pool

### Changes

- Replaced the public raw-page/unpin interface with move-only read, write, and basic guards that own pins; typed guard views validate size and alignment. Write guards mark pages dirty on mutation or release.
- Added aligned frames with atomic pin/dirty state, per-frame latches and I/O states. Misses reserve/remap under the pool mutex, perform disk I/O without it, then publish; same-page fetchers wait for the in-flight frame. Failed replacement reads restore the evicted page contents and mapping.
- Added Clock and LRU-K (K=2) replacers, selectable with `BUFFER_POOL_POLICY`; the scan-resistance comparison and choice of LRU-K default are recorded in [docs/benchmarks.md](docs/benchmarks.md).
- Added page-zero free-list allocation/reuse, including free-list reconstruction on reopen. Allocation uses guards; free-list updates are not transactional and WAL/no-steal remains deferred.
- Added `WriteBackPolicy`, policy-aware `FlushAll()` plus disk sync, validated `BUFFER_POOL_FRAMES` configuration (default 4096; minimum 16), 64-bit counters, pin gauges/assertions, and capped frame snapshots at `/api/buffer_pool`.
- Migrated B+ tree access to guards and enabled `LookupsNeverFalseMissAfterManyInserts`; no B+ tree algorithm changes were intended.

### Validation on this macOS host

| Command | Result |
|---|---|
| Release build with `RELDB_WERROR=ON` and full CTest | Passed; `100% tests passed`. |
| Debug build with `RELDB_WERROR=ON` and full CTest | Passed; `100% tests passed`. |
| ASan+UBSan build and full CTest | Passed; `100% tests passed`. |
| TSan same-page/write-back/failure rollback checks plus 16-thread × 1M-operation CRC stress (seed `20261030`) | Passed with no reported races. |
| `scripts/verify_resume_claims.sh` | Passed; health, static assets, buffer-pool snapshot, INSERT/SELECT, validation, and SIGTERM shutdown verified. |
| Buffer-pool benchmark | Clock/LRU-K latency, throughput, and hot-set/scan comparison recorded in [docs/benchmarks.md](docs/benchmarks.md). |
| `git diff --check` | Passed. |

### Limitations

- WAL ordering remains an extension hook only; the default write-back policy allows writes, and no transaction/no-steal behavior is implemented.
- `AssertNoPinsHeld()` is per calling thread; guards must not outlive their manager or be destroyed on a different thread from the one that acquired the latch.
- Historical pre-change buffer-pool timings were not recorded. The documented numbers compare the two new policies and are local observations, not CI thresholds.
