#ifndef RELDB_STORAGE_FORMAT_H
#define RELDB_STORAGE_FORMAT_H

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace relational_db {

struct HeapPageMetadataLayout {
    uint16_t num_slots;
    uint16_t free_ptr;
    int32_t next_page_id;
};

struct SlotLayout {
    uint8_t bytes[5];
};

struct VarcharDescriptorLayout {
    uint16_t offset;
    uint16_t length;
};

struct TableHeapHeaderLayout {
    int32_t first_page_id;
    int32_t last_page_id;
};

static_assert(sizeof(HeapPageMetadataLayout) == 8, "Heap page metadata layout changed");
static_assert(offsetof(HeapPageMetadataLayout, num_slots) == 0, "num_slots offset changed");
static_assert(offsetof(HeapPageMetadataLayout, free_ptr) == 2, "free_ptr offset changed");
static_assert(offsetof(HeapPageMetadataLayout, next_page_id) == 4, "next_page_id offset changed");
static_assert(sizeof(SlotLayout) == 5, "Slotted-page entry must be five bytes");
static_assert(offsetof(SlotLayout, bytes) == 0, "Slot byte offset changed");
static_assert(sizeof(VarcharDescriptorLayout) == 4, "VARCHAR descriptor must be four bytes");
static_assert(offsetof(VarcharDescriptorLayout, offset) == 0, "VARCHAR offset field moved");
static_assert(offsetof(VarcharDescriptorLayout, length) == 2, "VARCHAR length field moved");
static_assert(sizeof(TableHeapHeaderLayout) == 8, "Table heap header layout changed");
static_assert(offsetof(TableHeapHeaderLayout, first_page_id) == 0, "first-page offset changed");
static_assert(offsetof(TableHeapHeaderLayout, last_page_id) == 4, "last-page offset changed");

constexpr size_t HEAP_PAGE_METADATA_SIZE = sizeof(HeapPageMetadataLayout);
constexpr size_t SLOT_ENTRY_SIZE = sizeof(SlotLayout);
constexpr size_t VARCHAR_DESCRIPTOR_SIZE = sizeof(VarcharDescriptorLayout);
constexpr size_t MAX_TUPLE_SIZE = PAGE_DATA_SIZE - HEAP_PAGE_METADATA_SIZE - SLOT_ENTRY_SIZE;

static_assert(PAGE_DATA_SIZE <= UINT16_MAX, "Heap offsets must fit in uint16_t");
static_assert(MAX_TUPLE_SIZE + HEAP_PAGE_METADATA_SIZE + SLOT_ENTRY_SIZE == PAGE_DATA_SIZE,
              "Maximum tuple must fit exactly in an empty heap page");
static_assert(MAX_TUPLE_SIZE <= UINT16_MAX, "Tuple length must fit in uint16_t");

}  // namespace relational_db

#endif
