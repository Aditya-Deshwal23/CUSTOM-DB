# Resume Evidence Report

> Historical audit snapshot. Its implementation findings describe the code as audited then and are superseded by current evidence in [CLAIMS.md](./CLAIMS.md) and the regression suite.

## Scope and Environment

This report audits the working code and live Docker image as of 2026-07-13. It does not treat README claims, comments, or unused classes as implemented behavior.

Test environment:

- Host: macOS 26.5.1 on Apple Silicon (`arm64`).
- Build container: Ubuntu 24.04.4 LTS (`aarch64`), CMake 3.28.3, GCC 13.3.0.
- Image build: `docker build --no-cache --quiet -t custom-db:audit-final .`
- Runtime: local Docker Desktop, image `custom-db:audit-final`, mapped host ports `18081` or `18082`, with a named Docker volume mounted at `/data`.
- Benchmark client: Python 3 standard library over HTTP/1.1, one persistent connection, loopback networking. Results are not cloud, cross-platform, or production-capacity benchmarks.

## Project Architecture

The service is a single C++17 executable. `src/main.cpp` reads `PORT` and `DB_PATH`, constructs `DiskManager`, `BufferPoolManager`, `Executor`, and `ApiServer`, then starts Crow on `0.0.0.0`. `ApiServer` serves the three dashboard files and the HTTP endpoints. The dashboard is vanilla HTML/CSS/JavaScript and uses same-origin requests in deployment.

`Executor` owns its own `BPlusTree<int, int>`. That tree uses the buffer pool and disk manager for pages during the current process, but it does not persist a root-page catalog or reconstruct its root on startup. `main.cpp` also creates a separate B+ tree only for synthetic workload and metrics activity; it is not the API query tree.

## Implementation Inventory

| Feature | Status | Evidence | Relevant files |
| --- | --- | --- | --- |
| Disk persistence | Partial | `DiskManager` reads/writes fixed pages to `DB_PATH`; a named Docker volume retained a 176,128-byte database file after shutdown. API keys were not recoverable after restart because the executor tree root is not persisted. | `src/disk_manager.cpp`, `src/main.cpp`, `src/executor.cpp` |
| Record insertion | Working, session-scoped | `INSERT <int> <int>` inserts unique integer key/value pairs. 10,000 deterministic inserts passed. | `src/executor.cpp`, `src/b_plus_tree.cpp` |
| Record retrieval | Working, session-scoped | `SELECT <int>` returned all 10,000 inserted values across three deterministic read runs. | `src/executor.cpp`, `src/b_plus_tree.cpp` |
| Record update | Missing | `UPDATE` returns `Unknown command. Use INSERT or SELECT.` | `src/executor.cpp` |
| Record deletion | Missing | No executor command or REST route deletes API-query records. | `src/executor.cpp`, `include/api_server.h` |
| Schema handling | Missing | No table, schema, catalog, row-definition, or DDL path is exposed. Dashboard tables are static visualization data. | `src/executor.cpp`, `dashboard/index.js` |
| Query parsing | Partial | Whitespace token parser accepts only `INSERT key value` and `SELECT key`; malformed commands return result text. | `src/executor.cpp` |
| Query execution | Partial | Integer point insert and lookup execute through an in-memory executor-owned B+ tree; no SQL, tables, predicates, updates, or deletes. | `src/executor.cpp` |
| Indexing | Working for current process | B+ tree insert/lookup passed 10,000-key deterministic API verification. | `src/b_plus_tree.cpp`, `include/b_plus_tree_leaf_page.h` |
| Concurrency control | Partial | Crow is multithreaded and engine code uses mutex/shared_mutex. A 20-client, 400-request unique insert/read smoke test passed; this does not prove general thread safety. | `include/api_server.h`, `src/buffer_pool_manager.cpp`, `src/b_plus_tree.cpp` |
| REST API | Working | `/api/query` accepts POST JSON, `/api/metrics` accepts GET, and malformed/missing/non-string `query` bodies return 400. | `include/api_server.h` |
| Runtime metrics | Partial | Values are populated by a synthetic background workload, not validated engine-wide operational metrics. The fabricated `active_latches` value and derived I/O-latency field have been removed; the endpoint identifies its source. | `src/main.cpp`, `include/api_server.h` |
| Health checks | Working | `GET /health` returned `{"status":"ok"}` with HTTP 200. | `include/api_server.h` |
| Graceful shutdown | Working | Docker `SIGTERM` stop completed in 1 second and released the port; Docker `SIGINT` exit code was 0. | `src/main.cpp`, `include/api_server.h` |
| Docker deployment | Working | Multi-stage Dockerfile builds `relational_db`, copies dashboard files, and launches `/app/relational_db`. | `Dockerfile` |
| Persistent Docker volume | Partial | Compose and Docker mount `/data`, and the database file remains. API-query data does not recover after restart. | `docker-compose.yml`, `Dockerfile`, `src/main.cpp` |
| Browser dashboard | Working as static UI | `/`, `/index.css`, and `/index.js` returned HTTP 200. The UI includes simulation behavior and a same-origin live API mode. | `dashboard/`, `include/api_server.h` |
| Automated tests | Added smoke verification; no CTest suite | `scripts/verify_resume_claims.sh` provides build/start/API/shutdown verification. `CMakeLists.txt` does not register CTest tests. | `scripts/verify_resume_claims.sh`, `CMakeLists.txt` |

