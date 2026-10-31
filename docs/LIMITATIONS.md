# Limitations

CUSTOM-DB is a single-process learning and demonstration engine. The
limitations below are part of the current design, not configuration switches.

## Transactions and concurrency

- One writer at a time; readers may share the statement gate. Writes are
  serialized for a complete statement and its WAL commit.
- No MVCC, snapshot isolation, concurrent writers, or multi-statement
  transactions. A SQL write statement is one auto-commit unit.
- The WAL stores full-page after-images, so a small logical change can write a
  full 4096-byte physical page per changed page. This is write amplification.
- Direct low-level `TableHeap`/B+ tree mutation outside `Executor` requires the
  caller to provide equivalent statement-level synchronization.

## SQL and storage

- SQL is a deliberately bounded subset; there are no joins, window functions,
  subqueries, composite SQL indexes, table-level/composite foreign keys, or
  general standards-level compatibility.
- The serialized SQL catalog must fit in one 4080-byte page body.
- Tuples are limited to approximately 4 KB (maximum encoded tuple size 4,067
  bytes). There are no overflow pages.
- Sort and hash aggregation are in-memory and constrained by configured row
  limits; there is no external sort or spill-to-disk path.
- VARCHAR indexes support equality candidate lookup, but not ordered range
  scans.
- No replication, clustering, online backup protocol, or cross-process
  multi-writer coordination is implemented.

## Service and deployment

- The service is a single Crow process; there is no connection pool, separate
  frontend service, or horizontal replication layer.
- Cooperative statement timeouts do not interrupt a single blocking disk
  operation.
- On graceful shutdown, readiness changes and new work is rejected while
  current handlers drain. Crow's public API also stops its I/O services, so
  accepting sockets are closed after that drain.
- Render Free has ephemeral local storage and spins down when idle. A Free
  deployment is not durable storage; persistent disks require a paid instance.

For detailed semantics see [SQL_REFERENCE.md](SQL_REFERENCE.md),
[CONCURRENCY.md](CONCURRENCY.md), and [WAL_AND_RECOVERY.md](WAL_AND_RECOVERY.md).
