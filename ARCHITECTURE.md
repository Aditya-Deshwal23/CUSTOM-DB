# CUSTOM-DB architecture and status

CUSTOM-DB is a C++17 prototype deployed as one Crow service with a vanilla static dashboard. The executable serves `/`, `/health`, `/api/metrics`, `/api/buffer_pool`, and `/api/query`. The dashboard uses same-origin requests when served by the backend.

## Status & limitations

### Implemented

- Crow routes and static dashboard delivery.
- Integer `INSERT <key> <value>` and point `SELECT <key>` commands through an executor-owned B+ tree.
- Move-only page guards own pins; buffer-pool I/O runs outside the pool mutex, and Clock/LRU-K replacement policies are selectable.
- `PageAllocator` reuses a persisted page-zero free list; `FlushAll()` applies the write-back hook and syncs the disk.
- Page, buffer pool, replacers, disk manager, B+ tree, executor, and WAL code are present.
- Runtime configuration through `PORT` and `DB_PATH`, plus signal-driven server shutdown.

### Prototype

- Database files use a v2 little-endian 4096-byte page envelope with a checksummed common page header and a checksummed page-zero database header. Buffer-pool payloads are 4080 bytes; B+ tree body structures still use host-native representations, and WAL remains a separate prototype format.
- `/api/metrics` is sourced from a background synthetic workload. It is not a comprehensive or storage-I/O-specific measurement.
- Dashboard schema objects, buffer visualization, and execution trace are illustrations, not a live catalog or trace of backend activity.
- B+ tree algorithms, WAL ordering, and transactional page allocation have known gaps. See the known-gap regression tests and [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

### Not implemented

- Restoring the B+ tree root or API-inserted keys on restart.
- WAL recovery in the normal API startup path or WAL-before-page durability.
- General SQL parsing, tables, schemas, transactions, update/delete, or a query planner.
- Transactional allocation/reuse, WAL integration or recovery, transactional durability, or authoritative live disk metrics.

## Request path

1. Crow receives a request and serves a static dashboard file or routes an API request. `/api/buffer_pool` exposes live pool statistics and a capped frame snapshot.
2. `POST /api/query` validates a JSON `query` string and passes the command to `Executor`.
3. The executor currently handles integer `INSERT` and point `SELECT` commands using its B+ tree.
4. The tree fetches pages through `BufferPoolManager`; the file disk manager validates/stamps CRC32C page checksums and grows the logical high-water mark. Page writes are not durable until `IDiskManager::Sync()` is called; the root/catalog needed to recover an API tree is not persisted.
5. `/api/metrics` returns counters sampled from the separate synthetic workload started by `main`.

## Storage and concurrency notes

The database file stores 4096-byte page images at offsets derived from page IDs. The v2 envelope and page-zero header are explicitly little-endian and checksummed, but page payload structures and WAL are not yet portable across endianness or compiler/layout changes; see [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md). POSIX page reads and writes use positional I/O without a global I/O mutex. Current lock interactions and known deviations from desired invariants are recorded in [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

## Evidence

Status claims are tracked in [docs/CLAIMS.md](docs/CLAIMS.md). The CMake targets and CI jobs are the executable validation surface; remaining disabled cases in `tests/regression/` capture known engine and API gaps without making ordinary CTest runs red.