## Build and Run

Native build and run, when CMake is installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
PORT=8080 DB_PATH=./test.db ./build/relational_db
```

The audit host did not have local CMake installed, so the successful build was performed by the repository Dockerfile:

```bash
docker build --no-cache --quiet -t custom-db:audit-final .
docker run --rm -p 18082:8080 -e PORT=8080 -e DB_PATH=/data/final.db -v custom-db-final-data:/data custom-db:audit-final
```

The current compact smoke verification is:

```bash
scripts/verify_resume_claims.sh
```

It uses a temporary database file, checks static assets, health, session-scoped insert/select behavior, duplicate/missing-key behavior, missing-query validation, and SIGTERM shutdown.

## Functional and Reliability Results

| Test | Exact command or method | Input | Result |
| --- | --- | --- | --- |
| Health and static files | `curl http://127.0.0.1:18082/health`, `/`, `/index.css`, `/index.js` | One request per route | All returned HTTP 200; health body was `{"status":"ok"}`. |
| Input validation | `curl -X POST .../api/query -H 'Content-Type: application/json' -d '{}'` | Invalid JSON, missing `query`, non-string `query` | All returned HTTP 400 after the API guard fix. |
| Duplicate/missing/unsupported operations | POST query strings | Duplicate `INSERT 42 420`, missing `SELECT 999999`, `UPDATE 1 2`, malformed `INSERT bad 1` | HTTP 200 with result text: duplicate/missing/syntax/unknown-command messages. Update is not supported. |
| 100-key deterministic insert/read | `scripts/benchmark_api.py --base-url http://127.0.0.1:18082 --insert-count 100 --read-runs 1 --start-key 1` | Keys 1-100, values `key * 10` | All inserts and reads verified. |
| 1,000-key deterministic insert/read | `scripts/benchmark_api.py --base-url http://127.0.0.1:18082 --insert-count 1000 --read-runs 1 --start-key 101` | Keys 101-1100, values `key * 10` | All inserts and reads verified. |
| 10,000-key deterministic insert/read | `scripts/benchmark_api.py --base-url http://127.0.0.1:18082 --insert-count 10000 --read-runs 3` | Keys 1-10,000, values `key * 10` | All inserts and all 10,000 reads in each of three runs verified. |
| Concurrent smoke test | Python standard-library `ThreadPoolExecutor(max_workers=20)` | 200 unique inserts and 200 reads | 400 requests completed with zero errors; bounded smoke test only. |
| Invalid DB path | `docker run --rm -e DB_PATH=/not-a-directory/custom.db custom-db:audit-final` | Unwritable/nonexistent parent | Logged `Failed to initialize database ...` and exited 1. |
| SIGTERM | `docker stop custom-db-final-server` | Running service | Completed in 1 second; subsequent curl to port 18082 failed. |
| SIGINT | `docker kill --signal=SIGINT custom-db-final-restart` | Running service | Container exited with code 0. |
| Docker-volume restart | Stop service, restart with `-v custom-db-final-data:/data`, then `SELECT 1` | 10,000 inserted API records, retained 176,128-byte DB file | File remained, but `SELECT 1` returned `Key 1 not found.` |

