#include "storage/tuple.h"

#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace relational_db {
namespace {

size_t FixedWidth(ColumnType type) {
    switch (type) {
        case ColumnType::INT:
            return sizeof(int32_t);
        case ColumnType::BIGINT:
        case ColumnType::DOUBLE:
            return sizeof(uint64_t);
        case ColumnType::BOOLEAN:
            return sizeof(uint8_t);
        case ColumnType::VARCHAR:
            return VARCHAR_DESCRIPTOR_SIZE;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Unknown column type");
}

int32_t DecodeInt32(uint32_t bits) {
    if (bits <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return static_cast<int32_t>(bits);
    }
    return -1 - static_cast<int32_t>(~bits);
}

int64_t DecodeInt64(uint64_t bits) {
    if (bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(bits);
    }
    return -1 - static_cast<int64_t>(~bits);
}

size_t CheckedAdd(size_t left, size_t right) {
    if (right > std::numeric_limits<size_t>::max() - left) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple size arithmetic overflow");
    }
    return left + right;
}

template <typename UInt>
void WriteLittleEndian(Tuple& bytes, size_t offset, UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "Unsigned integer required");
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        throw DbError(ErrorCode::INVALID_TUPLE, "Tuple field is outside the encoded tuple");
    }
    for (size_t byte = 0; byte < sizeof(UInt); ++byte) {
        bytes[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
}

template <typename UInt>
UInt ReadLittleEndian(const Tuple& bytes, size_t offset) {
    static_assert(std::is_unsigned<UInt>::value, "Unsigned integer required");
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        throw DbError(ErrorCode::INVALID_TUPLE, "Tuple field is outside the encoded tuple");
    }
    UInt value = 0;
    for (size_t byte = 0; byte < sizeof(UInt); ++byte) {
        value |= static_cast<UInt>(std::to_integer<uint8_t>(bytes[offset + byte])) << (byte * 8U);
    }
    return value;
}

bool IsNull(const Tuple& tuple, size_t column_index) {
    const uint8_t bitmap = std::to_integer<uint8_t>(tuple[column_index / 8U]);
    return (bitmap & static_cast<uint8_t>(1U << (column_index % 8U))) != 0;
}

void SetNull(Tuple& tuple, size_t column_index) {
    const uint8_t bitmap = std::to_integer<uint8_t>(tuple[column_index / 8U]);
    tuple[column_index / 8U] =
        static_cast<std::byte>(bitmap | static_cast<uint8_t>(1U << (column_index % 8U)));
}

size_t FixedAreaSize(const Schema& schema) {
    size_t size = (schema.columns.size() + 7U) / 8U;
    for (const Column& column : schema.columns) {
        size = CheckedAdd(size, FixedWidth(column.type));
    }
    return size;
}

void ValidateValue(const Column& column, const Value& value, size_t index) {
    if (std::holds_alternative<std::monostate>(value)) {
        return;
    }
    bool matches = false;
    switch (column.type) {
        case ColumnType::INT:
            matches = std::holds_alternative<int32_t>(value);
            break;
        case ColumnType::BIGINT:
            matches = std::holds_alternative<int64_t>(value);
            break;
        case ColumnType::DOUBLE:
            matches = std::holds_alternative<double>(value);
            break;
        case ColumnType::BOOLEAN:
            matches = std::holds_alternative<bool>(value);
            break;
        case ColumnType::VARCHAR:
            matches = std::holds_alternative<std::string>(value);
            if (matches && std::get<std::string>(value).size() > column.varchar_length) {
                throw DbError(
                    ErrorCode::INVALID_ARGUMENT,
                    "VARCHAR value exceeds its declared length at column " + std::to_string(index));
            }
            break;
    }
    if (!matches) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Value type does not match schema at column " + std::to_string(index));
    }
}

}  // namespace

void Schema::Validate() const {
    if (columns.size() > MAX_TUPLE_SIZE) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Schema has too many columns");
    }
    std::unordered_set<std::string> names;
    for (const Column& column : columns) {
        if (column.name.empty() || !names.insert(column.name).second) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Column names must be non-empty and unique");
        }
        if (column.type == ColumnType::VARCHAR) {
            if (column.varchar_length > 255) {
                throw DbError(ErrorCode::INVALID_ARGUMENT, "VARCHAR length cannot exceed 255");
            }
        } else if (column.varchar_length != 0) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "Only VARCHAR columns may have a declared length");
        }
        static_cast<void>(FixedWidth(column.type));
    }
    if (primary_key && *primary_key >= columns.size()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Primary-key column is out of range");
    }
}

void ValidateRow(const Schema& schema, const Row& row) {
    schema.Validate();
    if (row.size() != schema.columns.size()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Row column count does not match schema");
    }
    for (size_t index = 0; index < row.size(); ++index) {
        ValidateValue(schema.columns[index], row[index], index);
    }
    if (schema.primary_key && std::holds_alternative<std::monostate>(row[*schema.primary_key])) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Primary-key values cannot be NULL");
    }
}

