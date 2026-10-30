# Claim evidence

| Claim | Status | Evidence |
|---|---|---|
| C++17 Crow backend serves a vanilla dashboard from one Docker service | verified | `CMakeLists.txt`, `Dockerfile`, and `.github/workflows/ci.yml` Docker build |
| `/health`, `/api/metrics`, and `/api/query` are available | verified | `scripts/verify_resume_claims.sh` exercises all three routes |
| `/api/buffer_pool` exposes live pool stats and a capped frame snapshot | verified | `scripts/verify_resume_claims.sh`; `BufferPoolManager::Snapshot()` caps at 4096 frames |
| The REST API drives live dashboard queries | verified | `scripts/verify_resume_claims.sh` posts and reads integer commands; dashboard calls `/api/query` |
| `PORT` and `DB_PATH` configure the service | verified | `scripts/verify_resume_claims.sh`; `src/main.cpp` reads both variables |
| SIGTERM stops the HTTP service gracefully | verified | `scripts/verify_resume_claims.sh`; `.github/workflows/ci.yml` runs the smoke check |
| Both SIGINT and SIGTERM shutdown behavior are tested | in progress | `src/main.cpp` handles both signals; automated smoke currently sends SIGTERM only |
| A Docker named volume retains the database file | verified | `docker-compose.yml` and the local Docker restart evidence in `docs/RESUME_EVIDENCE_REPORT.md` |
| A Docker named volume restores API-inserted records after restart | removed | A clean file-level executor restart is tested, but a Docker-volume restart is not part of CI |
| The root Dockerfile is multi-stage and builds a single runtime service | verified | `Dockerfile`; `.github/workflows/ci.yml` Docker build |
| Render deployment uses the repository Dockerfile | in progress | README deployment URL and root `Dockerfile`; no deployment health check is run in CI |
| Legacy executor index entries and heap rows survive a clean restart | verified | `PersistenceTest.ExecutorIndexAndRowsSurviveCleanRestart`; root metadata is reached through page-zero `catalog_root` |
| Database pages are CRC32C-checksummed and corruption is detected | verified | `DiskManagerTest.DetectsSingleBitErrorsInSampledOrExhaustivePageBits` samples PR bits and `.github/workflows/nightly.yml` flips every bit; `DiskManagerTest.RoundTripReopenAndAllZeroPage` validates normal and empty-page reads |
| `Sync()` performs an OS durability sync rather than a stream flush | verified | `src/disk_manager.cpp`; `scripts/verify_disk_syncs.sh` traces exactly the initialization and explicit sync calls; CI runs the trace on Linux |
| New database files persist a versioned v2 little-endian header and parent-directory entry | verified | `DiskManagerTest.RoundTripReopenAndAllZeroPage`, `DiskManagerTest.RejectsInvalidMagicVersionAndPageSize`, and the Linux sync trace |
| Page reads/writes use positional I/O without a global disk-I/O mutex | verified | `src/disk_manager.cpp`; `DiskManagerTest.ConcurrentDistinctPageIo` runs concurrent operations on distinct page IDs |
| Page allocation/free-list reuse is implemented by the disk manager | removed | allocation is outside `IDiskManager`; `PageAllocator` owns the free list through page guards |
| The page, buffer-pool, clock-replacer, B+ tree, executor, and WAL components exist | verified | `include/`, `src/`, and CMake's `reldb_core` target |
| Buffer-pool guards own pins and release frame latches safely | verified | `BufferPoolGuardTest.GuardsOwnPinsAndMoveWithoutLeaking`; `BPlusTreeTest.LookupsNeverFalseMissAfterManyInserts` |
| Buffer-pool misses perform disk I/O without holding the pool mutex and same-page fetches share a frame | verified | `src/buffer_pool_manager.cpp`; `BufferPoolManagerTest.ConcurrentFetchesOfSamePageShareOneFrame`; 16-thread stress |
| Buffer-pool exhaustion reports `BUFFER_POOL_EXHAUSTED`; stats and snapshots are 64-bit/observable | verified | `BufferPoolManagerTest.ThrowsTypedErrorWhenAllFramesArePinned`; `BufferPoolGuardTest`; `BUFFER_POOL_FRAMES` validation test |
| Dirty write-back honors the policy hook and `FlushAll()` syncs the disk | verified | `BufferPoolManagerTest.FlushAllUsesPolicyAndSyncsAllowedPages`; `BufferPoolManagerTest.DirtyVictimIsWrittenBackBeforeReuse` |
| Page allocation reuses a guard-managed free list persisted in page zero | verified | `PageAllocatorTest.DeletePageReturnsPageToGuardManagedFreeList`; `PageAllocatorTest.FreeListSurvivesReopening` |
| Schema-validated INT/BIGINT/DOUBLE/BOOLEAN/VARCHAR values round-trip through an explicit-endian tuple format | verified | `StorageTupleTest.RandomSchemasRoundTripWithNullsAndEmptyStrings`; `StorageTupleTest.MaximumTupleAndOneByteOverAreChecked` |
| Slotted pages represent empty rows, tombstones, in-place compaction, and update relocation without overflow pages | verified | `SlottedPageTest.FullPageOneByteAndZeroLengthTuples`; `SlottedPageTest.CompactionPreservesSlotsAndUpdatesInPlaceWhenPossible`; `TableHeapTest.PersistsRowsMovesUpdatesAndScans` |
| TableHeap supports guarded insert/get/update/delete and full scans; RID values pack to 64 bits | verified | `TableHeapTest.PersistsRowsMovesUpdatesAndScans`; `RIDTest.PacksAndUnpacksPageAndSlot`; `StorageFuzzTest.MillionOperationsAgainstRidOracle` |
| TableHeap rebuilds its reclaimed-page set lazily after reopen | verified | `TableHeapTest.RebuildsReclaimedPagesAfterReopen` |
| TableHeap has measured million-row insert, full-scan, and point-get benchmarks | verified | `reldb_storage_benchmarks`; `docs/benchmarks.md` records the measured host and run |
| Clock and LRU-K scan-resistance results are recorded | verified | `reldb_buffer_pool_benchmarks`; `docs/benchmarks.md` |
| The B+ tree provides reliable lookups after 40,000 inserts with a 50-frame pool | verified | `BPlusTreeTest.LookupsNeverFalseMissAfterManyInserts`; 5,000 lookups, seed 1337 |
| B+ tree insert/delete/get/range operations match an oracle under constant eviction | verified | `BPlusTreeFuzzTest.MillionOperationsAgainstMapWithConstantEviction`; 1,000 validations and pin checks every operation |
| B+ tree split leaves preserve a complete sorted chain and satisfy occupancy rules | verified | `BPlusTreeTest.RangeScanTerminatesAfterSplit`; `BPlusTreeTest.AscendingAndRandomFillFactorsBeatHalfFullBaseline`; `docs/benchmarks.md` |
| Range scans terminate and return all sorted results after splits | verified | `BPlusTreeTest.RangeScanTerminatesAfterSplit`; the Linux test retains its memory/time-limited child |
| API data is recovered using the WAL on normal startup | removed | `src/main.cpp` does not invoke WAL recovery; no WAL-backed API write path |
| The WAL can append records and recover after a torn tail | in progress | `WalTest.DISABLED_RecordAppendedAfterTornTailIsRecoverable` |
| The WAL provides recovery or WAL-before-page durability | removed | WAL remains disconnected from normal API startup/write recovery; page persistence uses the separate `IDiskManager::Sync()` contract |
| The whole database representation is portable across endianness and compiler ABI | removed | The B+ tree payload is explicitly encoded, but the separate WAL prototype format remains host-native |
| API query commands support integer `INSERT <key> <value>` and point `SELECT <key>` | verified | `scripts/verify_resume_claims.sh` exercises these commands |
| The executor supports arbitrary SQL tables or `SELECT * FROM Users` | removed | The executor supports only integer-key INSERT/SELECT; arbitrary SQL parsing/catalog queries are not implemented |
| The engine implements general SQL, a catalog, transactions, or a query planner | removed | The API executor only supports integer `INSERT`/point `SELECT`; its persistent rows and index do not provide general SQL, a catalog, transactions, or a query planner |
| TableHeap is safe for concurrent writers or readers | removed | `docs/CONCURRENCY.md`; no table-level synchronization or TSan stress test is provided |
| Query parsing is zero-copy using `std::string_view` | removed | `src/executor.cpp` uses `std::string` token copies |
| The B+ tree uses latch crabbing | removed | `docs/CONCURRENCY.md`; writers are serialized by the caller's statement gate |
| Executor serializes writers and permits concurrent readers without data races | verified | `ConcurrencyTest.ExecutorStatementGateTsanClean`; `.github/workflows/ci.yml` TSan job |
| Metrics describe a real engine-wide workload | removed | `/api/metrics` explicitly reports zero values with source `uninstrumented` until instrumentation is implemented |
| The dashboard reports real active latch counts and disk-I/O latency | removed | `active_latches` and derived I/O latency are not exposed; dashboard simulation uses a persistent warning |
| Dashboard schema and execution trace represent a live catalog/backend trace | removed | `README.md`; the simulation trace is explicitly illustrative |
| The dashboard uses no random-number calls to fabricate metrics | verified | CI's `Check metrics honesty` step runs `MetricsTest.NoFabricatedValues`; `scripts/run_known_bugs.sh` reports it |
| A 10,000-record HTTP benchmark measured specific insert/read latency and throughput | in progress | `scripts/benchmark_api.py` and historical results in `docs/RESUME_EVIDENCE_REPORT.md`; not a CI performance gate |
| The project is high-performance, production-grade, ACID, or fully durable | removed | No corresponding benchmark, transaction, recovery, or durability verification exists |
