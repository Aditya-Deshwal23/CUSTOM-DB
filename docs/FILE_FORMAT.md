# Database file format v2 and storage payloads

The database file is a sequence of 4096-byte pages addressed by
`page_id * 4096`. All multibyte values in this format are unsigned/signed
little-endian integers, independent of host byte order. The physical header
and every page checksum use CRC32C (Castagnoli polynomial, reflected
polynomial `0x82F63B78`, initial/final XOR `0xFFFFFFFF`).

## Common page header

Every page starts with this 16-byte header:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | CRC32C |
| 4 | 2 | page type |
| 6 | 2 | flags |
| 8 | 8 | LSN |

Page checksum is computed over bytes `[4, 4096)`, excluding its own stored
four bytes. Types are `HEADER=0`, `FREE=1`, `BTREE_INTERNAL=2`,
`BTREE_LEAF=3`, `HEAP=4`, `CATALOG=5`, and `INDEX_META=6`. Unknown type values
are reserved. Flags and LSN are currently zero unless a higher layer supplies
them. An all-zero page image is a valid never-written page.

The disk-manager and buffer-pool APIs expose only the 4080-byte body after
this header. The disk manager constructs or validates the common header and
never exposes it as payload. Body offsets below are relative to byte 16 of the
physical page.

## Page zero: database header

Page zero has common page type `HEADER`. Its body begins at file offset 16:

| File offset | Width | Field |
|---:|---:|---|
| 16 | 8 | Magic bytes `RELDB\0\0\2` |
| 24 | 4 | Format version (`2`) |
| 28 | 4 | Page size (`4096`) |
| 32 | 8 | Logical page count / high-water mark (`1` or greater) |
| 40 | 4 | Free-list head (`-1` initially; page ID of first free page otherwise) |
| 44 | 4 | Catalog root (`-1` initially; reserved) |
| 48 | 4 | Header CRC32C |
| 52 | 4044 | Reserved, zero when initialized |

The header CRC covers bytes `[16, 48)`. The common page CRC also covers bytes
`[4, 4096)`, including the stored header CRC. A file with an absent/invalid
magic is an invalid database file, a non-v2 version is unsupported, and
invalid page size or either header checksum is a corrupt header.

`page_count` is the logical high-water mark; page IDs in the range
`[0, page_count)` are addressable. The physical file may be larger because
space is reserved in 64-page growth chunks. The buffer-pool `PageAllocator`
owns allocation and reuse. A free page stores its next free-page ID as a
little-endian signed 32-bit value in the first four bytes of its body; `-1`
terminates the list. Page-zero metadata is updated through a page guard and
written with refreshed header and page CRC32C values. In the WAL-backed
executor, page zero and allocator changes are included in the statement's
before-images and committed page images. Direct buffer-pool use without a WAL
transaction does not provide that transactional guarantee.

## Table heap header page

A table heap has a caller-owned header page. Its first eight body bytes are:

| Body offset | Width | Field |
|---:|---:|---|
| 0 | 4 | First data page ID (`-1` for an empty heap) |
| 4 | 4 | Last data page ID (`-1` for an empty heap) |

Both IDs are signed little-endian 32-bit values and must either both be `-1`
or both name data pages. The remaining 4072 bytes are reserved and initialized
to zero. The page itself has the common 16-byte header described above.

## Table heap data page

The common physical page header is followed by a 4080-byte body. The body
starts with this eight-byte heap metadata header:

| Body offset | Width | Field |
|---:|---:|---|
| 0 | 2 | Number of slots |
| 2 | 2 | Free-space pointer |
| 4 | 4 | Next data page ID (`-1` at the tail) |

The metadata and all slot fields are little-endian. The slot directory starts
at body offset 8 and grows upward; tuple bytes are packed downward from body
offset 4080. `free_ptr` is the lowest tuple-byte offset. The slot directory
end must not exceed `free_ptr`.

Each slot is exactly five bytes:

| Slot offset | Width | Field |
|---:|---:|---|
| 0 | 2 | Tuple offset relative to the body |
| 2 | 2 | Tuple length |
| 4 | 1 | Flags; bit 0 is `TOMBSTONE` |

