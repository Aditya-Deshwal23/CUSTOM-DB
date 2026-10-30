# CUSTOM-DB

CUSTOM-DB is a C++17 storage-engine prototype with a Crow HTTP service and a vanilla HTML/CSS/JavaScript dashboard. One Docker image runs the backend and serves the static dashboard; there is no frontend framework or JavaScript build step.

## Status & limitations

### Implemented

- `GET /health`, `GET /api/metrics`, and `POST /api/query` are served by Crow.
- The query API supports integer commands: `INSERT <key> <value>` and `SELECT <key>`. The executor stores rows in `TableHeap` and indexes packed RIDs in a persistent B+ tree.
- The service reads `PORT` and `DB_PATH`, serves the dashboard from the same origin, and handles SIGINT/SIGTERM shutdown.
- The root Dockerfile builds a single service image. Docker Compose mounts a named volume at `/data`.

### Prototype

- The WAL-backed executor runs each write statement as a single-writer auto-commit transaction. It logs full-page after-images and a COMMIT record to `<DB_PATH>-wal`, restores before-images on statement failure, and replays committed changes before serving requests.
- `DURABILITY=full` (default) syncs the WAL on every commit. `normal` syncs on `WAL_SYNC_INTERVAL_MS` (default 1000 ms) and before a corresponding page can be written back; recently acknowledged statements can be lost within the configured interval. `off` is unsafe benchmark-only mode and requires `ALLOW_UNSAFE=1`.
- The one SQL index root is persisted in an `INDEX_META` page reached through `header.catalog_root`; executor initialization and page allocation are included in WAL-backed statements.
- `/api/metrics` returns zeros with `source: "uninstrumented"` until Phase 10 instrumentation is implemented.
- The dashboard's schema visualization and simulation trace are illustrative. Simulation mode shows a persistent warning; the trace is not a report of backend operations.
- Database pages and WAL records use explicit little-endian layouts and CRC32C. Checkpointing syncs dirty database pages before truncating the WAL.

### Not implemented

- General SQL syntax, multi-statement transactions/isolation, or SQL statements such as `SELECT * FROM Users`.
- Durable acknowledgment when `DURABILITY=normal` or `off`; use `full` for per-commit WAL sync.
- A user-facing `--check` command (planned for Phase 13), zero-copy query parsing, or writer latch crabbing.
- Production-grade concurrency, performance, or durability guarantees.

See [docs/CLAIMS.md](docs/CLAIMS.md) for claim-by-claim evidence and [docs/RESUME_EVIDENCE_REPORT.md](docs/RESUME_EVIDENCE_REPORT.md) for the existing local audit.
The disk and WAL formats are documented in [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), recovery behavior in [docs/WAL_AND_RECOVERY.md](docs/WAL_AND_RECOVERY.md), the design decision in [docs/adr/0001-wal-design.md](docs/adr/0001-wal-design.md), and measured local benchmarks in [docs/benchmarks.md](docs/benchmarks.md).

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
Linux CI runs 200 seeded crash iterations per PR, and the nightly workflow runs 5,000; the local runner is [tools/crashtest](tools/crashtest).
