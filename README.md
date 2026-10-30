# CUSTOM-DB

CUSTOM-DB is a C++17 storage-engine prototype with a Crow HTTP service and a vanilla HTML/CSS/JavaScript dashboard. One Docker image runs the backend and serves the static dashboard; there is no frontend framework or JavaScript build step.

## Status & limitations

### Implemented

- `GET /health`, `GET /api/metrics`, and `POST /api/query` are served by Crow.
- The query API supports session-scoped integer commands: `INSERT <key> <value>` and `SELECT <key>`.
- The service reads `PORT` and `DB_PATH`, serves the dashboard from the same origin, and handles SIGINT/SIGTERM shutdown.
- The root Dockerfile builds a single service image. Docker Compose mounts a named volume at `/data`.

### Prototype

- Disk pages, a buffer pool, a clock replacer, a B+ tree, a query executor, and a WAL implementation exist in the source tree. The API's B+ tree root is not persisted, and the WAL is not connected to the normal write path.
- `/api/metrics` reports observations from a seeded synthetic workload. It is not authoritative engine-wide telemetry.
- The dashboard's schema visualization and simulation trace are illustrative. Simulation mode shows a persistent warning; the trace is not a report of backend operations.
- The database file uses a host-native representation and is not a portable, versioned storage format.

### Not implemented

- SQL tables, schemas, general SQL syntax, transactions, crash recovery, durable API records, or SQL statements such as `SELECT * FROM Users`.
- Durable database persistence across process restarts. A mounted volume retains the file, but the B+ tree root is not restored.
- WAL-backed durability, `fsync` guarantees, zero-copy query parsing, writer latch crabbing, or measured disk-I/O latency.
- Production-grade concurrency, performance, or durability guarantees.

See [docs/CLAIMS.md](docs/CLAIMS.md) for claim-by-claim evidence and [docs/RESUME_EVIDENCE_REPORT.md](docs/RESUME_EVIDENCE_REPORT.md) for the existing local audit.

## Routes

- `/`: dashboard
- `/health`: health check
- `/api/metrics`: synthetic-workload metrics
- `/api/query`: integer insert/select API

The current Render demo is [custom-db-engine.onrender.com](https://custom-db-engine.onrender.com). Render availability and filesystem persistence depend on the configured service and plan; the service does not restore API records after restart.

## Build and run

See [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md) for native, Docker, and development commands. A quick Docker run is:

```bash
docker build -t custom-db .
docker run --rm -p 8080:8080 -e PORT=8080 -e DB_PATH=/data/custom.db \
  -v custom-db-data:/data custom-db
```

The dashboard console accepts `INSERT 42 100` and then `SELECT 42`. SQL examples such as `SELECT * FROM Users` are not supported.

## Verification

CI builds GCC and Clang Debug/Release configurations, runs CTest, builds ASan+UBSan and TSan configurations, checks changed C++ formatting, lints the Dockerfile, and builds the image without pushing it. `scripts/run_known_bugs.sh` runs the disabled regression cases and prints their current results.
