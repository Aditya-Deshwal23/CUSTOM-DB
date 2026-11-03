#include "internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "crc32c.h"
#include "slotted_page.h"

namespace relational_db {
namespace {

constexpr uint64_t kDefaultPageRange = 256;
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::array<std::string_view, kHistorySeriesCount> kHistorySeries{
    "pool_hits",
    "pool_misses",
    "pool_evictions",
    "pool_dirty_write_backs",
    "disk_reads",
    "disk_writes",
    "disk_syncs",
    "disk_bytes",
    "rows_read",
    "rows_written",
    "errors",
    "requests",
    "queries",
    "active_requests",
    "active_statements",
    "statement_waiters",
    "index_scans",
    "sequential_scans",
    "rate_limited",
    "inspect_rate_limited",
    "busy_rejections",
    "wal_bytes",
    "wal_commits",
    "checkpoints",
    "recovery_records",
    "disk_read_latency_count",
    "disk_read_latency_sum_us",
    "disk_write_latency_count",
    "disk_write_latency_sum_us",
    "disk_sync_latency_count",
    "disk_sync_latency_sum_us",
    "select_duration_count",
    "select_duration_sum_us",
    "insert_duration_count",
    "insert_duration_sum_us",
    "update_duration_count",
    "update_duration_sum_us",
    "delete_duration_count",
    "delete_duration_sum_us",
    "ddl_duration_count",
    "ddl_duration_sum_us",
};

bool ParseUnsigned(std::string_view text, uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

uint64_t UnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool QueryUnsigned(const crow::request& request, const char* key, uint64_t fallback,
                   uint64_t& value) {
    const char* const text = request.url_params.get(key);
    if (text == nullptr) {
        value = fallback;
        return true;
    }
    return ParseUnsigned(text, value);
}

uint16_t ReadU16(const uint8_t* bytes, size_t length, size_t offset) {
    if (offset > length || length - offset < 2U) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Decoded field exceeds page bounds");
    }
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1U]) << 8U);
}

uint32_t ReadU32(const uint8_t* bytes, size_t length, size_t offset) {
    if (offset > length || length - offset < 4U) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Decoded field exceeds page bounds");
    }
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

