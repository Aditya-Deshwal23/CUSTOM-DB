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
| Catalogs grow beyond one page, reopen after restart, and retain foreign-key behavior | [500-table restart/drop regression](../tests/sql_execution_test.cpp), [catalog format specification](FILE_FORMAT.md) |
| The lexer/parser and SQL executor implement the documented subset | [parser tests](../tests/sql_parser_test.cpp), [execution tests](../tests/sql_execution_test.cpp), [SQL reference](SQL_REFERENCE.md) |
| SQL statements can be differentially checked against SQLite for a seeded supported subset | [differential test](../tools/difftest.py), [CI job](../.github/workflows/ci.yml), [semantic boundaries](SQL_EXECUTION.md) |
| Each write statement commits full-page images and a WAL COMMIT before acknowledgement | [WAL commit tests](../tests/regression/known_bugs_test.cpp), [WAL design](WAL_AND_RECOVERY.md), [ADR 0001](adr/0001-wal-design.md) |
| Single-request batches apply up to 50 statements atomically with indexed failure reporting | [batch API tests](../tests/api/test_api.py), [batch rollback and crash regressions](../tests/sql_execution_test.cpp) and [../tests/regression/known_bugs_test.cpp](../tests/regression/known_bugs_test.cpp), [OpenAPI contract](openapi.yaml) |
| Recovery handles torn tails and replays committed images before the API starts | [torn-tail/recovery tests](../tests/regression/known_bugs_test.cpp), [startup implementation](../src/main.cpp), [recovery design](WAL_AND_RECOVERY.md) |
| Linux hard-kill recovery and container same-volume restart are exercised | [CI crash jobs](../.github/workflows/ci.yml), [nightly crash job](../.github/workflows/nightly.yml), [container proof](../scripts/demo_crash.sh) |
| The recorded container run recovered all 1,752 acknowledged writes | [captured recovery log and row count](assets/recovery.cast), [count assertion in demo](../scripts/demo_crash.sh) |
| The statement gate prefers writers and reports bounded overload as `BUSY` | [gate tests](../tests/sql_execution_test.cpp), [concurrency stress](../.github/workflows/ci.yml), [ADR 0002](adr/0002-concurrency.md) |
| WAL timer, disk I/O, SQL concurrency, and API middleware have seeded TSan workloads | [TSan workflow](../.github/workflows/ci.yml), [concurrency tests](../tests/sql_execution_test.cpp), [disk/WAL tests](../tests/regression/known_bugs_test.cpp), [API fuzzer](../tests/api/fuzz_api.py) |
| Environment config is validated once; invalid settings exit 78 and API keys are redacted | [startup tests](../tests/api/test_config_startup.py), [config implementation](../src/config.cpp), [config reference](CONFIG.md) |
| Shutdown toggles readiness, drains active requests, checkpoints, and syncs | [API lifecycle tests](../tests/api/test_config_startup.py), [container test](../scripts/demo_crash.sh), [main lifecycle](../src/main.cpp) |
| API, legacy aliases, response contracts, and route limits are tested against the real binary | [API tests](../tests/api/test_api.py), [OpenAPI validation](../tests/api/test_api.py), [OpenAPI document](openapi.yaml) |
| The inspector snapshot uses ETags, the compact buffer-pool payload stays within 20 KiB, and full-page scans do not change pool residency | [inspector API tests](../tests/api/test_inspector.py), [OpenAPI contract](openapi.yaml) |
| Warm `/api/v1/snapshot` p99 stays below 2 ms on the Release CI runner | [snapshot benchmark](../tools/benchmark_snapshot.py), [Release CI job](../.github/workflows/ci.yml) |
| Page inspection decodes each persistent page type, typed heap tuples, and SQL index keys; damaged page and WAL checksums are reported without crashing the service | [page, index, and WAL inspector tests](../tests/api/test_inspector.py), [OpenAPI contract](openapi.yaml) |
| Inspector endpoints provide keyset-paginated typed rows, index-tree traversal and lookup traces, WAL/free-list records, history, and slow-query samples | [inspector API tests](../tests/api/test_inspector.py), [OpenAPI contract](openapi.yaml) |
| The seeded HTTP fuzzer exercises query bodies and malformed inspector parameters in CI without a server crash | [API fuzzer](../tests/api/fuzz_api.py), [CI fuzz job](../.github/workflows/ci.yml) |
| Query responses report statement-local page touches and write-commit metadata | [query metadata test](../tests/api/test_inspector.py), [OpenAPI contract](openapi.yaml) |
| Dashboard data is sourced from the service and browser behavior is exercised against a real engine | [browser tests](../tests/dashboard/test_dashboard.py), [dashboard honesty guard](../scripts/check_dashboard_honesty.py), [CI](../.github/workflows/ci.yml) |
| Dashboard metrics do not use fabricated random values | [metrics regression](../tests/metrics_test.cpp), [honesty guard](../scripts/check_dashboard_honesty.py), [CI](../.github/workflows/ci.yml) |
| Docker image runs as UID 10001 with hardened container settings and is scanned for HIGH/CRITICAL vulnerabilities | [Dockerfile](../Dockerfile), [Compose file](../docker-compose.yml), [image CI](../.github/workflows/ci.yml) |
| The current container image measured 56.9 MB, down from a 146 MB pre-packaging baseline on the same local Docker builder | [measured comparison and reproduction command](BENCHMARKS.md#container-image-size), [CI image-size report](../.github/workflows/ci.yml) |
| The documented container quickstart is exercised by Docker CI | [quickstart script](../scripts/quickstart.sh), [Docker CI job](../.github/workflows/ci.yml), [README](../README.md#quickstart) |
| The SQLite headline numbers were measured on the documented five-run workload | [benchmark runner](../tools/benchmark_sqlite.py), [methodology and results](BENCHMARKS.md#headline-comparison-with-sqlite) |
| Render Free storage and spin-down behavior are described with current official documentation | [README deployment note](../README.md#live-demo), [Render Free docs](https://render.com/docs/free), [Render disk docs](https://render.com/docs/disks) |
| The coverage workflow publishes measured gcovr reports for storage, index, WAL, and SQL source groups | [coverage job](../.github/workflows/ci.yml), [coverage policy](CONTRIBUTING.md) |
| The repository is licensed under MIT | [LICENSE](../LICENSE) |
| GitHub Actions use read-only defaults, job timeouts, PR concurrency cancellation, and SHA-pinned third-party actions | [workflow policy check](../scripts/check_workflow_controls.py), [CI and nightly workflows](../.github/workflows/ci.yml), [nightly workflow](../.github/workflows/nightly.yml) |
| Test dependencies are hash-locked and update through weekly grouped Dependabot PRs | [hashed API requirements](../tests/api/requirements.txt), [hashed dashboard requirements](../tests/dashboard/requirements.txt), [Dependabot configuration](../.github/dependabot.yml) |
| Linux Release binaries are built with hardening flags and CI verifies PIE, full RELRO, stack canaries, NX, and FORTIFY | [hardening configuration](../CMakeLists.txt), [binary verifier](../scripts/check_binary_hardening.sh), [hardening CI job](../.github/workflows/ci.yml) |
| Informational cppcheck and clang-tidy scans cover the core build; core targets compile with conversion and shadow warnings | [CMake warning configuration](../CMakeLists.txt), [static-analysis CI job](../.github/workflows/ci.yml) |
| Container and Compose configuration is resource-bounded and scanned with Trivy, hadolint, and Dockle | [Compose limits](../docker-compose.yml), [container CI jobs](../.github/workflows/ci.yml) |
| OpenAPI-driven API fuzzing and OWASP ZAP baseline/API scans run against an isolated container | [OpenAPI contract](openapi.yaml), [ZAP rules](../.zap/rules.tsv), [dynamic security CI job](../.github/workflows/ci.yml) |
| CodeQL scans C++, Python, and JavaScript; high/critical open alerts on the analyzed ref fail the workflow | [CodeQL workflow](../.github/workflows/codeql.yml) |
| OpenSSF Scorecard results are published as SARIF and linked from the README badge | [Scorecard workflow](../.github/workflows/scorecard.yml), [README](../README.md) |
| Gitleaks runs in CI and as a pre-commit hook; C++ formatting and whitespace checks have a local pre-commit configuration | [gitleaks CI job](../.github/workflows/ci.yml), [pre-commit configuration](../.pre-commit-config.yaml) |
| The release workflow signs images with GitHub OIDC, verifies the digest, and attaches an SPDX SBOM and BuildKit provenance-bearing OCI index | [release workflow](../.github/workflows/release.yml), [README verification](../README.md#quickstart), [release runbook](RELEASING.md) |
| Security response targets, STRIDE scope, operator runbooks, and manual repository settings are documented | [root security policy](../SECURITY.md), [threat model](THREAT_MODEL.md), [operations runbook](OPERATIONS.md), [release checklist](RELEASING.md) |
| Contribution intake, ownership, and community expectations have repository templates and policies | [bug template](../.github/ISSUE_TEMPLATE/bug.yml), [feature template](../.github/ISSUE_TEMPLATE/feature.yml), [pull request template](../.github/PULL_REQUEST_TEMPLATE.md), [CODEOWNERS](../.github/CODEOWNERS), [Code of Conduct](../CODE_OF_CONDUCT.md) |
