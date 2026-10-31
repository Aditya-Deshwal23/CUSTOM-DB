# CUSTOM-DB

CUSTOM-DB is a C++17 storage-engine prototype with a Crow HTTP service and a vanilla HTML/CSS/JavaScript dashboard. One Docker image runs the backend and serves the static dashboard; there is no frontend framework or JavaScript build step.

## Status & limitations

### Implemented

- `GET /health`, `GET /api/metrics`, and `POST /api/query` are served by Crow.
- The query API preserves legacy integer `INSERT <key> <value>` / `SELECT <key>` commands and also executes the documented SQL subset in [docs/SQL_EXECUTION.md](docs/SQL_EXECUTION.md).
- SQL tables and indexes are persisted in a one-page catalog; point/range/index-order scans, sequential scans, filters, projections, sorting, limits, aggregates, and transactional DML are implemented.
- `EXPLAIN` displays the planned operator tree; `EXPLAIN ANALYZE` executes the query and reports operator rows, inclusive wall time, buffer-pool page deltas, and access path.
- SQL write statements run as individual WAL transactions and roll back completely on statement failure. Reads take the shared statement gate; writes take it exclusively.
- The service reads `PORT` and `DB_PATH`, serves the dashboard from the same origin, and handles SIGINT/SIGTERM shutdown.
- The root Dockerfile builds a single service image. Docker Compose mounts a named volume at `/data`.

### Prototype

- The WAL-backed executor runs each write statement as a single-writer auto-commit transaction. It logs full-page after-images and a COMMIT record to `<DB_PATH>-wal`, restores before-images on statement failure, and replays committed changes before serving requests.
- Every write statement syncs its WAL COMMIT before returning, regardless of `DURABILITY`. `full` is the default, `normal` also runs periodic background syncs using `WAL_SYNC_INTERVAL_MS` (default 1000 ms), and legacy `off` mode disables only that background thread.
- `/api/metrics` returns zeros with `source: "uninstrumented"` until Phase 10 instrumentation is implemented.
- The dashboard's schema visualization and simulation trace are illustrative. Simulation mode shows a persistent warning; real query plans are available through SQL `EXPLAIN`.
- Database pages and WAL records use explicit little-endian layouts and CRC32C. Checkpointing syncs dirty database pages before truncating the WAL.
- A source-backed SQL lexer and v1 recursive-descent/Pratt parser produce statement/expression ASTs with positioned syntax errors; parser tests and a nightly ASan/UBSan grammar fuzzer cover the front end.

### Not implemented

- General SQL, joins, foreign keys, composite indexes, multi-statement transactions, and general transaction isolation. The SQL catalog currently fits on one page; VARCHAR index ordering and ranges are not supported.
- A user-facing `--check` command (planned for Phase 13), or writer latch crabbing.
- Production-grade concurrency, performance, or durability guarantees.

See [docs/CLAIMS.md](docs/CLAIMS.md) for claim-by-claim evidence, [docs/CONCURRENCY.md](docs/CONCURRENCY.md) for the statement-level concurrency model, [docs/adr/0002-concurrency.md](docs/adr/0002-concurrency.md) for its design decision, and [docs/RESUME_EVIDENCE_REPORT.md](docs/RESUME_EVIDENCE_REPORT.md) for the existing local audit.
The disk and WAL formats are documented in [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md), recovery behavior in [docs/WAL_AND_RECOVERY.md](docs/WAL_AND_RECOVERY.md), the design decision in [docs/adr/0001-wal-design.md](docs/adr/0001-wal-design.md), and measured local benchmarks in [docs/benchmarks.md](docs/benchmarks.md).

## Routes

- `/`: dashboard
- `/health`: health check
- `/api/metrics`: currently uninstrumented metrics (zero values)
- `/api/query`: legacy integer commands and SQL subset

The current Render demo is [custom-db-engine.onrender.com](https://custom-db-engine.onrender.com). Render availability and filesystem persistence depend on the configured service and plan.

## Build and run

See [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md) for native, Docker, and development commands. A quick Docker run is:

```bash
docker build -t custom-db .
docker run --rm -p 8080:8080 -e PORT=8080 -e DB_PATH=/data/custom.db \
  -v custom-db-data:/data custom-db
```

The dashboard console accepts legacy `INSERT 42 100` / `SELECT 42` commands and SQL such as `CREATE TABLE`, `INSERT INTO`, and `SELECT`. See [docs/SQL_EXECUTION.md](docs/SQL_EXECUTION.md) for the supported subset and known semantic differences.

## Verification

CI builds GCC and Clang Debug/Release configurations, runs CTest, runs a seeded 10,000-statement SQLite differential test, builds ASan+UBSan and TSan configurations, checks changed C++ formatting, lints the Dockerfile, and builds the image without pushing it. The SQL execution benchmark covers point, 100-row range, insert, update, and full-scan aggregate workloads at 10k/100k/1M rows; see [docs/benchmarks.md](docs/benchmarks.md).
Linux CI runs 200 seeded crash iterations per PR, and the nightly workflow runs 5,000; the local runner is [tools/crashtest](tools/crashtest).
