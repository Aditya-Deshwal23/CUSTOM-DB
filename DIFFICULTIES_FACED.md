# Engineering post-mortem

This is a regression-focused account of defects found while building the
storage engine. Each item names the observable failure and the test that now
guards it; tests are evidence that the regression is covered, not proof that no
related defects remain.

## 1. Guard pin leak caused false misses under eviction

**Symptom and discovery.** With a small buffer pool and enough B+ tree inserts
to split/evict pages, lookups could report absent keys even though the tree
contained them. The investigation found page pins surviving beyond the
operation's intended scope: leaked pins reduced the frames available for
replacement and left traversal with incomplete access under pressure.

**Correction.** Page guards now own their pins and frame latches through RAII;
move transfers ownership, and scope exit releases it. B+ tree operations avoid
retaining page guards while fetching unrelated pages.

**Regression coverage.** `BufferPoolGuardTest.GuardsOwnPinsAndMoveWithoutLeaking`
and `BPlusTreeTest.LookupsNeverFalseMissAfterManyInserts` cover guard ownership
and repeated lookups through eviction in
[buffer_pool_test.cpp](tests/buffer_pool_test.cpp) and
[known_bugs_test.cpp](tests/regression/known_bugs_test.cpp).

## 2. A split leaf linked to itself

**Symptom and discovery.** A range scan could fail to terminate after a leaf
split because a new leaf's next-page link pointed to itself instead of the
former successor. The search path could still find point keys, so a range scan
after a split was needed to expose the cycle.

**Correction.** Split code preserves the old successor on the new right leaf
and links the left leaf to that new page. Structural validation checks leaf
ordering and linkage.

**Regression coverage.** `BPlusTreeTest.RangeScanTerminatesAfterSplit` checks
completion and returned rows after splitting in
[known_bugs_test.cpp](tests/regression/known_bugs_test.cpp); seeded oracle and
range tests are also in [b_plus_tree_test.cpp](tests/b_plus_tree_test.cpp).

## 3. Appending behind a torn WAL tail hid later commits

**Symptom and discovery.** Recovery correctly stopped at a short or
CRC-invalid record, but leaving the invalid bytes in place meant a later
append would sit beyond the first invalid record. The next recovery would
stop at the old tail and never reach the new commit.

**Correction.** Recovery truncates/replaces the WAL to a valid header only
after replay and database sync succeed, so the next transaction appends to a
valid stream. Replaying full-page images remains idempotent if recovery is
interrupted.

**Regression coverage.** `WalTest.RecordAppendedAfterTornTailIsRecoverable`
and `WalTest.RecoveryCanBeInterruptedAndRetried` cover append-after-tail and
recovery retry in [known_bugs_test.cpp](tests/regression/known_bugs_test.cpp).

## 4. Reader preference could starve statement writers

**Symptom and discovery.** A generic shared mutex does not promise that a
waiting exclusive writer will beat a continuous stream of new shared readers.
The concurrency contract requires bounded admission, so unbounded writer
wait was unacceptable.

**Correction.** The executor uses a writer-preferring gate: once a writer is
queued, later readers wait. Queue and admission bounds return typed `BUSY`
errors rather than allowing unbounded growth.

**Regression coverage.** `ConcurrencyTest.WriterPreferringGateBoundsWriterWait`
keeps eight readers acquiring repeatedly and checks the writer completes;
`ConcurrencyTest.GateTimeoutAndQueueOverflowReturnBusy` verifies overload
behavior in [sql_execution_test.cpp](tests/sql_execution_test.cpp).

## 5. TSan exposed races hidden by functional tests

**Symptom and discovery.** Sequential tests could not exercise overlapping
access among the periodic WAL sync worker, SQL readers/writers, page I/O, and
request middleware. ThreadSanitizer runs and seeded stress tests exposed
shared-state and lifetime boundaries that needed explicit synchronization.
The resulting coverage is split by concurrent component instead of relying on
one broad test.

**Correction.** The final design documents a ranked lock order, keeps disk/WAL
I/O outside the buffer-pool page-table mutex, uses atomics or narrowly scoped
locks for metrics and middleware counters, and keeps statement admission
separate from page-frame synchronization. See
[CONCURRENCY.md](docs/CONCURRENCY.md).

**Regression coverage.** CI runs `WalTest.NormalModeSyncThreadTsanClean`,
`ConcurrencyTest.ReaderWriterTsanClean`,
`ConcurrencyTest.ExecutorStatementGateTsanClean`,
`ConcurrencyTest.SqlExecutorStatementGateTsanStress`, and
`DiskManagerTest.ConcurrentDistinctPageIo` under TSan; API middleware also
receives concurrent seeded input. Test implementations are in
[known_bugs_test.cpp](tests/regression/known_bugs_test.cpp),
[sql_execution_test.cpp](tests/sql_execution_test.cpp), and
[disk_manager_test.cpp](tests/disk_manager_test.cpp); CI wiring is in
[ci.yml](.github/workflows/ci.yml).

## What this changed about the project

The recurring lesson was to test invariants at their ownership boundaries:
pins at guard destruction, leaf links after structural changes, WAL append
after invalid tails, writer progress under arrival pressure, and shared state
under actual concurrent execution. The evidence index is
[docs/CLAIMS.md](docs/CLAIMS.md); supported behavior and explicit omissions
are in [docs/LIMITATIONS.md](docs/LIMITATIONS.md).
