# ADR 0002: Statement-level single-writer / multiple-reader concurrency

## Status

Accepted for version 1.

## Context

The SQL engine has one shared catalog, one buffer pool, and a write-ahead log
that commits full-page images for one statement at a time. Allowing writers to
modify B+ tree structure concurrently would require a structural logging and
recovery design beyond the current WAL. The standard library's
`std::shared_mutex` also does not promise writer preference across platforms.

## Decision

Use a writer-preferring shared/exclusive statement gate. Reads (`SELECT`,
`SHOW TABLES`, `DESCRIBE`) hold a shared scope; writes (`INSERT`, `UPDATE`,
`DELETE`, DDL, `CHECKPOINT`) hold an exclusive scope through WAL commit and any
automatic checkpoint attempt. A failed automatic checkpoint is logged and
does not convert an already committed write into a failed statement. A
statement is atomic, durable, and serializable against other statements.
Version 1 does not expose multi-statement transactions.

Bound admission to 64 active statements and 128 queued statements by default.
Gate admission times out after one second and returns typed `BUSY`; the API
returns HTTP 503 with `Retry-After: 1`. A five-second cooperative statement
deadline is checked in execution loops.

Enforce this lock order in Debug with a thread-local rank checker that aborts
on inversion:

`statement gate -> catalog -> buffer-pool page-table mutex -> frame latch -> disk/WAL mutex`

No disk I/O runs under the page-table mutex. Page latches protect a frame while
its content is accessed during I/O/eviction; they are not a B+ tree concurrency
protocol. With the statement gate serializing tree mutations against all
readers, B+ tree operations release each page guard before fetching another
page and edit split/rebalance pages sequentially. There is no latch crabbing.

Each write statement uses one WAL transaction and syncs its COMMIT before
returning; a failure restores before-images before releasing the exclusive
scope.

## Consequences

- Readers can overlap; writers cannot overlap readers or other writers.
- A long-running read delays writers, but writer preference prevents newly
  arriving readers from starving an admitted writer.
- The gate and bounded queue can return `BUSY` under overload rather than
  consuming unbounded threads or memory.
- Direct `TableHeap` and B+ tree callers must provide the same statement-level
  synchronization; those low-level APIs do not acquire the executor gate.
- Concurrent B+ tree writers, latch crabbing, and multi-statement transactions
  remain out of scope. Concurrent structural writes would require a different
  logging/recovery design, such as physiological WAL with ARIES-style recovery.

## Verification

`ConcurrencyTest.WriterPreferringGateBoundsWriterWait` drives eight readers in
a tight loop and bounds writer wait. `ConcurrencyTest.ReaderWriterTsanClean`
prints a deterministic seed and is run in CI's TSan job. The seeded SQL stress
uses eight threads against three tables with random reads and writes, checks
the zero-balance invariant after operations, runs for ten seconds in PR TSan,
and for sixty seconds nightly. The SQL test also asserts no buffer-pool pins
remain after the workload.
`ConcurrencyTest.GateTimeoutAndQueueOverflowReturnBusy` verifies the typed
`BUSY` result for a timed-out waiter and a full admission queue.
