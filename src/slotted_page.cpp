#include "slotted_page.h"
#include <cstring>
#include <algorithm>

namespace relational_db {

SlottedPage::SlottedPage(char* data) : data_(data) {
    page_id_ptr_ = reinterpret_cast<page_id_t*>(data_);
    num_slots_ptr_ = reinterpret_cast<uint16_t*>(data_ + sizeof(page_id_t));
    free_space_ptr_ = reinterpret_cast<uint16_t*>(data_ + sizeof(page_id_t) + sizeof(uint16_t));
}

void SlottedPage::Init(page_id_t page_id) {
    *page_id_ptr_ = page_id;
    *num_slots_ptr_ = 0;
    *free_space_ptr_ = PAGE_DATA_SIZE;
}

Slot* SlottedPage::GetSlot(uint16_t slot_id) const {
    if (slot_id >= *num_slots_ptr_) return nullptr;
    size_t offset = HEADER_SIZE + slot_id * sizeof(Slot);
    return reinterpret_cast<Slot*>(data_ + offset);
}

uint16_t SlottedPage::GetFreeSpaceRemaining() const {
    size_t current_slot_end = HEADER_SIZE + (*num_slots_ptr_) * sizeof(Slot);
    if (current_slot_end >= *free_space_ptr_) return 0;
    return *free_space_ptr_ - static_cast<uint16_t>(current_slot_end);
}

uint16_t SlottedPage::InsertTuple(const char* tuple_data, uint16_t tuple_size) {
    // We need space for the tuple and potentially a new slot
    uint16_t space_needed = tuple_size;
    
    // Find an empty/deleted slot
    uint16_t slot_id = INVALID_SLOT_ID;
    for (uint16_t i = 0; i < *num_slots_ptr_; ++i) {
        if (GetSlot(i)->length == 0) {
            slot_id = i;
            break;
        }
    }

    if (slot_id == INVALID_SLOT_ID) {
        // Need to add a new slot
        space_needed += sizeof(Slot);
    }

    if (GetFreeSpaceRemaining() < space_needed) {
        // Not enough space, try compaction first
        Compact();
        if (GetFreeSpaceRemaining() < space_needed) {
            return INVALID_SLOT_ID; // Still no space
        }
    }

    // Allocate space
    *free_space_ptr_ -= tuple_size;
    uint16_t tuple_offset = *free_space_ptr_;
    
    // Copy data
    std::memcpy(data_ + tuple_offset, tuple_data, tuple_size);

    // Update or create slot
    if (slot_id == INVALID_SLOT_ID) {
        slot_id = (*num_slots_ptr_)++;
    }
    
    Slot* slot = GetSlot(slot_id);
    slot->offset = tuple_offset;
    slot->length = tuple_size;

    return slot_id;
}

bool SlottedPage::ReadTuple(uint16_t slot_id, std::vector<char>& out_data) const {
    Slot* slot = GetSlot(slot_id);
    if (!slot || slot->length == 0) return false;

    out_data.resize(slot->length);
    std::memcpy(out_data.data(), data_ + slot->offset, slot->length);
    return true;
}

bool SlottedPage::DeleteTuple(uint16_t slot_id) {
    Slot* slot = GetSlot(slot_id);
    if (!slot || slot->length == 0) return false;

    // Mark as deleted. Actual space is reclaimed during Compact()
    slot->length = 0;
    slot->offset = 0;
    return true;
}

void SlottedPage::Compact() {
    // Create a temporary buffer for compacted data
    std::vector<char> temp_buffer(PAGE_DATA_SIZE, 0);
    uint16_t temp_free_space = PAGE_DATA_SIZE;

    // Move all active tuples to the end of the temp buffer
    for (uint16_t i = 0; i < *num_slots_ptr_; ++i) {
        Slot* slot = GetSlot(i);
        if (slot->length > 0) {
            temp_free_space -= slot->length;
            std::memcpy(temp_buffer.data() + temp_free_space, data_ + slot->offset, slot->length);
            slot->offset = temp_free_space;
        }
    }

    // Copy the compacted data back to the original page
    std::memcpy(data_ + temp_free_space, temp_buffer.data() + temp_free_space,
                PAGE_DATA_SIZE - temp_free_space);

    // Update the free space pointer
    *free_space_ptr_ = temp_free_space;
}

page_id_t SlottedPage::GetPageId() const { return *page_id_ptr_; }
uint16_t SlottedPage::GetNumSlots() const { return *num_slots_ptr_; }
uint16_t SlottedPage::GetFreeSpacePtr() const { return *free_space_ptr_; }

} // namespace relational_db
