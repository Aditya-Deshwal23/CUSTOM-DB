# Contributing

Contributions should preserve the project's scope: C++17, Crow, vanilla
JavaScript, and one Docker service deployed on Render. See the root
[copilot instructions](../.github/copilot-instructions.md) for additional
repository-specific requirements.

## Local checks

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DRELDB_WERROR=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DRELDB_WERROR=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure

hadolint Dockerfile
docker compose config --quiet
bash -n scripts/demo_crash.sh scripts/quickstart.sh
```

For API and dashboard tests, install the pinned Python dependencies listed in
`tests/api/requirements.txt` and `tests/dashboard/requirements.txt`. For
sanitizer commands and the CI coverage report, inspect
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

The gcovr job groups the existing source layout as follows:
`src/storage/` plus `src/slotted_page.cpp` (storage), `src/b_plus_tree.cpp`
and `src/catalog.cpp` (index), `src/wal.cpp` and `src/crc32c.cpp` (WAL), and
`src/sql_parser.cpp`, `src/sql_executor.cpp`, and `src/executor.cpp` (SQL).
The project target is at least 80% line coverage in each group. CI publishes
measured reports but does not claim the target is met until a run demonstrates
it.

## Change expectations

- Add or update a focused regression test for behavior changes. Concurrency
  stress tests print their deterministic seed.
- Preserve typed `DbError` values, RAII page guards, explicit-endian storage
  formats, and the documented lock order. Do not hide errors with broad
  catches.
- Do not add made-up metrics, benchmark values, or production claims.
  Update [CLAIMS.md](CLAIMS.md) with evidence for any new user-facing claim.
- Update the relevant architecture, file-format, WAL, SQL, config, or
  limitation documentation when behavior changes.
- Keep formatting and compiler warnings clean; run the smallest applicable
  tests locally and rely on the CI matrix for full validation.

## Pull requests

Describe the behavior change, tests run, platform, and any known caveats.
Benchmark claims must include reproduction commands, workload, host, and
measurement path; local observations must not be presented as guarantees.