A tombstone has zero offset and length and remains in the directory so RIDs
for other slots do not change. An active zero-length tuple has flags zero and
is therefore distinct from a deleted tuple. Insert reuses tombstoned slots.
Compaction sorts active slots by tuple offset and moves tuples in place; slot
numbers are preserved. Update compacts first and either replaces the tuple in
the same slot or reports that it needs a new RID.

The physical common header is 16 bytes, followed by 8 bytes of heap metadata,
then five-byte slot entries and tuple data. `PageHeader` is statically
asserted to 16 bytes in `include/disk_manager.h`; heap metadata, slot, table
header, and tuple-size constants are statically asserted in
`include/storage/storage_format.h`.

## Tuple encoding

A tuple contains a null bitmap followed by a fixed area in schema column order.
The bitmap uses one bit per column, with bit 1 indicating NULL. Fixed-width
fields use 4 bytes for `INT`, 8 for `BIGINT`, 8 for `DOUBLE`, and 1 for
`BOOLEAN`. A `VARCHAR(n)` uses a four-byte fixed descriptor containing a
little-endian u16 offset and u16 length; its bytes are stored after the fixed
area, in column order. Offsets are relative to the start of the tuple. NULL
fixed fields are zero-filled, NULL VARCHAR descriptors are `(0, 0)`, and an
empty non-NULL string has length zero with its own offset. VARCHAR length is
limited to 255 bytes. Schema validation checks unique non-empty column names,
types, VARCHAR declarations, row value types, and non-NULL primary keys.

`MAX_TUPLE_SIZE` is 4067 bytes: the 4080-byte body capacity less the eight-byte
heap metadata header and one five-byte slot entry. A tuple larger than this is
rejected with `TUPLE_TOO_LARGE`; v1 has no overflow pages. This limit also
ensures that any accepted tuple can fit on an otherwise empty data page.

## Other pages

Pages 1 and above contain their common 16-byte physical page header followed
by a 4080-byte body owned by the buffer pool. The buffer pool preserves the
physical page type when reading, evicting, and flushing a page. B+ tree
payload fields below are little-endian; key encodings are explicitly defined
and do not use host structure padding.

### B+ tree pages

The common physical page header carries `BTREE_LEAF`, `BTREE_INTERNAL`, or
`INDEX_META`; the payload starts at physical offset 16. These fixed body
layouts are checked by `static_assert`s in `include/b_plus_tree.h`.

| Page body offset | Width | Leaf field |
|---:|---:|---|
| 0 | 2 | Entry count |
| 2 | 4 | Next leaf page ID (`-1` at the tail) |
| 6 | variable | Sorted leaf entries |

Each leaf entry is the encoded key followed by an 8-byte little-endian RID.
The key width is the encoded key's size (16 bytes for `IndexKey16`); capacity
is derived from the body size, not from compiler-dependent `std::pair` sizes.

| Page body offset | Width | Internal field |
|---:|---:|---|
| 0 | 2 | Separator-key count |
| 2 | variable | `count + 1` `(key, child_page_id)` slots |

Slot zero contains the first child ID and has no meaningful key. Slots 1
through `count` contain the separator for the slot's child followed by its
child ID. A separator is the minimum key in its right child. Internal nodes
have no parent pointer; the descent path is held by the operation.

An `INDEX_META` body begins with exactly eight bytes:

| Body offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Root page ID (`-1` for empty) |
| 4 | 2 | Tree height (`0` for empty) |
| 6 | 2 | Key-kind enum |

The database header's `catalog_root` points to the legacy owner `INDEX_META`
page. Its body retains the legacy heap header page ID at offset 8, and stores
the SQL catalog page ID at offset 12. A zero catalog page ID means that an old
database has no SQL catalog yet. SQL table indexes each have their own
`INDEX_META` page containing the B+ tree root, height, and key kind.

The SQL catalog is currently one `CATALOG` page. Its body is an explicit
little-endian encoding, not a native C++ structure:

