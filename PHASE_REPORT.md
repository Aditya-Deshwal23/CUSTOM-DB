# Phase 0 report

## Changes

- Added repository instructions, pinned CMake dependencies, core/server/test/benchmark targets, sanitizer and warning options, developer tooling, and CI workflows.
- Added eight disabled regression cases and a runner that prints their current status without making ordinary CTest runs fail.
- Replaced unsupported README and architecture claims with implemented/prototype/not-implemented status; added claim, file-format, and concurrency notes.
- Marked synthetic metrics as `synthetic_workload`, removed unsupported metrics, and labeled dashboard simulation and traces.
- Did not fix storage-engine behavior. Storage-core edits are warning fixes needed for warning-clean builds; pre-existing API startup and request-validation edits were retained.

## Validation and observed output

| Command | Result |
|---|---|
| `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DRELDB_WERROR=ON && cmake --build build-release --parallel && ctest --test-dir build-release --output-on-failure` | Passed; Release build completed and CTest reported `100% tests passed out of 1`. |
| `scripts/check.sh` | Exit 0; Debug and ASan+UBSan CTest each reported `100% tests passed out of 1`. The TSan stress run detected the known race at `BPlusTreePage::IncreaseSize(int)` (`include/b_plus_tree_page.h:37`). |
| `cmake -S . -B build-prod -DCMAKE_BUILD_TYPE=Release -DRELDB_BUILD_TESTS=OFF -DRELDB_BUILD_BENCHMARKS=OFF -DRELDB_WERROR=ON && cmake --build build-prod --parallel` | Passed; production configuration built `reldb_core` and `relational_db`. |
| `scripts/run_known_bugs.sh build/reldb_regression_tests` | Printed 4 failures, 2 platform skips, and 2 passes on macOS. Details below. |
| `scripts/verify_resume_claims.sh` | Passed; verified health, dashboard assets, metrics source, integer INSERT/SELECT, invalid request handling, and SIGTERM shutdown. |
| `hadolint Dockerfile .devcontainer/Dockerfile` | Passed. |
| `git diff --check` | Passed. |

### Known-regression results on this macOS host

| Test | Result |
|---|---|
| `PersistenceTest.DISABLED_RestartKeepsData` | Failed: key 5 not found after reopen. |
| `DurabilityTest.DISABLED_HardKillKeepsAckedWrites` | Skipped: Linux-only. |
| `BPlusTreeTest.DISABLED_LookupsNeverFalseMissAfterManyInserts` | Failed: 3,341 misses, first at lookup 64, seed 1337. |
| `BPlusTreeTest.DISABLED_RangeScanTerminatesAfterSplit` | Skipped: Linux-only resource-limited process test. |
| `WalTest.DISABLED_RecordAppendedAfterTornTailIsRecoverable` | Failed: recovery returned 2 instead of 3 records. |
| `ConcurrencyTest.DISABLED_ReaderWriterTsanClean` | Passed without TSan; the TSan run separately reported a data race in `IncreaseSize(int)`. Seed: 20261030. |
| `SqlTest.DISABLED_DocumentedExamplesWork` | Failed: `SELECT * FROM Users` is not supported. |
| `MetricsTest.DISABLED_NoFabricatedValues` | Passed after the honesty patch removed the fabricated metrics. |

The measured B+ tree miss count differs from the original report (3,341, first lookup 64 rather than 3,387/54). Values are recorded as observed, not adjusted to match an expected output. The metrics test passes intentionally after the required fix, so the eight cases do not all fail.

## Limitations

- Linux-only hard-kill and resource-limited range regressions were not runnable on this macOS host.
- Docker image build and actual Linux CI were not run locally; the Docker daemon is unavailable. CI is configured to run both.
- TSan does not pass cleanly by design in this phase: it reproduces the documented race for a later fix.
