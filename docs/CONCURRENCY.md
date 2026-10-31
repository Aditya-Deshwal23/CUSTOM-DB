# Concurrency model

## Statement contract

The engine uses a single-writer / multiple-reader gate at statement granularity.
`SELECT`, `SHOW TABLES`, and `DESCRIBE` acquire the gate shared. `INSERT`,
`UPDATE`, `DELETE`, DDL, and `CHECKPOINT` acquire it exclusively. The gate
remains held until a write statement's WAL commit (and any automatic checkpoint
attempt) has completed. An automatic checkpoint failure is logged without
changing the successful result of the already-durable statement. A statement is
atomic, durable, and serializable relative to other statements. Version 1 has
no multi-statement transactions.

The gate prefers waiting writers over new readers. Admission is bounded to 64
active statements and 128 queued statements by default. Waiting longer than
`STATEMENT_GATE_TIMEOUT_MS` (1,000 ms) or exceeding the queue returns
`DbError{BUSY, ...}`. The HTTP API maps `BUSY` to 503 and sends
`Retry-After: 1`. The HTTP API's cooperative statement deadline is
`STATEMENT_TIMEOUT_MS` (2,000 ms by default; configurable up to 600,000 ms).
Low-level `StatementScope` callers default to the 5,000 ms compile-time
constant. Executor loops check the cancellation token and report
`QUERY_TIMEOUT`.

Every write appends its page images and COMMIT record to the WAL and syncs the
WAL before returning, regardless of the configured background-sync mode. The
exclusive statement gate is therefore still held across the complete durable
commit boundary. WAL modes may control background synchronization, but cannot
weaken the statement-commit guarantee.

## Lock order

The single lock order is:

1. statement gate
2. catalog
3. buffer-pool page-table mutex
4. frame latch
5. disk/WAL mutex

Debug builds track ranked locks in thread-local state and abort immediately if
a thread tries to acquire a lower-ranked lock while holding a higher-ranked
one. The statement gate participates in the checker, as do the catalog gate,
buffer-pool mutex, frame latches, disk metadata/descriptor mutexes, and WAL
mutex. Disk/WAL I/O entry points also assert that the buffer-pool mutex is not
held. Equal-rank locks are allowed, but code must not wait for another page
while retaining a frame latch.

The HTTP API's per-IP token buckets use a separate mutex only while updating
the bounded in-memory bucket map. Middleware releases it before route handlers
can enter the statement gate or perform storage I/O. HTTP counters and server
readiness flags are atomics; no API mutex is nested with the storage lock order.
The middleware's active-request drain counter is atomic; its condition-variable
mutex is used only while waiting for zero active handlers and is released before
the server is stopped or storage is checkpointed. New work receives 503 once
draining begins.

Metrics use fixed-label arrays and relaxed atomic counters/gauges/histogram
buckets. Recording does not allocate or acquire a registry-wide mutex; a scrape
is a weakly consistent snapshot and never holds a metrics lock while reading
engine state. Metrics are independent of the ranked storage-lock order.

No disk read, write, extension, or sync occurs under the buffer-pool page-table
mutex. A fetch reserves and publishes a frame under that mutex, performs disk
I/O after releasing it, then publishes the ready frame while acquiring the
frame latch in the documented order. Buffer-pool waits release the page-table
mutex. Guards release their frame latch before returning the pin under the
page-table mutex. On POSIX, page I/O uses positional calls rather than a global
I/O mutex; Windows uses a ranked descriptor mutex.

Page latches protect an individual frame while its contents are read or
changed, and while frame state is used for I/O/eviction. They are not a
concurrent B+ tree writer protocol. The B+ tree requires no latch crabbing
under the statement gate: its descent stores page IDs, page guards are released
before following/fetching another page, and split/rebalance edits copy page
contents and write affected pages one at a time. Index iterators release the
leaf guard before returning a row to the executor. Direct B+ tree and
`TableHeap` callers outside `Executor` must provide equivalent statement-level
synchronization.

## Admission, cancellation, and statement scope

`StatementScope` is RAII: it acquires the shared or exclusive gate with the
earlier of the statement deadline and gate timeout, owns the cancellation
token, and releases the gate on every exit path. SQL DML and DDL run within one
WAL transaction; failures roll back before the scope releases the gate.
Cancellation is cooperative and checked in scan/execute loops; it does not
interrupt a blocking operating-system I/O call.

The gate is per `Executor`, shared with its SQL engine. The separate catalog
gate is ranked after the statement gate so catalog reload/persistence cannot
race readers. The buffer pool still provides internal page safety, but direct
low-level mutation APIs do not create an SQL statement or transaction.

## Buffer pool, table heap, and WAL

The buffer-pool mutex protects the page table, frame allocation, pin counts,
and I/O-state transitions. The replacer mutex is acquired only while holding
the pool mutex. Frame latches are acquired only after releasing the pool mutex,
except for publishing a just-loaded frame while the pool mutex is held; that
frame is still marked loading and cannot be fetched by another operation.
Disk I/O always occurs outside the pool mutex.

`TableHeap` uses page guards for headers and rows. Its iterator copies one page
at a time and releases the guard before returning a row; executor statement
scopes prevent scans from overlapping heap mutations. The B+ tree similarly
relies on the executor gate rather than tree-level crabbing.

The buffer pool permits one active write transaction. Before-images are
captured on first mutation, and active transaction pages cannot be evicted or
flushed. WAL commit takes the WAL mutex only after page work; write-back checks
the committed page LSN before database I/O. Checkpointing is an exclusive
statement and performs page I/O and sync outside the pool mutex. No code holds
the WAL mutex while acquiring the pool mutex or a frame latch.

## Verification

The CI TSan job runs `ConcurrencyTest.ReaderWriterTsanClean`,
`ConcurrencyTest.ExecutorStatementGateTsanClean`, and the seeded SQL
statement-gate stress test. The SQL stress uses eight threads, three tables,
random `SELECT`/`INSERT`/`UPDATE`/`DELETE` statements, and checks that every
observed balance sum remains zero. PR TSan runs it for 10 seconds;
`RELDB_CONCURRENCY_STRESS_SECONDS=60` enables the nightly duration. Each
workload prints its seed. The writer-preference test keeps eight readers in a
tight acquire/release loop and bounds writer wait to two seconds.
`ConcurrencyTest.GateTimeoutAndQueueOverflowReturnBusy` verifies the typed
`BUSY` result for a timed-out waiter and a full admission queue.

`BufferPoolStressTest.SixteenThreadsOneMillionVerifiedOperations` and
`DiskManagerTest.ConcurrentDistinctPageIo` separately exercise buffer-pool
and disk-manager concurrency. These tests do not make unsynchronized direct
`TableHeap` or B+ tree use safe.
The CI TSan job also sends 200 seeded concurrent malformed/valid HTTP requests
through the API rate limiter and SQL parser using `tests/api/fuzz_api.py`.
