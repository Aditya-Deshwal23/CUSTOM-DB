# Current file representation

This document records the implementation as it exists; it does not claim that the file is a stable interchange format.

## Database pages

- `PAGE_SIZE` is 4096 bytes (`include/config.h`).
- Page `n` is addressed at byte offset `n * PAGE_SIZE` (`src/disk_manager.cpp`).
- A page is written as the in-memory 4096-byte page buffer.
- B+ tree metadata and entries are represented by C++ structures over that buffer. Enum, integer, and `std::pair` layout use compiler/platform representation.

## WAL records

`src/wal.cpp` appends these serialized fields, in order: `lsn` (`uint64_t`), `prev_lsn` (`uint64_t`), record type (`uint8_t`), `page_id` (`page_id_t`), payload length (`uint32_t`), payload bytes, and CRC32 (`uint32_t`). Numeric fields are copied from native memory; byte order is not normalized. The source does not currently declare a versioned header or use explicit-endian conversion.

## Compatibility limitations

The current representation is not guaranteed to be portable across endianness, compiler ABI, or future structure changes. It has no version/magic header, migration path, or static assertions establishing a stable disk ABI. `DiskManager` reads and writes pages but does not restore the executor's B+ tree root. The explicit-endian, versioned format and layout assertions in the repository instructions are a future requirement, not a property of the current engine.