Tuple SerializeTuple(const Schema& schema, const Row& row) {
    ValidateRow(schema, row);
    const size_t bitmap_size = (schema.columns.size() + 7U) / 8U;
    const size_t fixed_size = FixedAreaSize(schema);
    size_t variable_size = 0;
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        if (schema.columns[index].type == ColumnType::VARCHAR &&
            !std::holds_alternative<std::monostate>(row[index])) {
            variable_size = CheckedAdd(variable_size, std::get<std::string>(row[index]).size());
        }
    }
    const size_t tuple_size = CheckedAdd(fixed_size, variable_size);
    if (tuple_size > MAX_TUPLE_SIZE) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }

    Tuple result(tuple_size, std::byte{0});
    size_t fixed_offset = bitmap_size;
    size_t variable_offset = fixed_size;
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        const Column& column = schema.columns[index];
        const Value& value = row[index];
        if (std::holds_alternative<std::monostate>(value)) {
            SetNull(result, index);
            fixed_offset += FixedWidth(column.type);
            continue;
        }
        switch (column.type) {
            case ColumnType::INT:
                WriteLittleEndian(result, fixed_offset,
                                  static_cast<uint32_t>(std::get<int32_t>(value)));
                break;
            case ColumnType::BIGINT:
                WriteLittleEndian(result, fixed_offset,
                                  static_cast<uint64_t>(std::get<int64_t>(value)));
                break;
            case ColumnType::DOUBLE: {
                static_assert(sizeof(double) == sizeof(uint64_t), "Double must be 64-bit");
                static_assert(std::numeric_limits<double>::is_iec559,
                              "DOUBLE storage requires IEEE-754 representation");
                uint64_t bits = 0;
                const double number = std::get<double>(value);
                std::memcpy(&bits, &number, sizeof(bits));
                WriteLittleEndian(result, fixed_offset, bits);
                break;
            }
            case ColumnType::BOOLEAN:
                result[fixed_offset] = std::get<bool>(value) ? std::byte{1} : std::byte{0};
                break;
            case ColumnType::VARCHAR: {
                const std::string& text = std::get<std::string>(value);
                WriteLittleEndian(result, fixed_offset, static_cast<uint16_t>(variable_offset));
                WriteLittleEndian(result, fixed_offset + sizeof(uint16_t),
                                  static_cast<uint16_t>(text.size()));
                for (char character : text) {
                    result[variable_offset++] =
                        static_cast<std::byte>(static_cast<unsigned char>(character));
                }
                break;
            }
        }
        fixed_offset += FixedWidth(column.type);
    }
    return result;
}

Row DeserializeTuple(const Schema& schema, const Tuple& tuple) {
    schema.Validate();
    if (tuple.size() > MAX_TUPLE_SIZE) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }
    const size_t bitmap_size = (schema.columns.size() + 7U) / 8U;
    const size_t fixed_size = FixedAreaSize(schema);
    if (tuple.size() < fixed_size) {
        throw DbError(ErrorCode::INVALID_TUPLE, "Tuple is shorter than its fixed fields");
    }
    if (!schema.columns.empty() && schema.columns.size() % 8U != 0U) {
        const uint8_t unused_mask = static_cast<uint8_t>(0xffU << (schema.columns.size() % 8U));
        if ((std::to_integer<uint8_t>(tuple[bitmap_size - 1U]) & unused_mask) != 0) {
            throw DbError(ErrorCode::INVALID_TUPLE, "Unused null-bitmap bits must be zero");
        }
    }

    Row row;
    row.reserve(schema.columns.size());
    size_t fixed_offset = bitmap_size;
    size_t variable_offset = fixed_size;
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        const Column& column = schema.columns[index];
        if (IsNull(tuple, index)) {
            if (column.type == ColumnType::VARCHAR &&
                (ReadLittleEndian<uint16_t>(tuple, fixed_offset) != 0 ||
                 ReadLittleEndian<uint16_t>(tuple, fixed_offset + sizeof(uint16_t)) != 0)) {
                throw DbError(ErrorCode::INVALID_TUPLE, "NULL VARCHAR descriptors must be zero");
            }
            row.emplace_back(std::monostate{});
            fixed_offset += FixedWidth(column.type);
            continue;
        }
        switch (column.type) {
            case ColumnType::INT:
                row.emplace_back(DecodeInt32(ReadLittleEndian<uint32_t>(tuple, fixed_offset)));
                break;
            case ColumnType::BIGINT:
                row.emplace_back(DecodeInt64(ReadLittleEndian<uint64_t>(tuple, fixed_offset)));
                break;
            case ColumnType::DOUBLE: {
                const uint64_t bits = ReadLittleEndian<uint64_t>(tuple, fixed_offset);
                double number = 0;
                std::memcpy(&number, &bits, sizeof(number));
                row.emplace_back(number);
                break;
            }
            case ColumnType::BOOLEAN: {
                const uint8_t value = std::to_integer<uint8_t>(tuple[fixed_offset]);
                if (value > 1) {
                    throw DbError(ErrorCode::INVALID_TUPLE, "Invalid BOOLEAN encoding");
                }
                row.emplace_back(value != 0);
                break;
            }
            case ColumnType::VARCHAR: {
                const uint16_t offset = ReadLittleEndian<uint16_t>(tuple, fixed_offset);
                const uint16_t length =
                    ReadLittleEndian<uint16_t>(tuple, fixed_offset + sizeof(uint16_t));
                if (offset != variable_offset || length > column.varchar_length ||
                    offset > tuple.size() || length > tuple.size() - offset) {
                    throw DbError(ErrorCode::INVALID_TUPLE, "Invalid VARCHAR offset or length");
                }
                std::string text;
                text.reserve(length);
                for (size_t byte = 0; byte < length; ++byte) {
                    const uint8_t character =
                        std::to_integer<uint8_t>(tuple[static_cast<size_t>(offset) + byte]);
                    char value = '\0';
                    std::memcpy(&value, &character, sizeof(value));
                    text.push_back(value);
                }
                variable_offset += length;
                row.emplace_back(std::move(text));
                break;
            }
        }
        fixed_offset += FixedWidth(column.type);
    }
    if (variable_offset != tuple.size()) {
        throw DbError(ErrorCode::INVALID_TUPLE, "Tuple has trailing or unreferenced bytes");
    }
    ValidateRow(schema, row);
    return row;
}

}  // namespace relational_db
