#ifndef RELDB_STORAGE_TUPLE_H
#define RELDB_STORAGE_TUPLE_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "db_error.h"
#include "storage/storage_format.h"
#include "storage/value.h"

namespace relational_db {

using Tuple = std::vector<std::byte>;

enum class ColumnType : uint8_t { INT, BIGINT, DOUBLE, BOOLEAN, VARCHAR };

struct Column {
    std::string name;
    ColumnType type{ColumnType::INT};
    uint16_t varchar_length{0};
};

struct Schema {
    std::vector<Column> columns;
    std::optional<size_t> primary_key;

    void Validate() const;
};

void ValidateRow(const Schema& schema, const Row& row);
Tuple SerializeTuple(const Schema& schema, const Row& row);
Row DeserializeTuple(const Schema& schema, const Tuple& tuple);

}  // namespace relational_db

#endif
