# CUSTOM-DB architecture and status

CUSTOM-DB is a C++17 prototype deployed as one Crow service with a vanilla static dashboard. The executable serves `/`, `/health`, `/api/metrics`, `/api/buffer_pool`, and `/api/query`. The dashboard uses same-origin requests when served by the backend.

## Status & limitations

### Implemented

- Crow routes and static dashboard delivery.
- Integer `INSERT <key> <value>` and point `SELECT <key>` commands through an executor-owned persistent B+ tree whose values are packed table RIDs.
- Move-only page guards own pins; buffer-pool I/O runs outside the pool mutex, and Clock/LRU-K replacement policies are selectable.
- `PageAllocator` reuses a persisted page-zero free list; `FlushAll()` applies the write-back hook and syncs the disk.
- Schema-validated values and little-endian tuple encoding feed a slotted-page `TableHeap` with insert/get/update/delete and full scans.
- WAL-backed statement transactions snapshot touched pages, append CRC32C-protected full-page images and COMMIT records, recover before service startup, and checkpoint through a no-steal write-back gate.
- `Executor::IntegrityCheck()` verifies on-disk page checksums, B+ tree structure, and heap/index correspondence.
- Runtime configuration through `PORT` and `DB_PATH`, plus signal-driven server shutdown.

### Prototype

- Database files use a v2 little-endian 4096-byte page envelope with a checksummed common page header and a checksummed page-zero database header. WAL format and recovery are documented in [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md).
- `DURABILITY=normal` bounds the unsynced interval through a sync timer and a WAL-before-page-write check; `DURABILITY=off` is unsafe and requires `ALLOW_UNSAFE=1`.
- `/api/metrics` returns zero values with source `uninstrumented`; engine-wide instrumentation is not implemented.
- Dashboard schema objects, buffer visualization, and execution trace are illustrations, not a live catalog or trace of backend activity.
- The B+ tree assumes a statement gate and does not use latch crabbing. Statements larger than the buffer pool roll back with `TXN_TOO_LARGE`; multi-writer concurrency is not supported. See [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

### Not implemented

- General SQL parsing, a user-facing catalog, multi-statement transaction isolation, or a query planner. The executor supports only integer-key INSERT/SELECT.
- A `--check` CLI option (planned for Phase 13), or authoritative live disk metrics.

## Request path

1. Crow receives a request and serves a static dashboard file or routes an API request. `/api/buffer_pool` exposes live pool statistics and a capped frame snapshot.
2. `POST /api/query` validates a JSON `query` string and passes the command to `Executor`.
3. The executor uses a statement gate, stores each inserted row in `TableHeap`, and indexes its packed RID in an `IndexKey16` B+ tree.
4. The tree fetches pages through `BufferPoolManager`; page-zero `catalog_root` identifies the persistent `INDEX_META` page. Each write statement records before-images and commits page images through the WAL before dirty pages can be written back.
5. `/api/metrics` returns zeros tagged `uninstrumented`.

## Storage and concurrency notes

The database file stores 4096-byte page images at offsets derived from page IDs. The v2 envelope, WAL, tuple encoding, table-heap headers, slotted-page fields, and B+ tree payload fields use explicit encodings. `TableHeap` itself has no internal concurrent-writer protocol; the executor gate serializes its use through the API. See [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), [docs/WAL_AND_RECOVERY.md](docs/WAL_AND_RECOVERY.md), and [docs/CONCURRENCY.md](docs/CONCURRENCY.md). POSIX page reads and writes use positional I/O without a global I/O mutex.

## Evidence

Status claims are tracked in [docs/CLAIMS.md](docs/CLAIMS.md). The CMake targets and CI jobs are the executable validation surface; remaining disabled cases in `tests/regression/` capture known engine and API gaps without making ordinary CTest runs red.
