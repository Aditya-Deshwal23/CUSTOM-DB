# Changelog

Changes are grouped by version. This file records implemented behavior; see
[`releases/v1.0.0.md`](releases/v1.0.0.md) for the prepared initial release
notes and the CI workflow for build/release evidence.

## v1.0.0

- C++17 page-oriented storage, guarded buffer pool, table heap, free-page
  allocator, and B+ tree indexes.
- Versioned little-endian database and WAL formats with CRC32C, statement
  rollback, redo recovery, checkpointing, and integrity checking.
- SQL lexer/parser, bounded SQL subset, statement-level planner/executor,
  `EXPLAIN ANALYZE`, and SQLite differential testing.
- Crow API, OpenAPI contract, JSON logs, validated environment configuration,
  Prometheus metrics, and live vanilla-JS dashboard.
- Graceful shutdown, operational CLI commands, hardened Docker/Compose and
  Render configuration, multi-architecture GHCR release workflow.
- Debug/Release, ASan/UBSan, TSan, crash-recovery, API, dashboard, and coverage
  workflows; local benchmark methodology and limitations documented.

This changelog does not claim that a tag or hosted release has been published.
