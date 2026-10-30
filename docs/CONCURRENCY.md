# Concurrency model and known gaps

This records lock interactions visible in the current implementation. It is not a claim that the engine is race-free.

## Observed lock order

When nested, the current acquisition order is:

1. `BPlusTree::root_latch_`
2. `BufferPoolManager::latch_`
3. `ClockReplacer::latch_`
4. `DiskManager::db_io_latch_`

`Page::page_latch_` is acquired after `FetchPage` returns, so current code does not establish a safe writer order around page mutation. `GetValue` uses a page read guard; insertion paths do not consistently take a page write latch. The regression test `ConcurrencyTest.DISABLED_ReaderWriterTsanClean` records the known read/write race.

## Known invariant violations

- `BufferPoolManager` holds its global pool latch while invoking `DiskManager` reads and writes. This is I/O under a global mutex and must be removed in a future concurrency phase.
- The B+ tree uses a root lock to serialize insertion but that does not make page contents safe against concurrent readers.
- `PageGuard` releases the page latch but does not unpin the buffer-pool page; repeated lookups can exhaust frames.
- API metrics are sampled from a separate synthetic workload and do not provide a complete concurrency view.

The CI TSan job runs the disabled reader/writer stress case as a known-bug reproduction. The test prints its fixed seed so a report can be reproduced.
