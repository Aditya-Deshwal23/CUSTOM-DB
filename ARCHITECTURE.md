# CUSTOM-DB architecture and status

CUSTOM-DB is a C++17 prototype deployed as one Crow service with a vanilla static dashboard. The executable serves `/`, `/health`, `/api/metrics`, and `/api/query`. The dashboard uses same-origin requests when served by the backend.

## Status & limitations

### Implemented

- Crow routes and static dashboard delivery.
- Integer `INSERT <key> <value>` and point `SELECT <key>` commands through an executor-owned B+ tree.
- Page, buffer pool, clock replacer, disk manager, B+ tree, executor, and WAL code are present.
- Runtime configuration through `PORT` and `DB_PATH`, plus signal-driven server shutdown.

### Prototype

- Page reads and writes use a fixed 4096-byte page buffer. The current page and WAL representations use host-native values, not a versioned explicit-endian format.
- `/api/metrics` is sourced from a background synthetic workload. It is not a comprehensive or storage-I/O-specific measurement.
- Dashboard schema objects, buffer visualization, and execution trace are illustrations, not a live catalog or trace of backend activity.
- Buffer pool and B+ tree concurrency behavior has known gaps. See the disabled regression tests and [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

### Not implemented

- Restoring the B+ tree root or API-inserted keys on restart.
- WAL recovery in the normal API startup path or WAL-before-page durability.
- General SQL parsing, tables, schemas, transactions, update/delete, or a query planner.
- Proven latch crabbing for writers, zero-copy parsing, `fsync` durability, or authoritative live disk metrics.

## Request path

1. Crow receives a request and serves a static dashboard file or routes an API request.
2. `POST /api/query` validates a JSON `query` string and passes the command to `Executor`.
3. The executor currently handles integer `INSERT` and point `SELECT` commands using its B+ tree.
4. The tree fetches pages through `BufferPoolManager`; dirty pages are flushed by the current buffer-pool/disk-manager implementation, but the root/catalog needed to recover an API tree is not persisted.
5. `/api/metrics` returns counters sampled from the separate synthetic workload started by `main`.

## Storage and concurrency notes

The database file stores 4096-byte page images at offsets derived from page IDs. Page and WAL layouts are not portable across endianness or compiler/layout changes; see [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md). Current lock interactions and known deviations from desired invariants are recorded in [docs/CONCURRENCY.md](docs/CONCURRENCY.md).

## Evidence

Status claims are tracked in [docs/CLAIMS.md](docs/CLAIMS.md). The CMake targets and CI jobs are the executable validation surface; the disabled cases in `tests/regression/` capture known engine and API gaps without making ordinary CTest runs red.
