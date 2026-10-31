#ifndef RELDB_STORAGE_TUPLE_H
#define RELDB_STORAGE_TUPLE_H

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db_error.h"
#include "storage/storage_format.h"
#include "storage/value.h"

namespace relational_db {

using Tuple = std::vector<std::byte>;

enum class ColumnType : uint8_t { INT, BIGINT, DOUBLE, BOOLEAN, VARCHAR };

struct ColumnReference {
    std::string table;
    std::string column;
};

struct Column {
    Column() = default;
    Column(std::string column_name, ColumnType column_type = ColumnType::INT,
           uint16_t length = 0, std::optional<ColumnReference> column_reference = std::nullopt)
        : name(std::move(column_name)),
          type(column_type),
          varchar_length(length),
          references(std::move(column_reference)) {}

    std::string name;
    ColumnType type{ColumnType::INT};
    uint16_t varchar_length{0};
    std::optional<ColumnReference> references;
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
