#include "slotted_page.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "db_error.h"

namespace relational_db {
namespace {

uint32_t ReadU32(const char* bytes, size_t offset) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes + offset);
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

void WriteU32(char* bytes, size_t offset, uint32_t value) {
    auto* data = reinterpret_cast<uint8_t*>(bytes + offset);
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        data[byte] = static_cast<uint8_t>(value >> (byte * 8U));
    }
}

}  // namespace

SlottedPage::SlottedPage(char* data) : data_(data), writable_data_(data) {
    if (data == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Slotted page requires page data");
    }
}

SlottedPage::SlottedPage(const char* data) : data_(data), writable_data_(nullptr) {
    if (data == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Slotted page requires page data");
    }
}

void SlottedPage::EnsureWritable() const {
    if (writable_data_ == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Cannot modify a read-only slotted page");
    }
}

uint16_t SlottedPage::ReadU16(size_t offset) const {
    if (offset > PAGE_DATA_SIZE || sizeof(uint16_t) > PAGE_DATA_SIZE - offset) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page header is out of bounds");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(data_ + offset);
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

void SlottedPage::WriteU16(size_t offset, uint16_t value) {
    EnsureWritable();
    if (offset > PAGE_DATA_SIZE || sizeof(uint16_t) > PAGE_DATA_SIZE - offset) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page header is out of bounds");
    }
    auto* bytes = reinterpret_cast<uint8_t*>(writable_data_ + offset);
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void SlottedPage::Init(page_id_t next_page_id) {
    EnsureWritable();
    std::fill(writable_data_, writable_data_ + PAGE_DATA_SIZE, 0);
    WriteU16(0, 0);
    WriteU16(sizeof(uint16_t), static_cast<uint16_t>(PAGE_DATA_SIZE));
    SetNextPageId(next_page_id);
}

uint16_t SlottedPage::GetNumSlots() const {
    const uint16_t count = ReadU16(0);
    const size_t slot_end = HEADER_SIZE + static_cast<size_t>(count) * SLOT_SIZE;
    if (slot_end > PAGE_DATA_SIZE) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page slot directory exceeds page");
    }
    return count;
}

uint16_t SlottedPage::GetFreeSpacePtr() const {
    const uint16_t free_ptr = ReadU16(sizeof(uint16_t));
    if (free_ptr > PAGE_DATA_SIZE) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page free pointer exceeds page");
    }
    return free_ptr;
}

uint16_t SlottedPage::GetFreeSpaceRemaining() const {
    const size_t slot_end = HEADER_SIZE + static_cast<size_t>(GetNumSlots()) * SLOT_SIZE;
    const uint16_t free_ptr = GetFreeSpacePtr();
    if (slot_end > free_ptr) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page free-space bounds overlap");
    }
    return static_cast<uint16_t>(static_cast<size_t>(free_ptr) - slot_end);
}

page_id_t SlottedPage::GetNextPageId() const {
    const uint32_t encoded = ReadU32(data_, 4);
    if (encoded == UINT32_MAX) {
        return INVALID_PAGE_ID;
    }
    if (encoded == 0 || encoded > static_cast<uint32_t>(std::numeric_limits<page_id_t>::max())) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Next heap-page ID is out of range");
    }
    return static_cast<page_id_t>(encoded);
}

void SlottedPage::SetNextPageId(page_id_t next_page_id) {
    EnsureWritable();
    if (next_page_id < INVALID_PAGE_ID || next_page_id == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid next heap-page ID");
    }
    WriteU32(writable_data_, 4, static_cast<uint32_t>(next_page_id));
}

