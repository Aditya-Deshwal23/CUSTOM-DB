# Claim evidence

| Claim | Status | Evidence |
|---|---|---|
| C++17 Crow backend serves a vanilla dashboard from one Docker service | verified | `CMakeLists.txt`, `Dockerfile`, and `.github/workflows/ci.yml` Docker build |
| `/health`, `/api/metrics`, and `/api/query` are available | verified | `scripts/verify_resume_claims.sh` exercises all three routes |
| The REST API drives live dashboard queries | verified | `scripts/verify_resume_claims.sh` posts and reads integer commands; dashboard calls `/api/query` |
| `PORT` and `DB_PATH` configure the service | verified | `scripts/verify_resume_claims.sh`; `src/main.cpp` reads both variables |
| SIGTERM stops the HTTP service gracefully | verified | `scripts/verify_resume_claims.sh`; `.github/workflows/ci.yml` runs the smoke check |
| Both SIGINT and SIGTERM shutdown behavior are tested | in progress | `src/main.cpp` handles both signals; automated smoke currently sends SIGTERM only |
| A Docker named volume retains the database file | verified | `docker-compose.yml` and the local Docker restart evidence in `docs/RESUME_EVIDENCE_REPORT.md` |
| A Docker named volume restores API-inserted records after restart | removed | `tests/regression/known_bugs_test.cpp`: `PersistenceTest.DISABLED_RestartKeepsData`; the tree root is not restored |
| The root Dockerfile is multi-stage and builds a single runtime service | verified | `Dockerfile`; `.github/workflows/ci.yml` Docker build |
| Render deployment uses the repository Dockerfile | in progress | README deployment URL and root `Dockerfile`; no deployment health check is run in CI |
| Disk-based persistence of API records survives graceful restart | removed | `PersistenceTest.DISABLED_RestartKeepsData` and `DurabilityTest.DISABLED_HardKillKeepsAckedWrites` |
| The page, buffer-pool, clock-replacer, B+ tree, executor, and WAL components exist | verified | `include/`, `src/`, and CMake's `reldb_core` target |
| The B+ tree provides reliable lookups after 40,000 inserts with a 50-frame pool | in progress | `BPlusTreeTest.DISABLED_LookupsNeverFalseMissAfterManyInserts`; known false misses are not fixed |
| Range scans terminate and return all sorted results after splits | in progress | `BPlusTreeTest.DISABLED_RangeScanTerminatesAfterSplit`; known self-link defect is not fixed |
| API data is recovered using the WAL on normal startup | removed | `src/main.cpp` does not invoke WAL recovery; no WAL-backed API write path |
| The WAL can append records and recover after a torn tail | in progress | `WalTest.DISABLED_RecordAppendedAfterTornTailIsRecoverable` |
| The WAL provides `fsync`-backed durability | removed | `src/wal.cpp` uses stream flush; there is no `fsync` guarantee or API write-path integration |
| The file format is explicit-endian, versioned, and portable | removed | `docs/FILE_FORMAT.md`; current page and WAL representations are host-native |
| API query commands support integer `INSERT <key> <value>` and point `SELECT <key>` | verified | `scripts/verify_resume_claims.sh` exercises these commands |
| The executor supports SQL tables or `SELECT * FROM Users` | removed | `SqlTest.DISABLED_DocumentedExamplesWork`; `src/executor.cpp` supports only integer commands |
| The engine implements schema/catalog, transactions, update/delete, or a query planner | removed | `ARCHITECTURE.md`; no such route or executor behavior is present |
| Query parsing is zero-copy using `std::string_view` | removed | `src/executor.cpp` uses `std::string` token copies |
| Writer operations use proven latch crabbing | removed | `ConcurrencyTest.DISABLED_ReaderWriterTsanClean`; `docs/CONCURRENCY.md` |
| The engine is proven thread-safe under concurrent reads and writes | in progress | `ConcurrencyTest.DISABLED_ReaderWriterTsanClean` and the CI TSan job |
| Metrics describe a real engine-wide workload | removed | API response labels its source `synthetic_workload`; counters represent only the separate seeded workload |
| The dashboard reports real active latch counts and disk-I/O latency | removed | `active_latches` and derived I/O latency are not exposed; dashboard simulation uses a persistent warning |
| Dashboard schema and execution trace represent a live catalog/backend trace | removed | `README.md`; the simulation trace is explicitly illustrative |
| The dashboard uses no random-number calls to fabricate metrics | verified | CI's `Check metrics honesty` step runs `MetricsTest.DISABLED_NoFabricatedValues`; `scripts/run_known_bugs.sh` reports it |
| A 10,000-record HTTP benchmark measured specific insert/read latency and throughput | in progress | `scripts/benchmark_api.py` and historical results in `docs/RESUME_EVIDENCE_REPORT.md`; not a CI performance gate |
| The project is high-performance, production-grade, ACID, or fully durable | removed | No corresponding benchmark, transaction, recovery, or durability verification exists |