## Benchmark Results

All numbers below are end-to-end local HTTP client timing from the environment above. Throughput includes client serialization, loopback TCP, Crow request handling, and response parsing.

| Workload | Requests | Average | p50 | p95 | p99 | Throughput | Errors |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Insert, 100 keys | 100 | 0.519 ms | 0.431 ms | 0.758 ms | 1.335 ms | 1,896.7 ops/s | 0 |
| Read, 100 keys | 100 | 0.453 ms | 0.422 ms | 0.616 ms | 0.758 ms | 2,175.7 ops/s | 0 |
| Insert, 1,000 keys | 1,000 | 0.378 ms | 0.364 ms | 0.453 ms | 0.596 ms | 2,602.9 ops/s | 0 |
| Read, 1,000 keys | 1,000 | 0.382 ms | 0.363 ms | 0.483 ms | 0.852 ms | 2,573.8 ops/s | 0 |
| Insert, 10,000 keys | 10,000 | 0.530 ms | 0.423 ms | 0.927 ms | 1.846 ms | 1,848.9 ops/s | 0 |
| Read, 10,000 keys, run 1 | 10,000 | 0.740 ms | 0.519 ms | 1.294 ms | 4.192 ms | 1,331.1 ops/s | 0 |
| Read, 10,000 keys, run 2 | 10,000 | 0.443 ms | 0.421 ms | 0.582 ms | 0.789 ms | 2,220.4 ops/s | 0 |
| Read, 10,000 keys, run 3 | 10,000 | 0.458 ms | 0.430 ms | 0.627 ms | 0.836 ms | 2,145.2 ops/s | 0 |

## Code Quality Findings

- `DiskManager` serializes file access with `std::mutex`; buffer-pool and B+ tree root access use `std::shared_mutex`.
- The API now rejects malformed query bodies without throwing. Database-open errors now return a clear nonzero startup failure instead of aborting.
- `WriteAheadLog`, `SlottedPage`, `ScanWithPredicate`, and several WAL-related types exist in source, but `main.cpp` does not create a WAL or call recovery, and the public API does not use slotted pages or predicate scans.
- The dashboard execution trace and schema are visual/simulated; they do not represent a table catalog or all engine operations. Simulation mode now shows a persistent warning, and live mode only displays API-reported metrics and query results.
- No unit-test target or CTest registration exists. The added scripts are live smoke/benchmark tools, not a replacement for unit and recovery tests.

## Known Limitations

- API query data is not durable across process or container restart despite the retained database file.
- Only integer `INSERT` and point `SELECT` commands are supported. There are no tables, schemas, SQL grammar, range API, update, delete, transaction, or recovery behavior exposed by the service.
- Do not claim ACID compliance, SQL compatibility, durable WAL recovery, persistent API records, complete thread safety, or production-scale performance.
- The metrics endpoint should be described as demonstration telemetry, not as authoritative database performance instrumentation.

## Files Changed for This Audit

- `include/api_server.h`: return HTTP 400 when `query` is missing or not a JSON string.
- `src/main.cpp`: return a clear exit code 1 when database-file initialization fails.
- `scripts/verify_resume_claims.sh`: reproducible local build/start/health/query/shutdown smoke verification.
- `scripts/benchmark_api.py`: deterministic, dependency-free HTTP benchmark runner.
- `docs/RESUME_EVIDENCE_REPORT.md`: this evidence report.

## Recommended Demo Flow

1. Start the Docker service with a named volume and open `/`.
2. Show `/health` returning `{"status":"ok"}` and `/api/metrics` returning JSON.
3. POST `INSERT 42 100`, then `SELECT 42`, and show duplicate and missing-key responses.
4. Explain that the dashboard is served by the same Crow process and uses same-origin API calls.
5. Do not demonstrate a restart as durable API-query persistence; it currently demonstrates the documented gap.

## Resume-Safe Technical Claims

- Built a C++17 storage-engine prototype with a buffer pool, clock replacement, page-oriented disk manager, and integer B+ tree used by a Crow query API.
- Integrated a Crow HTTP service with a vanilla static dashboard, same-origin API calls, Docker packaging, health checks, environment-based port/database configuration, and signal-driven shutdown.
- Verified deterministic session-scoped insertion and lookup for 10,000 integer records over HTTP; local Docker measurements showed 0.530 ms average insert latency and 1,848.9 inserts/s in the stated environment.

