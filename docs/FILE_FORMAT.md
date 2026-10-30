# Database file format v2

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

The disk-manager API reads and writes the 4080-byte body after this header.
Existing buffer-pool page structures use this body; their available capacity
is therefore 4080 bytes. The disk manager constructs or validates the common
header and never exposes it as B+ tree payload.

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
written with refreshed header and page CRC32C values. This allocator is not
transactional and does not provide crash-safe free-list updates.

## Other pages

Pages 1 and above contain their common page header followed by 4080 bytes
owned by the buffer pool. The disk manager stamps type `HEAP`, flags zero, and
LSN zero unless a caller specifies metadata. B+ tree/catalog-specific body
layouts remain owned by their higher layers and are not yet a portable
interchange format.

The WAL remains a separate prototype format and is not upgraded by this
change. It does not participate in this page format's durability guarantees.
