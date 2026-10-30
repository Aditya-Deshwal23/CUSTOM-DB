# Concurrency model and known gaps

## Buffer pool

`BufferPoolManager::pool_mutex_` protects the page table, free-frame/free-page
lists, frame IDs, and I/O-state transitions. The replacer has its own mutex;
when both are held, the order is pool mutex then replacer mutex. No disk read,
write, extension, or sync is performed while holding the pool mutex.

Each frame has a read/write latch and a condition variable. A fetch that finds
a `LOADING` or `WRITING_BACK` mapping waits on that frame's condition variable;
the wait releases the pool mutex, and all waiters for the page use the same
published frame. A miss reserves/remaps the frame and publishes its I/O state
under the mutex, then performs eviction write-back and page read outside it.
Failed I/O restores the old mapping and contents or returns a previously free
frame.

The server reads `BUFFER_POOL_FRAMES` (default 4096; environment values below
16 or outside the frame-ID range fail startup) and
`BUFFER_POOL_POLICY=clock|lruk` (default `lruk`, K=2). Explicit constructor
sizes remain available to tests, including smaller exhaustion/stress pools.
`/api/buffer_pool` returns the 64-bit counters, pinned-frame gauge, and at most
4096 frame snapshots.

Fetches pin before acquiring a page latch and acquire that latch only after
releasing the pool mutex. Guards release the page latch before decrementing the
pin under the pool mutex. `FlushFrame` takes the page latch and briefly reads
frame metadata under the pool mutex; the pool mutex is never held while waiting
for a page latch. `PageAllocator` accesses page-zero metadata and page contents
through guards. Page retirement marks the frame unavailable before write-back,
then removes its mapping and records its free-list status.

`AssertNoPinsHeld()` checks guards owned by the calling thread, so the Debug
HTTP request check is not confused by unrelated concurrent requests. The
`pinned_frames()` gauge remains pool-wide. A guard must not outlive its
`BufferPoolManager` or be destroyed on a different thread from the one that
acquired its page latch.

## Table heap

`TableHeap` uses page guards for every header/data-page access and retains at
most one page guard in a `TableIterator`. Buffer-pool latches protect individual
pages, but `TableHeap` does not serialize concurrent insert, update, delete, or
reclaimed-page-set operations. Treat each heap as single-writer and do not
concurrently mutate or scan it until table-level synchronization and TSan
stress coverage are added.

## B+ tree and statement gate

The B+ tree itself does not provide tree-level synchronization or latch
crabbing. Its caller must enforce the Phase 8 statement-gate contract: one
writer at a time, or multiple readers with no concurrent writer. Operations
retain only the current page guard except while adjusting a parent/sibling;
the descent path stores page IDs and child indexes, not parent pointers.
`Executor` implements the gate with a per-instance `std::shared_mutex`: INSERT
takes the exclusive lock and SELECT/range reads take shared locks. The tree's
page guards still protect individual page bodies and scans retain one leaf
guard at a time. Direct B+ tree callers must provide the same gate themselves.

This is not latch crabbing and does not permit a tree writer to run concurrently
with readers. `TableHeap` has no independent concurrent-writer protocol; the
executor gate also serializes its heap writes and guards heap reads against
those writes.

## WAL and checkpointing

The buffer pool permits one active write transaction. Its first write-guard
touch snapshots the page while holding that page's write latch, then briefly
takes `pool_mutex_`; no code waits for a page latch while holding the pool
mutex. Active transaction pages are excluded from eviction and flush. WAL
write-back checks happen outside `pool_mutex_` and enforce the page-LSN
durability boundary before database I/O.

Checkpointing sets a gate under `pool_mutex_`, then releases the mutex before
page I/O, database sync, WAL sync, and WAL replacement. New transactions are
rejected while the gate is active. The WAL mutex serializes log operations and
the periodic `normal`-mode sync thread; neither takes `pool_mutex_`. The
checkpoint order is pool gate, page latch as needed, then WAL latch for
write-ahead sync; no path holds the WAL latch while acquiring a page latch or
the pool mutex.

## Other lock interactions

`FileDiskManager::header_mutex_` serializes page-zero metadata reads/writes and
file extension. POSIX page reads and writes use positional I/O and do not take
a global disk-I/O mutex. Windows seek-plus-I/O uses its descriptor mutex.

## Validation and known gaps

`BufferPoolStressTest.SixteenThreadsOneMillionVerifiedOperations` exercises
16 threads with one million page-ID/CRC-verified operations per thread and
prints its seed. `DiskManagerTest.ConcurrentDistinctPageIo` exercises
independent page I/O. Run the buffer-pool stress test under TSan as well as
ASan/UBSan on the Linux CI host.

The WAL's no-steal rule is enforced by transaction page snapshots and
write-back checks. Direct buffer-pool write access remains a low-level API:
callers using a durable executor must use its transaction gate and WAL-backed
write path.
