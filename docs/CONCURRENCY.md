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

## Other lock interactions

`BPlusTree::root_latch_` serializes writers and protects root traversal.
Read/write page guards protect page contents. The current B+ tree algorithms
are still a prototype: scans, split behavior, root persistence, and
transactional allocation are not certified.

`FileDiskManager::header_mutex_` serializes page-zero metadata reads/writes and
file extension. POSIX page reads and writes use positional I/O and do not take
a global disk-I/O mutex. Windows seek-plus-I/O uses its descriptor mutex.

## Validation and known gaps

`BufferPoolStressTest.SixteenThreadsOneMillionVerifiedOperations` exercises
16 threads with one million page-ID/CRC-verified operations per thread and
prints its seed. `DiskManagerTest.ConcurrentDistinctPageIo` exercises
independent page I/O. Run the buffer-pool stress test under TSan as well as
ASan/UBSan on the Linux CI host.

No WAL/no-steal implementation is included in this phase. The default
write-back policy allows dirty pages; Phase 5 must install a policy that
enforces WAL ordering.
