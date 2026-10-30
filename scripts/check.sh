#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRELDB_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DRELDB_SANITIZE=address -DRELDB_WERROR=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
    -DRELDB_SANITIZE=thread -DRELDB_WERROR=ON
cmake --build build-tsan --parallel
GTEST_FILTER='ConcurrencyTest.DISABLED_ReaderWriterTsanClean' scripts/run_known_bugs.sh \
    build-tsan/reldb_regression_tests
