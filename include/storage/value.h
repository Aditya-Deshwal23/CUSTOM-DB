#ifndef RELDB_STORAGE_VALUE_H
#define RELDB_STORAGE_VALUE_H

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace relational_db {

using Value = std::variant<std::monostate, int32_t, int64_t, double, bool, std::string>;
using Row = std::vector<Value>;

}  // namespace relational_db

#endif
