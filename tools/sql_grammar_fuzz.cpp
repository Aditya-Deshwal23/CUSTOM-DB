#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sql_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    try {
        const relational_db::SqlStatement statement = relational_db::SqlParser(input).Parse();
        const std::string printed = relational_db::PrintSql(statement);
        static_cast<void>(relational_db::SqlParser(printed).Parse());
    } catch (const relational_db::SqlError&) {
    }
    return 0;
}
