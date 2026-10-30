# CUSTOM-DB

CUSTOM-DB is a C++17 storage-engine prototype with a Crow HTTP service and a vanilla HTML/CSS/JavaScript dashboard. One Docker image runs the backend and serves the static dashboard; there is no frontend framework or JavaScript build step.

## Status & limitations

### Implemented

- `GET /health`, `GET /api/metrics`, and `POST /api/query` are served by Crow.
- The query API supports integer commands: `INSERT <key> <value>` and `SELECT <key>`. The executor stores rows in `TableHeap` and indexes packed RIDs in a persistent B+ tree.
- The service reads `PORT` and `DB_PATH`, serves the dashboard from the same origin, and handles SIGINT/SIGTERM shutdown.
- The root Dockerfile builds a single service image. Docker Compose mounts a named volume at `/data`.

### Prototype

- Disk pages, a buffer pool, a clock replacer, a B+ tree, a query executor, and a WAL implementation exist in the source tree. The one SQL index root is persisted in an `INDEX_META` page reached through `header.catalog_root`; WAL is not connected to the normal write path.
- `/api/metrics` returns zeros with `source: "uninstrumented"` until Phase 10 instrumentation is implemented.
- The dashboard's schema visualization and simulation trace are illustrative. Simulation mode shows a persistent warning; the trace is not a report of backend operations.
- Database files use a v2 little-endian checksummed page envelope, and B+ tree key/value/page fields use explicit encodings. WAL records remain a separate prototype format. Page writes require an explicit `Sync()` for an OS durability barrier.

### Not implemented

- General SQL syntax, transactions, crash recovery, crash-safe page allocation, or SQL statements such as `SELECT * FROM Users`.
- Automatic OS durability for each acknowledged API write. Clean shutdown/reopen restores index entries and rows, but a crash-safe WAL/recovery path is not implemented.
- WAL-backed durability, automatic durability for every page write, zero-copy query parsing, or writer latch crabbing.
- Production-grade concurrency, performance, or durability guarantees.

See [docs/CLAIMS.md](docs/CLAIMS.md) for claim-by-claim evidence and [docs/RESUME_EVIDENCE_REPORT.md](docs/RESUME_EVIDENCE_REPORT.md) for the existing local audit.
The disk format is documented in [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), and a measured local disk benchmark is recorded in [docs/benchmarks.md](docs/benchmarks.md).

## Routes

- `/`: dashboard
- `/health`: health check
- `/api/metrics`: currently uninstrumented metrics (zero values)
- `/api/query`: integer insert/select API

The current Render demo is [custom-db-engine.onrender.com](https://custom-db-engine.onrender.com). Render availability and filesystem persistence depend on the configured service and plan.

## Build and run

See [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md) for native, Docker, and development commands. A quick Docker run is:

```bash
docker build -t custom-db .
docker run --rm -p 8080:8080 -e PORT=8080 -e DB_PATH=/data/custom.db \
  -v custom-db-data:/data custom-db
```

The dashboard console accepts `INSERT 42 100` and then `SELECT 42`. SQL examples such as `SELECT * FROM Users` are not supported.

## Verification

CI builds GCC and Clang Debug/Release configurations, runs CTest, builds ASan+UBSan and TSan configurations, checks changed C++ formatting, lints the Dockerfile, and builds the image without pushing it. The B+ tree oracle regression covers one million operations with a 16-frame pool; see [docs/CLAIMS.md](docs/CLAIMS.md) for evidence.