## Claims That Must Not Be Used

- Durable relational database, persistent records, ACID transactions, crash recovery, WAL-backed durability, SQL database, schema/table engine, update/delete support, or complete query planner.
- Proven high concurrency, production scalability, benchmark results outside the stated local Docker environment, or dashboard telemetry as actual engine-wide observability.
- Any claim that `DB_PATH` alone makes API-query data recover after restart.

## Resume Bullets

Project title:

```text
Custom Relational Storage Engine | C++, Crow, CMake, Docker
```

### Concise ATS-Friendly Bullets

- Built a C++17 custom storage-engine prototype with page-oriented disk I/O, a clock-managed buffer pool, and integer B+ tree indexing, verified through 10,000 deterministic API inserts.
- Integrated a Crow HTTP service and same-origin vanilla dashboard, adding Docker configuration, health checks, environment-based database paths, input validation, and clean SIGINT/SIGTERM shutdown behavior.
- Measured session-scoped query performance in Docker: 10,000 integer inserts averaged 0.530 ms at 1,848.9 ops/s, while deterministic reads completed without errors across three runs.

### Detailed Alternatives

- Implemented a C++17 storage-engine prototype composed of a page-oriented `DiskManager`, 50-frame clock-managed `BufferPoolManager`, and integer B+ tree used for session-scoped insert and point-lookup operations.
- Connected the C++ engine to a Crow REST API and vanilla HTML/CSS/JavaScript dashboard, serving UI assets and API endpoints from one Dockerized process using same-origin browser requests.
- Added environment-driven `PORT` and `DB_PATH` configuration, invalid database-path reporting, malformed JSON validation, and signal-driven Crow shutdown; verified Docker SIGTERM port release and SIGINT exit code 0.
- Created deterministic Python HTTP benchmarks and a shell smoke verifier; validated 10,000 inserts and three full 10,000-read runs locally, with measured insert average latency of 0.530 ms.

### Interview Evidence

| Bullet focus | Supporting files | Supporting tests | Metric source | Accurate explanation |
| --- | --- | --- | --- | --- |
| Storage-engine prototype | `src/disk_manager.cpp`, `src/buffer_pool_manager.cpp`, `src/b_plus_tree.cpp`, `src/executor.cpp` | `scripts/benchmark_api.py` with 10,000 inserts | 10,000-key run in this report | The API exposes integer insert/select through an executor-owned B+ tree; the page and buffer-pool components support that tree during the process lifetime. |
| API and dashboard | `include/api_server.h`, `dashboard/index.html`, `dashboard/index.js`, `Dockerfile` | HTTP 200 checks for `/`, assets, `/health`, and API routes | Not a performance metric | Crow serves the dashboard and API from one origin; dashboard development mode alone uses a localhost fallback. |
| Reliability and configuration | `src/main.cpp`, `include/api_server.h` | Invalid JSON/missing query returned 400; invalid DB path exited 1; SIGTERM/SIGINT Docker tests | SIGTERM 1 second, SIGINT exit 0 | The application validates request shape, reports a database-open failure clearly, and routes signals through a watcher that stops Crow. |
| Measured performance | `scripts/benchmark_api.py` | `--insert-count 10000 --read-runs 3` | Benchmark table above | The result is a local Docker loopback HTTP measurement, not a cloud throughput or persistent-database benchmark. |

Likely interviewer questions:

- **Why is persistence marked partial?** The disk file survives because pages are flushed to `DB_PATH`, but the executor B+ tree root is neither cataloged nor reconstructed, so API-inserted keys are unavailable after restart.
- **Is this a SQL database?** No. The supported public grammar is only `INSERT <int> <int>` and `SELECT <int>`; table, schema, update, delete, transaction, and SQL claims are not supported.
- **What does the dashboard show?** It is a static same-origin UI that can call the live metrics/query routes, but parts of its schema and execution trace are simulation/visualization rather than a live table catalog.
- **How were the metrics produced?** The Python script uses a persistent local HTTP connection, deterministic key/value data, and `perf_counter` for end-to-end client timing; the full command and environment are documented above.