Slot SlottedPage::GetSlot(uint16_t slot_id) const {
    const uint16_t count = GetNumSlots();
    if (slot_id >= count) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Slotted-page slot ID is out of range");
    }
    const size_t offset = HEADER_SIZE + static_cast<size_t>(slot_id) * SLOT_SIZE;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data_ + offset);
    Slot slot;
    slot.offset = static_cast<uint16_t>(bytes[0]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
    slot.length = static_cast<uint16_t>(bytes[2]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(bytes[3]) << 8U);
    slot.flags = bytes[4];
    if ((slot.flags & static_cast<uint8_t>(~TOMBSTONE)) != 0 || slot.offset > PAGE_DATA_SIZE ||
        slot.length > PAGE_DATA_SIZE - static_cast<size_t>(slot.offset)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid slotted-page entry");
    }
    const uint16_t free_ptr = GetFreeSpacePtr();
    const size_t slot_end = HEADER_SIZE + static_cast<size_t>(count) * SLOT_SIZE;
    if (slot_end > free_ptr || ((slot.flags & TOMBSTONE) == 0 && slot.offset < free_ptr)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page entry overlaps free space");
    }
    if ((slot.flags & TOMBSTONE) != 0 && (slot.offset != 0 || slot.length != 0)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Tombstone entry contains tuple bounds");
    }
    return slot;
}

void SlottedPage::SetSlot(uint16_t slot_id, const Slot& slot) {
    EnsureWritable();
    const size_t offset = HEADER_SIZE + static_cast<size_t>(slot_id) * SLOT_SIZE;
    if (offset > PAGE_DATA_SIZE || SLOT_SIZE > PAGE_DATA_SIZE - offset ||
        slot.offset > PAGE_DATA_SIZE ||
        slot.length > PAGE_DATA_SIZE - static_cast<size_t>(slot.offset) ||
        (slot.flags & static_cast<uint8_t>(~TOMBSTONE)) != 0) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Cannot write invalid slotted-page entry");
    }
    auto* bytes = reinterpret_cast<uint8_t*>(writable_data_ + offset);
    bytes[0] = static_cast<uint8_t>(slot.offset);
    bytes[1] = static_cast<uint8_t>(slot.offset >> 8U);
    bytes[2] = static_cast<uint8_t>(slot.length);
    bytes[3] = static_cast<uint8_t>(slot.length >> 8U);
    bytes[4] = slot.flags;
}

void SlottedPage::InsertAtSlot(uint16_t slot_id, bool new_slot, const std::byte* tuple_data,
                               uint16_t tuple_size) {
    const size_t required = static_cast<size_t>(tuple_size) + (new_slot ? SLOT_SIZE : 0U);
    if (GetFreeSpaceRemaining() < required) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page insertion lacks free space");
    }
    const uint16_t free_ptr = GetFreeSpacePtr();
    const size_t tuple_offset = static_cast<size_t>(free_ptr) - tuple_size;
    if (tuple_offset > UINT16_MAX || tuple_offset > PAGE_DATA_SIZE) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Tuple offset does not fit on page");
    }
    if (tuple_size > 0) {
        std::memcpy(writable_data_ + tuple_offset, tuple_data, tuple_size);
    }
    SetSlot(slot_id, Slot{static_cast<uint16_t>(tuple_offset), tuple_size, 0});
    WriteU16(sizeof(uint16_t), static_cast<uint16_t>(tuple_offset));
    if (new_slot) {
        const uint16_t old_count = GetNumSlots();
        if (old_count == UINT16_MAX) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Slotted-page slot count overflow");
        }
        WriteU16(0, static_cast<uint16_t>(old_count + 1U));
    }
}

uint16_t SlottedPage::InsertTuple(const std::byte* tuple_data, size_t tuple_size) {
    EnsureWritable();
    if (tuple_size > MAX_TUPLE_SIZE || tuple_size > UINT16_MAX) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }
    if (tuple_size > 0 && tuple_data == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Tuple data cannot be null");
    }
    const uint16_t count = GetNumSlots();
    uint16_t target_slot = INVALID_SLOT_ID;
    for (uint32_t index = 0; index < count; ++index) {
        const uint16_t slot_id = static_cast<uint16_t>(index);
        if ((GetSlot(slot_id).flags & TOMBSTONE) != 0) {
            target_slot = slot_id;
            break;
        }
    }
    const bool new_slot = target_slot == INVALID_SLOT_ID;
    if (new_slot) {
        if (count == UINT16_MAX) {
            return INVALID_SLOT_ID;
        }
        target_slot = count;
    }
    const size_t required = tuple_size + (new_slot ? SLOT_SIZE : 0U);
    if (GetFreeSpaceRemaining() < required) {
        Compact();
        if (GetFreeSpaceRemaining() < required) {
            return INVALID_SLOT_ID;
        }
    }
    InsertAtSlot(target_slot, new_slot, tuple_data, static_cast<uint16_t>(tuple_size));
    return target_slot;
}

