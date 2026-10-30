#ifndef SLOTTED_PAGE_H
#define SLOTTED_PAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "storage/storage_format.h"

namespace relational_db {

struct Slot {
    uint16_t offset{0};
    uint16_t length{0};
    uint8_t flags{0};
};

class SlottedPage {
public:
 static constexpr size_t HEADER_SIZE = HEAP_PAGE_METADATA_SIZE;
 static constexpr size_t SLOT_SIZE = SLOT_ENTRY_SIZE;
 static constexpr uint16_t INVALID_SLOT_ID = UINT16_MAX;
 static constexpr uint8_t TOMBSTONE = 0x01;

 explicit SlottedPage(char* data);
 explicit SlottedPage(const char* data);

 void Init(page_id_t next_page_id = INVALID_PAGE_ID);
 uint16_t InsertTuple(const std::byte* tuple_data, size_t tuple_size);
 bool ReadTuple(uint16_t slot_id, std::vector<std::byte>& out_data) const;
 bool DeleteTuple(uint16_t slot_id);

 enum class UpdateResult { UPDATED, NEEDS_MOVE, NOT_FOUND };
 UpdateResult UpdateTuple(uint16_t slot_id, const std::byte* tuple_data, size_t tuple_size);

 void Compact();
 page_id_t GetNextPageId() const;
 void SetNextPageId(page_id_t next_page_id);
 uint16_t GetNumSlots() const;
 uint16_t GetFreeSpacePtr() const;
 uint16_t GetFreeSpaceRemaining() const;
 bool HasTombstones() const;

private:
 const char* data_;
 char* writable_data_;

 void EnsureWritable() const;
 Slot GetSlot(uint16_t slot_id) const;
 void SetSlot(uint16_t slot_id, const Slot& slot);
 uint16_t ReadU16(size_t offset) const;
 void WriteU16(size_t offset, uint16_t value);
 void InsertAtSlot(uint16_t slot_id, bool new_slot, const std::byte* tuple_data,
                   uint16_t tuple_size);
};

}  // namespace relational_db

#endif