uint64_t ReadU64(const uint8_t* bytes, size_t length, size_t offset) {
    if (offset > length || length - offset < 8U) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Decoded field exceeds page bounds");
    }
    uint64_t value = 0;
    for (size_t byte = 0; byte < 8U; ++byte) {
        value |= static_cast<uint64_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

int32_t ReadPageId(const uint8_t* bytes, size_t length, size_t offset) {
    return static_cast<int32_t>(ReadU32(bytes, length, offset));
}

void WriteU16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void WriteU32(uint8_t* bytes, uint32_t value) {
    for (size_t index = 0; index < 4U; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void WriteU64(uint8_t* bytes, uint64_t value) {
    for (size_t index = 0; index < 8U; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

std::string Base64(const uint8_t* data, size_t size) {
    std::string result;
    result.reserve(((size + 2U) / 3U) * 4U);
    for (size_t index = 0; index < size; index += 3U) {
        const uint32_t first = data[index];
        const uint32_t second = index + 1U < size ? data[index + 1U] : 0U;
        const uint32_t third = index + 2U < size ? data[index + 2U] : 0U;
        const uint32_t group = (first << 16U) | (second << 8U) | third;
        result.push_back(kBase64Alphabet[(group >> 18U) & 0x3fU]);
        result.push_back(kBase64Alphabet[(group >> 12U) & 0x3fU]);
        result.push_back(index + 1U < size ? kBase64Alphabet[(group >> 6U) & 0x3fU] : '=');
        result.push_back(index + 2U < size ? kBase64Alphabet[group & 0x3fU] : '=');
    }
    return result;
}

const char* PageTypeName(PageType type) {
    switch (type) {
        case PageType::HEADER:
            return "HEADER";
        case PageType::FREE:
            return "FREE";
        case PageType::BTREE_INTERNAL:
            return "BTREE_INTERNAL";
        case PageType::BTREE_LEAF:
            return "BTREE_LEAF";
        case PageType::HEAP:
            return "HEAP";
        case PageType::CATALOG:
            return "CATALOG";
        case PageType::INDEX_META:
            return "INDEX_META";
    }
    return "UNKNOWN";
}

uint8_t PageTypeByte(const InspectedPage& page) {
    return page.read_error.empty() ? static_cast<uint8_t>(page.type) : UINT8_MAX;
}

void AddRegion(crow::json::wvalue::list& regions, std::string_view name, uint32_t offset,
               uint32_t length, std::string_view kind) {
    crow::json::wvalue region;
    region["name"] = std::string(name);
    region["offset"] = offset;
    region["length"] = length;
    region["kind"] = std::string(kind);
    regions.push_back(std::move(region));
}

crow::json::wvalue DecodeIndexKeyJson(const uint8_t* bytes, ColumnType type);

crow::json::wvalue DecodePage(const InspectedPage& page,
                              crow::json::wvalue::list& regions,
                              std::optional<ColumnType> index_key_type = std::nullopt) {
    const auto* const body = reinterpret_cast<const uint8_t*>(page.body.data());
    constexpr size_t kBodySize = PAGE_DATA_SIZE;
    crow::json::wvalue decoded;
    AddRegion(regions, "page_header", 0, PAGE_HEADER_SIZE, "header");

    switch (page.type) {
        case PageType::HEADER:
            decoded["format_version"] = ReadU32(body, kBodySize, 8);
            decoded["page_size"] = ReadU32(body, kBodySize, 12);
            decoded["page_count"] = ReadU64(body, kBodySize, 16);
            decoded["free_list_head"] = ReadPageId(body, kBodySize, 24);
            decoded["catalog_root"] = ReadPageId(body, kBodySize, 28);
            decoded["header_crc32c"] = ReadU32(body, kBodySize, 32);
            AddRegion(regions, "database_header", PAGE_HEADER_SIZE, 36, "metadata");
            break;
        case PageType::FREE:
            decoded["next_page_id"] = ReadPageId(body, kBodySize, 0);
            AddRegion(regions, "free_link", PAGE_HEADER_SIZE, 4, "pointer");
            break;
        case PageType::HEAP: {
            const SlottedPage slotted(reinterpret_cast<const char*>(body));
            const uint16_t slots = slotted.GetNumSlots();
            const uint16_t free_ptr = slotted.GetFreeSpacePtr();
            decoded["slot_count"] = slots;
            decoded["free_space_bytes"] = slotted.GetFreeSpaceRemaining();
            decoded["next_page_id"] = slotted.GetNextPageId();
            AddRegion(regions, "heap_metadata", PAGE_HEADER_SIZE,
                      static_cast<uint32_t>(SlottedPage::HEADER_SIZE), "metadata");
            crow::json::wvalue::list slot_list;
            slot_list.reserve(slots);
            for (uint16_t slot_id = 0; slot_id < slots; ++slot_id) {
                const Slot slot = slotted.GetSlot(slot_id);
                crow::json::wvalue slot_json;
                slot_json["slot"] = slot_id;
                slot_json["offset"] = slot.offset;
                slot_json["length"] = slot.length;
                slot_json["flags"] = slot.flags;
                slot_json["tombstone"] = (slot.flags & SlottedPage::TOMBSTONE) != 0;
                slot_list.push_back(std::move(slot_json));
            }
            decoded["slots"] = std::move(slot_list);
            AddRegion(regions, "slot_directory", PAGE_HEADER_SIZE + SlottedPage::HEADER_SIZE,
                      static_cast<uint32_t>(slots) * SlottedPage::SLOT_SIZE, "slots");
            if (free_ptr < PAGE_DATA_SIZE) {
                AddRegion(regions, "tuple_data",
                          PAGE_HEADER_SIZE + free_ptr,
                          static_cast<uint32_t>(PAGE_DATA_SIZE - free_ptr), "tuple");
            }
            break;
        }
        case PageType::BTREE_LEAF: {
            const uint16_t count = ReadU16(body, kBodySize, 0);
            constexpr size_t kEntrySize = 24;
            constexpr size_t kCapacity = (PAGE_DATA_SIZE - 6U) / kEntrySize;
            if (count > kCapacity) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "B+ tree leaf count exceeds capacity",
                              page.page_id);
            }
            decoded["count"] = count;
            decoded["next_page_id"] = ReadPageId(body, kBodySize, 2);
            crow::json::wvalue::list entries;
            entries.reserve(count);
            for (uint16_t index = 0; index < count; ++index) {
                const size_t offset = 6U + static_cast<size_t>(index) * kEntrySize;
                crow::json::wvalue entry;
                entry["key_base64"] = Base64(body + offset, 16);
                if (index_key_type) {
                    entry["key"] = DecodeIndexKeyJson(body + offset, *index_key_type);
                }
                entry["rid"] = ReadU64(body, kBodySize, offset + 16U);
                entries.push_back(std::move(entry));
            }
            decoded["entries"] = std::move(entries);
            AddRegion(regions, "leaf_header", PAGE_HEADER_SIZE, 6, "metadata");
            AddRegion(regions, "leaf_entries", PAGE_HEADER_SIZE + 6U,
                      static_cast<uint32_t>(count) * kEntrySize, "index");
            break;
        }
        case PageType::BTREE_INTERNAL: {
            const uint16_t count = ReadU16(body, kBodySize, 0);
            constexpr size_t kEntrySize = 20;
            constexpr size_t kCapacity = (PAGE_DATA_SIZE - 2U) / kEntrySize - 1U;
            if (count > kCapacity) {
                throw DbError(ErrorCode::PAGE_CORRUPT,
                              "B+ tree internal count exceeds capacity", page.page_id);
            }
            decoded["count"] = count;
            crow::json::wvalue::list children;
            crow::json::wvalue::list entries;
            for (uint16_t index = 0; index <= count; ++index) {
                const size_t offset = 2U + static_cast<size_t>(index) * kEntrySize;
                const int32_t child = ReadPageId(body, kBodySize, offset + 16U);
                children.emplace_back(child);
                if (index != 0U) {
                    crow::json::wvalue entry;
                    entry["key_base64"] = Base64(body + offset, 16);
                    if (index_key_type) {
                        entry["key"] = DecodeIndexKeyJson(body + offset, *index_key_type);
                    }
                    entry["child_page_id"] = child;
                    entries.push_back(std::move(entry));
                }
            }
            decoded["children"] = std::move(children);
            decoded["entries"] = std::move(entries);
            AddRegion(regions, "internal_header", PAGE_HEADER_SIZE, 2, "metadata");
            AddRegion(regions, "internal_entries", PAGE_HEADER_SIZE + 2U,
                      static_cast<uint32_t>(count + 1U) * kEntrySize, "index");
            break;
        }
        case PageType::CATALOG: {
            const uint32_t version = ReadU32(body, kBodySize, 0);
            decoded["version"] = version;
            if (version == 3U) {
                const uint32_t next_page = ReadU32(body, kBodySize, 4);
                const uint32_t records = ReadU32(body, kBodySize, 8);
                const uint32_t payload_bytes = ReadU32(body, kBodySize, 12);
                if (payload_bytes > kBodySize - 16U) {
                    throw DbError(ErrorCode::PAGE_CORRUPT,
                                  "Catalog payload length exceeds page bounds", page.page_id);
                }
                decoded["next_page_id"] = static_cast<int32_t>(next_page);
                decoded["records_on_page"] = records;
                decoded["payload_bytes"] = payload_bytes;
                AddRegion(regions, "catalog_header", PAGE_HEADER_SIZE, 16, "metadata");
                AddRegion(regions, "catalog_records", PAGE_HEADER_SIZE + 16U, payload_bytes,
                          "records");
            } else if (version == 1U || version == 2U) {
                decoded["table_count"] = ReadU16(body, kBodySize, 4);
                AddRegion(regions, "catalog_header", PAGE_HEADER_SIZE, 6, "metadata");
                AddRegion(regions, "catalog_records", PAGE_HEADER_SIZE + 6U,
                          static_cast<uint32_t>(kBodySize - 6U), "records");
            } else {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Unsupported catalog page version",
                              page.page_id);
            }
            break;
        }
        case PageType::INDEX_META: {
            decoded["root_page_id"] = ReadPageId(body, kBodySize, 0);
            decoded["height"] = ReadU16(body, kBodySize, 4);
            decoded["key_kind"] = ReadU16(body, kBodySize, 6);
            AddRegion(regions, "index_metadata", PAGE_HEADER_SIZE, 8, "metadata");
            break;
        }
    }
    return decoded;
}

std::array<uint8_t, PAGE_SIZE> RebuildPhysicalPage(const InspectedPage& page) {
    std::array<uint8_t, PAGE_SIZE> physical{};
    WriteU16(physical.data() + 4, static_cast<uint16_t>(page.type));
    WriteU16(physical.data() + 6, page.flags);
    WriteU64(physical.data() + 8, page.lsn);
    std::memcpy(physical.data() + PAGE_HEADER_SIZE, page.body.data(), PAGE_DATA_SIZE);
    WriteU32(physical.data(), Crc32c(physical.data() + 4, PAGE_SIZE - 4U));
    return physical;
}

crow::response Error(ApiServer::Impl& server, const crow::request& request, int status,
                     std::string code, std::string message) {
    ApiMiddleware::context& context = server.app.get_context<ApiMiddleware>(request);
    crow::response response =
        ErrorResponse(status, std::move(code), std::move(message), context.request_id);
    if (status == 429 || status == 503) {
        response.add_header("Retry-After", "1");
    }
    return response;
}

crow::json::wvalue DecodeIndexKeyJson(const uint8_t* bytes, ColumnType type) {
    switch (type) {
        case ColumnType::INT: {
            IndexKey16 key;
            std::copy_n(bytes, key.bytes.size(), key.bytes.begin());
            return crow::json::wvalue(DecodeIndexKeyInt32(key));
        }
        case ColumnType::BIGINT: {
            uint64_t ordered = 0;
            for (size_t byte = 0; byte < sizeof(ordered); ++byte) {
                ordered = (ordered << 8U) | bytes[byte];
            }
            ordered ^= 0x8000000000000000ULL;
            int64_t value = 0;
            std::memcpy(&value, &ordered, sizeof(value));
            return crow::json::wvalue(value);
        }
        case ColumnType::DOUBLE: {
            uint64_t sortable = 0;
            for (size_t byte = 0; byte < sizeof(sortable); ++byte) {
                sortable = (sortable << 8U) | bytes[byte];
            }
            const uint64_t bits = (sortable & 0x8000000000000000ULL) != 0U
                                      ? sortable ^ 0x8000000000000000ULL
                                      : ~sortable;
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            return crow::json::wvalue(value);
        }
        case ColumnType::BOOLEAN:
            if (bytes[0] > 1U) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid boolean index key");
            }
            return crow::json::wvalue(bytes[0] != 0U);
        case ColumnType::VARCHAR: {
            crow::json::wvalue value;
            value["hash_base64"] = Base64(bytes, 8);
            return value;
        }
    }
    throw DbError(ErrorCode::PAGE_CORRUPT, "Unknown index key type");
}

crow::json::wvalue IndexKeyJson(const std::optional<IndexKey16>& key, ColumnType type) {
    if (!key) {
        return crow::json::wvalue(nullptr);
    }
    return DecodeIndexKeyJson(key->bytes.data(), type);
}

crow::json::wvalue SqlValueJson(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return crow::json::wvalue(nullptr);
    }
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        return crow::json::wvalue(*integer);
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return crow::json::wvalue(*integer);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return crow::json::wvalue(*number);
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return crow::json::wvalue(*boolean);
    }
    return crow::json::wvalue(std::get<std::string>(value));
}

