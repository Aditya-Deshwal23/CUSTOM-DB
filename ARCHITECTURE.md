# CUSTOM-DB architecture and status

CUSTOM-DB is a C++17 prototype deployed as one Crow service with a vanilla static dashboard. The executable serves `/`, `/health`, `/api/metrics`, `/api/buffer_pool`, and `/api/query`. The dashboard uses same-origin requests when served by the backend.

## Status & limitations

### Implemented

- Crow routes and static dashboard delivery.
- Legacy integer `INSERT <key> <value>` / point `SELECT <key>` plus the documented SQL subset, using a persistent catalog, `TableHeap`, and B+ tree indexes.
- Volcano-style SQL scans, filters, projections, sorting, limits, aggregates, and transactional INSERT/UPDATE/DELETE operators; EXPLAIN and EXPLAIN ANALYZE expose planned and executed operators.
- Move-only page guards own pins; buffer-pool I/O runs outside the pool mutex, and Clock/LRU-K replacement policies are selectable.
- `PageAllocator` reuses a persisted page-zero free list; `FlushAll()` applies the write-back hook and syncs the disk.
- Schema-validated INT/BIGINT/DOUBLE/BOOLEAN/VARCHAR values and little-endian tuple encoding feed a slotted-page `TableHeap` with insert/get/update/delete and full scans.
- WAL-backed statement transactions snapshot touched pages, append CRC32C-protected full-page images and COMMIT records, recover before service startup, and checkpoint through a no-steal write-back gate.
- `SqlLexer` emits source-backed `std::string_view` tokens with line/column locations; `SqlParser` builds a v1 statement/expression AST and `PrintSql()` provides a canonical round-trip surface. Lexer/parser behavior is covered by `SqlLexerTest` and `SqlParserTest`; the nightly workflow runs the AST round-trip grammar fuzzer under ASan/UBSan.
- CI differentially checks 10,000 seeded supported-subset SQL statements against SQLite; the semantic scope and deliberate differences are documented in [docs/SQL_EXECUTION.md](docs/SQL_EXECUTION.md).
- `Executor::IntegrityCheck()` verifies on-disk page checksums, B+ tree structure, and heap/index correspondence.
- Runtime configuration through `PORT` and `DB_PATH`, plus signal-driven server shutdown.

### Prototype

- Database files use a v2 little-endian 4096-byte page envelope with a checksummed common page header and a checksummed page-zero database header. WAL format and recovery are documented in [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md).
- `DURABILITY=normal` bounds the unsynced interval through a sync timer and a WAL-before-page-write check; `DURABILITY=off` is unsafe and requires `ALLOW_UNSAFE=1`.
- `/api/metrics` returns zero values with source `uninstrumented`; engine-wide instrumentation is not implemented.
- Dashboard schema objects, buffer visualization, and execution trace are illustrations, not a live catalog or trace of backend activity.
- The B+ tree assumes a statement gate and does not use latch crabbing. Statements larger than the buffer pool roll back with `TXN_TOO_LARGE`; multi-writer concurrency is not supported. See [docs/CONCURRENCY.md](docs/CONCURRENCY.md).
- SQL support is intentionally bounded: the catalog occupies one page, joins/foreign keys/composite indexes/multi-statement transactions are absent, and VARCHAR indexes do not provide ordered range scans.

### Not implemented

- General SQL joins, foreign-key constraints, composite indexes, and multi-statement transaction isolation.
- A `--check` CLI option (planned for Phase 13), or authoritative live disk metrics.

## Request path

1. Crow receives a request and serves a static dashboard file or routes an API request. `/api/buffer_pool` exposes live pool statistics and a capped frame snapshot.
2. `POST /api/query` validates a JSON `query` string and passes the legacy command or SQL statement to `Executor`.
3. The executor takes a shared gate for SELECT and an exclusive gate for DDL/DML; SQL writes use one WAL transaction per statement. Rows live in `TableHeap`, and indexed column values map to packed RIDs in B+ trees.
4. The tree fetches pages through `BufferPoolManager`; page-zero `catalog_root` identifies the persistent `INDEX_META` page. Each write statement records before-images and commits page images through the WAL before dirty pages can be written back.
5. `/api/metrics` returns zeros tagged `uninstrumented`.

## Storage and concurrency notes

The database file stores 4096-byte page images at offsets derived from page IDs. The v2 envelope, WAL, tuple encoding, table-heap headers, slotted-page fields, and B+ tree payload fields use explicit encodings. `TableHeap` itself has no internal concurrent-writer protocol; the executor gate serializes its use through the API. See [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), [docs/WAL_AND_RECOVERY.md](docs/WAL_AND_RECOVERY.md), and [docs/CONCURRENCY.md](docs/CONCURRENCY.md). POSIX page reads and writes use positional I/O without a global I/O mutex.

## Evidence

Status claims are tracked in [docs/CLAIMS.md](docs/CLAIMS.md). The CMake targets and CI jobs are the executable validation surface; remaining disabled cases in `tests/regression/` capture known engine and API gaps without making ordinary CTest runs red.
