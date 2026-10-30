#ifndef RELDB_INDEX_KEY_H
#define RELDB_INDEX_KEY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "db_error.h"

#include <algorithm>

namespace relational_db {

struct IndexKey16 {
    std::array<uint8_t, 16> bytes{};

    bool operator==(const IndexKey16& other) const noexcept {
        return bytes == other.bytes;
    }
    bool operator<(const IndexKey16& other) const noexcept {
        return std::memcmp(bytes.data(), other.bytes.data(), bytes.size()) < 0;
    }
};

struct IndexKey16Less {
    bool operator()(const IndexKey16& lhs, const IndexKey16& rhs) const noexcept {
        return std::memcmp(lhs.bytes.data(), rhs.bytes.data(), lhs.bytes.size()) < 0;
    }
};

enum class IndexKeyKind : uint16_t {
    RAW_INTEGRAL = 0,
    INT32 = 1,
    INT64 = 2,
    BOOLEAN = 3,
    VARCHAR16 = 4,
    TWO_INT32 = 5,
    NON_UNIQUE = 6
};

inline IndexKey16 EncodeIndexKey(int32_t value) noexcept {
    IndexKey16 key;
    const uint32_t ordered = static_cast<uint32_t>(value) ^ 0x80000000U;
    for (size_t index = 0; index < 4; ++index) {
        key.bytes[index] = static_cast<uint8_t>(ordered >> ((3U - index) * 8U));
    }
    return key;
}

inline int32_t DecodeIndexKeyInt32(const IndexKey16& key) noexcept {
    uint32_t ordered = 0;
    for (size_t index = 0; index < 4; ++index) {
        ordered = (ordered << 8U) | key.bytes[index];
    }
    ordered ^= 0x80000000U;
    int32_t value{};
    std::memcpy(&value, &ordered, sizeof(value));
    return value;
}

inline IndexKey16 EncodeIndexKey(int64_t value) noexcept {
    IndexKey16 key;
    const uint64_t ordered = static_cast<uint64_t>(value) ^ 0x8000000000000000ULL;
    for (size_t index = 0; index < 8; ++index) {
        key.bytes[index] = static_cast<uint8_t>(ordered >> ((7U - index) * 8U));
    }
    return key;
}

inline IndexKey16 EncodeIndexKey(bool value) noexcept {
    IndexKey16 key;
    key.bytes[0] = value ? 1U : 0U;
    return key;
}

inline IndexKey16 EncodeIndexKey(std::string_view value) {
    if (value.size() > 16 || value.find('\0') != std::string_view::npos) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "VARCHAR index keys must be at most 16 bytes and contain no NUL");
    }
    IndexKey16 key;
    std::memcpy(key.bytes.data(), value.data(), value.size());
    return key;
}

inline IndexKey16 EncodeCompositeInt32(int32_t first, int32_t second) noexcept {
    IndexKey16 key = EncodeIndexKey(first);
    const IndexKey16 second_key = EncodeIndexKey(second);
    std::copy_n(second_key.bytes.begin(), 4, key.bytes.begin() + 4);
    return key;
}

inline IndexKey16 EncodeNonUniqueIndexKey(const uint8_t* value, size_t value_size,
                                         uint64_t rid) {
    if (value_size > 8 || (value == nullptr && value_size != 0)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Non-unique index values must contain at most eight bytes");
    }
    IndexKey16 key;
    if (value_size != 0) {
        std::copy_n(value, value_size, key.bytes.begin());
    }
    for (size_t index = 0; index < 8; ++index) {
        key.bytes[value_size + index] = static_cast<uint8_t>(rid >> ((7U - index) * 8U));
    }
    return key;
}

inline IndexKey16 EncodeNonUniqueIndexKey(uint64_t value, uint64_t rid) noexcept {
    std::array<uint8_t, 8> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(value >> ((7U - index) * 8U));
    }
    IndexKey16 key;
    std::copy(bytes.begin(), bytes.end(), key.bytes.begin());
    for (size_t index = 0; index < 8; ++index) {
        key.bytes[index + 8] = static_cast<uint8_t>(rid >> ((7U - index) * 8U));
    }
    return key;
}

}  // namespace relational_db

#endif
