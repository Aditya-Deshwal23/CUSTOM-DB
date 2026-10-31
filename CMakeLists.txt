cmake_minimum_required(VERSION 3.20)
project(CustomRelationalDB VERSION 1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RELDB_WERROR "Treat project compiler warnings as errors" OFF)
option(RELDB_BUILD_TESTS "Build GoogleTest regression tests" ON)
option(RELDB_BUILD_BENCHMARKS "Build Google Benchmark targets" ON)
option(RELDB_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
set(RELDB_SANITIZE "none" CACHE STRING "Sanitizer: address, undefined, thread, or none")
set_property(CACHE RELDB_SANITIZE PROPERTY STRINGS address undefined thread none)

if(NOT RELDB_SANITIZE MATCHES "^(address|undefined|thread|none)$")
  message(FATAL_ERROR "RELDB_SANITIZE must be address, undefined, thread, or none")
endif()

include(FetchContent)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG 147f7225a96d45a2807a64e443177f621844e51c
  GIT_SHALLOW FALSE
)
FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG 05f7f337d3ab4fb8ba2a50865d3824628c574ca1
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")
FetchContent_MakeAvailable(crow)
include(cmake/patch_crow_options.cmake)

if(RELDB_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(RELDB_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG 344117638c8ff7e239044fd0fa7085839fc03021
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(benchmark)
endif()

if(RELDB_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "RELDB_BUILD_FUZZERS requires Clang with libFuzzer support")
  endif()
  add_executable(sql_grammar_fuzz EXCLUDE_FROM_ALL
    tools/sql_grammar_fuzz.cpp
    src/sql_parser.cpp
  )
  target_include_directories(sql_grammar_fuzz PRIVATE include)
  target_compile_options(sql_grammar_fuzz PRIVATE
    -Wall -Wextra -Wpedantic -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
  )
  target_link_options(sql_grammar_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(reldb_core STATIC
  src/metrics/metrics.cpp
  src/crc32c.cpp
  src/disk_manager.cpp
  src/concurrency.cpp
  src/slotted_page.cpp
  src/clock_replacer.cpp
  src/page_allocator.cpp
  src/buffer_pool_manager.cpp
  src/storage/tuple.cpp
  src/storage/table_heap.cpp
  src/catalog.cpp
  src/sql_executor.cpp
  src/b_plus_tree.cpp
  src/sql_parser.cpp
  src/executor.cpp
  src/wal.cpp
)
target_include_directories(reldb_core PUBLIC include)
target_link_libraries(reldb_core PUBLIC Threads::Threads)

add_executable(relational_db src/main.cpp)
execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE RELDB_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT RELDB_GIT_SHA)
  set(RELDB_GIT_SHA "unknown")
endif()
string(TIMESTAMP RELDB_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
target_sources(relational_db PRIVATE
  src/api/server.cpp
  src/api/routes.cpp
  src/api/middleware.cpp
  src/api/json.cpp
  src/demo/load_generator.cpp
)
target_include_directories(relational_db PRIVATE include)
target_include_directories(relational_db SYSTEM PRIVATE
  ${asio_SOURCE_DIR}/asio/include
  ${crow_SOURCE_DIR}/include
)
target_compile_definitions(relational_db PRIVATE
  CROW_STANDALONE
  ASIO_STANDALONE
  RELDB_GIT_SHA="${RELDB_GIT_SHA}"
  RELDB_BUILD_TIME="${RELDB_BUILD_TIME}"
)
target_link_libraries(relational_db PRIVATE reldb_core Threads::Threads)
if(WIN32)
  target_link_libraries(relational_db PRIVATE ws2_32 mswsock)
endif()

function(reldb_configure_project_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(RELDB_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(RELDB_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(RELDB_SANITIZE STREQUAL "address")
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    elseif(RELDB_SANITIZE STREQUAL "undefined")
      target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=undefined)
    elseif(RELDB_SANITIZE STREQUAL "thread")
      target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
  endif()
endfunction()

if(RELDB_BUILD_TESTS)
  enable_testing()
  add_executable(reldb_regression_tests
    tests/b_plus_tree_test.cpp
    tests/regression/known_bugs_test.cpp
    tests/disk_manager_test.cpp
    tests/buffer_pool_test.cpp
    tests/storage_test.cpp
    tests/sql_parser_test.cpp
    tests/sql_execution_test.cpp
    tests/metrics_test.cpp
  )
  target_compile_definitions(reldb_regression_tests PRIVATE
    RELDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    RELDB_SERVER_BINARY="$<TARGET_FILE:relational_db>"
  )
  target_include_directories(reldb_regression_tests PRIVATE include tests)
  target_link_libraries(reldb_regression_tests PRIVATE reldb_core GTest::gtest_main)
  add_dependencies(reldb_regression_tests relational_db)
  add_test(NAME reldb_regression_tests COMMAND reldb_regression_tests)

  add_executable(reldb_disk_sync_probe tests/disk_sync_probe.cpp)
  target_include_directories(reldb_disk_sync_probe PRIVATE include)
  target_link_libraries(reldb_disk_sync_probe PRIVATE reldb_core)
endif()

if(RELDB_BUILD_BENCHMARKS)
  add_executable(reldb_benchmarks bench/b_plus_tree_bench.cpp)
  target_link_libraries(reldb_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_disk_benchmarks bench/bench_disk.cpp)
  target_link_libraries(reldb_disk_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_buffer_pool_benchmarks bench/bench_buffer_pool.cpp)
  target_link_libraries(reldb_buffer_pool_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_storage_benchmarks bench/storage_bench.cpp)
  target_link_libraries(reldb_storage_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_wal_benchmarks bench/wal_bench.cpp)
  target_link_libraries(reldb_wal_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_sql_execution_benchmarks bench/sql_execution_bench.cpp)
  target_link_libraries(reldb_sql_execution_benchmarks PRIVATE reldb_core)
  add_executable(reldb_concurrency_benchmarks bench/concurrency_bench.cpp)
  target_link_libraries(reldb_concurrency_benchmarks PRIVATE reldb_core Threads::Threads)
  add_executable(reldb_metrics_overhead_benchmarks bench/metrics_overhead_bench.cpp)
  target_link_libraries(reldb_metrics_overhead_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_sql_parse_benchmarks bench/sql_parse_bench.cpp)
  target_link_libraries(reldb_sql_parse_benchmarks PRIVATE reldb_core benchmark::benchmark)
  add_executable(reldb_ycsb_benchmarks bench/ycsb_bench.cpp)
  target_link_libraries(reldb_ycsb_benchmarks PRIVATE reldb_core Threads::Threads)
endif()

set(project_targets reldb_core relational_db)
if(RELDB_BUILD_TESTS)
  list(APPEND project_targets reldb_regression_tests)
  list(APPEND project_targets reldb_disk_sync_probe)
endif()
if(RELDB_BUILD_BENCHMARKS)
  list(APPEND project_targets reldb_benchmarks reldb_disk_benchmarks reldb_buffer_pool_benchmarks
       reldb_storage_benchmarks reldb_wal_benchmarks reldb_sql_execution_benchmarks)
  list(APPEND project_targets reldb_concurrency_benchmarks)
  list(APPEND project_targets reldb_metrics_overhead_benchmarks)
  list(APPEND project_targets reldb_sql_parse_benchmarks)
  list(APPEND project_targets reldb_ycsb_benchmarks)
endif()
foreach(target IN LISTS project_targets)
  reldb_configure_project_target(${target})
endforeach()