bool SlottedPage::ReadTuple(uint16_t slot_id, std::vector<std::byte>& out_data) const {
    if (slot_id >= GetNumSlots()) {
        return false;
    }
    const Slot slot = GetSlot(slot_id);
    if ((slot.flags & TOMBSTONE) != 0) {
        return false;
    }
    out_data.resize(slot.length);
    if (slot.length > 0) {
        std::memcpy(out_data.data(), data_ + slot.offset, slot.length);
    }
    return true;
}

bool SlottedPage::DeleteTuple(uint16_t slot_id) {
    EnsureWritable();
    if (slot_id >= GetNumSlots()) {
        return false;
    }
    const Slot slot = GetSlot(slot_id);
    if ((slot.flags & TOMBSTONE) != 0) {
        return false;
    }
    SetSlot(slot_id, Slot{0, 0, TOMBSTONE});
    return true;
}

SlottedPage::UpdateResult SlottedPage::UpdateTuple(uint16_t slot_id, const std::byte* tuple_data,
                                                   size_t tuple_size) {
    EnsureWritable();
    if (tuple_size > MAX_TUPLE_SIZE || tuple_size > UINT16_MAX) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }
    if (tuple_size > 0 && tuple_data == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Tuple data cannot be null");
    }
    if (slot_id >= GetNumSlots()) {
        return UpdateResult::NOT_FOUND;
    }
    const Slot old_slot = GetSlot(slot_id);
    if ((old_slot.flags & TOMBSTONE) != 0) {
        return UpdateResult::NOT_FOUND;
    }
    Compact();
    const size_t available = static_cast<size_t>(GetFreeSpaceRemaining()) + old_slot.length;
    if (tuple_size > available) {
        return UpdateResult::NEEDS_MOVE;
    }
    SetSlot(slot_id, Slot{0, 0, TOMBSTONE});
    Compact();
    InsertAtSlot(slot_id, false, tuple_data, static_cast<uint16_t>(tuple_size));
    return UpdateResult::UPDATED;
}

void SlottedPage::Compact() {
    EnsureWritable();
    struct ActiveSlot {
        uint16_t id;
        uint16_t offset;
    };
    std::vector<ActiveSlot> active;
    const uint16_t count = GetNumSlots();
    active.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint16_t slot_id = static_cast<uint16_t>(index);
        const Slot slot = GetSlot(slot_id);
        if ((slot.flags & TOMBSTONE) == 0) {
            active.push_back(ActiveSlot{slot_id, slot.offset});
        }
    }
    std::sort(active.begin(), active.end(), [](const ActiveSlot& left, const ActiveSlot& right) {
        return left.offset > right.offset;
    });

    size_t destination = PAGE_DATA_SIZE;
    for (const ActiveSlot& item : active) {
        Slot slot = GetSlot(item.id);
        if (slot.length > destination) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Tuple lengths exceed page capacity");
        }
        destination -= slot.length;
        if (slot.length > 0) {
            std::memmove(writable_data_ + destination, writable_data_ + slot.offset, slot.length);
        }
        slot.offset = static_cast<uint16_t>(destination);
        SetSlot(item.id, slot);
    }
    if (destination > UINT16_MAX) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Compacted free pointer overflow");
    }
    WriteU16(sizeof(uint16_t), static_cast<uint16_t>(destination));
}

bool SlottedPage::HasTombstones() const {
    const uint16_t count = GetNumSlots();
    for (uint32_t index = 0; index < count; ++index) {
        if ((GetSlot(static_cast<uint16_t>(index)).flags & TOMBSTONE) != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace relational_db
