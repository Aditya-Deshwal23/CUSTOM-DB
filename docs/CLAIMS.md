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
| API data is recovered using the WAL on normal startup | verified | `src/main.cpp`; `PersistenceTest.ExecutorIndexAndRowsSurviveCleanRestart` |
| The WAL can append after a torn tail and recover committed statements | verified | `WalTest.RecordAppendedAfterTornTailIsRecoverable` |
| WAL commits are synced before page write-back in `full` and `normal` modes | verified | `WalTest.NormalModeSyncsBeforeDatabaseWriteBack`; buffer-pool eviction and flush paths call `WriteAheadLog::EnsureDurable` |
| WAL records and page images are explicit little-endian and CRC32C checked | verified | `docs/FILE_FORMAT.md`; `src/wal.cpp`; `src/crc32c.cpp`; `WalTest.RecordAppendedAfterTornTailIsRecoverable` |
| Statement rollback restores before-images, page allocation state, and no-steal pages | verified | `WalTest.RollbackRestoresPageImagesAndAllocationState` |
| `Executor::IntegrityCheck()` checks page checksums, B+ tree structure, and heap/index agreement | verified | `src/executor.cpp`; `BPlusTree::Validate()`; clean-restart integrity assertions in `PersistenceTest.ExecutorIndexAndRowsSurviveCleanRestart` |
| A process crash during a normal API write preserves acknowledged writes | in progress | `DurabilityTest.HardKillKeepsAckedWrites` is Linux-only; local validation skipped on macOS |
| A 200-iteration PR and 5,000-iteration nightly crash harness reports kill-site coverage | in progress | Pending Linux crash runner and CI/nightly execution evidence |
| WAL commit latency/throughput per durability mode and checkpoint pause are measured | verified | `reldb_wal_benchmarks`; `docs/benchmarks.md` records the run command, host, modes, and results |
| API query commands support both legacy integer commands and the implemented SQL subset | verified | `scripts/verify_resume_claims.sh`; `SqlExecutionTest.*`; `tools/difftest.py` |
| SQL lexer tokens are zero-copy source views with line/column positions, and the parser/printer round-trip the supported v1 statement and expression grammar | verified | `SqlLexerTest.*` and `SqlParserTest.*` in `tests/sql_parser_test.cpp`; nightly ASan/UBSan grammar fuzz in `.github/workflows/nightly.yml` |
| Durable SQL catalog and the documented SQL DDL/DML/SELECT subset are connected to the executor | verified | `SqlExecutionTest.*`; catalog restart, rollback, and index-maintenance cases |
| SQL planning selects point, range, ordered-index, and sequential access paths; EXPLAIN ANALYZE reports runtime operator and buffer-pool deltas | verified | `SqlExecutionTest.CreatesTablesAndExecutesIndexedSelects`; `SqlExecutionTest.ExecutesFiltersRangesProjectionSortAndLimits` |
| SQL statement writes roll back completely on failure, and reads share the statement gate while DDL/DML take it exclusively | verified | `SqlExecutionTest.FailedMultiRowInsertRollsBackEveryRow`; `SqlExecutionTest.FailedUpdateRollsBackEarlierRowsAndIndexes`; `ConcurrencyTest.SqlExecutorStatementGateTsanStress`; CI TSan job |
| The supported SQL subset differentially matches SQLite over 10,000 seeded operations | verified | `.github/workflows/ci.yml` runs `tools/difftest.py --statements 10000`; deliberate comparison limits are documented in `docs/SQL_EXECUTION.md` |
| SQL point select, 100-row range select, insert, update, and full-scan aggregate workloads are benchmarked at 10k/100k/1M rows | verified | `reldb_sql_execution_benchmarks`; `docs/benchmarks.md` |
| Direct unsynchronized TableHeap access is safe for concurrent readers/writers | removed | `docs/CONCURRENCY.md`; the SQL executor gate, not TableHeap itself, serializes writes against readers |
| SQL lexer token text is source-backed and zero-copy | verified | `SqlLexerTest.*` and `SqlParserTest.*` in `tests/sql_parser_test.cpp` |
| The B+ tree uses latch crabbing | removed | `docs/CONCURRENCY.md`; writers are serialized by the caller's statement gate |
| Executor serializes writers and permits concurrent readers without data races | verified | `ConcurrencyTest.ExecutorStatementGateTsanClean`; `ConcurrencyTest.SqlExecutorStatementGateTsanStress`; `.github/workflows/ci.yml` TSan job |
| Metrics describe a real engine-wide workload | removed | `/api/metrics` explicitly reports zero values with source `uninstrumented` until instrumentation is implemented |
| The dashboard reports real active latch counts and disk-I/O latency | removed | `active_latches` and derived I/O latency are not exposed; dashboard simulation uses a persistent warning |
| Dashboard schema and execution trace represent a live catalog/backend trace | removed | `README.md`; the simulation trace is explicitly illustrative |
| The dashboard uses no random-number calls to fabricate metrics | verified | CI's `Check metrics honesty` step runs `MetricsTest.NoFabricatedValues`; `scripts/run_known_bugs.sh` reports it |
| SQL benchmark timings are performance guarantees | removed | `docs/benchmarks.md` labels local measurements as observations, not CI thresholds |
| The project is high-performance, production-grade, or general-purpose ACID | removed | The engine supports single-writer, statement-atomic redo recovery; it does not implement general SQL transaction isolation |
