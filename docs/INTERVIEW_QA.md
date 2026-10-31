# Interview Q&A

Answers describe this repository's current implementation. Evidence is linked
inline; local benchmark numbers are observations from
[BENCHMARKS.md](BENCHMARKS.md), not guarantees.

## 25 hard questions

1. **What does a successful write acknowledgement guarantee?** One write
   statement is a WAL transaction; its page after-images and COMMIT are synced
   before return. See [WAL_AND_RECOVERY.md](WAL_AND_RECOVERY.md) and
   `WalTest.EveryCommittedStatementIsDurableBeforeReturning` in
   [known_bugs_test.cpp](../tests/regression/known_bugs_test.cpp).
2. **How does recovery decide which page images to replay?** It scans the
   checksummed valid WAL prefix and replays page images only for transactions
   with a valid COMMIT. [Recovery tests](../tests/regression/known_bugs_test.cpp)
   include committed replay and interrupted recovery.
3. **What happens when the final WAL record is torn?** Recovery stops at the
   invalid tail and rewrites a valid header before future appends;
   `WalTest.RecordAppendedAfterTornTailIsRecoverable` guards the regression in
   [known_bugs_test.cpp](../tests/regression/known_bugs_test.cpp).
4. **How can the database file be behind a committed transaction?** WAL is
   authoritative until checkpoint. A committed page may be written back only
   after its commit record is durable; [WAL_AND_RECOVERY.md](WAL_AND_RECOVERY.md)
   specifies checkpoint ordering.
5. **How are failed statements rolled back without an undo log on disk?**
   Before-images and allocator state are held in memory for the active
   transaction; active pages are no-steal. See
   `WalTest.RollbackRestoresPageImagesAndAllocationState` in
   [known_bugs_test.cpp](../tests/regression/known_bugs_test.cpp).
6. **What isolation does SQL provide?** Reads share a statement gate; writes
   take it exclusively through commit. This serializes statements, but there
   is no MVCC or multi-statement transaction API. See
   [CONCURRENCY.md](CONCURRENCY.md) and [ADR 0002](adr/0002-concurrency.md).
7. **Can two writers mutate the B+ tree concurrently?** Not through the SQL
   executor. The executor serializes them; page latches protect frames, not
   structural tree mutation. `ConcurrencyTest.SqlExecutorStatementGateTsanStress`
   exercises concurrent SQL callers in
   [sql_execution_test.cpp](../tests/sql_execution_test.cpp).
8. **How does the B+ tree avoid pin leaks and false misses under eviction?**
   RAII guards own pins and release them when moved/destroyed. Regression
   coverage includes guard ownership and lookups after many inserts:
   [buffer-pool tests](../tests/buffer_pool_test.cpp),
   [B+ tree tests](../tests/regression/known_bugs_test.cpp).
9. **How are leaf splits and range scans validated?** Leaf links are checked
   by a range-scan-after-split regression, and a seeded million-operation
   oracle test validates insert/delete/get/range behavior in
   [b_plus_tree_test.cpp](../tests/b_plus_tree_test.cpp) and
   [known_bugs_test.cpp](../tests/regression/known_bugs_test.cpp).
10. **How does the buffer pool avoid doing disk I/O under its global mutex?**
    It reserves/publishes frame state under the page-table lock, releases it
    for I/O, then publishes the loaded frame. Lock order and stress evidence:
    [CONCURRENCY.md](CONCURRENCY.md),
    `DiskManagerTest.ConcurrentDistinctPageIo` in
    [disk_manager_test.cpp](../tests/disk_manager_test.cpp).
