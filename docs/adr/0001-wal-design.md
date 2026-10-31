# ADR 0001: Redo-only full-page WAL

## Status

Accepted for the durable-storage phase.

## Decision

Use a SQLite-style redo-only write-ahead log. A write statement is one
auto-committed transaction. Each transaction keeps before-images in memory for
rollback and writes full-page after-images plus one COMMIT record to
`<DB_PATH>-wal`. Dirty pages touched by the active statement are no-steal: they
cannot be evicted or flushed before their transaction commits.

The commit sequence is:

1. Assign each page image an LSN and encode its page checksum.
2. Append every PAGE_IMAGE and one COMMIT record into a single buffered batch.
3. Write that batch to the WAL.
4. Sync the WAL batch before commit returns, independent of the selected
   background-sync mode.
5. Only then permit a committed page to be written back and acknowledge the
   statement.

Recovery validates the WAL header and record CRC32C values, replays page images
belonging to committed transactions in LSN order, syncs the database, and
replaces the WAL contents with a header whose base LSN follows the valid prefix.
Applying an image more than once is safe. Checkpointing holds an exclusive
buffer-pool gate, flushes dirty pages, syncs the database, then truncates and
syncs the WAL.

One writer is allowed. `Executor`'s statement gate serializes application
writers; the buffer pool independently rejects overlapping write transactions
and blocks new transactions for the entire checkpoint.

## Durability modes

| `DURABILITY` | Acknowledgement boundary | Trade-off |
|---|---|---|
| `full` (default) | WAL `fdatasync` (or the platform's equivalent) on every commit | Durable statement acknowledgements |
| `normal` | WAL sync on every commit and periodic background syncs | Same commit durability as `full`; background sync interval remains configurable |
| `off` | WAL sync on every commit; no periodic sync thread | Same commit durability as `full`; legacy mode name retained for benchmark compatibility |

`WAL_MAX_BYTES` controls the automatic checkpoint threshold (default 16 MiB).
An explicit checkpoint and clean server shutdown use the same checkpoint
protocol.

## Rejected alternative: ARIES

ARIES requires per-page LSN handling, undo logging, compensation log records,
and atomic treatment of structural modifications. Those mechanisms are
disproportionate for this engine's single-writer, statement-atomic workload and
would be substantially harder to verify than replaying full-page images.
Before-images are retained only in the active process for statement rollback;
they are not an on-disk undo log.

## Consequences

- WAL space is bounded by checkpointing and recovery truncation.
- Full-page images make replay independent of B+ tree operation semantics.
- No-steal plus per-commit WAL sync prevents an uncommitted or non-durable
  page from reaching the database file.
- Large statements can exhaust the buffer pool and fail with
  `DbError{TXN_TOO_LARGE}`; their in-memory before-images restore the statement.
- A crash during replay can repeat writes, so page-image application and
  database sync must remain idempotent.
- This design does not provide multi-writer concurrency or general SQL
  transaction isolation.
