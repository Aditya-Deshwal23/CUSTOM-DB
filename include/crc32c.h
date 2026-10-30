#ifndef CRC32C_H
#define CRC32C_H

#include <cstddef>
#include <cstdint>

namespace relational_db {

uint32_t Crc32c(const uint8_t* data, size_t length);
uint32_t Crc32cSoftware(const uint8_t* data, size_t length);

}  // namespace relational_db

#endif