11. **Why have both Clock and LRU-K?** Clock prioritizes lower overhead;
    LRU-K is the default and resists sequential scan pollution. In the recorded
    16-page hot-set/256-page flood benchmark, measured hit rate was 100% for
    LRU-K and 0.066% for Clock, with an observed hit-path cost. See
    [BENCHMARKS.md](BENCHMARKS.md#buffer-pool-measurements).
12. **What can the planner optimize?** It chooses point/range indexes,
    ordered-index access when supported, or sequential scan, then composes
    filter/projection/sort/limit/aggregate operators. See
    [SQL_EXECUTION.md](SQL_EXECUTION.md) and
    [execution tests](../tests/sql_execution_test.cpp).
13. **Does `EXPLAIN ANALYZE` report actual work?** Yes: it executes the SELECT
    and reports operator rows, inclusive wall time, and buffer-pool deltas;
    [execution tests](../tests/sql_execution_test.cpp) cover supported plan
    shapes. It is not a simulated trace.
14. **Which SQL features are absent?** Joins, general subqueries, composite
    indexes/foreign keys, general SQL compatibility, and multi-statement
    transactions are absent. The full boundary is in
    [SQL_REFERENCE.md](SQL_REFERENCE.md) and
    [LIMITATIONS.md](LIMITATIONS.md).
15. **How does the API constrain hostile requests?** It validates JSON and
    SQL, enforces body/SQL/result limits, rate limits, optional API keys,
    CORS, and database quotas. Real-binary coverage is in
    [test_api.py](../tests/api/test_api.py); route shapes are in
    [openapi.yaml](openapi.yaml).
16. **What is the difference between liveness and readiness?** `/healthz`
    reports process liveness; `/readyz` rejects recovery/draining states and
    checks database sync. `test_health_readiness_and_version_routes` and
    lifecycle tests in [test_api.py](../tests/api/test_api.py) cover routes.
17. **What happens during graceful shutdown?** Readiness becomes unavailable,
    new statements are rejected, active HTTP handlers drain up to the
    configured timeout, then Crow stops and storage checkpoints/syncs. A
    second POSIX signal forces exit. Crow stops its I/O services as one unit,
    so accepting sockets are stopped after the drain. See
    [main.cpp](../src/main.cpp) and [demo_crash.sh](../scripts/demo_crash.sh).
18. **How does Docker preserve data while remaining hardened?** The image runs
    as UID 10001 and expects `/data` as a mounted volume; Compose uses a
    read-only root, tmpfs, dropped capabilities, and no-new-privileges. See
    [Dockerfile](../Dockerfile) and
    [docker-compose.yml](../docker-compose.yml).
19. **Is the Render demo durable?** No. Render Free has ephemeral storage and
    spins down while idle; persistent disks require paid instances. Use the
    local/CI hard-kill proof instead. Sources:
    [README](../README.md#live-demo), [Render Free docs](https://render.com/docs/free).
20. **How is service configuration handled?** One `Config` reads and validates
    environment settings at startup; invalid values exit 78 and the effective
    config redacts `API_KEY`. See [CONFIG.md](CONFIG.md),
    [config.cpp](../src/config.cpp), and
    [test_config_startup.py](../tests/api/test_config_startup.py).
21. **What is the storage portability story?** Multibyte on-disk fields are
    explicitly little-endian, checksums are CRC32C, and compile-time layout
    assertions guard fixed structures. See [FILE_FORMAT.md](FILE_FORMAT.md)
    and [disk_manager_test.cpp](../tests/disk_manager_test.cpp).
22. **What does the SQLite comparison prove?** Only one seeded warm point-read
    workload through CUSTOM-DB's HTTP path versus Python `sqlite3`; it does not
    prove engine-core performance equivalence. The observed medians were
    5,861 and 648,263 reads/s respectively. Methodology and command:
    [BENCHMARKS.md](BENCHMARKS.md#headline-comparison-with-sqlite).
23. **What is the testing strategy for concurrency bugs?** Debug/Release and
    ASan/UBSan tests cover regression suites; TSan exercises disk, WAL timer,
    statement-gate, and API middleware paths. Seeds and durations are printed
    by tests/workflows in [ci.yml](../.github/workflows/ci.yml).
24. **What recovery evidence can be reproduced locally?** Run
    `scripts/demo_crash.sh`: it writes through the API, kills the container,
    restarts with the same volume, checks integrity and recovered rows, and
    verifies graceful stop. A real terminal capture is
    [recovery.cast](assets/recovery.cast).
25. **What are the first scaling limits?** The single-writer gate and
    full-page WAL writes are likely constraints; the recorded 8-thread
    read-only result was 186,741 ops/s on one Apple M5 run. This is a local
    observation, not a capacity claim. See
    [BENCHMARKS.md](BENCHMARKS.md#statement-concurrency) and
    [LIMITATIONS.md](LIMITATIONS.md).

## What I would do next

1. **Add MVCC only after a multi-version recovery design is specified.** The
   current exclusive write gate and redo-only WAL are explicit limits in
   [ADR 0001](adr/0001-wal-design.md) and [ADR 0002](adr/0002-concurrency.md).
2. **Add overflow pages and external sort/spill.** That addresses the
   [4,067-byte tuple limit and in-memory operators](LIMITATIONS.md) without
   weakening page-format validation.
3. **Make coverage gaps test-driven.** Use the CI gcovr artifact to bring each
   named component toward the 80% target rather than publish an unmeasured
   coverage percentage; see [CONTRIBUTING.md](CONTRIBUTING.md).
4. **Build a fair engine-core SQLite benchmark.** The current comparison
   includes an HTTP service boundary, as disclosed in
   [BENCHMARKS.md](BENCHMARKS.md#headline-comparison-with-sqlite).
5. **Define online backup and replication semantics before implementing
   them.** Neither exists today; see [LIMITATIONS.md](LIMITATIONS.md).
