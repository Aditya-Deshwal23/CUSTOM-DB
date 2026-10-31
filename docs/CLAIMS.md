# Claim ledger

This ledger is the evidence index for claims in the README and project
documentation. CI-backed claims point to the workflow and the specific test or
script that produces the evidence. Benchmark values are observations, not
guarantees.

| Claim | Evidence |
|---|---|
| The project uses C++17, Crow, vanilla web assets, and one service image | [CMake targets](../CMakeLists.txt), [Dockerfile](../Dockerfile), [API/dashboard CI](../.github/workflows/ci.yml) |
| Physical database and WAL formats are explicit-endian, versioned, and CRC32C checked | [format specification](FILE_FORMAT.md), [disk format tests](../tests/disk_manager_test.cpp), [WAL tests](../tests/regression/known_bugs_test.cpp) |
| Buffer-pool guards own pins and release them on move/destruction | [guard tests](../tests/buffer_pool_test.cpp), including `BufferPoolGuardTest.GuardsOwnPinsAndMoveWithoutLeaking` |
| B+ tree lookups survive constant eviction, range scans terminate after leaf splits, and seeded operations match an oracle | [B+ tree regressions](../tests/b_plus_tree_test.cpp), [known-bug regressions](../tests/regression/known_bugs_test.cpp) |
| Table heap and tuple encoding are page-bounded and persistent | [storage tests](../tests/storage_test.cpp), [FILE_FORMAT.md](FILE_FORMAT.md) |
| The lexer/parser and SQL executor implement the documented subset | [parser tests](../tests/sql_parser_test.cpp), [execution tests](../tests/sql_execution_test.cpp), [SQL reference](SQL_REFERENCE.md) |
| SQL statements can be differentially checked against SQLite for a seeded supported subset | [differential test](../tools/difftest.py), [CI job](../.github/workflows/ci.yml), [semantic boundaries](SQL_EXECUTION.md) |
| Each write statement commits full-page images and a WAL COMMIT before acknowledgement | [WAL commit tests](../tests/regression/known_bugs_test.cpp), [WAL design](WAL_AND_RECOVERY.md), [ADR 0001](adr/0001-wal-design.md) |
| Recovery handles torn tails and replays committed images before the API starts | [torn-tail/recovery tests](../tests/regression/known_bugs_test.cpp), [startup implementation](../src/main.cpp), [recovery design](WAL_AND_RECOVERY.md) |
| Linux hard-kill recovery and container same-volume restart are exercised | [CI crash jobs](../.github/workflows/ci.yml), [nightly crash job](../.github/workflows/nightly.yml), [container proof](../scripts/demo_crash.sh) |
| The recorded container run recovered all 1,752 acknowledged writes | [captured recovery log and row count](assets/recovery.cast), [count assertion in demo](../scripts/demo_crash.sh) |
| The statement gate prefers writers and reports bounded overload as `BUSY` | [gate tests](../tests/sql_execution_test.cpp), [concurrency stress](../.github/workflows/ci.yml), [ADR 0002](adr/0002-concurrency.md) |
| WAL timer, disk I/O, SQL concurrency, and API middleware have seeded TSan workloads | [TSan workflow](../.github/workflows/ci.yml), [concurrency tests](../tests/sql_execution_test.cpp), [disk/WAL tests](../tests/regression/known_bugs_test.cpp), [API fuzzer](../tests/api/fuzz_api.py) |
| Environment config is validated once; invalid settings exit 78 and API keys are redacted | [startup tests](../tests/api/test_config_startup.py), [config implementation](../src/config.cpp), [config reference](CONFIG.md) |
| Shutdown toggles readiness, drains active requests, checkpoints, and syncs | [API lifecycle tests](../tests/api/test_config_startup.py), [container test](../scripts/demo_crash.sh), [main lifecycle](../src/main.cpp) |
| API, legacy aliases, response contracts, and route limits are tested against the real binary | [API tests](../tests/api/test_api.py), [OpenAPI validation](../tests/api/test_api.py), [OpenAPI document](openapi.yaml) |
| Dashboard data is sourced from the service and browser behavior is exercised against a real engine | [browser tests](../tests/dashboard/test_dashboard.py), [dashboard honesty guard](../scripts/check_dashboard_honesty.py), [CI](../.github/workflows/ci.yml) |
| Dashboard metrics do not use fabricated random values | [metrics regression](../tests/metrics_test.cpp), [honesty guard](../scripts/check_dashboard_honesty.py), [CI](../.github/workflows/ci.yml) |
| Docker image runs as UID 10001 with hardened container settings and is scanned for HIGH/CRITICAL vulnerabilities | [Dockerfile](../Dockerfile), [Compose file](../docker-compose.yml), [image CI](../.github/workflows/ci.yml) |
| The documented 60-second quickstart is run by Docker CI | [quickstart script](../scripts/quickstart.sh), [Docker CI job](../.github/workflows/ci.yml), [README](../README.md#60-second-quickstart) |
| The SQLite headline numbers were measured on the documented five-run workload | [benchmark runner](../tools/benchmark_sqlite.py), [methodology and results](BENCHMARKS.md#headline-comparison-with-sqlite) |
| Render Free storage and spin-down behavior are described with current official documentation | [README deployment note](../README.md#live-demo), [Render Free docs](https://render.com/docs/free), [Render disk docs](https://render.com/docs/disks) |
| The coverage workflow publishes measured gcovr reports for storage, index, WAL, and SQL source groups | [coverage job](../.github/workflows/ci.yml), [coverage policy](CONTRIBUTING.md) |
| The repository is licensed under MIT | [LICENSE](../LICENSE) |