| Field | Encoding |
|---|---|
| Version | `u32` (`2`; version `1` remains readable) |
| Table count | `u16` |
| Table name | `u16` byte length, then UTF-8 bytes |
| Heap header page ID | `u32` page ID |
| Column count | `u16` |
| Primary-key column | `u16` ordinal; `0xffff` means none |
| Each column (v1) | name string, `u8` type enum, `u16` VARCHAR length, `u8` NOT NULL flag |
| Each column (v2) | v1 fields, `u8` REFERENCES-present flag, then referenced table and column strings when present |
| Index count | `u16` |
| Each index | name string, `u16` column ordinal, `u8` unique flag, `u32` metadata page ID |

The complete encoding must fit in the page body (`PAGE_DATA_SIZE`); a catalog
that exceeds one page fails rather than silently truncating metadata.

Non-root internal nodes have at least `ceil(max_children / 2) - 1`
separators. Non-root leaves have at least ten percent of leaf capacity, rounded
up; this permits the rightmost-insert split to leave the existing left leaf
about 90% full. Monotone ascending insertion uses a 90/10 split; other
insertion sequences use a 50/50 split. Deletion borrows before merging and
collapses a one-child root. Retired nodes return to the allocator's free list.

### Index key encodings

`IndexKey16` is exactly 16 bytes and compares by unsigned `memcmp`:

* Signed INT32/INT64 are sign-bit-flipped, then big-endian.
* BOOLEAN is `00` or `01`, followed by zero bytes.
* The generic B+ tree VARCHAR key encoding is at most 16 bytes, zero-padded;
  SQL catalog indexes instead use a 64-bit FNV-1a hash prefix for VARCHAR
  equality candidates.
* A composite key contains up to two independently sign-flipped INT32 fields,
  each big-endian, followed by zero bytes.
* A non-unique key is the 8-byte big-endian value followed by the 8-byte
  big-endian RID. Both components are fixed-width, making every stored key
  unique and preserving range order.

SQL non-unique index keys contain the ordered 8-byte scalar prefix followed by
the 8-byte big-endian packed RID, keeping duplicate values distinct while
preserving numeric range order. VARCHAR hashes are not order-preserving, so
VARCHAR range/order access falls back to a heap scan. Candidate rows are
rechecked against their values, including for unique-index enforcement.
B+ tree value fields are fixed-width 8-byte integers; SQL indexes store packed
RIDs there. SQL rows remain in `TableHeap`, not in an in-memory-only structure.

RIDs contain a signed 32-bit page ID and unsigned 16-bit slot number. Their
64-bit index representation stores the page ID in bits 16-47 and the slot in
bits 0-15; bits 48-63 are reserved and must be zero.

## Write-ahead log

The default WAL path is `<DB_PATH>-wal`; `WAL_PATH` can select another path.
All multibyte values are explicit
little-endian; no host structure layout is written to disk. WAL and page
checksums use CRC32C.

The WAL begins with a 24-byte header:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 8 | Magic `RDBWAL\0\1` |
| 8 | 4 | WAL version (`1`) |
| 12 | 8 | Base LSN |
| 20 | 4 | CRC32C over bytes `[0, 20)` |

Each record is `u32 length`, `u8 type`, `u64 txn_id`, `u64 lsn`, a
type-dependent payload, and `u32 CRC32C`. Length includes the length field and
trailing CRC. The record CRC covers every record byte except the trailing CRC,
including the length field.

| Type | Value | Payload |
|---|---:|---|
| `PAGE_IMAGE` | 1 | `u32 page_id` followed by one complete 4096-byte physical page image, including its page header and CRC32C |
| `COMMIT` | 2 | Empty; the transaction ID is in the record header |
| `CHECKPOINT` | 3 | Empty |

LSNs increase by one across records and begin at the header base LSN. A page
image contains the complete encoded page with its LSN set to the record LSN.
Only page images preceding a valid COMMIT for the same transaction are replayed.
Recovery truncates the log to a synced header after replay, so the next append
begins at the recovered base LSN. Compile-time layout assertions in
`src/wal.cpp` pin the header and record sizes and their field offsets.