std::string ColumnTypeLabel(ColumnType type) {
    switch (type) {
        case ColumnType::INT:
            return "INT";
        case ColumnType::BIGINT:
            return "BIGINT";
        case ColumnType::DOUBLE:
            return "DOUBLE";
        case ColumnType::BOOLEAN:
            return "BOOLEAN";
        case ColumnType::VARCHAR:
            return "VARCHAR";
    }
    return "UNKNOWN";
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

void ApiServer::Impl::RegisterInspectorRoutes() {
    CROW_ROUTE(app, "/api/v1/tables/<string>/rows")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& table_name) {
                uint64_t limit = 100;
                const char* after_text = request.url_params.get("after");
                const char* order_text = request.url_params.get("order");
                const char* filter_text = request.url_params.get("filter");
                if (table_name.empty() || table_name.size() > 256U ||
                    !QueryUnsigned(request, "limit", 100, limit) || limit == 0U ||
                    limit > 500U || (after_text != nullptr &&
                                     std::char_traits<char>::length(after_text) > 9000U) ||
                    (order_text != nullptr &&
                     std::char_traits<char>::length(order_text) > 256U) ||
                    (filter_text != nullptr &&
                     std::char_traits<char>::length(filter_text) > 8192U)) {
                    return Error(*this, request, 400, "INVALID_ROWS_RANGE",
                                 "rows parameters exceed their size or value limits");
                }

                std::optional<SqlTableMetadata> table;
                try {
                    table = executor->GetTable(table_name);
                } catch (const DbError& error) {
                    const int status = error.Code() == ErrorCode::UNKNOWN_TABLE
                                           ? 404
                                           : (error.Code() == ErrorCode::RESULT_LIMIT_EXCEEDED
                                                  ? 413
                                                  : 500);
                    return Error(*this, request, status, "ROWS_METADATA_FAILED", error.what());
                }
                if (!table) {
                    return Error(*this, request, 404, "UNKNOWN_TABLE",
                                 "Unknown table: " + table_name);
                }

                size_t order_column = table->schema.primary_key.value_or(0U);
                bool descending = false;
                if (order_text != nullptr) {
                    std::string order(order_text);
                    const size_t suffix = order.rfind(':');
                    if (suffix != std::string::npos) {
                        const std::string_view direction(order.data() + suffix + 1U,
                                                         order.size() - suffix - 1U);
                        if (!EqualsIgnoreCase(direction, "desc")) {
                            return Error(*this, request, 400, "INVALID_ORDER",
                                         "order direction must be :desc when specified");
                        }
                        descending = true;
                        order.resize(suffix);
                    }
                    const auto column = std::find_if(
                        table->schema.columns.begin(), table->schema.columns.end(),
                        [&order](const Column& candidate) {
                            return EqualsIgnoreCase(candidate.name, order);
                        });
                    if (column == table->schema.columns.end()) {
                        return Error(*this, request, 400, "INVALID_ORDER",
                                     "order must name a column in the requested table");
                    }
                    order_column =
                        static_cast<size_t>(column - table->schema.columns.begin());
                }

                std::vector<SqlInspectFilter> filters;
                if (filter_text != nullptr) {
                    const crow::json::rvalue parsed_filters = crow::json::load(filter_text);
                    if (!parsed_filters ||
                        (parsed_filters.t() != crow::json::type::Object &&
                         parsed_filters.t() != crow::json::type::List)) {
                        return Error(*this, request, 400, "INVALID_FILTER",
                                     "filter must be a JSON object or array of objects");
                    }
                    const auto parse_filter = [&filters, &table](const crow::json::rvalue& item) {
                        if (item.t() != crow::json::type::Object || !item.has("column") ||
                            !item.has("op") || item["column"].t() != crow::json::type::String ||
                            item["op"].t() != crow::json::type::String) {
                            throw std::invalid_argument(
                                "Each filter requires string column and op fields");
                        }
                        const std::string column_name = item["column"].s();
                        const auto column = std::find_if(
                            table->schema.columns.begin(), table->schema.columns.end(),
                            [&column_name](const Column& candidate) {
                                return EqualsIgnoreCase(candidate.name, column_name);
                            });
                        if (column == table->schema.columns.end()) {
                            throw std::invalid_argument("Filter column does not exist");
                        }
                        std::string operation = item["op"].s();
                        std::transform(operation.begin(), operation.end(), operation.begin(),
                                       [](unsigned char ch) {
                                           return static_cast<char>(std::toupper(ch));
                                       });
                        const size_t column_index =
                            static_cast<size_t>(column - table->schema.columns.begin());
                        if (operation == "ISNULL") {
                            if (item.has("value")) {
                                throw std::invalid_argument("ISNULL does not accept a value");
                            }
                            filters.push_back(
                                SqlInspectFilter{column_index, operation, std::monostate{}});
                            return;
                        }
                        if (operation != "=" && operation != "<>" && operation != "<" &&
                            operation != "<=" && operation != ">" && operation != ">=" &&
                            operation != "LIKE") {
                            throw std::invalid_argument("Unsupported filter operator");
                        }
                        if (!item.has("value")) {
                            throw std::invalid_argument("Filter value is required");
                        }
                        const crow::json::rvalue& raw_value = item["value"];
                        Value value;
                        if (raw_value.t() == crow::json::type::Null) {
                            value = std::monostate{};
                        } else {
                            const std::string number =
                                raw_value.t() == crow::json::type::Number
                                    ? static_cast<std::string>(raw_value)
                                    : std::string{};
                            switch (column->type) {
                                case ColumnType::INT: {
                                    int32_t parsed = 0;
                                    if (raw_value.t() != crow::json::type::Number) {
                                        throw std::invalid_argument("INT filter requires a number");
                                    }
                                    const auto result = std::from_chars(
                                        number.data(), number.data() + number.size(), parsed);
                                    if (result.ec != std::errc{} ||
                                        result.ptr != number.data() + number.size()) {
                                        throw std::invalid_argument("INT filter is out of range");
                                    }
                                    value = parsed;
                                    break;
                                }
                                case ColumnType::BIGINT: {
                                    int64_t parsed = 0;
                                    if (raw_value.t() != crow::json::type::Number) {
                                        throw std::invalid_argument(
                                            "BIGINT filter requires a number");
                                    }
                                    const auto result = std::from_chars(
                                        number.data(), number.data() + number.size(), parsed);
                                    if (result.ec != std::errc{} ||
                                        result.ptr != number.data() + number.size()) {
                                        throw std::invalid_argument(
                                            "BIGINT filter is out of range");
                                    }
                                    value = parsed;
                                    break;
                                }
                                case ColumnType::DOUBLE: {
                                    double parsed = 0.0;
                                    if (raw_value.t() != crow::json::type::Number) {
                                        throw std::invalid_argument(
                                            "DOUBLE filter requires a number");
                                    }
                                    const auto result = std::from_chars(
                                        number.data(), number.data() + number.size(), parsed);
                                    if (result.ec != std::errc{} ||
                                        result.ptr != number.data() + number.size()) {
                                        throw std::invalid_argument(
                                            "DOUBLE filter is not a finite number");
                                    }
                                    value = parsed;
                                    break;
                                }
                                case ColumnType::BOOLEAN:
                                    if (raw_value.t() == crow::json::type::True) {
                                        value = true;
                                    } else if (raw_value.t() == crow::json::type::False) {
                                        value = false;
                                    } else {
                                        throw std::invalid_argument(
                                            "BOOLEAN filter requires true or false");
                                    }
                                    break;
                                case ColumnType::VARCHAR:
                                    if (raw_value.t() != crow::json::type::String ||
                                        raw_value.s().size() > column->varchar_length ||
                                        raw_value.s().size() > 1024U) {
                                        throw std::invalid_argument(
                                            "VARCHAR filter exceeds the column or API limit");
                                    }
                                    if (operation == "LIKE" &&
                                        raw_value.s().size() > 256U) {
                                        throw std::invalid_argument(
                                            "LIKE pattern exceeds 256 bytes");
                                    }
                                    value = raw_value.s();
                                    break;
                            }
                        }
                        if (operation == "LIKE" &&
                            !std::holds_alternative<std::string>(value) &&
                            !std::holds_alternative<std::monostate>(value)) {
                            throw std::invalid_argument("LIKE filter requires a string value");
                        }
                        filters.push_back(
                            SqlInspectFilter{column_index, operation, std::move(value)});
                    };
                    try {
                        if (parsed_filters.t() == crow::json::type::Object) {
                            parse_filter(parsed_filters);
                        } else {
                            if (parsed_filters.size() > 16U) {
                                return Error(*this, request, 400, "INVALID_FILTER",
                                             "filter supports at most 16 predicates");
                            }
                            for (size_t index = 0; index < parsed_filters.size(); ++index) {
                                parse_filter(parsed_filters[index]);
                            }
                        }
                    } catch (const std::invalid_argument& error) {
                        return Error(*this, request, 400, "INVALID_FILTER", error.what());
                    }
                }

                SqlInspectedRows inspected;
                try {
                    inspected = executor->InspectRows(
                        table_name, limit, after_text == nullptr ? std::string_view{}
                                                                  : std::string_view(after_text),
                        order_column, descending, filters, config.inspect_max_pages,
                        std::chrono::milliseconds(config.statement_timeout_ms));
                } catch (const DbError& error) {
                    const int status =
                        error.Code() == ErrorCode::UNKNOWN_TABLE
                            ? 404
                            : (error.Code() == ErrorCode::INVALID_ARGUMENT
                                   ? 400
                                   : (error.Code() == ErrorCode::RESULT_LIMIT_EXCEEDED
                                          ? 413
                                          : (error.Code() == ErrorCode::QUERY_TIMEOUT ? 503 : 500)));
                    return Error(*this, request, status, "ROWS_INSPECT_FAILED", error.what());
                }

                crow::json::wvalue response;
                crow::json::wvalue::list columns;
                for (const Column& column : table->schema.columns) {
                    crow::json::wvalue item;
                    item["name"] = column.name;
                    item["type"] = ColumnTypeLabel(column.type);
                    columns.push_back(std::move(item));
                }
                response["columns"] = std::move(columns);
                crow::json::wvalue::list rows;
                for (const SqlInspectedRow& row : inspected.rows) {
                    crow::json::wvalue item;
                    item["rid"] =
                        std::to_string(row.rid.page_id) + ":" + std::to_string(row.rid.slot);
                    crow::json::wvalue values;
                    for (size_t column = 0; column < row.values.size(); ++column) {
                        values[table->schema.columns[column].name] =
                            SqlValueJson(row.values[column]);
                    }
                    item["values"] = std::move(values);
                    rows.push_back(std::move(item));
                }
                response["rows"] = std::move(rows);
                response["row_count"] = inspected.rows.size();
                response["next_cursor"] = inspected.next_cursor.empty()
                                              ? crow::json::wvalue(nullptr)
                                              : crow::json::wvalue(inspected.next_cursor);
                const std::string serialized = response.dump();
                if (serialized.size() > 8U * 1024U * 1024U) {
                    return Error(*this, request, 413, "INSPECT_RESULT_TOO_LARGE",
                                 "Table rows response exceeds 8 MiB");
                }
                return JsonResponse(200, response);
            });

    CROW_ROUTE(app, "/api/v1/inspect/indexes/<string>/tree")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& name) {
                uint64_t max_nodes = config.inspect_max_nodes;
                if (name.empty() || name.size() > 256U ||
                    !QueryUnsigned(request, "max_nodes", config.inspect_max_nodes, max_nodes) ||
                    max_nodes == 0U || max_nodes > config.inspect_max_nodes) {
                    return Error(*this, request, 400, "INVALID_INDEX_TREE_RANGE",
                                 "index name and max_nodes must fit configured limits");
                }
                try {
                    const SqlIndexTreeInspection inspection =
                        executor->InspectIndexTree(name, max_nodes);
                    crow::json::wvalue response;
                    crow::json::wvalue::list nodes;
                    nodes.reserve(inspection.tree.nodes.size());
                    for (const SqlIndexTree::InspectionNode& node : inspection.tree.nodes) {
                        crow::json::wvalue item;
                        item["page_id"] = node.page_id;
                        item["level"] = node.level;
                        item["count"] = node.count;
                        item["min_key"] = IndexKeyJson(node.minimum, inspection.column_type);
                        item["max_key"] = IndexKeyJson(node.maximum, inspection.column_type);
                        crow::json::wvalue::list children;
                        for (page_id_t child : node.children) {
                            children.emplace_back(child);
                        }
                        item["children"] = std::move(children);
                        item["next"] = node.next == INVALID_PAGE_ID
                                           ? crow::json::wvalue(nullptr)
                                           : crow::json::wvalue(node.next);
                        item["fill_pct"] = node.fill_pct;
                        nodes.push_back(std::move(item));
                    }
                    response["nodes"] = std::move(nodes);
                    response["truncated"] = inspection.tree.truncated;
                    return JsonResponse(200, response);
                } catch (const DbError& error) {
                    const int status = error.Code() == ErrorCode::UNKNOWN_TABLE ? 404 : 500;
                    return Error(*this, request, status,
                                 status == 404 ? "UNKNOWN_INDEX" : "INDEX_TREE_INSPECT_FAILED",
                                 error.what());
                }
            });

    CROW_ROUTE(app, "/api/v1/inspect/indexes/<string>/trace")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& name) {
                const char* key = request.url_params.get("key");
                if (name.empty() || name.size() > 256U || key == nullptr ||
                    std::char_traits<char>::length(key) > 1024U) {
                    return Error(*this, request, 400, "INVALID_INDEX_TRACE",
                                 "index name and key are required and bounded");
                }
                try {
                    const auto [lookup, decisions] =
                        executor->TraceIndex(name, std::string_view(key));
                    crow::json::wvalue response;
                    response["found"] = lookup.value.has_value();
                    if (lookup.value) {
                        const RID rid = RID::Unpack(*lookup.value);
                        response["rid"] = std::to_string(rid.page_id) + ":" +
                                          std::to_string(rid.slot);
                    } else {
                        response["rid"] = crow::json::wvalue(nullptr);
                    }
                    crow::json::wvalue::list path;
                    path.reserve(decisions.size());
                    for (const SqlIndexTree::TraceDecision& decision : decisions) {
                        crow::json::wvalue item;
                        item["page_id"] = decision.page_id;
                        item["level"] = decision.level;
                        item["slot"] = decision.slot;
                        item["decision"] = decision.decision;
                        path.push_back(std::move(item));
                    }
                    response["path"] = std::move(path);
                    return JsonResponse(200, response);
                } catch (const DbError& error) {
                    const int status = error.Code() == ErrorCode::UNKNOWN_TABLE
                                           ? 404
                                           : (error.Code() == ErrorCode::INVALID_ARGUMENT ? 400
                                                                                         : 500);
                    return Error(*this, request, status,
                                 status == 404 ? "UNKNOWN_INDEX"
                                               : (status == 400 ? "INVALID_INDEX_KEY"
                                                                : "INDEX_TRACE_FAILED"),
                                 error.what());
                }
            });

    CROW_ROUTE(app, "/api/v1/inspect/wal")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            uint64_t limit = 100;
            if (!QueryUnsigned(request, "limit", 100, limit) || limit == 0U || limit > 500U) {
                return Error(*this, request, 400, "INVALID_WAL_LIMIT",
                             "limit must be an integer from 1 through 500");
            }
            std::optional<uint64_t> before_lsn;
            if (const char* value = request.url_params.get("before_lsn")) {
                uint64_t parsed = 0;
                if (!ParseUnsigned(value, parsed)) {
                    return Error(*this, request, 400, "INVALID_LSN",
                                 "before_lsn must be a non-negative integer");
                }
                before_lsn = parsed;
            }
            if (executor->Wal() == nullptr) {
                return Error(*this, request, 503, "WAL_UNAVAILABLE",
                             "WAL inspection is unavailable");
            }
            WalInspectionSnapshot snapshot;
            try {
                snapshot =
                    executor->InspectWalRecords(static_cast<size_t>(limit), before_lsn);
            } catch (const DbError& error) {
                return Error(*this, request, 500, "WAL_INSPECT_FAILED", error.what());
            }
            crow::json::wvalue response;
            crow::json::wvalue::list records;
            crow::json::wvalue::list commits;
            for (const WalInspectionRecord& record : snapshot.records) {
                crow::json::wvalue item;
                item["lsn"] = record.lsn;
                item["type"] = record.type == LogRecordType::PAGE_IMAGE
                                   ? "PAGE_IMAGE"
                                   : (record.type == LogRecordType::COMMIT ? "COMMIT"
                                                                          : "CHECKPOINT");
                item["txn_id"] = record.txn_id;
                if (record.page_id == INVALID_PAGE_ID) {
                    item["page_id"] = crow::json::wvalue(nullptr);
                } else {
                    item["page_id"] = record.page_id;
                }
                item["payload_bytes"] = record.payload_bytes;
                item["crc_ok"] = record.crc_ok;
                item["file_offset"] = record.file_offset;
                records.push_back(std::move(item));
                if (record.type == LogRecordType::COMMIT) {
                    commits.emplace_back(record.lsn);
                }
            }
            response["records"] = std::move(records);
            response["commit_lsns"] = std::move(commits);
            response["size_bytes"] = snapshot.size_bytes;
            response["base_lsn"] = snapshot.base_lsn;
            response["truncated"] = snapshot.truncated;
            return JsonResponse(200, response);
        });

    CROW_ROUTE(app, "/api/v1/inspect/freelist")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            std::vector<InspectedPage> header_pages;
            try {
                header_pages = executor->InspectPages(0, 1);
            } catch (const DbError& error) {
                return Error(*this, request, 500, "FREELIST_INSPECT_FAILED", error.what());
            }
            if (!header_pages.front().read_error.empty()) {
                return Error(*this, request, 500, "FREELIST_INSPECT_FAILED",
                             header_pages.front().read_error);
            }
            const auto* header_body =
                reinterpret_cast<const uint8_t*>(header_pages.front().body.data());
            page_id_t page_id = ReadPageId(header_body, PAGE_DATA_SIZE, 24);
            std::unordered_set<page_id_t> visited;
            crow::json::wvalue::list pages;
            bool truncated = false;
            while (page_id != INVALID_PAGE_ID) {
                if (pages.size() >= config.inspect_max_pages) {
                    truncated = true;
                    break;
                }
                if (page_id <= 0 || !visited.insert(page_id).second) {
                    return Error(*this, request, 500, "FREELIST_CORRUPT",
                                 "Free-list page chain is invalid or cyclic");
                }
                std::vector<InspectedPage> current;
                try {
                    current = executor->InspectPages(page_id, 1);
                } catch (const DbError& error) {
                    return Error(*this, request, 500, "FREELIST_INSPECT_FAILED", error.what());
                }
                if (!current.front().read_error.empty() ||
                    current.front().type != PageType::FREE) {
                    return Error(*this, request, 500, "FREELIST_CORRUPT",
                                 "Free-list contains an unreadable or non-free page");
                }
                pages.emplace_back(page_id);
                const auto* body = reinterpret_cast<const uint8_t*>(current.front().body.data());
                page_id = ReadPageId(body, PAGE_DATA_SIZE, 0);
            }
            crow::json::wvalue response;
            response["pages"] = std::move(pages);
            response["truncated"] = truncated;
            return JsonResponse(200, response);
        });

    CROW_ROUTE(app, "/api/v1/slow-queries")
        .methods(crow::HTTPMethod::GET)([this]() {
            const auto inspection_gate = executor->AcquireInspectionGate();
            (void)inspection_gate;
            std::deque<SlowQuerySample> samples;
            {
                std::lock_guard<std::mutex> lock(slow_queries_mutex);
                samples = slow_queries;
            }
            crow::json::wvalue response;
            crow::json::wvalue::list queries;
            queries.reserve(samples.size());
            for (const SlowQuerySample& sample : samples) {
                crow::json::wvalue item;
                item["ts_ms"] = sample.ts_ms;
                item["elapsed_us"] = sample.elapsed_us;
                item["statement"] = sample.statement;
                item["sql_preview"] = sample.sql_preview;
                item["plan_summary"] = sample.plan_summary;
                item["request_id"] = sample.request_id;
                queries.push_back(std::move(item));
            }
            response["queries"] = std::move(queries);
            return JsonResponse(200, response);
        });

    CROW_ROUTE(app, "/api/v1/stats/history")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            uint64_t window_seconds = 900;
            uint64_t step_seconds = 1;
            if (!QueryUnsigned(request, "window_s", 900, window_seconds) ||
                !QueryUnsigned(request, "step_s", 1, step_seconds) ||
                window_seconds == 0U || window_seconds > 3600U || step_seconds == 0U ||
                step_seconds > 60U) {
                return Error(*this, request, 400, "INVALID_HISTORY_RANGE",
                             "window_s must be 1..3600 and step_s must be 1..60");
            }
            const auto inspection_gate = executor->AcquireInspectionGate();
            (void)inspection_gate;
            std::vector<ApiHistorySample> samples;
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                samples.assign(history.begin(), history.end());
            }
            const uint64_t now_ms = UnixTimeMs();
            const uint64_t earliest = now_ms - std::min(now_ms, window_seconds * 1000U);
            const uint64_t step_ms = step_seconds * 1000U;
            std::vector<ApiHistorySample> selected;
            uint64_t next_sample_ms = 0;
            for (const ApiHistorySample& sample : samples) {
                if (sample.ts_ms < earliest || sample.ts_ms > now_ms) {
                    continue;
                }
                if (next_sample_ms == 0U || sample.ts_ms >= next_sample_ms) {
                    selected.push_back(sample);
                    next_sample_ms = sample.ts_ms + step_ms;
                }
            }

            crow::json::wvalue response;
            crow::json::wvalue::list timestamps;
            timestamps.reserve(selected.size());
            crow::json::wvalue series;
            for (size_t series_index = 0; series_index < kHistorySeries.size(); ++series_index) {
                crow::json::wvalue::list values;
                values.reserve(selected.size());
                for (const ApiHistorySample& sample : selected) {
                    values.emplace_back(sample.values[series_index]);
                }
                series[std::string(kHistorySeries[series_index])] = std::move(values);
            }
            for (const ApiHistorySample& sample : selected) {
                timestamps.emplace_back(sample.ts_ms);
            }
            response["ts_ms"] = std::move(timestamps);
            response["series"] = std::move(series);
            response["step_s"] = step_seconds;
            return JsonResponse(200, response);
        });

    CROW_ROUTE(app, "/api/v1/inspect/pages")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            uint64_t from = 0;
            uint64_t count = kDefaultPageRange;
            if (!QueryUnsigned(request, "from", 0, from) ||
                !QueryUnsigned(request, "count", kDefaultPageRange, count) ||
                count == 0U || count > config.inspect_max_pages ||
                from > static_cast<uint64_t>(std::numeric_limits<page_id_t>::max())) {
                return Error(*this, request, 400, "INVALID_INSPECT_RANGE",
                             "from and count must be non-negative integers within inspection limits");
            }
            const uint64_t total = disk_manager->SizeInPages();
            if (from > total) {
                return Error(*this, request, 422, "PAGE_OUT_OF_RANGE",
                             "from is beyond the logical database page count");
            }
            count = std::min(count, total - from);
            std::vector<InspectedPage> pages;
            try {
                pages = executor->InspectPages(static_cast<page_id_t>(from), count);
            } catch (const DbError& error) {
                return Error(*this, request, 500, "INSPECT_FAILED", error.what());
            }
            std::vector<uint8_t> types;
            types.reserve(pages.size());
            uint64_t corrupt = 0;
            for (const InspectedPage& page : pages) {
                types.push_back(PageTypeByte(page));
                corrupt += page.crc_ok ? 0U : 1U;
            }
            crow::json::wvalue response;
            response["from"] = from;
            response["count"] = pages.size();
            response["total"] = total;
            response["page_types_base64"] = Base64(types.data(), types.size());
            response["corrupt_pages"] = corrupt;
            return JsonResponse(200, response);
        });

    CROW_ROUTE(app, "/api/v1/inspect/pages/<int>")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request, int page_id) {
            if (page_id < 0 ||
                static_cast<uint64_t>(page_id) >= disk_manager->SizeInPages()) {
                return Error(*this, request, 422, "PAGE_OUT_OF_RANGE",
                             "Page ID is outside the logical database page range");
            }
            const char* raw_param = request.url_params.get("raw");
            const std::string_view raw_value = raw_param == nullptr ? "0" : raw_param;
            if (raw_value != "0" && raw_value != "1") {
                return Error(*this, request, 400, "INVALID_RAW_FLAG", "raw must be 0 or 1");
            }
            const bool include_raw = raw_value == "1";
            if (include_raw && !config.inspect_raw_bytes) {
                return Error(*this, request, 403, "RAW_INSPECTION_DISABLED",
                             "Raw page bytes are disabled by INSPECT_RAW_BYTES");
            }
            std::vector<InspectedPage> pages;
            try {
                pages = executor->InspectPages(static_cast<page_id_t>(page_id), 1U);
            } catch (const DbError& error) {
                return Error(*this, request, 500, "INSPECT_FAILED", error.what());
            }
            const InspectedPage& page = pages.front();
            crow::json::wvalue response;
            response["page_id"] = page.page_id;
            response["type"] = PageTypeName(page.type);
            response["crc_ok"] = page.crc_ok;
            response["lsn"] = page.lsn;
            crow::json::wvalue::list regions;
            if (page.read_error.empty()) {
                try {
                    if (page.type == PageType::HEAP) {
                        const std::optional<SqlHeapPageInspection> heap =
                            executor->InspectHeapPage(page.page_id, config.inspect_max_pages);
                        if (heap && heap->header_page) {
                            const auto* body =
                                reinterpret_cast<const uint8_t*>(page.body.data());
                            response["decoded"]["table"] = heap->table;
                            response["decoded"]["first_page_id"] =
                                ReadPageId(body, PAGE_DATA_SIZE, 0);
                            response["decoded"]["last_page_id"] =
                                ReadPageId(body, PAGE_DATA_SIZE, 4);
                            AddRegion(regions, "page_header", 0, PAGE_HEADER_SIZE, "header");
                            AddRegion(regions, "heap_header", PAGE_HEADER_SIZE, 8, "metadata");
                        } else {
                            response["decoded"] = DecodePage(page, regions);
                            if (heap) {
                                response["decoded"]["table"] = heap->table;
                                crow::json::wvalue::list tuples;
                                for (const SqlInspectedRow& row : heap->rows) {
                                    crow::json::wvalue tuple;
                                    tuple["slot"] = row.rid.slot;
                                    tuple["rid"] = std::to_string(row.rid.page_id) + ":" +
                                                   std::to_string(row.rid.slot);
                                    crow::json::wvalue values;
                                    for (size_t column = 0; column < row.values.size(); ++column) {
                                        values[heap->column_names[column]] =
                                            SqlValueJson(row.values[column]);
                                    }
                                    tuple["values"] = std::move(values);
                                    tuples.push_back(std::move(tuple));
                                }
                                response["decoded"]["tuples"] = std::move(tuples);
                            }
                        }
                    } else {
                        std::optional<SqlIndexPageOwner> index_owner;
                        if (page.type == PageType::BTREE_LEAF ||
                            page.type == PageType::BTREE_INTERNAL ||
                            page.type == PageType::INDEX_META) {
                            index_owner = executor->FindIndexPageOwner(
                                page.page_id, config.inspect_max_pages);
                        }
                        response["decoded"] =
                            DecodePage(page, regions,
                                       index_owner ? std::optional<ColumnType>(
                                                         index_owner->column_type)
                                                   : std::nullopt);
                        if (index_owner) {
                            response["decoded"]["index"] = index_owner->name;
                            response["decoded"]["column"] = index_owner->column;
                        }
                    }
                } catch (const DbError& error) {
                    response["decoded"]["decode_error"] = error.what();
                }
            } else {
                response["decoded"]["decode_error"] = page.read_error;
            }
            response["regions"] = std::move(regions);
            if (include_raw && page.read_error.empty()) {
                const auto physical = RebuildPhysicalPage(page);
                response["hex_base64"] = Base64(physical.data(), physical.size());
            }
            return JsonResponse(200, response);
        });
}

}  // namespace relational_db
