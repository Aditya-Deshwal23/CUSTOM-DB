# CUSTOM-DB architecture and status

CUSTOM-DB is a C++17 prototype deployed as one Crow service with a vanilla static dashboard. The executable serves `/`, `/health`, `/api/metrics`, `/api/buffer_pool`, and `/api/query`. The dashboard uses same-origin requests when served by the backend.

## Status & limitations

### Implemented

- Crow routes and static dashboard delivery.
- Integer `INSERT <key> <value>` and point `SELECT <key>` commands through an executor-owned persistent B+ tree whose values are packed table RIDs.
- Move-only page guards own pins; buffer-pool I/O runs outside the pool mutex, and Clock/LRU-K replacement policies are selectable.
- `PageAllocator` reuses a persisted page-zero free list; `FlushAll()` applies the write-back hook and syncs the disk.
- Schema-validated values and little-endian tuple encoding feed a slotted-page `TableHeap` with insert/get/update/delete and full scans.
- Page, buffer pool, replacers, disk manager, B+ tree, executor, and WAL code are present.
- Runtime configuration through `PORT` and `DB_PATH`, plus signal-driven server shutdown.

### Prototype

- Database files use a v2 little-endian 4096-byte page envelope with a checksummed common page header and a checksummed page-zero database header. Buffer-pool payloads are 4080 bytes; B+ tree key, value, and page-ID fields use explicit encodings, while WAL remains a separate prototype format.
- `/api/metrics` returns zero values with source `uninstrumented`; engine-wide instrumentation is not implemented.
- Dashboard schema objects, buffer visualization, and execution trace are illustrations, not a live catalog or trace of backend activity.
- The B+ tree assumes a statement gate and does not use latch crabbing. WAL ordering and transactional page allocation have known gaps. See [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

### Not implemented

- Crash recovery and durable acknowledgment of API writes.
- WAL recovery in the normal API startup path or WAL-before-page durability.
- General SQL parsing, a user-facing catalog, transactions, or a query planner. The executor supports only integer-key INSERT/SELECT.
- Transactional allocation/reuse, WAL integration or recovery, transactional durability, or authoritative live disk metrics.

## Request path

1. Crow receives a request and serves a static dashboard file or routes an API request. `/api/buffer_pool` exposes live pool statistics and a capped frame snapshot.
2. `POST /api/query` validates a JSON `query` string and passes the command to `Executor`.
3. The executor uses a statement gate, stores each inserted row in `TableHeap`, and indexes its packed RID in an `IndexKey16` B+ tree.
4. The tree fetches pages through `BufferPoolManager`; page-zero `catalog_root` identifies the persistent `INDEX_META` page. The file disk manager validates/stamps CRC32C page checksums and grows the logical high-water mark. Page writes are not crash-safe until WAL recovery and ordering are implemented.
5. `/api/metrics` returns zeros tagged `uninstrumented`.

## Storage and concurrency notes

The database file stores 4096-byte page images at offsets derived from page IDs. The v2 envelope, tuple encoding, table-heap headers, slotted-page fields, and B+ tree payload fields use explicit encodings; the WAL remains a prototype format. `TableHeap` itself has no internal concurrent-writer protocol; the executor gate serializes its use through the API. See [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md) and [docs/CONCURRENCY.md](docs/CONCURRENCY.md). POSIX page reads and writes use positional I/O without a global I/O mutex.

## Evidence

Status claims are tracked in [docs/CLAIMS.md](docs/CLAIMS.md). The CMake targets and CI jobs are the executable validation surface; remaining disabled cases in `tests/regression/` capture known engine and API gaps without making ordinary CTest runs red.
