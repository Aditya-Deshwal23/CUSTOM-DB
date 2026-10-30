#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>

namespace relational_db {

// Define the global page size (e.g., 4KB)
constexpr static const uint32_t PAGE_SIZE = 4096;
constexpr static const uint32_t PAGE_HEADER_SIZE = 16;
constexpr static const uint32_t PAGE_DATA_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

// Type alias for page identifiers
using page_id_t = int32_t;
using frame_id_t = int32_t;

// Invalid page ID to indicate null/empty
constexpr static const page_id_t INVALID_PAGE_ID = -1;

} // namespace relational_db

#endif // CONFIG_H
