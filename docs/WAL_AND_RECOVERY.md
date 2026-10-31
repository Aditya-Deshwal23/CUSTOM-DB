# WAL and recovery

The WAL defaults to `<DB_PATH>-wal` and can be relocated with `WAL_PATH`; its byte layout is specified in
[FILE_FORMAT.md](./FILE_FORMAT.md). A statement is an auto-commit transaction.
Its first write-guard touch snapshots the page's body, type, flags, LSN, and
dirty bit. A failed statement restores those images and page-allocation
metadata. Frames touched by the active statement are not eligible for eviction
or write-back.

## Commit and checkpoint order

Before commit, page after-images are assigned LSNs and encoded with CRC32C.
The images and COMMIT record are assembled into one batch write and the WAL is
synced before every commit returns, independent of mode. `normal` additionally
runs a periodic sync thread; `off` disables that thread but does not disable
per-statement durability. A committed page is eligible for write-back only
after its COMMIT record is durable.

Checkpointing takes the buffer-pool checkpoint gate before flushing. It writes
all dirty pages, syncs the database, writes and syncs a CHECKPOINT record,
rewrites the WAL header with the next base LSN, and syncs the replacement WAL
and its parent directory. Transactions cannot begin between the database sync
and WAL truncation.

## Recovery

Startup performs recovery before constructing the serving executor. Recovery
validates the header, then scans the record stream in LSN order until the first
short, malformed, or CRC-invalid record. It replays every PAGE_IMAGE that has
a COMMIT in the valid prefix, in log order, syncs the database, and replaces
the WAL with a valid header. This also discards a torn tail so the next commit
is appendable. If recovery is interrupted, the original WAL remains available
and the same images are safely replayed on the next startup.

Recovery reports `records`, `commits`, `pages_replayed`, `truncated_bytes`,
and elapsed milliseconds in the `[WAL] recovery` log line.

## Crash-point table

`RELDB_FAILPOINT=<name>:kill` sends SIGKILL on Linux; `:exit` calls `_exit`;
`:throw` raises a typed I/O error. These are deterministic test hooks, not
production error handling.

| Failpoint | State at failure | Why recovery is correct |
|---|---|---|
| `page_image_before` | The current batch has not yet been written; earlier statements remain committed | No partial transaction record is visible; replay uses only committed valid records |
| `page_image_after` | The image was added to the in-memory commit batch, but that batch has not yet been written | As above; an unpersisted batch cannot be replayed |
| `commit_after` | The complete batch, including COMMIT, was written but has not yet been synced | The statement has not returned; recovery accepts it only if the complete records and CRCs survived |
| `wal_fsync_after` | The commit WAL batch has been synced | Its COMMIT is durable, so all of its page images are replayed |
| `writeback_mid` | A database page write is torn; the WAL COMMIT is already durable | The page checksum fails on read, and recovery rewrites the complete committed page image |
| `checkpoint_db_fsync_after` | All database pages are synced; the old WAL is still present | Replaying already-applied images is idempotent |
| `checkpoint_before_truncate` | The database is synced and a CHECKPOINT record is appended/synced; WAL replacement has not begun | Recovery can replay the previous committed page images before truncating |
| `wal_before_truncate` | The database is synced after recovery replay; the WAL still contains its pre-recovery records | The next startup repeats the same idempotent replay |
| `recovery_mid` | At least one committed page image has been applied; the WAL has not been truncated | Recovery repeats all committed images, then syncs and truncates only after replay completes |

The target for `writeback_mid` is the next page write. Failpoints surrounding
PAGE_IMAGE records run for each image in the commit batch.

## Integrity check

`Executor::IntegrityCheck()` reads every logical page through the disk/buffer
manager (thereby checking physical page checksums), runs
`BPlusTree::Validate()`, and verifies that heap rows and index RIDs agree in
both directions. The `--check` one-off command runs this check after startup
recovery and exits nonzero when an integrity issue is found.

## Verification

The WAL regressions cover clean restart, rollback of touched and newly
allocated pages, append-after-torn-tail recovery, and interrupted recovery.
Linux CI runs the hard-kill crash harness (200 iterations per push/PR and
5,000 nightly); the Docker proof and its recording are linked from
[DEMO.md](./DEMO.md). Evidence is indexed in [CLAIMS.md](./CLAIMS.md).
