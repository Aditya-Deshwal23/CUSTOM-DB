#ifndef SLOTTED_PAGE_H
#define SLOTTED_PAGE_H

#include "config.h"
#include <cstdint>
#include <vector>
#include <cstddef>

namespace relational_db {

/**
 * @brief Represents an individual slot entry in a slotted page.
 */
struct Slot {
    uint16_t offset{0};
    uint16_t length{0}; // Length 0 means the slot is deleted
};

/**
 * @brief SlottedPage provides an interface over raw page memory to manage 
 * variable-length records.
 * 
 * Memory Layout:
 * -------------------------------------------------------------
 * | PageHeader | SlotArray ---> | FreeSpace | <--- Tuple Data |
 * -------------------------------------------------------------
 */
class SlottedPage {
public:
    static constexpr size_t HEADER_SIZE = sizeof(page_id_t) + sizeof(uint16_t) * 2;
    static constexpr uint16_t INVALID_SLOT_ID = static_cast<uint16_t>(-1);

    /**
     * @brief Constructor wrappers the raw data buffer.
     * @param data The 4KB buffer (typically from Page::GetData()).
     */
    explicit SlottedPage(char* data);

    /**
     * @brief Initialize a new, empty slotted page.
     * @param page_id The ID to set in the header.
     */
    void Init(page_id_t page_id);

    /**
     * @brief Insert a tuple into the page.
     * @param tuple_data Pointer to the data.
     * @param tuple_size Size of the tuple.
     * @return The slot ID where the tuple was inserted, or INVALID_SLOT_ID if no space.
     */
    uint16_t InsertTuple(const char* tuple_data, uint16_t tuple_size);

    /**
     * @brief Read a tuple from the page.
     * @param slot_id The ID of the slot.
     * @param[out] out_data Buffer to copy data into.
     * @return True if successful, false if slot is invalid/deleted.
     */
    bool ReadTuple(uint16_t slot_id, std::vector<char>& out_data) const;

    /**
     * @brief Delete a tuple (marks slot as deleted).
     * @param slot_id The ID of the slot to delete.
     * @return True if deleted, false if already deleted or invalid.
     */
    bool DeleteTuple(uint16_t slot_id);

    /**
     * @brief Compact the page to defragment free space.
     */
    void Compact();

    // Getters for header info
    page_id_t GetPageId() const;
    uint16_t GetNumSlots() const;
    uint16_t GetFreeSpacePtr() const;
    uint16_t GetFreeSpaceRemaining() const;

private:
    char* data_;
    
    // Header field pointers
    page_id_t* page_id_ptr_;
    uint16_t* num_slots_ptr_;
    uint16_t* free_space_ptr_;

    Slot* GetSlot(uint16_t slot_id) const;
};

} // namespace relational_db

#endif // SLOTTED_PAGE_H
